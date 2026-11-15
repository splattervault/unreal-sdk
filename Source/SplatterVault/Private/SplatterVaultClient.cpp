// Copyright (c) SplatterVault. Licensed under the MIT License.

#include "SplatterVaultClient.h"

#include "Containers/Ticker.h"
#include "HAL/PlatformTime.h"
#include "SplatterVaultModule.h"
#include "SvCrypto.h"
#include "SvJson.h"
#include "SvPipeline.h"

namespace
{
	struct FUrlParts
	{
		FString Scheme;
		FString Host;
		int32 Port = 0;
		bool bDefaultPort = true;
	};

	bool ParseUrl(const FString& Url, FUrlParts& Out)
	{
		int32 SchemeEnd = Url.Find(TEXT("://"), ESearchCase::CaseSensitive);
		if (SchemeEnd <= 0) return false;
		Out.Scheme = Url.Left(SchemeEnd).ToLower();
		FString Rest = Url.Mid(SchemeEnd + 3);
		int32 End = Rest.Len();
		for (int32 i = 0; i < Rest.Len(); ++i)
		{
			if (Rest[i] == TEXT('/') || Rest[i] == TEXT('?') || Rest[i] == TEXT('#'))
			{
				End = i;
				break;
			}
		}
		FString Authority = Rest.Left(End);
		int32 At = INDEX_NONE;
		if (Authority.FindLastChar(TEXT('@'), At)) Authority = Authority.Mid(At + 1);
		if (Authority.IsEmpty()) return false;
		FString Host = Authority;
		FString PortText;
		if (Authority.StartsWith(TEXT("[")))
		{
			int32 Close = INDEX_NONE;
			if (!Authority.FindChar(TEXT(']'), Close)) return false;
			Host = Authority.Left(Close + 1);
			if (Authority.Len() > Close + 1 && Authority[Close + 1] == TEXT(':')) PortText = Authority.Mid(Close + 2);
		}
		else
		{
			int32 Colon = INDEX_NONE;
			if (Authority.FindLastChar(TEXT(':'), Colon))
			{
				Host = Authority.Left(Colon);
				PortText = Authority.Mid(Colon + 1);
			}
		}
		if (Host.IsEmpty()) return false;
		Out.Host = Host.ToLower();
		const int32 Default = Out.Scheme == TEXT("https") ? 443 : 80;
		Out.Port = Default;
		Out.bDefaultPort = true;
		if (!PortText.IsEmpty())
		{
			if (!PortText.IsNumeric()) return false;
			Out.Port = FCString::Atoi(*PortText);
			Out.bDefaultPort = Out.Port == Default;
		}
		return true;
	}

	bool IsLoopback(const FString& Host)
	{
		if (Host == TEXT("localhost") || Host == TEXT("[::1]")) return true;
		// 127.0.0.0/8, written as four decimal numbers.
		TArray<FString> Parts;
		Host.ParseIntoArray(Parts, TEXT("."), false);
		if (Parts.Num() != 4 || Parts[0] != TEXT("127")) return false;
		for (const FString& P : Parts)
		{
			if (P.IsEmpty() || P.Len() > 3 || !P.IsNumeric() || P.Contains(TEXT("-")) || P.Contains(TEXT(".")) || FCString::Atoi(*P) > 255) return false;
		}
		return true;
	}

	bool FiniteIn(double V, double Min, double Max)
	{
		return FMath::IsFinite(V) && V >= Min && V <= Max;
	}

	constexpr double MaxTimerSeconds = 2147483.647; // 2^31-1 ms


