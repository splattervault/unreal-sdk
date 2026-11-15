// Copyright (c) SplatterVault. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"

/** The SDK's version (semver). */
#define SPLATTERVAULT_SDK_VERSION TEXT("0.1.0")

/** The ways a player signs in (the token's amr, and identities[].provider). */
namespace SvProviders
{
	inline constexpr const TCHAR* Xsolla = TEXT("xsolla");
	inline constexpr const TCHAR* Meta = TEXT("meta");
	inline constexpr const TCHAR* Steam = TEXT("steam");
	inline constexpr const TCHAR* PlayFab = TEXT("playfab");
	inline constexpr const TCHAR* Supabase = TEXT("supabase");
	/** Your backend vouched for the player (organization API key, on your server only). */
	inline constexpr const TCHAR* Studio = TEXT("studio");
	/** A SplatterVault email account. */
	inline constexpr const TCHAR* Email = TEXT("email");
	/** A guest on this device. */
	inline constexpr const TCHAR* Device = TEXT("device");
}

/** Error codes the SDK makes itself (FSvError::Status is 0 for all of them). */
namespace SvErrorCodes
{
	/** No answer: no connection, a reset, a TLS failure. The request may or may not have arrived. */
	inline constexpr const TCHAR* NetworkError = TEXT("network_error");
	/** No answer in time. The request may or may not have arrived. */
	inline constexpr const TCHAR* Timeout = TEXT("timeout");
	/** A player route was called with no signed-in player. */
	inline constexpr const TCHAR* NotSignedIn = TEXT("not_signed_in");
	/** The answer was not the JSON the SDK expected (Status keeps the HTTP status). */
	inline constexpr const TCHAR* InvalidResponse = TEXT("invalid_response");
	/** The device's secure storage could not be read. Nothing was changed: no new guest was made over the old one. */
	inline constexpr const TCHAR* StorageUnreadable = TEXT("storage_unreadable");
	/** An argument was refused by the SDK before anything was sent (see the message). */
	inline constexpr const TCHAR* InvalidArgument = TEXT("invalid_argument");
	/** The call does not fit the current state (no guest on this device, nothing to refresh...). */
	inline constexpr const TCHAR* InvalidState = TEXT("invalid_state");
	/** The SDK is not configured (no game id or key, a refused base URL). */
	inline constexpr const TCHAR* NotConfigured = TEXT("not_configured");
	/** The client was shut down (the game instance ended) before an answer arrived. */
	inline constexpr const TCHAR* Shutdown = TEXT("client_shutdown");
	/**
	 * The API answered with a redirect (3xx), or (UE 5.5+) the HTTP stack followed one to another URL. The SDK never
	 * uses such an answer and never sends the request again by itself. Status keeps the HTTP status.
	 */
	inline constexpr const TCHAR* RedirectRefused = TEXT("redirect_refused");
	/**
	 * A store change (or a resumed one) belongs to a player who is no longer the signed-in player: nothing was sent
	 * (it would be charged to the wrong player). The change stays pending for its own player.
	 */
	inline constexpr const TCHAR* PlayerChanged = TEXT("player_changed");
	/**
	 * Another sign-in, a sign-out or an erase happened while a renewal (refresh, guest renewal) or a restore was in
	 * flight: its answer was not used (its refresh token was signed out). Nothing about the current sign-in changed.
	 */
	inline constexpr const TCHAR* SignInChanged = TEXT("sign_in_changed");
	/**
	 * The same store change (same player and Idempotency-Key) is being sent right now with another request body: nothing
	 * was sent. OutcomeUnknown is set; wait for the first send, or ResumePending.
	 */
	inline constexpr const TCHAR* ChangeInProgress = TEXT("change_in_progress");
}
