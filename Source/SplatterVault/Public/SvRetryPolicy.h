// Copyright (c) SplatterVault. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "SvTypes.h"

/**
 * When the SDK sends a request again by itself.
 *
 * It applies only to requests that are safe to repeat: reads (GET), store changes (they carry the same
 * Idempotency-Key every time), player-data writes whose every op has ifVersion, and a refresh (only within the
 * API's 30-second resend window). Sign-ins, email codes, new guests, account changes, sign-out, the Meta purchase
 * check and data writes without ifVersion are never repeated by the SDK.
 *
 * Only transient failures are repeated: no answer (network error, timeout), 408, 429, 500, 502, 503 and 504 (the
 * same set as FSvError::IsTransient and the Unity SDK). Any other 4xx is never repeated. A Retry-After is always
 * honoured: the request waits at least that long, and when the wait is longer than MaxRetryAfterSeconds it is not
 * repeated at all (the error carries RetryAfterSeconds for the game to schedule). Without one, waits grow exponentially (BaseDelaySeconds, doubling, capped at MaxBackoffSeconds) with
 * jitter.
 */
struct SPLATTERVAULT_API FSvRetryPolicy
{
	/** Sends in all, the first one included. 1 turns retries off. */
	int32 MaxAttempts = 3;

	/** The wait before the first repeat when the answer named none; it doubles each time. */
	double BaseDelaySeconds = 0.5;

	/** The longest wait between repeats when the answer named none. */
	double MaxBackoffSeconds = 4.0;

	/** The longest Retry-After the SDK waits out by itself. */
	double MaxRetryAfterSeconds = 10.0;

	/** Spreads waits: 0 = exact waits, 1 = anywhere from half to all of the wait. Never shortens a Retry-After. */
	double Jitter = 1.0;

	/** A random number in [0, 1) for the jitter (tests make it fixed). Default: FMath::FRand. */
	TFunction<double()> Random;

	/** No retries at all. */
	static FSvRetryPolicy None();

	/** The failures the SDK repeats (on retry-safe requests only): no answer, 408, 429, 500, 502, 503, 504. */
	static bool IsRetryable(const FSvError& Error);

	/**
	 * Whether a request that failed with Error on send number Attempt (1 for the first) should be sent again, and
	 * after how many seconds.
	 */
	bool ShouldRetry(const FSvError& Error, int32 Attempt, double& OutDelaySeconds) const;

	/** The wait after send number Attempt when the answer named none. */
	double Backoff(int32 Attempt) const;
};
