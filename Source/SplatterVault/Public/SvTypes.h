// Copyright (c) SplatterVault. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "SvConstants.h"
#include "SvTypes.generated.h"

/**
 * Models of the SplatterVault studio API's player routes (see the studio API description, studio-spec.json).
 *
 * Dates are ISO 8601 strings exactly as the API sends them. Counts, versions, balances and quantities are int64 (the
 * API keeps them within 2^53 - 1). A field the API may send as null has a companion "Has..." flag. Arbitrary JSON
 * (a data value, an item's custom object) is kept as JSON text in a "...Json" field: read it with
 * USplatterVaultBlueprintLibrary or FJsonSerializer.
 *
 * Bool fields that mirror an API field are named exactly like it (no "b" prefix) so FJsonObjectConverter maps them.
 */

/**
 * A refusal from the SplatterVault API, a request that got no answer, or a call the SDK refused itself.
 *
 * Act on Code (the API's stable "error" code such as token_expired or insufficient_funds), never on Message: the
 * message is a plain sentence to log or show and may change. Status 0 means no HTTP answer came with this error
 * (see SvErrorCodes). A framework-shaped answer ({ name, message, status }) has no code: Code is empty and ErrorName
 * holds its name.
 */
USTRUCT(BlueprintType)
struct SPLATTERVAULT_API FSvError
{
	GENERATED_BODY()

	/** HTTP status; 0 when no answer arrived or the SDK refused the call itself. */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Error")
	int32 Status = 0;

	/** The API's error code, an SDK code (status 0), or empty (a framework-shaped answer). */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Error")
	FString Code;

	/** A plain sentence to log or show. Never carries a token or a proof. */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Error")
	FString Message;

	/** The answer carried Retry-After: do not send this request again sooner than RetryAfterSeconds. */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Error")
	bool HasRetryAfter = false;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Error")
	float RetryAfterSeconds = 0.f;

	/** The refusal's "details" as JSON text (for example the current versions of a version_conflict), or empty. */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Error")
	FString DetailsJson;

	/** A framework answer's "name" (such as NOT_FOUND), or empty. */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Error")
	FString ErrorName;

	/**
	 * The change may or may not have been applied: a write got no answer (network error, timeout) or 500 / 502 / 504,
	 * or a player-data write that was sent again got version_conflict (the earlier send may be the one that applied).
	 * Recover by reading again: for player data, if the key's version moved and the value is what you wrote (an inc:
	 * the counter went up by your amount), it applied; do not apply it again. For a store change, call ResumePending
	 * (or send it again with IdempotencyKey): the API applies it at most once.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Error")
	bool OutcomeUnknown = false;

	/**
	 * Store changes: the Idempotency-Key the change was sent with. To try again, call ResumePending or pass this key
	 * to the same call; never send the change again without it (that would be a second purchase).
	 */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Error")
	FString IdempotencyKey;

	/**
	 * Store changes: the change is still kept on this device for ResumePending (no answer, 408 / 429 / 500 / 502 / 503
	 * / 504, a sign-in problem, another player signed in, the client shut down). While it is pending, offer the player
	 * "finish" (ResumePending, or the same call with IdempotencyKey), never "buy again". False: nothing is kept (the
	 * API refused it, or it was never sent).
	 */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Error")
	bool Pending = false;

	/** How many times this request was sent (0: never sent; 2 or more: the SDK sent it again). */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Error")
	int32 Attempts = 0;

	FSvError() = default;
	FSvError(int32 InStatus, const FString& InCode, const FString& InMessage);

	/** True when Code is InCode. */
	bool Is(const TCHAR* InCode) const { return Code.Equals(InCode, ESearchCase::CaseSensitive); }
	bool Is(const FString& InCode) const { return Code.Equals(InCode, ESearchCase::CaseSensitive); }

	/** No answer arrived (network failure or timeout). */
	bool IsNetworkError() const;

	/** The same request may succeed later: no answer, or 408, 429, 500, 502, 503 or 504. */
	bool IsTransient() const;

	/** The statuses IsTransient() counts. */
	static bool IsTransientStatus(int32 InStatus);

	/** "code (status): message", for logs. */
	FString ToString() const;
};

