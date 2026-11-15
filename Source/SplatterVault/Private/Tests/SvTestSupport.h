// Copyright (c) SplatterVault. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "SplatterVaultClient.h"
#include "SvJson.h"

/** Every SplatterVault test: editor and game contexts, product filter. */
#define SV_TEST_FLAGS (EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)

namespace SvTest
{
	/** A transport that answers from a handler (synchronously, or later with Pump), and records every request. */
	class FFakeTransport : public ISvHttpTransport
	{
	public:
		TFunction<FSvHttpResponse(const FSvHttpRequest&)> Handler;
		TArray<FSvHttpRequest> Requests;
		/** Answer later (Pump) instead of inside Send: the way a real HTTP stack answers. */
		bool bDeferred = false;

		virtual void Send(const FSvHttpRequest& Request, TFunction<void(const FSvHttpResponse&)> OnComplete) override
		{
			Requests.Add(Request);
			const FSvHttpResponse Response = Handler ? Handler(Request) : FSvHttpResponse::Failed(ESvTransportFailure::Network);
			if (bDeferred)
			{
				Queue.Add(TPair<FSvHttpResponse, TFunction<void(const FSvHttpResponse&)>>(Response, MoveTemp(OnComplete)));
				QueuedUrls.Add(Request.Url);
				return;
			}
			OnComplete(Response);
		}

		/** Answers the queued requests (and those they cause) until none is left. */
		int32 Pump()
		{
			int32 N = 0;
			while (Queue.Num() > 0)
			{
				TPair<FSvHttpResponse, TFunction<void(const FSvHttpResponse&)>> Next = MoveTemp(Queue[0]);
				Queue.RemoveAt(0);
				QueuedUrls.RemoveAt(0);
				Next.Value(Next.Key);
				++N;
			}
			return N;
		}

		/** Answers only the queued request whose URL contains PathPart (the first one), out of order. */
		bool PumpOne(const FString& PathPart)
		{
			for (int32 i = 0; i < Queue.Num(); ++i)
			{
				if (QueuedUrls[i].Contains(PathPart, ESearchCase::CaseSensitive))
				{
					TPair<FSvHttpResponse, TFunction<void(const FSvHttpResponse&)>> Next = MoveTemp(Queue[i]);
					Queue.RemoveAt(i);
					QueuedUrls.RemoveAt(i);
					Next.Value(Next.Key);
					return true;
				}
			}
			return false;
		}

		int32 Queued() const { return Queue.Num(); }

		int32 CountPath(const FString& PathPart) const
		{
			int32 N = 0;
			for (const FSvHttpRequest& R : Requests)
			{
				if (R.Url.Contains(PathPart, ESearchCase::CaseSensitive)) ++N;
			}
			return N;
		}

	private:
		TArray<TPair<FSvHttpResponse, TFunction<void(const FSvHttpResponse&)>>> Queue;
		TArray<FString> QueuedUrls;
	};

	/** A memory store that counts reads of keys ending with a suffix (to measure unsealing work). */
	class FCountingStore : public FSvMemorySecureStore
	{
	public:
		FString CountSuffix;
		int32 Reads = 0;

		virtual ESvStoreRead Get(const FString& Key, FString& OutValue, FString& OutError) override
		{
			if (!CountSuffix.IsEmpty() && Key.EndsWith(CountSuffix, ESearchCase::CaseSensitive)) ++Reads;
			return FSvMemorySecureStore::Get(Key, OutValue, OutError);
		}
	};

	/** A secure store whose reads of chosen keys fail (Error), to test "unreadable is never missing". */
	class FFlakyStore : public FSvMemorySecureStore
	{
	public:
		TArray<FString> UnreadableSuffixes;
		bool bFailWrites = false;

		virtual ESvStoreRead Get(const FString& Key, FString& OutValue, FString& OutError) override
		{
			for (const FString& S : UnreadableSuffixes)
			{
				if (Key.EndsWith(S, ESearchCase::CaseSensitive))
				{
					OutError = TEXT("test: unreadable");
					return ESvStoreRead::Error;
				}
			}
			return FSvMemorySecureStore::Get(Key, OutValue, OutError);
		}