	void DefaultLog(ELogVerbosity::Type Verbosity, const FString& Message)
	{
		switch (Verbosity)
		{
		case ELogVerbosity::Error:
			UE_LOG(LogSplatterVault, Error, TEXT("%s"), *Message);
			break;
		case ELogVerbosity::Warning:
			UE_LOG(LogSplatterVault, Warning, TEXT("%s"), *Message);
			break;
		case ELogVerbosity::Display:
			UE_LOG(LogSplatterVault, Display, TEXT("%s"), *Message);
			break;
		case ELogVerbosity::Verbose:
		case ELogVerbosity::VeryVerbose:
			UE_LOG(LogSplatterVault, Verbose, TEXT("%s"), *Message);
			break;
		default:
			UE_LOG(LogSplatterVault, Log, TEXT("%s"), *Message);
			break;
		}
	}

	/** One-shot FTSTicker delays; the handles are kept so a shut-down client removes the ones still waiting. */
	struct FDelayRegistry
	{
		struct FWaiting
		{
			FTSTicker::FDelegateHandle Handle;
			TSharedPtr<TFunction<void()>> Then;
		};
		TMap<uint64, FWaiting> Waiting;
		uint64 Next = 0;
		bool bStopped = false;

		void RemoveAll()
		{
			bStopped = true;
			TMap<uint64, FWaiting> Stopped = MoveTemp(Waiting);
			Waiting.Reset();
			for (TPair<uint64, FWaiting>& W : Stopped)
			{
				FTSTicker::GetCoreTicker().RemoveTicker(W.Value.Handle);
				// The ticker frees a removed delegate only on its next tick: drop the continuation now, so the request
				// waiting on it completes with client_shutdown right away.
				*W.Value.Then = nullptr;
			}
		}
	};

	TFunction<void(double, TFunction<void()>)> MakeDefaultDelay(TSharedRef<FDelayRegistry> Registry)
	{
		return [Registry](double Seconds, TFunction<void()> Then)
		{
			if (Registry->bStopped) return;
			// Next tick at the earliest, never re-entrantly.
			const float Wait = (float)FMath::Max(Seconds, 0.0001);
			const uint64 Id = ++Registry->Next;
			TSharedPtr<TFunction<void()>> Fn = MakeShared<TFunction<void()>>(MoveTemp(Then));
			FDelayRegistry::FWaiting Entry;
			Entry.Then = Fn;
			Entry.Handle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Registry, Id, Fn](float)
			{
				Registry->Waiting.Remove(Id);
				TFunction<void()> Run = MoveTemp(*Fn);
				*Fn = nullptr;
				if (Run) Run();
				return false;
			}), Wait);
			Registry->Waiting.Add(Id, MoveTemp(Entry));
		};
	}
}

FString FSplatterVaultClient::CheckBaseUrl(const FString& InBaseUrl, bool bAllowInsecure, FString& OutError)
{
	FString Url = InBaseUrl.TrimStartAndEnd();
	if (Url.IsEmpty()) Url = DefaultBaseUrl;
	FUrlParts Parts;
	if (!ParseUrl(Url, Parts) || (Parts.Scheme != TEXT("https") && Parts.Scheme != TEXT("http")))
	{
		OutError = FString(TEXT("BaseUrl must be an absolute http(s) URL, such as ")) + DefaultBaseUrl + TEXT(".");
		return FString();
	}
#if UE_BUILD_SHIPPING
	bAllowInsecure = false; // never in a shipped game
#endif
	if (Parts.Scheme == TEXT("http") && !bAllowInsecure && !IsLoopback(Parts.Host))
	{
		OutError = TEXT("BaseUrl must use https (player tokens and secrets travel on it); http is allowed only for 127.0.0.1, ::1 and localhost.");
		return FString();
	}
	while (Url.EndsWith(TEXT("/"))) Url.LeftChopInline(1);
	return Url;
}

FString FSplatterVaultClient::DefaultStorageNamespace(const FString& InBaseUrl, int32 InGameId, const FString& InGameKey)
{
	FUrlParts Parts;
	ParseUrl(InBaseUrl, Parts);
	FString Origin = Parts.Scheme + TEXT("://") + Parts.Host;
	if (!Parts.bDefaultPort) Origin += FString::Printf(TEXT(":%d"), Parts.Port);
	return Origin + TEXT("|") + (InGameId != 0 ? FString::Printf(TEXT("id:%d"), InGameId) : TEXT("key:") + InGameKey);
}

