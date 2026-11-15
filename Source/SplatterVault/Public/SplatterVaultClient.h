// Copyright (c) SplatterVault. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "SvConstants.h"
#include "SvDataOps.h"
#include "SvHttp.h"
#include "SvResult.h"
#include "SvRetryPolicy.h"
#include "SvSecureStore.h"
#include "SvTypes.h"

class FSplatterVaultClient;
class FSvPipeline;
struct FSvCall;
struct FSvAuthState;

/** TMap key functions that compare FString keys case-sensitively (TMap<FString, V> alone ignores case). */
template <typename ValueType>
struct TSvCaseSensitiveKeyFuncs : BaseKeyFuncs<TPair<FString, ValueType>, FString, false>
{
	static const FString& GetSetKey(const TPair<FString, ValueType>& Element) { return Element.Key; }
	static bool Matches(const FString& A, const FString& B) { return A.Equals(B, ESearchCase::CaseSensitive); }
	static uint32 GetKeyHash(const FString& Key) { return FCrc::StrCrc32(*Key); }
};

/** A log line of the SDK (it never contains a token, a secret, a proof, a password or a value). */
using FSvLogger = TFunction<void(ELogVerbosity::Type Verbosity, const FString& Message)>;

/** How an FSplatterVaultClient is set up. Name the game with exactly one of GameId and GameKey. */
struct SPLATTERVAULT_API FSvClientOptions
{
	/** Your game id (the number on the game console's Sign-in tab). It never changes: prefer it. */
	int32 GameId = 0;

	/** The game's Game Key. It changes when the game moves to another owner. */
	FString GameKey;

	/** The API, /rest included. Default https://splattervault.com/rest. */
	FString BaseUrl;

	/**
	 * Allows an http:// base URL that is not a loopback host (127.0.0.1, ::1, localhost): a development server on your
	 * network only. Ignored in Shipping builds. Off by default: player tokens and secrets travel on this URL.
	 */
	bool bAllowInsecureHttpForDevelopment = false;

	/** HTTP transport; default FSvUnrealHttpTransport. */
	TSharedPtr<ISvHttpTransport> Transport;

	/** Where refresh tokens, guest secrets and pending store changes are kept; default SvCreatePlatformSecureStore. */
	TSharedPtr<ISvSecureStore> SecureStore;

	/**
	 * Separates this game's stored secrets from others in the same store. Default (fixed): the API's origin
	 * (scheme, host and port, lower-case) and the game (its id, or its key). Set it to keep a guest when you switch
	 * from GameKey to GameId, or move the API to another host.
	 */
	FString StorageNamespace;

	FSvRetryPolicy RetryPolicy;

	/** How long one HTTP request may take. */
	double RequestTimeoutSeconds = 30.0;

	/**
	 * How long one refresh send may take (at most 10). Short on purpose: a refresh token is spent when the API reads
	 * it, and a lost answer may be resent only within 30 seconds.
	 */
	double RefreshTimeoutSeconds = 8.0;

	/** A renewable sign-in (email, guest) is renewed this long before its player token runs out. */
	double RenewBeforeExpirySeconds = 120.0;

	/** Default: UE_LOG to LogSplatterVault. */
	FSvLogger Logger;

	/** The wall clock, UTC (tests). Default FDateTime::UtcNow. */
	TFunction<FDateTime()> Clock;

	/** A monotonic clock in seconds (tests). Default FPlatformTime::Seconds. */
	TFunction<double()> MonotonicClock;

	/** Runs Then after Seconds on the game thread (tests). Default an FTSTicker one-shot. */
	TFunction<void(double Seconds, TFunction<void()> Then)> Delay;
};

DECLARE_MULTICAST_DELEGATE_OneParam(FSvOnSignedIn, const FSvSignInResult&);
DECLARE_MULTICAST_DELEGATE(FSvOnSignedOut);
DECLARE_MULTICAST_DELEGATE_OneParam(FSvOnSignInRequired, const FSvSignInRequired&);
DECLARE_MULTICAST_DELEGATE_OneParam(FSvOnStorageFailed, const FSvStorageFailure&);