/** Why the game must sign the player in again. */
UENUM(BlueprintType)
enum class ESvSignInRequiredReason : uint8
{
	/** The player token ran out and the SDK cannot renew it (no refresh token or guest secret). */
	TokenExpired,
	/** The token is not valid any more (the game changed owner, or it is not one of ours). */
	TokenInvalid,
	/** The player was erased. */
	PlayerDeleted,
	/** The email sign-in ended (password changed elsewhere, signed out, expired, game moved). */
	RefreshInvalid,
	/** No guest has this device's secret any more (unlinked or erased). */
	DeviceSecretRejected
};

UENUM(BlueprintType)
enum class ESvStorageOperation : uint8
{
	Read,
	Write,
	Delete
};

/** Secure storage failed. It never carries a value. */
USTRUCT(BlueprintType)
struct SPLATTERVAULT_API FSvStorageFailure
{
	GENERATED_BODY()

	/** refresh_token, guest_secret, last_sign_in or pending_store_changes. */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Auth")
	FString Item;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Auth")
	ESvStorageOperation Operation = ESvStorageOperation::Read;

	/** What went wrong (never a value). */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Auth")
	FString Message;
};

/** Raised when the player must sign in again with a fresh proof. */
USTRUCT(BlueprintType)
struct SPLATTERVAULT_API FSvSignInRequired
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Auth")
	ESvSignInRequiredReason Reason = ESvSignInRequiredReason::TokenExpired;

	/** How the player had signed in (SvProviders), or empty. */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Auth")
	FString Method;

	/** The API's code that ended the sign-in. */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Auth")
	FString ErrorCode;
};

// ─── sign-in ──────────────────────────────────────────────────────────────────────────────────────────────────────

/** A way the player can sign in, and when it was linked. */
USTRUCT(BlueprintType)
struct SPLATTERVAULT_API FSvIdentity
{
	GENERATED_BODY()

	/** xsolla, meta, steam, studio, playfab, supabase, email or device. */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Auth")
	FString Provider;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Auth")
	FString LinkedAt;
};

/** The player of a sign-in answer. */
USTRUCT(BlueprintType)
struct SPLATTERVAULT_API FSvSignedInPlayer
{
	GENERATED_BODY()

	/** The SplatterVault player id (uuid): the token's sub. It never changes. */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Auth")
	FString Id;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Auth")
	FString DisplayName;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Auth")
	FString CreatedAt;

	/** This sign-in made the player. */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Auth")
	bool NewPlayer = false;
};

/**
 * What a sign-in answers. The player token stays in the client (FSvAuth::GetPlayerToken); refresh tokens and device
 * secrets go straight to secure storage and are never handed out.
 */
USTRUCT(BlueprintType)
struct SPLATTERVAULT_API FSvSignInResult
{
	GENERATED_BODY()

	/** How the player signed in (SvProviders). */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Auth")
	FString Method;

	/** When the player token runs out (ISO 8601, the API's clock). */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Auth")
	FString ExpiresAt;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Auth")
	FSvSignedInPlayer Player;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Auth")
	TArray<FSvIdentity> Identities;

	/**
	 * The SDK can renew this sign-in by itself, also after a restart: its refresh token, or this device's guest
	 * secret, is saved in secure storage.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Auth")
	bool Renewable = false;

	/**
	 * False when this sign-in returned a refresh token or a new guest secret and secure storage could not save it
	 * (OnStorageFailed was raised). The secret is kept in memory for this session only.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Auth")
	bool SavedToDevice = true;
};

/** The player of GET /player-auth/me. */
USTRUCT(BlueprintType)
struct SPLATTERVAULT_API FSvPlayerInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Auth")
	FString Id;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Auth")
	FString DisplayName;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Auth")
	FString CreatedAt;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Auth")
	FString LastSignedInAt;
};

USTRUCT(BlueprintType)
struct SPLATTERVAULT_API FSvGameRef
{
	GENERATED_BODY()

	/** The game's Game Key when the token was issued (may be empty). */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Auth")
	FString GameKey;
};

/** The signed-in player (GET /player-auth/me, and the answer of link and unlink). */
USTRUCT(BlueprintType)
struct SPLATTERVAULT_API FSvPlayerProfile
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Auth")
	FSvPlayerInfo Player;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Auth")
	FSvGameRef Game;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Auth")
	TArray<FSvIdentity> Identities;

	/** The player can sign in with Provider. */
	bool HasIdentity(const FString& Provider) const;
};