bool FSplatterVaultClient::ValidateOptions(const FSvClientOptions& Options, FString& OutError)
{
	if (!FiniteIn(Options.RequestTimeoutSeconds, 0.001, MaxTimerSeconds) || Options.RequestTimeoutSeconds <= 0)
	{
		OutError = TEXT("RequestTimeoutSeconds must be a finite number above 0 and at most 2147483 (2^31-1 ms).");
		return false;
	}
	if (!FiniteIn(Options.RefreshTimeoutSeconds, 0.001, 10.0) || Options.RefreshTimeoutSeconds <= 0)
	{
		OutError = TEXT("RefreshTimeoutSeconds must be a finite number above 0 and at most 10 (the API's refresh resend window).");
		return false;
	}
	if (!FiniteIn(Options.RenewBeforeExpirySeconds, 0.0, 3600.0))
	{
		OutError = TEXT("RenewBeforeExpirySeconds must be a finite number from 0 to 3600.");
		return false;
	}
	const FSvRetryPolicy& R = Options.RetryPolicy;
	if (R.MaxAttempts < 1 || R.MaxAttempts > 10)
	{
		OutError = TEXT("RetryPolicy.MaxAttempts must be 1 to 10 (1 turns retries off).");
		return false;
	}
	if (!FiniteIn(R.BaseDelaySeconds, 0.0, MaxTimerSeconds) || !FiniteIn(R.MaxBackoffSeconds, 0.0, MaxTimerSeconds) ||
		!FiniteIn(R.MaxRetryAfterSeconds, 0.0, MaxTimerSeconds))
	{
		OutError = TEXT("RetryPolicy delays must be finite numbers from 0 to 2147483 seconds.");
		return false;
	}
	if (!FiniteIn(R.Jitter, 0.0, 1.0))
	{
		OutError = TEXT("RetryPolicy.Jitter must be a finite number from 0 to 1.");
		return false;
	}
	return true;
}