/**
 * Player sign-in and the player's account (the /rest/player-auth routes).
 *
 * Tokens. The player token (about an hour) lives in memory only (GetPlayerToken). The refresh token of an email
 * sign-in and a guest's device secret go to secure storage and are never handed out. Every player request carries
 * the token; when it runs out the SDK renews it by itself where it can: an email sign-in with its refresh token (one
 * refresh at a time; the new refresh token is saved before the new player token is used), a guest with this
 * device's secret. Other sign-ins (Meta, Steam, Xsolla, PlayFab, Supabase, your backend) need a fresh proof: the SDK
 * raises OnSignInRequired and, when SetReauthenticate was called, awaits it and sends the request again once.
 *
 * Expiry is measured on this device's clock: the token's lifetime (its exp minus its iat, else one hour) counted
 * from when the sign-in request was sent, so a device clock that is off by hours changes nothing.
 *
 * Storage. A secure store that cannot be read is never taken as "nothing stored": the SDK then refuses to make a new
 * guest over the old one (storage_unreadable) and reads again next time.
 */
class SPLATTERVAULT_API FSvAuth
{
public:
	explicit FSvAuth(FSplatterVaultClient& InOwner);
	~FSvAuth();

	// ─── state ───────────────────────────────────────────────────────────────────────────────────────────────────

	/** A player is signed in (their token may still need renewing). */
	bool IsSignedIn() const;

	/**
	 * The player token, for your own game servers to check (offline, with GET /rest/player-auth/keys). Keep it in
	 * memory: never log it or write it anywhere. Empty when nobody is signed in.
	 */
	FString GetPlayerToken() const;

	/** The SplatterVault player id (the token's sub), or empty. */
	FString GetPlayerId() const;

	/** The signed-in player (empty fields when nobody is). */
	FSvSignedInPlayer GetPlayer() const;

	/** How the player signed in (SvProviders), or empty. */
	FString GetMethod() const;

	/** The ways the player can sign in, as last answered. */
	TArray<FSvIdentity> GetIdentities() const;

	/** When the player token runs out as the API said it (ISO 8601, the API's clock), or empty. */
	FString GetTokenExpiresAt() const;

	/** When the player token runs out on THIS device's clock (what the SDK uses). */
	TOptional<FDateTime> GetTokenExpiresAtUtc() const;

	/** A player signed in (any way, renewals included). */
	FSvOnSignedIn OnSignedIn;

	/** The player signed out, or the sign-in ended (see OnSignInRequired). */
	FSvOnSignedOut OnSignedOut;

	/** The player must sign in again with a fresh proof. Raised once per ended sign-in. */
	FSvOnSignInRequired OnSignInRequired;

	/**
	 * Secure storage could not read, write or delete something (never carries a value). A failed write of a new
	 * guest secret means that guest exists in memory only: tell the player, or call RetrySavingSecrets.
	 */
	FSvOnStorageFailed OnStorageFailed;

	/**
	 * Optional: called (once, with requests waiting) when a player token runs out and cannot be renewed. Sign the
	 * player in again inside it (for example get a new Steam ticket and call SignInWithSteam) and complete the future
	 * with true: the waiting requests are then sent again once with the new token. False lets them fail with
	 * token_expired.
	 */
	void SetReauthenticate(TFunction<TFuture<bool>(const FSvSignInRequired&)> InReauthenticate);

	// ─── restore ─────────────────────────────────────────────────────────────────────────────────────────────────

	/**
	 * Signs the player back in without asking them, the way they signed in last on this device: an email sign-in with
	 * its stored refresh token, a guest with this device's secret. Never another player: after a Meta, Steam, Xsolla,
	 * PlayFab, Supabase or studio sign-in, or a sign-out, this answers false even when a guest is stored (sign in with
	 * that platform again: GetLastSignInMethod says which). False also when nothing is stored or the API refused it.
	 * A network failure is an error (IsNetworkError), and unreadable storage is storage_unreadable, so "offline" or
	 * "storage locked" is never "signed out".
	 */
	TSvFuture<bool> Restore();