/** An email code was asked for (always answered the same, whether or not a code was sent). */
USTRUCT(BlueprintType)
struct SPLATTERVAULT_API FSvCodeSent
{
	GENERATED_BODY()

	/** How long a code lasts (600). */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Auth")
	int32 ExpiresInSeconds = 0;

	/** Wait this long before asking again (60). */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Auth")
	int32 ResendAfterSeconds = 0;
};

// ─── player data ──────────────────────────────────────────────────────────────────────────────────────────────────

/** One stored key: its JSON value, its version and when it last changed. */
USTRUCT(BlueprintType)
struct SPLATTERVAULT_API FSvDataEntry
{
	GENERATED_BODY()

	/** The key (case-sensitive). */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Data")
	FString Key;

	/** The value as JSON text, or empty when the read asked for keys only. */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Data")
	FString ValueJson;

	/** Only ever goes up when the key changes. Send it back as ifVersion to change only what you read. */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Data")
	int64 Version = 0;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Data")
	FString UpdatedAt;

	/** The value parsed (null when there is none). */
	TSharedPtr<class FJsonValue> GetValue() const;
};

/** The keys of one store. */
USTRUCT(BlueprintType)
struct SPLATTERVAULT_API FSvDataStore
{
	GENERATED_BODY()

	/** The entries (keys are case-sensitive, so they are a list: use Find, or the Find Data Entry node). */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Data")
	TArray<FSvDataEntry> Entries;

	/** The entry, or null. Keys are case-sensitive. */
	const FSvDataEntry* Find(const FString& Key) const;

	/** The version of Key, or 0 when it does not exist (0 is what ifVersion takes for "must not exist"). */
	int64 VersionOf(const FString& Key) const;
};

/** The signed-in player's data (GET /player-data/me): the player and readonly stores. */
USTRUCT(BlueprintType)
struct SPLATTERVAULT_API FSvPlayerData
{
	GENERATED_BODY()

	/** What the player writes (untrusted: never keep currency, XP or unlocks here). */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Data")
	FSvDataStore Player;

	/** What only your backend writes; the player reads it. */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Data")
	FSvDataStore Readonly;
};

/** A game's public data, with the ETag it came with. */
USTRUCT(BlueprintType)
struct SPLATTERVAULT_API FSvGameData
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Data")
	FSvDataStore Data;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Data")
	FString ETag;

	/** The API answered 304: nothing changed since the copy the SDK kept, which is returned. */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Data")
	bool NotModified = false;
};

UENUM(BlueprintType)
enum class ESvDataOpType : uint8
{
	Set,
	Delete,
	Inc
};

/**
 * One op of a player-data write (see FSvDataOps for C++, or the Make... nodes of USplatterVaultBlueprintLibrary).
 * Players write only their player store.
 */
USTRUCT(BlueprintType)
struct SPLATTERVAULT_API FSvDataOp
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "SplatterVault|Data")
	ESvDataOpType Op = ESvDataOpType::Set;

	/** 1 to 128 letters, digits and _ . : - (starting with a letter, digit or _); sv. and __proto__ are reserved. */
	UPROPERTY(BlueprintReadWrite, Category = "SplatterVault|Data")
	FString Key;

	/** Set: the value as JSON text (any JSON but null), for example 42, "text", {"fov":90}. */
	UPROPERTY(BlueprintReadWrite, Category = "SplatterVault|Data")
	FString ValueJson;

	/** Inc: what to add (negative to subtract). */
	UPROPERTY(BlueprintReadWrite, Category = "SplatterVault|Data")
	int64 By = 0;

	/** Inc: the whole-number field inside an object value, or empty for the value itself. */
	UPROPERTY(BlueprintReadWrite, Category = "SplatterVault|Data")
	FString Field;

	UPROPERTY(BlueprintReadWrite, Category = "SplatterVault|Data")
	bool HasMin = false;

	UPROPERTY(BlueprintReadWrite, Category = "SplatterVault|Data")
	int64 Min = 0;

	UPROPERTY(BlueprintReadWrite, Category = "SplatterVault|Data")
	bool HasMax = false;

	UPROPERTY(BlueprintReadWrite, Category = "SplatterVault|Data")
	int64 Max = 0;

	/** Change the key only while it is at IfVersion (0: only while it does not exist). */
	UPROPERTY(BlueprintReadWrite, Category = "SplatterVault|Data")
	bool HasIfVersion = false;

	UPROPERTY(BlueprintReadWrite, Category = "SplatterVault|Data")
	int64 IfVersion = 0;
};

