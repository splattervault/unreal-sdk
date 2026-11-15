// Copyright (c) SplatterVault. Licensed under the MIT License.

#include "SvRetryPolicy.h"

FSvRetryPolicy FSvRetryPolicy::None()
{
	FSvRetryPolicy P;
	P.MaxAttempts = 1;
	return P;
}

bool FSvRetryPolicy::IsRetryable(const FSvError& Error)
{
	// The same set as FSvError::IsTransient (and the Unity SDK): no answer, 408, 429, 500, 502, 503, 504.
	return Error.IsTransient();
}

bool FSvRetryPolicy::ShouldRetry(const FSvError& Error, int32 Attempt, double& OutDelaySeconds) const
{
	OutDelaySeconds = 0;
	if (Attempt >= MaxAttempts || !IsRetryable(Error)) return false;
	if (Error.HasRetryAfter)
	{
		if (Error.RetryAfterSeconds > MaxRetryAfterSeconds) return false;
		OutDelaySeconds = FMath::Max(0.0, (double)Error.RetryAfterSeconds);
		return true;
	}
	OutDelaySeconds = Backoff(Attempt);
	return true;
}

double FSvRetryPolicy::Backoff(int32 Attempt) const
{
	double Seconds = BaseDelaySeconds * FMath::Pow(2.0, (double)FMath::Max(0, Attempt - 1));
	Seconds = FMath::Min(Seconds, MaxBackoffSeconds);
	const double J = FMath::Clamp(Jitter, 0.0, 1.0);
	if (J > 0)
	{
		const double R = Random ? FMath::Clamp(Random(), 0.0, 1.0) : (double)FMath::FRand();
		Seconds = Seconds * (1 - J / 2) + Seconds * (J / 2) * R;
	}
	return Seconds;
}