	/** How the player last signed in on this device (SvProviders), or empty when nobody did or they signed out. */
	TSvResult<FString> GetLastSignInMethod();

	/** Saves again a refresh token or guest secret that secure storage could not save earlier. True when nothing is left unsaved. */
	bool RetrySavingSecrets();

	// ─── platform proofs ─────────────────────────────────────────────────────────────────────────────────────────

	/**
	 * Meta Quest: UserId from ovr_User_GetLoggedInUser (the 64-bit id as decimal text) and Nonce from
	 * ovr_User_GetUserProof. A nonce works once: get a new one for every sign-in, also after provider_unavailable.
	 */
	TSvFuture<FSvSignInResult> SignInWithMeta(const FString& UserId, const FString& Nonce, const FString& AppId = FString());

	/** Meta Quest, with the id as ovrID (uint64). */
	TSvFuture<FSvSignInResult> SignInWithMeta(uint64 UserId, const FString& Nonce, const FString& AppId = FString());

	/**
	 * Steam: the hex ticket of ISteamUser::GetAuthTicketForWebApi("splattervault"). A ticket works once; cancel it
	 * with Steam (CancelAuthTicket) after this answers.
	 */
	TSvFuture<FSvSignInResult> SignInWithSteam(const FString& TicketHex, const FString& AppId = FString());

	/** Steam, with the ticket bytes of GetTicketForWebApiResponse_t. */
	TSvFuture<FSvSignInResult> SignInWithSteamTicket(const TArray<uint8>& Ticket, const FString& AppId = FString());

	/** Xsolla: the player's Xsolla Login token. AppId is the Login project id. */
	TSvFuture<FSvSignInResult> SignInWithXsolla(const FString& Token, const FString& AppId = FString());

	/** PlayFab: the SessionTicket of the player's PlayFab login. AppId is the title id. */
	TSvFuture<FSvSignInResult> SignInWithPlayFab(const FString& SessionTicket, const FString& AppId = FString());

	/** Supabase: the player's access token. AppId is the project ref. */
	TSvFuture<FSvSignInResult> SignInWithSupabase(const FString& AccessToken, const FString& AppId = FString());

	/**
	 * Uses a player token your backend got from POST /rest/player-auth/studio (with its organization key, on your
	 * server). The token is checked with GET /player-auth/me first: a refused one changes nothing. It cannot be
	 * renewed: when it runs out, OnSignInRequired is raised and your backend issues a new one. ExpiresAt (the API's
	 * expiresAt) is optional: the token's exp is used otherwise, corrected for this device's clock with the answer's
	 * Date header.
	 */
	TSvFuture<FSvPlayerProfile> SignInWithStudioToken(const FString& PlayerToken, const FString& ExpiresAt = FString());

	// ─── email accounts ──────────────────────────────────────────────────────────────────────────────────────────

	/** Emails a one-time code (sign in, sign up, reset the password). Always answers the same. At most one a minute per address. */
	TSvFuture<FSvCodeSent> RequestEmailCode(const FString& Email);

	/**
	 * Signs in with the emailed code. With no account yet a player is made (when the game lets players sign up).
	 * NewPassword sets (or resets) the password and signs out the player's other devices. Renewable: the refresh token
	 * is kept in secure storage.
	 */
	TSvFuture<FSvSignInResult> VerifyEmailCode(const FString& Email, const FString& Code, const FString& NewPassword = FString(),
		const FString& DisplayName = FString());

	/** Signs in with an email and password. A wrong pair is always invalid_credentials. */
	TSvFuture<FSvSignInResult> SignInWithPassword(const FString& Email, const FString& Password);

	/** Renews an email sign-in now with its refresh token (the SDK also does this by itself). refresh_invalid when it ended. */
	TSvFuture<FSvSignInResult> Refresh();

