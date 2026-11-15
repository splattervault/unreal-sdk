// Copyright (c) SplatterVault. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "SplatterVaultClient.h"
#include "SvHttp.h"
#include "SvResult.h"
#include "SvRetryPolicy.h"

/** Tokens, keys and ids are compared case-sensitively (FString's == ignores case). */
inline bool SvSame(const FString& A, const FString& B)
{
	return A.Equals(B, ESearchCase::CaseSensitive);
}

/**
 * A strong reference to the client for the length of a synchronous call that may raise an event: a handler that drops
 * the last reference then cannot destroy the client under the call. Null while the client is being destroyed.
 */
inline TSharedPtr<FSplatterVaultClient> SvPin(FSplatterVaultClient& Client)
{
	return Client.DoesSharedInstanceExist() ? TSharedPtr<FSplatterVaultClient>(Client.AsShared()) : TSharedPtr<FSplatterVaultClient>();
}

/** Whether the SDK may send a request again by itself (see FSvRetryPolicy). */
enum class ESvRetry : uint8
{
	/** Never: a sign-in proof, an email code, a new guest, an account change, a data write without ifVersion. */
	Never,
	/** Repeating it cannot do anything twice (a read, or a write with the same Idempotency-Key). */
	Safe,
	/**
	 * A player-data write whose every op has ifVersion. Sent again only after an answer that proves nothing was applied
	 * (408, 429, 503); after no answer, 500, 502 or 504 the outcome is unknown and the game decides (OutcomeUnknown).
	 */
	IfVersionWrite
};

/** One API call, before the credential is added. */
struct FSvCall
{
	FString Method = TEXT("GET");
	/** Path below the base URL, starting with "/", query included. */
	FString Path;
	/** JSON text, or empty for no body. */
	FString Body;
	ESvRetry Retry = ESvRetry::Never;
	/** Stop repeating once this long (seconds) has passed since the first send (a refresh's resend window). */
	TOptional<double> RetryWindowSeconds;
	bool bAllow304 = false;
	TMap<FString, FString> Headers;
	/** This call's timeout per send (default: the client's request timeout). */
	TOptional<double> TimeoutSeconds;
	/** Sends of this same request before this call (the 401 token_expired renewal resends it): counted in FSvError::Attempts. */
	int32 PriorAttempts = 0;
	/**
	 * When set, the request is sent only while this player is the signed-in player (store changes: never under another
	 * player's token). Otherwise it fails with player_changed and nothing is sent.
	 */
	FString ExpectedPlayerId;
	/**
	 * Called with a 2xx answer that arrives after the client was shut down (when the normal callback is dropped). It
	 * must not touch the client (it may be gone): a refresh uses it to keep the rotated refresh token.
	 */
	TFunction<void(const FSvHttpResponse&)> OnAnswerAfterShutdown;

	static FSvCall Get(const FString& InPath, ESvRetry InRetry = ESvRetry::Safe)
	{
		FSvCall C;
		C.Method = TEXT("GET");
		C.Path = InPath;
		C.Retry = InRetry;
		return C;
	}

	static FSvCall Post(const FString& InPath, const FString& InBody, ESvRetry InRetry)
	{
		FSvCall C;
		C.Method = TEXT("POST");
		C.Path = InPath;
		C.Body = InBody;
		C.Retry = InRetry;
		return C;
	}
};

/** Sends calls: headers, body, timeout, error answers, and the retry rules. Holds no credential. */
class FSvPipeline
{
public:
	FSvPipeline(const FString& InBaseUrl, TSharedRef<ISvHttpTransport> InTransport, const FSvRetryPolicy& InRetry, double InTimeout,
		TFunction<FDateTime()> InClock, TFunction<double()> InMono, TFunction<void(double, TFunction<void()>)> InDelay,
		TFunction<void(ELogVerbosity::Type, const FString&)> InLog);
	~FSvPipeline();