/** One op's result, in the order of the ops. */
USTRUCT(BlueprintType)
struct SPLATTERVAULT_API FSvDataOpResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Data")
	FString Store;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Data")
	FString Key;

	/** False when the key no longer exists (a delete). */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Data")
	bool HasVersion = false;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Data")
	int64 Version = 0;

	/** An inc's stored value after it, as JSON text; empty for other ops. */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Data")
	FString ValueJson;
};

USTRUCT(BlueprintType)
struct SPLATTERVAULT_API FSvDataWriteResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Data")
	TArray<FSvDataOpResult> Results;
};

/** One current version of a version_conflict (USplatterVaultBlueprintLibrary::GetVersionConflicts). */
USTRUCT(BlueprintType)
struct SPLATTERVAULT_API FSvVersionInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Data")
	FString Store;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Data")
	FString Key;

	/** False when the key does not exist. */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Data")
	bool HasVersion = false;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Data")
	int64 Version = 0;
};

// ─── store ────────────────────────────────────────────────────────────────────────────────────────────────────────

USTRUCT(BlueprintType)
struct SPLATTERVAULT_API FSvStoreSettings
{
	GENERATED_BODY()

	/** Players may buy with virtual currency from the game (the item's own switch applies too). */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	bool PlayerPurchases = false;

	/** Players may use up items from the game (the item's own switch applies too). */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	bool PlayerConsume = false;
};

USTRUCT(BlueprintType)
struct SPLATTERVAULT_API FSvCurrency
{
	GENERATED_BODY()

	/** 2-8 capital letters and digits. */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	FString Code;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	FString Name;

	/** False when there is no cap. */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	bool HasCap = false;

	/** The most one player may hold. */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	int64 Cap = 0;
};

USTRUCT(BlueprintType)
struct SPLATTERVAULT_API FSvPrice
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	FString Currency;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	int64 Amount = 0;
};

/** A bundle line: exactly one of Sku and Currency. */
USTRUCT(BlueprintType)
struct SPLATTERVAULT_API FSvBundleLine
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	FString Sku;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	FString Currency;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	int64 Quantity = 0;
};

USTRUCT(BlueprintType)
struct SPLATTERVAULT_API FSvCatalogItem
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	FString Sku;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	FString Name;

	/** item or bundle (a bundle gives its Contents). */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	FString Kind;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	bool Stackable = false;

	/** False when there is no limit. */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	bool HasMaxOwned = false;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	int64 MaxOwned = 0;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	TArray<FSvPrice> Prices;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	TArray<FSvBundleLine> Contents;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	TArray<FString> Tags;

	/** The studio's JSON object for the item as JSON text, or empty. Never secrets. */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	FString CustomJson;

	/** This player may buy it. */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	bool Purchasable = false;

	/** This player may use it up. */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	bool Consumable = false;

	/** The unit price in Currency; false when the item has none. */
	bool PriceIn(const FString& InCurrency, int64& OutAmount) const;
};

/** The catalog for the signed-in player (GET /store/catalog). */
USTRUCT(BlueprintType)
struct SPLATTERVAULT_API FSvStoreCatalog
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	FSvStoreSettings Settings;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	TArray<FSvCurrency> Currencies;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	TArray<FSvCatalogItem> Items;

	/** The answer's ETag (sent back as If-None-Match), or empty. */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	FString ETag;

	/** The API answered 304: the copy the SDK kept is returned. */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	bool NotModified = false;

	/** The item, or null. */
	const FSvCatalogItem* Find(const FString& Sku) const;
};

USTRUCT(BlueprintType)
struct SPLATTERVAULT_API FSvBalance
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	FString Currency;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	int64 Amount = 0;
};

/** An inventory row: one stack, or one copy of a one-of-a-kind item. */
USTRUCT(BlueprintType)
struct SPLATTERVAULT_API FSvInventoryItem
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	FString InstanceId;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	FString Sku;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	int64 Quantity = 0;

	/** Your own data on this one item as JSON text (at most 1 KB), or empty. */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	FString CustomJson;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	FString AcquiredAt;
};