	/**
	 * Signs out on this device: ends the email sign-in (its refresh token) and forgets the player token. The next
	 * Restore answers false. The guest secret stays unless bForgetGuest: it is the guest's only way back in.
	 */
	TSvFuture<FSvVoid> SignOut(bool bForgetGuest = false);

	// ─── guests ──────────────────────────────────────────────────────────────────────────────────────────────────

	/**
	 * Signs in as this device's guest, making one on the first launch. The device secret is saved to secure storage
	 * before the player token is used; it is the guest's only way back in. When storage cannot be read this fails with
	 * storage_unreadable and makes NO new guest. Check FSvSignInResult::SavedToDevice after a new guest.
	 */
	TSvFuture<FSvSignInResult> SignInAsGuest(const FString& DisplayName = FString());

	/**
	 * Makes a new guest. Refused (invalid_state) when this device already has one (storage_unreadable when storage
	 * cannot tell), unless bReplaceExistingGuest: the old guest is then lost unless it has an email.
	 */
	TSvFuture<FSvSignInResult> CreateGuest(const FString& DisplayName = FString(), bool bReplaceExistingGuest = false);

	/** Signs in with this device's guest secret. proof_invalid when no guest has it any more. */
	TSvFuture<FSvSignInResult> SignInWithDevice();

	/** This device holds a guest secret (storage_unreadable when storage cannot tell). */
	TSvResult<bool> HasGuestOnDevice();

	/** Forgets this device's guest secret (the guest is lost unless it has another way to sign in). */
	bool ForgetGuestOnDevice();

	// ─── the signed-in player ────────────────────────────────────────────────────────────────────────────────────

	/** The signed-in player, their game and their ways to sign in. */
	TSvFuture<FSvPlayerProfile> GetMe();

	/**
	 * Erases the player and everything every studio service keeps about them. It cannot be undone. Signs out on this
	 * device and forgets what this device kept for that player.
	 */
	TSvFuture<FSvVoid> EraseMe();

	/** Links a Meta account to the signed-in player (a fresh nonce, as for sign-in). */
	TSvFuture<FSvPlayerProfile> LinkMeta(const FString& UserId, const FString& Nonce, const FString& AppId = FString());
	TSvFuture<FSvPlayerProfile> LinkSteam(const FString& TicketHex, const FString& AppId = FString());
	/** Links Steam with the ticket bytes of GetTicketForWebApiResponse_t (as SignInWithSteamTicket). */
	TSvFuture<FSvPlayerProfile> LinkSteamTicket(const TArray<uint8>& Ticket, const FString& AppId = FString());
	TSvFuture<FSvPlayerProfile> LinkXsolla(const FString& Token, const FString& AppId = FString());
	TSvFuture<FSvPlayerProfile> LinkPlayFab(const FString& SessionTicket, const FString& AppId = FString());
	TSvFuture<FSvPlayerProfile> LinkSupabase(const FString& AccessToken, const FString& AppId = FString());

	/**
	 * Unlinks a way to sign in: xsolla, meta, steam, playfab, supabase or device (unlinking the device also forgets
	 * its secret here, when it is this player's). The email has its own call (RemoveEmail); the studio link cannot be
	 * removed by the player. Refused (last_identity) for the player's only way in.
	 */
	TSvFuture<FSvPlayerProfile> Unlink(const FString& Provider);

	/** Emails a code to Email to add it (or change to it). Then AddEmail or ChangeEmail. */
	TSvFuture<FSvCodeSent> RequestLinkEmailCode(const FString& Email);

	/** Emails a code to the CURRENT address of an account without a password: the OldCode RemoveEmail needs. */
	TSvFuture<FSvCodeSent> RequestEmailRemovalCode();

	/** Adds a verified email to the signed-in player (a guest keeps their player), with an optional password. */
	TSvFuture<FSvPlayerProfile> AddEmail(const FString& Email, const FString& Code, const FString& Password = FString());

	/**
	 * Changes the email: Code from the new address, plus CurrentPassword (an account with a password) or OldCode
	 * (sent to the current address). Signs out the player's other devices; this device keeps its sign-in.
	 */
	TSvFuture<FSvPlayerProfile> ChangeEmail(const FString& NewEmail, const FString& Code, const FString& CurrentPassword = FString(),
		const FString& OldCode = FString());

