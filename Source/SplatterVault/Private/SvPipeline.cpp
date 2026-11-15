// Copyright (c) SplatterVault. Licensed under the MIT License.

#include "SvPipeline.h"

#include "SvJson.h"

struct FSvPipeline::FAttempt
{
	FSvCall Call;
	FString Bearer;
	TSvCallback<FSvHttpResponse> Done;
	int32 Attempt = 0;
	double StartedMono = 0;
};

FSvPipeline::FSvPipeline(const FString& InBaseUrl, TSharedRef<ISvHttpTransport> InTransport, const FSvRetryPolicy& InRetry, double InTimeout,
	TFunction<FDateTime()> InClock, TFunction<double()> InMono, TFunction<void(double, TFunction<void()>)> InDelay,
	TFunction<void(ELogVerbosity::Type, const FString&)> InLog)
	: BaseUrl(InBaseUrl)
	, Transport(InTransport)
	, RetryPolicy(InRetry)
	, TimeoutSeconds(InTimeout)
	, Clock(MoveTemp(InClock))
	, MonoClock(MoveTemp(InMono))
	, DelayFn(MoveTemp(InDelay))
	, LogFn(MoveTemp(InLog))
{
}

FSvPipeline::~FSvPipeline()
{
	*Alive = false;
}

void FSvPipeline::Log(ELogVerbosity::Type Verbosity, const FString& Message) const
{
	if (LogFn) LogFn(Verbosity, Message);
}

FString FSvPipeline::PathOnly(const FString& Path)
{
	int32 Q = INDEX_NONE;
	return Path.FindChar(TEXT('?'), Q) ? Path.Left(Q) : Path;
}

FSvHttpRequest FSvPipeline::BuildRequest(const FSvCall& Call, const FString& Bearer) const
{
	FSvHttpRequest R;
	R.Method = Call.Method;
	R.Url = BaseUrl + Call.Path;
	R.TimeoutSeconds = Call.TimeoutSeconds.IsSet() && Call.TimeoutSeconds.GetValue() > 0 ? Call.TimeoutSeconds.GetValue() : TimeoutSeconds;
	R.Headers.Add(TEXT("Accept"), TEXT("application/json"));
	if (!Call.Body.IsEmpty())
	{
		R.Body = Call.Body;
		// The API refuses any body that is not application/json.
		R.Headers.Add(TEXT("Content-Type"), TEXT("application/json"));
	}
	if (!Bearer.IsEmpty()) R.Headers.Add(TEXT("Authorization"), TEXT("Bearer ") + Bearer);
	for (const TPair<FString, FString>& H : Call.Headers)
	{
		R.Headers.Add(H.Key, H.Value);
	}
	// A player client never sends an API key, whatever a caller put in the headers.
	R.Headers.Remove(TEXT("X-API-Key"));
	return R;
}

bool FSvPipeline::ParseRetryAfter(const FString& Header, const FDateTime& Now, double& OutSeconds)
{
	const FString Text = Header.TrimStartAndEnd();
	if (Text.IsEmpty()) return false;
	bool bDigits = true;
	for (const TCHAR C : Text)
	{
		if (C < TEXT('0') || C > TEXT('9'))
		{
			bDigits = false;
			break;
		}
	}
	if (bDigits)
	{
		const int64 Seconds = Text.Len() > 6 ? 86400 : FCString::Atoi64(*Text);
		OutSeconds = (double)FMath::Min<int64>(Seconds, 86400);
		return true;
	}
	FDateTime At;
	if (FDateTime::ParseHttpDate(Text, At))
	{
		OutSeconds = FMath::Max(0.0, (At - Now).GetTotalSeconds());
		return true;
	}
	return false;
}

bool FSvPipeline::ProvesNotApplied(const FSvError& Error)
{
	return Error.Status == 408 || Error.Status == 429 || Error.Status == 503;
}

