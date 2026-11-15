// Copyright (c) SplatterVault. Licensed under the MIT License.

#include "SvHttp.h"

#include "HAL/PlatformTime.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/EngineVersionComparison.h"

namespace
{
	/** scheme://host:port (lower-case, default port spelled out) and the path, without the query. */
	bool OriginAndPath(const FString& Url, FString& OutOrigin, FString& OutPath)
	{
		const int32 SchemeEnd = Url.Find(TEXT("://"), ESearchCase::CaseSensitive);
		if (SchemeEnd <= 0) return false;
		const FString Scheme = Url.Left(SchemeEnd).ToLower();
		const FString Rest = Url.Mid(SchemeEnd + 3);
		int32 PathStart = Rest.Len();
		for (int32 i = 0; i < Rest.Len(); ++i)
		{
			if (Rest[i] == TEXT('/') || Rest[i] == TEXT('?') || Rest[i] == TEXT('#'))
			{
				PathStart = i;
				break;
			}
		}
		FString Authority = Rest.Left(PathStart).ToLower();
		int32 At = INDEX_NONE;
		if (Authority.FindLastChar(TEXT('@'), At)) Authority = Authority.Mid(At + 1);
		int32 Colon = INDEX_NONE;
		const bool bHasPort = Authority.FindLastChar(TEXT(':'), Colon) && !Authority.EndsWith(TEXT("]"));
		if (!bHasPort) Authority += Scheme == TEXT("https") ? TEXT(":443") : TEXT(":80");
		OutOrigin = Scheme + TEXT("://") + Authority;
		FString PathAndQuery = Rest.Mid(PathStart);
		int32 Query = INDEX_NONE;
		if (PathAndQuery.FindChar(TEXT('?'), Query)) PathAndQuery.LeftInline(Query);
		int32 Fragment = INDEX_NONE;
		if (PathAndQuery.FindChar(TEXT('#'), Fragment)) PathAndQuery.LeftInline(Fragment);
		OutPath = PathAndQuery.IsEmpty() ? FString(TEXT("/")) : PathAndQuery;
		return true;
	}
}

bool FSvHttpResponse::IsRedirectOf(const FString& RequestedUrl, const FString& EffectiveUrl)
{
	if (EffectiveUrl.IsEmpty()) return false;
	FString OriginA, PathA, OriginB, PathB;
	if (!OriginAndPath(RequestedUrl, OriginA, PathA) || !OriginAndPath(EffectiveUrl, OriginB, PathB)) return false;
	return !OriginA.Equals(OriginB, ESearchCase::CaseSensitive) || !PathA.Equals(PathB, ESearchCase::CaseSensitive);
}

FString FSvHttpResponse::GetHeader(const FString& Name) const
{
	const FString* V = Headers.Find(Name);
	return V ? *V : FString();
}

FSvHttpResponse FSvHttpResponse::Make(int32 InStatus, const FString& InBody, const TMap<FString, FString>& InHeaders)
{
	FSvHttpResponse R;
	R.Failure = ESvTransportFailure::None;
	R.Status = InStatus;
	R.Body = InBody;
	R.Headers = InHeaders;
	return R;
}

FSvHttpResponse FSvHttpResponse::Failed(ESvTransportFailure InFailure)
{
	FSvHttpResponse R;
	R.Failure = InFailure == ESvTransportFailure::None ? ESvTransportFailure::Network : InFailure;
	R.Status = 0;
	return R;
}

void FSvUnrealHttpTransport::Send(const FSvHttpRequest& Request, TFunction<void(const FSvHttpResponse&)> OnComplete)
{
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Req = FHttpModule::Get().CreateRequest();
	Req->SetURL(Request.Url);
	Req->SetVerb(Request.Method);
	for (const TPair<FString, FString>& H : Request.Headers)
	{
		Req->SetHeader(H.Key, H.Value);
	}
	if (!Request.Body.IsEmpty())
	{
		Req->SetContentAsString(Request.Body);
	}
	const double Timeout = Request.TimeoutSeconds > 0 ? Request.TimeoutSeconds : 30.0;
	Req->SetTimeout((float)Timeout);
	const double Started = FPlatformTime::Seconds();

	const FString RequestedUrl = Request.Url;
	Req->OnProcessRequestComplete().BindLambda(
		[OnComplete = MoveTemp(OnComplete), Started, Timeout, RequestedUrl](FHttpRequestPtr Sent, FHttpResponsePtr Response, bool bConnected)
		{
			if (bConnected && Response.IsValid() && Response->GetResponseCode() > 0)
			{
				FSvHttpResponse Out;
				Out.Failure = ESvTransportFailure::None;
				Out.Status = Response->GetResponseCode();
#if !UE_VERSION_OLDER_THAN(5, 5, 0)
				// libcurl in Unreal always follows redirects; an answer from another URL is refused by the pipeline.
				Out.bRedirected = FSvHttpResponse::IsRedirectOf(RequestedUrl, Response->GetEffectiveURL());
#else
				(void)RequestedUrl; // UE 5.3-5.4 cannot tell where a followed redirect led (see FSvHttpResponse::bRedirected)
#endif
				Out.Body = Response->GetContentAsString();
				for (const FString& Line : Response->GetAllHeaders())
				{
					int32 Colon = INDEX_NONE;
					if (!Line.FindChar(TEXT(':'), Colon) || Colon <= 0) continue;
					const FString Name = Line.Left(Colon).TrimStartAndEnd();
					const FString Value = Line.Mid(Colon + 1).TrimStartAndEnd();
					if (!Name.IsEmpty()) Out.Headers.Add(Name, Value);
				}
				OnComplete(Out);
				return;
			}
			// No answer. A send that ran the whole timeout is a timeout; newer engines also say so directly.
			bool bTimedOut = (FPlatformTime::Seconds() - Started) >= Timeout - 0.25;
#if !UE_VERSION_OLDER_THAN(5, 5, 0)
			if (Sent.IsValid() && Sent->GetFailureReason() == EHttpFailureReason::TimedOut) bTimedOut = true;
#endif
			OnComplete(FSvHttpResponse::Failed(bTimedOut ? ESvTransportFailure::Timeout : ESvTransportFailure::Network));
		});
	Req->ProcessRequest();
}