	/**
	 * Removes the email (and its password): CurrentPassword, or for an account without a password OldCode from
	 * RequestEmailRemovalCode. Every email sign-in of the player ends, this device's too. Sent as POST
	 * /player-auth/link/email/remove; an API without that route (404) gets the older DELETE /player-auth/link/email once.
	 */
	TSvFuture<FSvPlayerProfile> RemoveEmail(const FString& CurrentPassword = FString(), const FString& OldCode = FString());

	/** Changes the password. Signs out the player's other devices; this device keeps its sign-in (a new refresh token is saved). */
	TSvFuture<FSvVoid> ChangePassword(const FString& CurrentPassword, const FString& NewPassword);

	/** A Steam ticket's bytes as the hex the API takes. */
	static FString ToHex(const TArray<uint8>& Bytes);

	/** The token's lifetime in seconds: exp (or ExpiresAt) minus iat, kept within 60 s to 24 h; 3600 when unknown. */
	static double TokenLifetimeSeconds(const FString& Token, const FString& ExpiresAt);

private:
	friend class FSplatterVaultClient;
	friend class FSvPlayerDataApi;
	friend class FSvStoreApi;
	friend struct FSvAuthOps;

	void Configure(double RenewLeadSeconds, double RefreshTimeoutSeconds);

	/** Fails everyone waiting on a renewal, a guest sign-in or the guest lock (shutdown). */
	void Abandon();

	/** Sends a player request: renews first when due, and after a 401 token_expired renews once and sends once more. */
	void SendAsPlayer(const FSvCall& InCall, TSvCallback<FSvHttpResponse> Done);

	void RaiseStorageFailed(const FString& Item, ESvStorageOperation Operation, const FString& Why);
	static FSvError Unreadable(const FString& Item);

	FSplatterVaultClient& Owner;
	TUniquePtr<FSvAuthState> State;
};

/** Player data (the /rest/player-data routes): the player's own stores and the game's public data. */
class SPLATTERVAULT_API FSvPlayerDataApi
{
public:
	/** The most keys one read may name. */
	static constexpr int32 MaxReadKeys = 100;

	explicit FSvPlayerDataApi(FSplatterVaultClient& InOwner);

	/**
	 * The signed-in player's player and readonly stores (or only Stores), every key or only Keys (at most 100);
	 * bKeysOnly leaves values out.
	 */
	TSvFuture<FSvPlayerData> GetMine(const TArray<FString>& Keys = TArray<FString>(), bool bKeysOnly = false,
		const TArray<FString>& Stores = TArray<FString>());

	/**
	 * Applies Ops to the player's player store, all or nothing. 30 writes a minute per player.
	 *
	 * There is no Idempotency-Key for data writes. When every op has ifVersion, the SDK sends the write again only after
	 * an answer that proves nothing was applied (408, 429, 503, or an expired token it renewed). After no answer, 500,
	 * 502 or 504 it does not: the error has OutcomeUnknown set. A version_conflict after the write was sent more than
	 * once also has OutcomeUnknown (our own earlier send may be the one that applied). Recover by reading again: if the
	 * key's version moved and the value is yours (an inc: it went up by your amount), it applied; never re-apply blindly.
	 */
	TSvFuture<FSvDataWriteResult> Write(const FSvDataOps& Ops);

	/** @internal Flags a failed write whose outcome is unknown (see Write). */
	static FSvError MarkWriteOutcome(const FSvError& Error);

	/**
	 * The game's public data, as the signed-in player. The SDK keeps the last answer and its ETag and asks with
	 * If-None-Match: when nothing changed the API answers 304 and the kept copy is returned (NotModified).
	 */
	TSvFuture<FSvGameData> GetGameData(const TArray<FString>& Keys = TArray<FString>());