TSharedPtr<FSplatterVaultClient> FSplatterVaultClient::Create(const FSvClientOptions& Options, FString& OutError)
{
	if (!ValidateOptions(Options, OutError)) return nullptr;
	const bool bHasId = Options.GameId != 0;
	const bool bHasKey = !Options.GameKey.IsEmpty();
	if (bHasId == bHasKey)
	{
		OutError = TEXT("Name the game with exactly one of GameId or GameKey.");
		return nullptr;
	}
	if (bHasId && Options.GameId < 0)
	{
		OutError = TEXT("GameId must be a positive number.");
		return nullptr;
	}
	const FString Url = CheckBaseUrl(Options.BaseUrl, Options.bAllowInsecureHttpForDevelopment, OutError);
	if (Url.IsEmpty()) return nullptr;
	FUrlParts UrlParts;
	const bool bPlainHttp = ParseUrl(Url, UrlParts) && UrlParts.Scheme == TEXT("http") && !IsLoopback(UrlParts.Host);

	TSharedRef<FSplatterVaultClient> Client = MakeShareable(new FSplatterVaultClient());
	Client->GameId = bHasId ? Options.GameId : 0;
	Client->GameKey = bHasKey ? Options.GameKey : FString();
	Client->BaseUrl = Url;
	Client->Logger = Options.Logger ? Options.Logger : FSvLogger(&DefaultLog);

	TSharedPtr<ISvSecureStore> Store = Options.SecureStore;
	if (!Store.IsValid())
	{
		FString Warning;
		Store = SvCreatePlatformSecureStore(TEXT("players"), &Warning);
		if (!Warning.IsEmpty()) Client->Log(ELogVerbosity::Warning, Warning);
	}
	Client->SecureStore = Store;
	if (bPlainHttp)
	{
		Client->Log(ELogVerbosity::Warning, TEXT("bAllowInsecureHttpForDevelopment: the base URL is plain http to another host. Development only."));
	}

	TSharedRef<ISvHttpTransport> Transport = Options.Transport.IsValid() ? Options.Transport.ToSharedRef() : StaticCastSharedRef<ISvHttpTransport>(MakeShared<FSvUnrealHttpTransport>());
	TFunction<FDateTime()> Clock = Options.Clock ? Options.Clock : TFunction<FDateTime()>([]() { return FDateTime::UtcNow(); });
	TFunction<double()> Mono = Options.MonotonicClock ? Options.MonotonicClock : TFunction<double()>([]() { return FPlatformTime::Seconds(); });
	TSharedRef<FDelayRegistry> Registry = MakeShared<FDelayRegistry>();
	TFunction<void(double, TFunction<void()>)> Delay = Options.Delay ? Options.Delay : MakeDefaultDelay(Registry);
	Client->StopDelays = [Registry]() { Registry->RemoveAll(); };
	const double Timeout = Options.RequestTimeoutSeconds;
	FSvLogger LogFn = Client->Logger;
	Client->Pipeline = MakeUnique<FSvPipeline>(Url, Transport, Options.RetryPolicy, Timeout, Clock, Mono, Delay,
		[LogFn](ELogVerbosity::Type V, const FString& M) { LogFn(V, TEXT("[SplatterVault] ") + M); });

	const FString Namespace = Options.StorageNamespace.IsEmpty() ? DefaultStorageNamespace(Url, Client->GameId, Client->GameKey) : Options.StorageNamespace;
	Client->StoragePrefix = TEXT("svue1.") + SvCrypto::Hex(SvCrypto::Sha256(SvCrypto::Utf8(Namespace))).Left(16) + TEXT(".");

	// While an answer is handled the client stays alive, even if a handler drops the last reference to it.
	TWeakPtr<FSplatterVaultClient> WeakClient = Client;
	Client->Pipeline->PinOwner = [WeakClient]() -> TSharedPtr<void> { return WeakClient.Pin(); };

	Client->AuthApi = MakeUnique<FSvAuth>(*Client);
	Client->DataApi = MakeUnique<FSvPlayerDataApi>(*Client);
	Client->StoreApi = MakeUnique<FSvStoreApi>(*Client);

	Client->AuthApi->Configure(Options.RenewBeforeExpirySeconds, Options.RefreshTimeoutSeconds);
	return Client;
}

FSplatterVaultClient::FSplatterVaultClient() = default;

FSplatterVaultClient::~FSplatterVaultClient()
{
	Shutdown();
	// The parts go first (their pending callbacks complete with client_shutdown), then the pipeline.
	StoreApi.Reset();
	DataApi.Reset();
	AuthApi.Reset();
	Pipeline.Reset();
}

void FSplatterVaultClient::Shutdown()
{
	if (bShutdown) return;
	bShutdown = true;
	// Answers that arrive from now on are dropped (a refresh still keeps its rotated token); waits still queued are
	// removed; everyone waiting on a renewal, a guest sign-in or the guest lock gets client_shutdown.
	if (Pipeline) Pipeline->Kill();
	if (StopDelays) StopDelays();
	if (AuthApi) AuthApi->Abandon();
	if (StoreApi) StoreApi->Abandon();
}

void FSplatterVaultClient::AddGame(const TSharedRef<FJsonObject>& Body) const
{
	if (GameId != 0) Body->SetNumberField(TEXT("gameId"), GameId);
	else Body->SetStringField(TEXT("gameKey"), GameKey);
}

void FSplatterVaultClient::Log(ELogVerbosity::Type Verbosity, const FString& Message) const
{
	if (Logger) Logger(Verbosity, TEXT("[SplatterVault] ") + Message);
}