/** The signed-in player's balances and items (GET /store/me). */
USTRUCT(BlueprintType)
struct SPLATTERVAULT_API FSvStoreHoldings
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	FString PlayerId;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	TArray<FSvBalance> Balances;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	TArray<FSvInventoryItem> Items;

	/** The balance of Currency (0 when the player holds none). */
	int64 BalanceOf(const FString& Currency) const;

	/** How many of Sku the player holds, all stacks and copies together. */
	int64 CountOf(const FString& Sku) const;
};

/** A ledger change: exactly one of Currency and Sku. */
USTRUCT(BlueprintType)
struct SPLATTERVAULT_API FSvLedgerChange
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	FString Currency;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	FString Sku;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	FString InstanceId;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	int64 Delta = 0;

	/** The balance, stack quantity, or 1 / 0 for a one-of-a-kind copy, after the change. */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	int64 After = 0;
};

USTRUCT(BlueprintType)
struct SPLATTERVAULT_API FSvShortfall
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	FString Currency;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	FString Sku;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	int64 Missing = 0;
};

USTRUCT(BlueprintType)
struct SPLATTERVAULT_API FSvTxn
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	FString Id;

	/** grant, consume, purchase, platform_purchase, reversal, adjust, update_item, import, erase. */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	FString Kind;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	FString CreatedAt;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	FString Reason;

	/** The change a reversal took back, or empty. */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	FString Reverses;
};

/**
 * The answer of a store change. Trust its balances and items, not your own arithmetic. Replayed true means the API
 * had already applied this change (same Idempotency-Key): it is still a success.
 */
USTRUCT(BlueprintType)
struct SPLATTERVAULT_API FSvStoreTxnResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	FSvTxn Txn;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	TArray<FSvLedgerChange> Changes;

	/** The balances this change touched, after it. */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	TArray<FSvBalance> Balances;

	/** The inventory rows this change touched that still exist, after it. */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	TArray<FSvInventoryItem> Items;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	TArray<FSvShortfall> Shortfall;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	bool Replayed = false;

	/** The Idempotency-Key this change was sent with (set by the SDK). */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	FString IdempotencyKey;
};

/** A ledger entry of the player's history (newest first). */
USTRUCT(BlueprintType)
struct SPLATTERVAULT_API FSvHistoryEntry
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	FString Id;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	FString Kind;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	FString CreatedAt;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	FString Reverses;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	TArray<FSvLedgerChange> Changes;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	TArray<FSvShortfall> Shortfall;
};

/** One page (50) of the player's store history. */
USTRUCT(BlueprintType)
struct SPLATTERVAULT_API FSvStoreHistoryPage
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	TArray<FSvHistoryEntry> Txns;

	/** Pass as Before for the next, older page; empty at the end. */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	FString Next;
};

/** The Meta Quest purchase check's counts. */
USTRUCT(BlueprintType)
struct SPLATTERVAULT_API FSvMetaSyncResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	int32 Granted = 0;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	int32 AlreadyGranted = 0;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	int32 Unmapped = 0;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	int32 ConsumePending = 0;
};

/** A store change the player committed that has not been answered yet (kept in secure storage). */
USTRUCT(BlueprintType)
struct SPLATTERVAULT_API FSvPendingStoreChange
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	FString IdempotencyKey;

	/** purchase or consume. */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	FString Kind;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	FString Path;

	/** The exact JSON body first sent (a repeat must be the same request). */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	FString Body;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	FString PlayerId;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	FString CreatedAt;
};

/** A pending store change that the API refused when it was sent again (it is dropped: nothing changed). */
USTRUCT(BlueprintType)
struct SPLATTERVAULT_API FSvRefusedStoreChange
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	FSvPendingStoreChange Change;

	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	FSvError Error;
};

/** What ResumePending did. */
USTRUCT(BlueprintType)
struct SPLATTERVAULT_API FSvResumeResult
{
	GENERATED_BODY()

	/** The changes that were answered (each a success; Replayed when it had already happened). */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	TArray<FSvStoreTxnResult> Completed;

	/** The changes the API refused (dropped). */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	TArray<FSvRefusedStoreChange> Refused;

	/** How many still got no answer (or were not sent) and stay pending. */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	int32 StillPending = 0;

	/** Another player signed in while resuming: it stopped, and the rest stays pending for its own player. */
	UPROPERTY(BlueprintReadOnly, Category = "SplatterVault|Store")
	bool PlayerChanged = false;
};