	/**
	 * A game's public data with no credential (before sign-in, a launcher): GameIdOrKey is a game id or Game Key,
	 * default this game. Same ETag cache.
	 */
	TSvFuture<FSvGameData> GetPublic(const FString& GameIdOrKey = FString(), const TArray<FString>& Keys = TArray<FString>());

	/** Forgets the kept copies (and their ETags). */
	void ClearCache();

private:
	TSvFuture<FSvGameData> GetCached(const FString& Path, bool bAsPlayer);
	static bool KeysParam(const TArray<FString>& Keys, FString& OutParam, FString& OutError);

	struct FCached
	{
		FString ETag;
		FSvDataStore Data;
	};

	FSplatterVaultClient& Owner;
	TMap<FString, FCached, FDefaultSetAllocator, TSvCaseSensitiveKeyFuncs<FCached>> Cache;
};

/**
 * The studio store for the signed-in player (the /rest/store routes): catalog, balances and items, history, buying with
 * virtual currency, using items up, and the Meta Quest purchase check.
 *
 * Idempotency. Every change gets an Idempotency-Key when the player commits to it (a new random key, or yours),
 * and that same key on every retry, so the API applies it once. The key and the exact request are kept in secure
 * storage until the API answers; if the game quits or loses its connection first, ResumePending sends them again
 * (the subsystem does it after each sign-in only when the game turns that on). A repeat of a change that already
 * happened answers Replayed and is a success. Every store error carries the key (FSvError::IdempotencyKey): a "Retry"
 * button calls ResumePending or passes that key, never a new purchase without it. Pending changes belong to their
 * player and are never sent under another player's token.
 */
class SPLATTERVAULT_API FSvStoreApi
{
public:
	/** The most of one item a single purchase buys. */
	static constexpr int32 MaxPurchaseQuantity = 100;

	explicit FSvStoreApi(FSplatterVaultClient& InOwner);

	/**
	 * Active currencies and items with prices, and what this player may buy or use up. The SDK keeps the last answer
	 * and its ETag (when the API sends one) and asks with If-None-Match: a 304 returns the kept copy (NotModified).
	 */
	TSvFuture<FSvStoreCatalog> GetCatalog();

	/** The player's balances and items. */
	TSvFuture<FSvStoreHoldings> GetHoldings();

	/** The player's store history, newest first, 50 a page; pass the previous page's Next as Before. */
	TSvFuture<FSvStoreHistoryPage> GetHistory(const FString& Before = FString());

	/**
	 * Buys Quantity (1 to 100) of Sku with Currency. ExpectedPrice is the unit price the player saw: a changed price
	 * is refused (price_changed). Pass IdempotencyKey to name the purchase yourself; by default a new key is made.
	 */
	TSvFuture<FSvStoreTxnResult> Purchase(const FString& Sku, const FString& Currency, int64 ExpectedPrice, int32 Quantity = 1,
		const FString& IdempotencyKey = FString());

	/** Uses up a held item: a one-of-a-kind copy, or Quantity of a stack (Quantity 0: all of a one-of-a-kind item). */
	TSvFuture<FSvStoreTxnResult> ConsumeInstance(const FString& InstanceId, int64 Quantity = 0, const FString& IdempotencyKey = FString());

	/** Uses up Quantity of Sku, oldest first. */
	TSvFuture<FSvStoreTxnResult> ConsumeSku(const FString& Sku, int64 Quantity, const FString& IdempotencyKey = FString());

	/**
	 * Asks Meta for the player's Quest purchases and grants each mapped one once. Call it after a purchase and when the
	 * game starts; at most 6 a minute per player. meta_not_linked: the player has no Meta sign-in for the app.
	 */
	TSvFuture<FSvMetaSyncResult> SyncMetaPurchases(const FString& AppId = FString());

	/** Store changes of the signed-in player that have not been answered yet (storage_unreadable when storage cannot tell). */
	TSvResult<TArray<FSvPendingStoreChange>> GetPending();

