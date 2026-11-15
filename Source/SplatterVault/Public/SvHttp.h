// Copyright (c) SplatterVault. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"

/** One HTTP request, as the SDK hands it to an ISvHttpTransport. Body is JSON text (or empty for none). */
struct SPLATTERVAULT_API FSvHttpRequest
{
	FString Method;
	FString Url;
	/** Header names are case-insensitive (TMap<FString> compares FString keys without case). */
	TMap<FString, FString> Headers;
	FString Body;
	/** Seconds this send may take. */
	double TimeoutSeconds = 30.0;
};

/** How a request ended without an HTTP answer. */
enum class ESvTransportFailure : uint8
{
	/** An HTTP answer arrived (any status, a 4xx or 5xx included). */
	None,
	/** No connection, DNS failure, connection reset, TLS failure, or an answer that could not be read. */
	Network,
	/** No answer within the request's timeout. */
	Timeout
};

/** An HTTP answer (Failure == None, whatever its status) or a transport failure. */
struct SPLATTERVAULT_API FSvHttpResponse
{
	ESvTransportFailure Failure = ESvTransportFailure::None;
	int32 Status = 0;
	/**
	 * The HTTP stack followed a redirect: this answer came from another URL than the one requested. The SDK refuses it
	 * (redirect_refused). Unreal's HTTP module (libcurl) always follows redirects; it does not forward the
	 * Authorization header to another host, but a 307/308 re-sends the request body there. Detected on UE 5.5+.
	 */
	bool bRedirected = false;
	FString Body;
	TMap<FString, FString> Headers;

	/** The header's value, or empty (names are case-insensitive). */
	FString GetHeader(const FString& Name) const;

	bool IsSuccess() const { return Failure == ESvTransportFailure::None && Status >= 200 && Status < 300; }

	static FSvHttpResponse Make(int32 InStatus, const FString& InBody, const TMap<FString, FString>& InHeaders = TMap<FString, FString>());

	/** EffectiveUrl (where the answer came from) is another origin or path than RequestedUrl. Empty: unknown, false. */
	static bool IsRedirectOf(const FString& RequestedUrl, const FString& EffectiveUrl);
	static FSvHttpResponse Failed(ESvTransportFailure InFailure);
};

/**
 * Sends HTTP requests. The SDK uses FSvUnrealHttpTransport (FHttpModule) by default; tests and other hosts plug in
 * their own through FSvClientOptions::Transport.
 *
 * Contract: call OnComplete exactly once, on the game thread, with the answer (any status) or a failure.
 */
class SPLATTERVAULT_API ISvHttpTransport
{
public:
	virtual ~ISvHttpTransport() = default;
	virtual void Send(const FSvHttpRequest& Request, TFunction<void(const FSvHttpResponse&)> OnComplete) = 0;
};

/** The default transport: Unreal's HTTP module (libcurl, or the platform's stack). Answers on the game thread. */
class SPLATTERVAULT_API FSvUnrealHttpTransport : public ISvHttpTransport
{
public:
	virtual void Send(const FSvHttpRequest& Request, TFunction<void(const FSvHttpResponse&)> OnComplete) override;
};