		virtual bool Set(const FString& Key, const FString& Value, FString& OutError) override
		{
			if (bFailWrites)
			{
				OutError = TEXT("test: write refused");
				return false;
			}
			return FSvMemorySecureStore::Set(Key, Value, OutError);
		}
	};

	/** A test clock: wall and monotonic time move only when told, and every delay is recorded and run at once. */
	struct FClock
	{
		FDateTime Wall = FDateTime(2026, 11, 15, 12, 0, 0);
		double Mono = 1000.0;
		TArray<double> Delays;

		void Advance(double Seconds)
		{
			Wall += FTimespan::FromSeconds(Seconds);
			Mono += Seconds;
		}
	};

	inline FString B64Url(const FString& Json)
	{
		FTCHARToUTF8 Utf8(*Json);
		FString B = FBase64::Encode(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		B.ReplaceInline(TEXT("+"), TEXT("-"));
		B.ReplaceInline(TEXT("/"), TEXT("_"));
		B.ReplaceInline(TEXT("="), TEXT(""));
		return B;
	}

	/** An unsigned JWT-shaped token with the given claims (the SDK reads iat/exp/amr/sub only; it never checks signatures). */
	inline FString MakeToken(const FString& Sub, int64 Iat, int64 Exp, const FString& Amr = TEXT("device"), const FString& Unique = FString())
	{
		const FString Header = B64Url(TEXT("{\"alg\":\"EdDSA\",\"typ\":\"JWT\"}"));
		const FString Payload = B64Url(FString::Printf(TEXT("{\"sub\":\"%s\",\"iat\":%lld,\"exp\":%lld,\"amr\":[\"%s\"],\"jti\":\"%s\"}"), *Sub, (long long)Iat,
			(long long)Exp, *Amr, *Unique));
		return Header + TEXT(".") + Payload + TEXT(".c2lnbmF0dXJl");
	}

	inline int64 Unix(const FDateTime& T)
	{
		return (T - FDateTime(1970, 1, 1)).GetTotalSeconds();
	}

	inline FSvHttpResponse JsonReply(int32 Status, const FString& Body, const TMap<FString, FString>& Headers = TMap<FString, FString>())
	{
		TMap<FString, FString> H = Headers;
		H.Add(TEXT("Content-Type"), TEXT("application/json"));
		return FSvHttpResponse::Make(Status, Body, H);
	}

	inline FSvHttpResponse ErrorReply(int32 Status, const FString& Code, const FString& RetryAfter = FString())
	{
		TMap<FString, FString> H;
		if (!RetryAfter.IsEmpty()) H.Add(TEXT("Retry-After"), RetryAfter);
		return JsonReply(Status, FString::Printf(TEXT("{\"error\":\"%s\",\"message\":\"Injected.\"}"), *Code), H);
	}

	/** The request's JSON body. */
	inline TSharedPtr<FJsonObject> BodyOf(const FSvHttpRequest& R)
	{
		return SvJson::ParseObject(R.Body);
	}

	/** A client wired to a fake transport, a test clock and a store, with log lines captured. */
	struct FHarness
	{
		TSharedRef<FFakeTransport> Transport = MakeShared<FFakeTransport>();
		TSharedRef<FClock> Clock = MakeShared<FClock>();
		TSharedPtr<ISvSecureStore> Store;
		TSharedPtr<FSplatterVaultClient> Client;
		TSharedRef<TArray<FString>> Logs = MakeShared<TArray<FString>>();
		FSvClientOptions Options;

		explicit FHarness(TSharedPtr<ISvSecureStore> InStore = nullptr, int32 MaxAttempts = 3)
		{
			Store = InStore.IsValid() ? InStore : TSharedPtr<ISvSecureStore>(MakeShared<FSvMemorySecureStore>());
			FSvClientOptions& O = Options;
			O.GameId = 4242;
			O.BaseUrl = TEXT("https://api.test/rest");
			O.Transport = Transport;
			O.SecureStore = Store;
			O.RetryPolicy.MaxAttempts = MaxAttempts;
			O.RetryPolicy.Random = []() { return 0.5; };
			TSharedRef<FClock> C = Clock;
			O.Clock = [C]() { return C->Wall; };
			O.MonotonicClock = [C]() { return C->Mono; };
			O.Delay = [C](double Seconds, TFunction<void()> Then)
			{
				C->Delays.Add(Seconds);
				C->Advance(Seconds);
				Then();
			};
			TSharedRef<TArray<FString>> L = Logs;
			O.Logger = [L](ELogVerbosity::Type, const FString& Line) { L->Add(Line); };
			FString Error;
			Client = FSplatterVaultClient::Create(O, Error);
			check(Client.IsValid());
		}

		/** The game quits and starts again: a new client on the same store, transport and clock (nobody signed in). */
		void Restart()
		{
			Client.Reset();
			FString Error;
			Client = FSplatterVaultClient::Create(Options, Error);
			check(Client.IsValid());
		}

		/** No captured log line contains Secret. */
		bool LogsFreeOf(const FString& Secret) const
		{
			for (const FString& L : *Logs)
			{
				if (!Secret.IsEmpty() && L.Contains(Secret)) return false;
			}
			return true;
		}
	};

	/** The value of a future that the fake transport already completed (synchronous answers). */
	template <typename T>
	TSvResult<T> Now(TSvFuture<T>&& F)
	{
		if (!F.IsReady()) return TSvResult<T>::Fail(FSvError(0, TEXT("test_not_ready"), TEXT("The future is not complete.")));
		return F.Get();
	}

	// ─── latent steps (integration tests) ────────────────────────────────────────────────────────────────────────

	/** A list of asynchronous steps run in order; each calls Next(true) to go on or Next(false) to stop the test. */
	class FSteps : public TSharedFromThis<FSteps>
	{
	public:
		using FNext = TFunction<void(bool)>;
		using FStep = TFunction<void(FNext)>;

		explicit FSteps(FAutomationTestBase* InTest) : Test(InTest) {}

		void Add(const FString& Name, FStep Step)
		{
			Names.Add(Name);
			Steps.Add(MoveTemp(Step));
		}

		void Start()
		{
			StartedAt = FPlatformTime::Seconds();
			RunNext();
		}

		bool IsDone() const { return bDone; }
		FString Current() const { return Names.IsValidIndex(Index) ? Names[Index] : FString(TEXT("(end)")); }
		double StartedAt = 0;

	private:
		void RunNext()
		{
			if (Index >= Steps.Num())
			{
				bDone = true;
				return;
			}
			TWeakPtr<FSteps> Weak = AsShared();
			const int32 Mine = Index;
			Steps[Index]([Weak, Mine](bool bOk)
			{
				TSharedPtr<FSteps> Self = Weak.Pin();
				if (!Self.IsValid() || Self->bDone || Self->Index != Mine) return;
				if (!bOk)
				{
					Self->Test->AddError(FString::Printf(TEXT("Step failed: %s"), *Self->Names[Mine]));
					Self->bDone = true;
					return;
				}
				Self->Index++;
				Self->RunNext();
			});
		}

		FAutomationTestBase* Test;
		TArray<FString> Names;
		TArray<FStep> Steps;
		int32 Index = 0;
		bool bDone = false;
	};

	/** Waits (ticking the engine) until the steps are done, or fails the test after TimeoutSeconds. */
	class FWaitForSteps : public IAutomationLatentCommand
	{
	public:
		FWaitForSteps(TSharedRef<FSteps> InSteps, FAutomationTestBase* InTest, double InTimeout)
			: Steps(InSteps)
			, Test(InTest)
			, Timeout(InTimeout)
		{
		}

		virtual bool Update() override
		{
			if (Steps->IsDone()) return true;
			if (FPlatformTime::Seconds() - Steps->StartedAt > Timeout)
			{
				Test->AddError(FString::Printf(TEXT("Timed out after %.0f s at step: %s"), Timeout, *Steps->Current()));
				return true;
			}
			return false;
		}

	private:
		TSharedRef<FSteps> Steps;
		FAutomationTestBase* Test;
		double Timeout;
	};
}

#endif