	/**
	 * Sends the signed-in player's unanswered changes again, each with its own key and exact request. A refused one
	 * (for example insufficient_funds) is dropped and reported in Refused; one that still gets no answer stays pending.
	 * Right before each send it checks that the change is still kept and that its player is still the signed-in
	 * player; when the player changed it stops (PlayerChanged) and leaves the rest pending.
	 *
	 * A change in this queue may be one the API refused for a passing reason (a 401, a 503) and never applied: resuming
	 * applies it then. Resume at a moment the player expects the purchase to go through, or DiscardPending it.
	 */
	TSvFuture<FSvResumeResult> ResumePending();

	/**
	 * Forgets an unanswered change of PlayerId (default: the signed-in player); it may still have happened: check the
	 * history. When the kept list cannot be read, the whole list is deleted: nothing in it could be finished anyway.
	 */
	bool DiscardPending(const FString& IdempotencyKey, const FString& PlayerId = FString());

	/** Forgets every kept change of every player, without reading them. */
	bool DiscardAllPending();

	/** Forgets the kept catalog (and its ETag). */
	void ClearCatalogCache();

	/** A new Idempotency-Key: 32 hex characters from a secure random source. */
	static FString NewIdempotencyKey();

	/** A key the API accepts: 1 to 128 visible ASCII characters. */
	static bool IsValidIdempotencyKey(const FString& Key);

private:
	friend class FSvAuth;
	friend struct FSvAuthOps;

	template <typename T>
	static void GetModel(FSvAuth& Auth, const FString& Path, TSvCallback<T> Done);

	TSvFuture<FSvStoreTxnResult> Change(const FString& Kind, const FString& Path, const FString& BodyJson, const FString& IdempotencyKey);
	void SendChange(const FSvPendingStoreChange& Change, bool bKept, TSvCallback<FSvStoreTxnResult> Done);
	/** What a store change's future gets if the client shuts down before its answer: its key, and whether it is kept. */
	static FSvError DroppedError(const FString& Key, bool bKept);

public:
	/** Shutdown: callers that joined a send in flight get client_shutdown (with the key, still pending) at once. */
	void Abandon();

private:
	void ResumeNext(TSharedRef<TArray<FSvPendingStoreChange>> Queue, int32 Index, TSharedRef<FSvResumeResult> Result,
		TSvCallback<FSvResumeResult> Done);
	static bool StaysPending(const FSvError& Error);

	bool LoadPending(TArray<FSvPendingStoreChange>& OutList, FSvError& OutError);
	bool UpdatePending(TFunctionRef<void(TArray<FSvPendingStoreChange>&)> Mutate, bool bDeleteIfUnreadable = false);
	bool DeletePendingList();
	void ForgetPendingForPlayer(const FString& PlayerId);
	FString PendingKey() const;

	struct FCachedCatalog
	{
		FString ETag;
		FSvStoreCatalog Catalog;
	};

	FSplatterVaultClient& Owner;
	TMap<FString, FCachedCatalog, FDefaultSetAllocator, TSvCaseSensitiveKeyFuncs<FCachedCatalog>> CatalogCache;
	/** A store change being sent right now: its exact request (path and body), and the callers that joined it. */
	struct FInFlightChange
	{
		FString Request;
		TArray<TSvCallback<FSvStoreTxnResult>> Joined;
	};

	/** Player + Idempotency-Key of the changes being sent right now: one send per change at a time. */
	TMap<FString, FInFlightChange, FDefaultSetAllocator, TSvCaseSensitiveKeyFuncs<FInFlightChange>> InFlight;

	/** The kept pending list as last read or written (write-through cache of the secure store). */
	TArray<FSvPendingStoreChange> PendingCache;
	bool bPendingCached = false;
};

/**
 * The game client's entry point to SplatterVault's studio services: player sign-in and accounts (Auth), player data
 * (Data) and the store (Store).
 *
 * It never takes an API key: a game build must never hold one. Players sign in with a platform proof, a
 * SplatterVault email account or a guest on the device, and every request then carries the player token.
 *
 * In a game, use USplatterVaultSubsystem (configured from Project Settings) and its GetClient(). Make your own with
 * Create only for tools or tests. Every call must be made on the game thread.
 */