	/** Stops: answers that arrive later are dropped (their futures complete with client_shutdown). */
	void Kill() { *Alive = false; }
	bool IsAlive() const { return *Alive; }

	/** Shared with in-flight callbacks: false once the pipeline is gone. */
	TSharedRef<bool> GetAliveFlag() const { return Alive; }

	/** Returns a strong reference to the client that owns this pipeline, held while an answer is handled. */
	TFunction<TSharedPtr<void>()> PinOwner;

	/** Wall clock (UTC): only where it is compared with the API's times. */
	FDateTime Now() const { return Clock(); }

	/** Monotonic seconds: resend windows and elapsed times. */
	double Mono() const { return MonoClock(); }

	void Log(ELogVerbosity::Type Verbosity, const FString& Message) const;

	/** Runs Then after Seconds. */
	void Delay(double Seconds, TFunction<void()> Then) const { DelayFn(Seconds, MoveTemp(Then)); }

	/**
	 * Sends Call with Bearer (a player token) or no credential, repeating it under the retry rules. Done gets a 2xx
	 * answer (or a 304 when allowed), or the error.
	 */
	void Send(const FSvCall& Call, const FString& Bearer, TSvCallback<FSvHttpResponse> Done);

	/** The request for one send (tests). */
	FSvHttpRequest BuildRequest(const FSvCall& Call, const FString& Bearer) const;

	/** The error for a non-success answer (both answer shapes, Retry-After). */
	static FSvError ErrorFromResponse(const FSvHttpResponse& Response, const FDateTime& Now);

	/** Retry-After as seconds: whole seconds, or an HTTP date. False when absent or unreadable. */
	static bool ParseRetryAfter(const FString& Header, const FDateTime& Now, double& OutSeconds);

	static FString PathOnly(const FString& Path);

	/** An answer that proves the request was not applied (408, 429, 503): an ifVersion write may be sent again. */
	static bool ProvesNotApplied(const FSvError& Error);

	const FSvRetryPolicy& GetRetryPolicy() const { return RetryPolicy; }

private:
	struct FAttempt;
	void SendAttempt(TSharedRef<FAttempt> State);

	FString BaseUrl;
	TSharedRef<ISvHttpTransport> Transport;
	FSvRetryPolicy RetryPolicy;
	double TimeoutSeconds;
	TFunction<FDateTime()> Clock;
	TFunction<double()> MonoClock;
	TFunction<void(double, TFunction<void()>)> DelayFn;
	TFunction<void(ELogVerbosity::Type, const FString&)> LogFn;
	TSharedRef<bool> Alive = MakeShared<bool>(true);
};

/**
 * Fulfils a TPromise exactly once. When the last callback holding it goes away without answering (an HTTP request
 * dropped at shutdown), the future completes with client_shutdown: a TPromise left unset would assert.
 */
template <typename T>
class TSvPromiseBox
{
public:
	TSvPromiseBox()
		: Dropped(0, SvErrorCodes::Shutdown, TEXT("The SplatterVault client was shut down before an answer arrived."))
	{
	}

	/** InDropped: the error the future gets when nobody answers (a store change: its key and whether it is kept). */
	explicit TSvPromiseBox(const FSvError& InDropped)
		: Dropped(InDropped)
	{
	}

	~TSvPromiseBox()
	{
		if (!bSet)
		{
			bSet = true;
			Promise.SetValue(TSvResult<T>::Fail(Dropped));
		}
	}

	TFuture<TSvResult<T>> GetFuture() { return Promise.GetFuture(); }

	void Set(const TSvResult<T>& Result)
	{
		if (bSet) return;
		bSet = true;
		Promise.SetValue(Result);
	}

private:
	TPromise<TSvResult<T>> Promise;
	FSvError Dropped;
	bool bSet = false;
};