FSvError FSvPipeline::ErrorFromResponse(const FSvHttpResponse& Response, const FDateTime& Now)
{
	if (Response.Failure != ESvTransportFailure::None)
	{
		return FSvError(0, Response.Failure == ESvTransportFailure::Timeout ? SvErrorCodes::Timeout : SvErrorCodes::NetworkError, FString());
	}
	if (Response.Status == 304)
	{
		return FSvError(304, SvErrorCodes::InvalidResponse, TEXT("Not Modified, but nothing was kept for this request."));
	}
	if (Response.bRedirected || (Response.Status >= 300 && Response.Status < 400))
	{
		// Never followed, never read, never sent again: credentials must not travel to another origin.
		return FSvError(Response.Status, SvErrorCodes::RedirectRefused,
			TEXT("The API answered with a redirect, which the SDK never follows. Check the base URL (it must be the API itself, https)."));
	}
	FString Code, Message, Name, Details;
	// An HTML error page from a proxy, for example, is never quoted into the message.
	const TSharedPtr<FJsonObject> Body = SvJson::ParseObject(Response.Body);
	if (Body.IsValid())
	{
		Code = SvJson::GetString(Body, TEXT("error"));
		Message = SvJson::GetString(Body, TEXT("message"));
		Details = SvJson::GetRawJson(Body, TEXT("details"));
		if (Code.IsEmpty()) Name = SvJson::GetString(Body, TEXT("name"));
	}
	FSvError E(Response.Status, Code, Message);
	E.DetailsJson = Details;
	E.ErrorName = Name;
	double Wait = 0;
	if (ParseRetryAfter(Response.GetHeader(TEXT("Retry-After")), Now, Wait))
	{
		E.HasRetryAfter = true;
		E.RetryAfterSeconds = (float)Wait;
	}
	return E;
}

void FSvPipeline::Send(const FSvCall& Call, const FString& Bearer, TSvCallback<FSvHttpResponse> Done)
{
	TSharedRef<FAttempt> State = MakeShared<FAttempt>();
	State->Call = Call;
	State->Bearer = Bearer;
	State->Done = MoveTemp(Done);
	// Monotonic: a device time sync in the middle of a refresh can neither stretch nor cut its resend window.
	State->StartedMono = Mono();
	SendAttempt(State);
}

void FSvPipeline::SendAttempt(TSharedRef<FAttempt> State)
{
	if (!*Alive) return;
	State->Attempt++;
	const FSvHttpRequest Request = BuildRequest(State->Call, State->Bearer);
	TSharedRef<bool> AliveFlag = Alive;
	Transport->Send(Request, [this, State, AliveFlag](const FSvHttpResponse& Response)
	{
		// The pipeline (the client) is gone: drop the answer; the caller's future completes with client_shutdown. Only
		// OnAnswerAfterShutdown may still see a successful answer (it never touches the client).
		if (!*AliveFlag)
		{
			if (State->Call.OnAnswerAfterShutdown && Response.IsSuccess() && !Response.bRedirected) State->Call.OnAnswerAfterShutdown(Response);
			return;
		}
		// Keep the client alive while this answer is handled: a handler may release the last reference to it.
		const TSharedPtr<void> Hold = PinOwner ? PinOwner() : nullptr;
		if (Response.Failure == ESvTransportFailure::None && !Response.bRedirected &&
			(Response.IsSuccess() || (State->Call.bAllow304 && Response.Status == 304)))
		{
			State->Done(TSvResult<FSvHttpResponse>::Ok(Response));
			return;
		}
		FSvError Failure = ErrorFromResponse(Response, Now());
		Failure.Attempts = State->Call.PriorAttempts + State->Attempt;
		double Wait = 0;
		bool bRetry = false;
		if (State->Call.Retry == ESvRetry::Safe)
		{
			bRetry = RetryPolicy.ShouldRetry(Failure, State->Attempt, Wait);
		}
		else if (State->Call.Retry == ESvRetry::IfVersionWrite)
		{
			bRetry = ProvesNotApplied(Failure) && RetryPolicy.ShouldRetry(Failure, State->Attempt, Wait);
		}
		if (bRetry && State->Call.RetryWindowSeconds.IsSet() && (Mono() - State->StartedMono) + Wait >= State->Call.RetryWindowSeconds.GetValue())
		{
			bRetry = false;
		}
		if (!bRetry)
		{
			State->Done(TSvResult<FSvHttpResponse>::Fail(Failure));
			return;
		}
		Log(ELogVerbosity::Verbose, FString::Printf(TEXT("%s %s failed (%s); sending it again in %.1f s"), *State->Call.Method, *PathOnly(State->Call.Path),
			Failure.Code.IsEmpty() ? *FString::FromInt(Failure.Status) : *Failure.Code, Wait));
		Delay(Wait, [this, State, AliveFlag]()
		{
			if (!*AliveFlag) return;
			const TSharedPtr<void> Hold = PinOwner ? PinOwner() : nullptr;
			SendAttempt(State);
		});
	});
}
