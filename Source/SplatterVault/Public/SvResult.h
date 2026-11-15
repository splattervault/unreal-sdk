// Copyright (c) SplatterVault. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Async/Future.h"
#include "SvTypes.h"

/** The value of an operation that answers nothing but success or failure. */
struct FSvVoid
{
};

/**
 * The outcome of an SDK operation: a value, or an FSvError.
 *
 * Every asynchronous C++ call returns TFuture<TSvResult<T>>. The future is completed on the game thread (the SDK
 * sends its HTTP requests from there and Unreal answers them there), so continuations attached with Next() run on
 * the game thread too. Never call Get() or Wait() on the game thread before the future is ready: the answer is
 * delivered there, so the wait would never end.
 *
 * @code
 * Client->Auth().SignInAsGuest().Next([](const TSvResult<FSvSignInResult>& R)
 * {
 *     if (R.IsOk()) { UE_LOG(LogTemp, Log, TEXT("Player %s"), *R.GetValue().Player.Id); }
 *     else if (R.GetError().IsNetworkError()) { // offline: try again later }
 * });
 * @endcode
 */
template <typename T>
struct TSvResult
{
	TSvResult() = default;

	static TSvResult Ok(T InValue)
	{
		TSvResult R;
		R.bOk = true;
		R.Value = MoveTemp(InValue);
		return R;
	}

	static TSvResult Fail(FSvError InError)
	{
		TSvResult R;
		R.bOk = false;
		R.Error = MoveTemp(InError);
		return R;
	}

	bool IsOk() const { return bOk; }
	bool IsError() const { return !bOk; }

	/** The value (default-constructed when IsError()). */
	const T& GetValue() const { return Value; }
	T& GetValue() { return Value; }

	/** The error (empty when IsOk()). */
	const FSvError& GetError() const { return Error; }

private:
	bool bOk = false;
	T Value{};
	FSvError Error;
};

template <typename T>
using TSvCallback = TFunction<void(const TSvResult<T>&)>;

template <typename T>
using TSvFuture = TFuture<TSvResult<T>>;