/** Starts Work with a callback and returns the future that callback completes. */
template <typename T>
TSvFuture<T> SvAsync(TFunction<void(TSvCallback<T>)> Work)
{
	TSharedRef<TSvPromiseBox<T>> Box = MakeShared<TSvPromiseBox<T>>();
	TSvFuture<T> Future = Box->GetFuture();
	Work([Box](const TSvResult<T>& R) { Box->Set(R); });
	return Future;
}

/** SvAsync whose future completes with IfDropped (not a bare client_shutdown) when the work is dropped unanswered. */
template <typename T>
TSvFuture<T> SvAsyncOr(const FSvError& IfDropped, TFunction<void(TSvCallback<T>)> Work)
{
	TSharedRef<TSvPromiseBox<T>> Box = MakeShared<TSvPromiseBox<T>>(IfDropped);
	TSvFuture<T> Future = Box->GetFuture();
	Work([Box](const TSvResult<T>& R) { Box->Set(R); });
	return Future;
}

/** An already-failed future. */
template <typename T>
TSvFuture<T> SvFailed(const FSvError& Error)
{
	TPromise<TSvResult<T>> P;
	TSvFuture<T> F = P.GetFuture();
	P.SetValue(TSvResult<T>::Fail(Error));
	return F;
}

/** One operation in flight at a time: callers that arrive while it runs share its result. */
template <typename T>
class TSvSingleFlight
{
public:
	bool IsRunning() const { return bRunning; }

	/** Runs Operation unless one is already running, and gets its result. */
	void Run(TFunction<void(TSvCallback<T>)> Operation, TSvCallback<T> Waiter)
	{
		Waiters.Add(MoveTemp(Waiter));
		if (bRunning) return;
		bRunning = true;
		const uint64 Generation = ++Run_Generation;
		Operation([this, Generation](const TSvResult<T>& Result)
		{
			if (Generation != Run_Generation || !bRunning) return;
			bRunning = false;
			TArray<TSvCallback<T>> Ready = MoveTemp(Waiters);
			Waiters.Reset();
			for (TSvCallback<T>& W : Ready)
			{
				W(Result);
			}
		});
	}

	/** Fails everyone waiting (shutdown). */
	void Abandon(const FSvError& Error)
	{
		bRunning = false;
		++Run_Generation;
		TArray<TSvCallback<T>> Ready = MoveTemp(Waiters);
		Waiters.Reset();
		for (TSvCallback<T>& W : Ready)
		{
			W(TSvResult<T>::Fail(Error));
		}
	}

private:
	bool bRunning = false;
	uint64 Run_Generation = 0;
	TArray<TSvCallback<T>> Waiters;
};

/** Serializes asynchronous work (one holder at a time, the others queued in order). */
class FSvAsyncLock
{
public:
	/** Runs Work when the lock is free; Work must call Release exactly once. */
	void Run(TFunction<void(TFunction<void()> Release)> Work)
	{
		if (bClosed) return; // shut down: Work (and the callbacks it holds) is dropped
		Queue.Add(MoveTemp(Work));
		if (!bHeld) Next();
	}

	/** Drops the queued work (shutdown): its callbacks are released, so their futures complete with client_shutdown. */
	void Clear()
	{
		bClosed = true;
		TArray<TFunction<void(TFunction<void()>)>> Dropped = MoveTemp(Queue);
		Queue.Reset();
		bHeld = false;
	}

private:
	void Next()
	{
		if (Queue.Num() == 0)
		{
			bHeld = false;
			return;
		}
		bHeld = true;
		TFunction<void(TFunction<void()>)> Work = MoveTemp(Queue[0]);
		Queue.RemoveAt(0);
		TSharedRef<bool> Released = MakeShared<bool>(false);
		Work([this, Released]()
		{
			if (*Released || bClosed) return;
			*Released = true;
			Next();
		});
	}

	bool bHeld = false;
	bool bClosed = false;
	TArray<TFunction<void(TFunction<void()>)>> Queue;
};