class SPLATTERVAULT_API FSplatterVaultClient : public TSharedFromThis<FSplatterVaultClient>
{
public:
	static constexpr const TCHAR* DefaultBaseUrl = TEXT("https://splattervault.com/rest");

	/**
	 * A client for Options; null (and OutError) when the options are refused: no game or both game id and key, a base
	 * URL that is not https (http only for loopback hosts unless bAllowInsecureHttpForDevelopment), or a number out of
	 * range (see ValidateOptions).
	 */
	static TSharedPtr<FSplatterVaultClient> Create(const FSvClientOptions& Options, FString& OutError);

	/**
	 * Checks the numeric options: finite, timeouts above 0 and at most 2^31-1 ms (refresh timeout at most 10 s),
	 * RenewBeforeExpirySeconds 0 to 3600, MaxAttempts 1 to 10, retry waits 0 to 2^31-1 ms, Jitter 0 to 1.
	 */
	static bool ValidateOptions(const FSvClientOptions& Options, FString& OutError);

	~FSplatterVaultClient();

	/** Sign-in, accounts and the player token. */
	FSvAuth& Auth() { return *AuthApi; }

	/** The player's saved data and the game's public data. */
	FSvPlayerDataApi& Data() { return *DataApi; }

	/** Currencies, catalog, the player's inventory, purchases and consumption. */
	FSvStoreApi& Store() { return *StoreApi; }

	/** The game id, or 0 when the game is named by its key. */
	int32 GetGameId() const { return GameId; }
	const FString& GetGameKey() const { return GameKey; }
	const FString& GetBaseUrl() const { return BaseUrl; }

	/** The store the SDK keeps secrets in. */
	ISvSecureStore& GetSecureStore() const { return *SecureStore; }
	/** @internal */
	TSharedRef<ISvSecureStore> GetSecureStoreRef() const { return SecureStore.ToSharedRef(); }

	/**
	 * Ends the client: answers that arrive later complete with client_shutdown and change nothing (a refresh answer
	 * still saves its rotated refresh token), waits are cancelled, and calls waiting on a renewal or the guest lock fail
	 * with client_shutdown.
	 *
	 * Do not drop the last reference to a client from inside one of its own events or callbacks; answers keep it alive
	 * while they are handled, but synchronous calls cannot. USplatterVaultSubsystem releases a replaced client on the next
	 * tick for this reason.
	 */
	void Shutdown();
	bool IsShutdown() const { return bShutdown; }

	/** The default storage namespace: scheme://host[:port] (lower-case, no path) and the game. */
	static FString DefaultStorageNamespace(const FString& BaseUrl, int32 GameId, const FString& GameKey);

	/** Checks a base URL; the trimmed URL without a trailing slash, or empty and OutError. */
	static FString CheckBaseUrl(const FString& BaseUrl, bool bAllowInsecure, FString& OutError);

	/** @internal */
	FSvPipeline& GetPipeline() const { return *Pipeline; }
	/** @internal The prefix of every stored key of this client. */
	const FString& GetStoragePrefix() const { return StoragePrefix; }
	/** @internal Adds gameId or gameKey to a request body. */
	void AddGame(const TSharedRef<class FJsonObject>& Body) const;
	/** @internal */
	void Log(ELogVerbosity::Type Verbosity, const FString& Message) const;

private:
	FSplatterVaultClient();

	int32 GameId = 0;
	FString GameKey;
	FString BaseUrl;
	FString StoragePrefix;
	bool bShutdown = false;
	FSvLogger Logger;
	/** Removes the default delays still waiting (FTSTicker). */
	TFunction<void()> StopDelays;
	TSharedPtr<ISvSecureStore> SecureStore;
	TUniquePtr<FSvPipeline> Pipeline;
	TUniquePtr<FSvAuth> AuthApi;
	TUniquePtr<FSvPlayerDataApi> DataApi;
	TUniquePtr<FSvStoreApi> StoreApi;
};
