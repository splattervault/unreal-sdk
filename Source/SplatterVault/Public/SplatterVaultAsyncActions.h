// Copyright (c) SplatterVault. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "SvTypes.h"
#include "SplatterVaultAsyncActions.generated.h"

class FSplatterVaultClient;

/**
 * Blueprint async nodes for SplatterVault. Every node has On Success and On Failure pins; both carry the result and
 * an Error (key your handling on Error.Code, show Error.Message). The nodes use the game instance's
 * USplatterVaultSubsystem (Project Settings > Plugins > SplatterVault).
 */
UCLASS(Abstract)
class SPLATTERVAULT_API USvAsyncActionBase : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	virtual void Activate() override;

protected:
	/** Starts the call with the configured client. */
	virtual void Start(FSplatterVaultClient& Client) {}
	/** Fails at once (no configured client). */
	virtual void FailNow(const FSvError& Error) {}

	void Init(UObject* WorldContextObject);

	TWeakObjectPtr<UObject> WorldContext;
};

// ─── sign-in ──────────────────────────────────────────────────────────────────────────────────────────────────────

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FSvSignInPin, const FSvSignInResult&, Result, const FSvError&, Error);

UCLASS()
class SPLATTERVAULT_API USvSignInAsyncAction : public USvAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FSvSignInPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FSvSignInPin OnFailure;

	/** Signs in as this device's guest, making one on the first launch (check Result.Saved To Device after a new guest). */
	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Auth", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", AdvancedDisplay = "DisplayName", DisplayName = "Sign In As Guest (SplatterVault)"))
	static USvSignInAsyncAction* SignInAsGuest(UObject* WorldContextObject, const FString& DisplayName);

	/** Makes a new guest; refused when this device already has one unless Replace Existing Guest (the old guest is then lost). */
	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Auth", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Create Guest (SplatterVault)"))
	static USvSignInAsyncAction* CreateGuest(UObject* WorldContextObject, const FString& DisplayName, bool bReplaceExistingGuest);

	/** Signs in with this device's guest secret (proof_invalid when no guest has it any more). */
	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Auth", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Sign In With Device (SplatterVault)"))
	static USvSignInAsyncAction* SignInWithDevice(UObject* WorldContextObject);

	/** Signs in with the emailed code (makes the player when the game lets players sign up). New Password sets or resets the password. */
	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Auth", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", AdvancedDisplay = "NewPassword,DisplayName", DisplayName = "Verify Email Code (SplatterVault)"))
	static USvSignInAsyncAction* VerifyEmailCode(UObject* WorldContextObject, const FString& Email, const FString& Code, const FString& NewPassword, const FString& DisplayName);

	/** Signs in with an email and password (a wrong pair is always invalid_credentials). */
	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Auth", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Sign In With Password (SplatterVault)"))
	static USvSignInAsyncAction* SignInWithPassword(UObject* WorldContextObject, const FString& Email, const FString& Password);

	/** Meta Quest: User Id from Get Logged In User and Nonce from Get User Proof (Oculus/Meta Platform SDK). A nonce works once. */
	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Auth", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", AdvancedDisplay = "AppId", DisplayName = "Sign In With Meta (SplatterVault)"))
	static USvSignInAsyncAction* SignInWithMeta(UObject* WorldContextObject, const FString& UserId, const FString& Nonce, const FString& AppId);

	/** Steam: the hex ticket of GetAuthTicketForWebApi("splattervault"). A ticket works once. */
	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Auth", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", AdvancedDisplay = "AppId", DisplayName = "Sign In With Steam (SplatterVault)"))
	static USvSignInAsyncAction* SignInWithSteam(UObject* WorldContextObject, const FString& TicketHex, const FString& AppId);

	/** Steam, with the ticket bytes of GetTicketForWebApiResponse_t. A ticket works once. */
	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Auth", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", AdvancedDisplay = "AppId", DisplayName = "Sign In With Steam Ticket Bytes (SplatterVault)"))
	static USvSignInAsyncAction* SignInWithSteamTicket(UObject* WorldContextObject, const TArray<uint8>& Ticket, const FString& AppId);

	/** Xsolla: the player's Xsolla Login token (App Id: the Login project id). */
	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Auth", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", AdvancedDisplay = "AppId", DisplayName = "Sign In With Xsolla (SplatterVault)"))
	static USvSignInAsyncAction* SignInWithXsolla(UObject* WorldContextObject, const FString& Token, const FString& AppId);

	/** PlayFab: the SessionTicket of the player's PlayFab login (App Id: the title id). */
	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Auth", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", AdvancedDisplay = "AppId", DisplayName = "Sign In With PlayFab (SplatterVault)"))
	static USvSignInAsyncAction* SignInWithPlayFab(UObject* WorldContextObject, const FString& SessionTicket, const FString& AppId);

	/** Supabase: the player's access token (App Id: the project ref). */
	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Auth", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", AdvancedDisplay = "AppId", DisplayName = "Sign In With Supabase (SplatterVault)"))
	static USvSignInAsyncAction* SignInWithSupabase(UObject* WorldContextObject, const FString& AccessToken, const FString& AppId);

	/** Renews an email sign-in now (the SDK also does it by itself). */
	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Auth", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Refresh Sign In (SplatterVault)"))
	static USvSignInAsyncAction* RefreshSignIn(UObject* WorldContextObject);

protected:
	virtual void Start(FSplatterVaultClient& Client) override;
	virtual void FailNow(const FSvError& Error) override;

private:
	uint8 Kind = 0;
	FString A1, A2, A3, A4;
	bool bFlag = false;
};

// ─── the player ───────────────────────────────────────────────────────────────────────────────────────────────────

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FSvProfilePin, const FSvPlayerProfile&, Profile, const FSvError&, Error);

UCLASS()
class SPLATTERVAULT_API USvProfileAsyncAction : public USvAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FSvProfilePin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FSvProfilePin OnFailure;

	/** The signed-in player, their game and their ways to sign in. */
	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Auth", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Get Me (SplatterVault)"))
	static USvProfileAsyncAction* GetMe(UObject* WorldContextObject);

	/** Uses a player token your backend issued (POST /player-auth/studio with its org key, on your server). */
	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Auth", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", AdvancedDisplay = "ExpiresAt", DisplayName = "Sign In With Studio Token (SplatterVault)"))
	static USvProfileAsyncAction* SignInWithStudioToken(UObject* WorldContextObject, const FString& PlayerToken, const FString& ExpiresAt);

	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Auth", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", AdvancedDisplay = "AppId", DisplayName = "Link Meta (SplatterVault)"))
	static USvProfileAsyncAction* LinkMeta(UObject* WorldContextObject, const FString& UserId, const FString& Nonce, const FString& AppId);

	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Auth", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", AdvancedDisplay = "AppId", DisplayName = "Link Steam (SplatterVault)"))
	static USvProfileAsyncAction* LinkSteam(UObject* WorldContextObject, const FString& TicketHex, const FString& AppId);

	/** Links Steam with the ticket bytes of GetTicketForWebApiResponse_t. */
	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Auth", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", AdvancedDisplay = "AppId", DisplayName = "Link Steam Ticket Bytes (SplatterVault)"))
	static USvProfileAsyncAction* LinkSteamTicket(UObject* WorldContextObject, const TArray<uint8>& Ticket, const FString& AppId);

	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Auth", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", AdvancedDisplay = "AppId", DisplayName = "Link Xsolla (SplatterVault)"))
	static USvProfileAsyncAction* LinkXsolla(UObject* WorldContextObject, const FString& Token, const FString& AppId);

	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Auth", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", AdvancedDisplay = "AppId", DisplayName = "Link PlayFab (SplatterVault)"))
	static USvProfileAsyncAction* LinkPlayFab(UObject* WorldContextObject, const FString& SessionTicket, const FString& AppId);

	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Auth", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", AdvancedDisplay = "AppId", DisplayName = "Link Supabase (SplatterVault)"))
	static USvProfileAsyncAction* LinkSupabase(UObject* WorldContextObject, const FString& AccessToken, const FString& AppId);

	/** Unlinks xsolla, meta, steam, playfab, supabase or device (last_identity: the player's only way in). */
	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Auth", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Unlink (SplatterVault)"))
	static USvProfileAsyncAction* Unlink(UObject* WorldContextObject, const FString& Provider);

	/** Adds a verified email (Request Link Email Code first) to the signed-in player, with an optional password. */
	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Auth", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", AdvancedDisplay = "Password", DisplayName = "Add Email (SplatterVault)"))
	static USvProfileAsyncAction* AddEmail(UObject* WorldContextObject, const FString& Email, const FString& Code, const FString& Password);

	/** Changes the email: Code from the new address, plus Current Password or Old Code. */
	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Auth", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Change Email (SplatterVault)"))
	static USvProfileAsyncAction* ChangeEmail(UObject* WorldContextObject, const FString& NewEmail, const FString& Code, const FString& CurrentPassword, const FString& OldCode);

	/** Removes the email: Current Password, or Old Code from Request Email Removal Code. */
	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Auth", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Remove Email (SplatterVault)"))
	static USvProfileAsyncAction* RemoveEmail(UObject* WorldContextObject, const FString& CurrentPassword, const FString& OldCode);

protected:
	virtual void Start(FSplatterVaultClient& Client) override;
	virtual void FailNow(const FSvError& Error) override;

private:
	uint8 Kind = 0;
	FString A1, A2, A3, A4;
};

// ─── email codes ──────────────────────────────────────────────────────────────────────────────────────────────────

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FSvCodeSentPin, const FSvCodeSent&, Sent, const FSvError&, Error);

UCLASS()
class SPLATTERVAULT_API USvCodeSentAsyncAction : public USvAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FSvCodeSentPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FSvCodeSentPin OnFailure;

	/** Emails a one-time code (sign in, sign up, reset the password). At most one a minute per address. */
	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Auth", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Request Email Code (SplatterVault)"))
	static USvCodeSentAsyncAction* RequestEmailCode(UObject* WorldContextObject, const FString& Email);

	/** Emails a code to an address to add it to the signed-in player (or change to it). */
	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Auth", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Request Link Email Code (SplatterVault)"))
	static USvCodeSentAsyncAction* RequestLinkEmailCode(UObject* WorldContextObject, const FString& Email);

	/** Emails the code Remove Email needs to the current address of an account without a password. */
	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Auth", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Request Email Removal Code (SplatterVault)"))
	static USvCodeSentAsyncAction* RequestEmailRemovalCode(UObject* WorldContextObject);

protected:
	virtual void Start(FSplatterVaultClient& Client) override;
	virtual void FailNow(const FSvError& Error) override;

private:
	uint8 Kind = 0;
	FString A1;
};

// ─── yes / no ─────────────────────────────────────────────────────────────────────────────────────────────────────

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FSvBoolPin, bool, Value, const FSvError&, Error);

UCLASS()
class SPLATTERVAULT_API USvBoolAsyncAction : public USvAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FSvBoolPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FSvBoolPin OnFailure;

	/** Signs the player back in the way they signed in last on this device (email or guest). Value: signed in. Offline is a failure, never "signed out". */
	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Auth", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Restore Sign In (SplatterVault)"))
	static USvBoolAsyncAction* RestoreSignIn(UObject* WorldContextObject);

	/** Signs out on this device (the guest secret stays unless Forget Guest). */
	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Auth", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Sign Out (SplatterVault)"))
	static USvBoolAsyncAction* SignOut(UObject* WorldContextObject, bool bForgetGuest);

	/** Erases the player and everything every studio service keeps about them. Cannot be undone. */
	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Auth", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Erase Player (SplatterVault)"))
	static USvBoolAsyncAction* EraseMe(UObject* WorldContextObject);

	/** Changes the password (signs out the player's other devices). */
	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Auth", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Change Password (SplatterVault)"))
	static USvBoolAsyncAction* ChangePassword(UObject* WorldContextObject, const FString& CurrentPassword, const FString& NewPassword);

protected:
	virtual void Start(FSplatterVaultClient& Client) override;
	virtual void FailNow(const FSvError& Error) override;

private:
	uint8 Kind = 0;
	FString A1, A2;
	bool bFlag = false;
};

// ─── player data ──────────────────────────────────────────────────────────────────────────────────────────────────

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FSvPlayerDataPin, const FSvPlayerData&, Data, const FSvError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FSvDataWritePin, const FSvDataWriteResult&, Result, const FSvError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FSvGameDataPin, const FSvGameData&, GameData, const FSvError&, Error);

UCLASS()
class SPLATTERVAULT_API USvPlayerDataAsyncAction : public USvAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FSvPlayerDataPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FSvPlayerDataPin OnFailure;

	/** The signed-in player's player and readonly stores: every key, or only Keys (at most 100). */
	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Data", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", AutoCreateRefTerm = "Keys", AdvancedDisplay = "bKeysOnly", DisplayName = "Get My Data (SplatterVault)"))
	static USvPlayerDataAsyncAction* GetMyData(UObject* WorldContextObject, const TArray<FString>& Keys, bool bKeysOnly);

protected:
	virtual void Start(FSplatterVaultClient& Client) override;
	virtual void FailNow(const FSvError& Error) override;

private:
	TArray<FString> Keys;
	bool bKeysOnly = false;
};

UCLASS()
class SPLATTERVAULT_API USvDataWriteAsyncAction : public USvAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FSvDataWritePin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FSvDataWritePin OnFailure;

	/**
	 * Applies Ops (Make Set / Delete / Inc Data Op) to the player's store, all or nothing. version_conflict: a key was
	 * not at its If Version (Get Version Conflicts reads the current versions).
	 */
	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Data", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Write My Data (SplatterVault)"))
	static USvDataWriteAsyncAction* WriteMyData(UObject* WorldContextObject, const TArray<FSvDataOp>& Ops);

protected:
	virtual void Start(FSplatterVaultClient& Client) override;
	virtual void FailNow(const FSvError& Error) override;

private:
	TArray<FSvDataOp> Ops;
};

UCLASS()
class SPLATTERVAULT_API USvGameDataAsyncAction : public USvAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FSvGameDataPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FSvGameDataPin OnFailure;

	/** The game's public data, as the signed-in player (ETag-cached: Not Modified when nothing changed). */
	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Data", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", AutoCreateRefTerm = "Keys", DisplayName = "Get Game Data (SplatterVault)"))
	static USvGameDataAsyncAction* GetGameData(UObject* WorldContextObject, const TArray<FString>& Keys);

	/** A game's public data with no sign-in (Game Id Or Key empty: this game). */
	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Data", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", AutoCreateRefTerm = "Keys", AdvancedDisplay = "GameIdOrKey", DisplayName = "Get Public Game Data (SplatterVault)"))
	static USvGameDataAsyncAction* GetPublicGameData(UObject* WorldContextObject, const FString& GameIdOrKey, const TArray<FString>& Keys);

protected:
	virtual void Start(FSplatterVaultClient& Client) override;
	virtual void FailNow(const FSvError& Error) override;

private:
	bool bPublic = false;
	FString Game;
	TArray<FString> Keys;
};

// ─── store ────────────────────────────────────────────────────────────────────────────────────────────────────────

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FSvCatalogPin, const FSvStoreCatalog&, Catalog, const FSvError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FSvHoldingsPin, const FSvStoreHoldings&, Holdings, const FSvError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FSvTxnPin, const FSvStoreTxnResult&, Result, const FSvError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FSvHistoryPin, const FSvStoreHistoryPage&, Page, const FSvError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FSvMetaSyncPin, const FSvMetaSyncResult&, Result, const FSvError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FSvResumePin, const FSvResumeResult&, Result, const FSvError&, Error);

UCLASS()
class SPLATTERVAULT_API USvCatalogAsyncAction : public USvAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FSvCatalogPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FSvCatalogPin OnFailure;

	/** Currencies and items with prices, and what this player may buy or use up (ETag-cached). */
	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Store", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Get Catalog (SplatterVault)"))
	static USvCatalogAsyncAction* GetCatalog(UObject* WorldContextObject);

protected:
	virtual void Start(FSplatterVaultClient& Client) override;
	virtual void FailNow(const FSvError& Error) override;
};

UCLASS()
class SPLATTERVAULT_API USvHoldingsAsyncAction : public USvAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FSvHoldingsPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FSvHoldingsPin OnFailure;

	/** The player's balances and items. */
	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Store", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Get Holdings (SplatterVault)"))
	static USvHoldingsAsyncAction* GetHoldings(UObject* WorldContextObject);

protected:
	virtual void Start(FSplatterVaultClient& Client) override;
	virtual void FailNow(const FSvError& Error) override;
};

UCLASS()
class SPLATTERVAULT_API USvStoreTxnAsyncAction : public USvAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FSvTxnPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FSvTxnPin OnFailure;

	/**
	 * Buys Quantity (1-100) of Sku with Currency at Expected Price (the unit price the player saw: price_changed
	 * otherwise). Sent with an Idempotency-Key (yours, or a new one) that is kept until the API answers, so a lost
	 * answer is finished later with the same key.
	 */
	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Store", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", AdvancedDisplay = "IdempotencyKey", DisplayName = "Purchase (SplatterVault)"))
	static USvStoreTxnAsyncAction* Purchase(UObject* WorldContextObject, const FString& Sku, const FString& Currency, int64 ExpectedPrice, int32 Quantity = 1,
		const FString& IdempotencyKey = TEXT(""));

	/** Uses up a held item: a one-of-a-kind copy, or Quantity of a stack (0: all of a one-of-a-kind item). */
	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Store", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", AdvancedDisplay = "IdempotencyKey", DisplayName = "Consume Item (SplatterVault)"))
	static USvStoreTxnAsyncAction* ConsumeInstance(UObject* WorldContextObject, const FString& InstanceId, int64 Quantity, const FString& IdempotencyKey = TEXT(""));

	/** Uses up Quantity of Sku, oldest first. */
	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Store", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", AdvancedDisplay = "IdempotencyKey", DisplayName = "Consume Sku (SplatterVault)"))
	static USvStoreTxnAsyncAction* ConsumeSku(UObject* WorldContextObject, const FString& Sku, int64 Quantity, const FString& IdempotencyKey = TEXT(""));

protected:
	virtual void Start(FSplatterVaultClient& Client) override;
	virtual void FailNow(const FSvError& Error) override;

private:
	uint8 Kind = 0;
	FString A1, A2, Key;
	int64 Number = 0;
	int32 Quantity = 1;
};

UCLASS()
class SPLATTERVAULT_API USvHistoryAsyncAction : public USvAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FSvHistoryPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FSvHistoryPin OnFailure;

	/** The player's store history, newest first, 50 a page; pass the previous page's Next as Before. */
	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Store", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Get Store History (SplatterVault)"))
	static USvHistoryAsyncAction* GetHistory(UObject* WorldContextObject, const FString& Before);

protected:
	virtual void Start(FSplatterVaultClient& Client) override;
	virtual void FailNow(const FSvError& Error) override;

private:
	FString Before;
};

UCLASS()
class SPLATTERVAULT_API USvMetaSyncAsyncAction : public USvAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FSvMetaSyncPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FSvMetaSyncPin OnFailure;

	/** Grants the player's Meta Quest purchases (each once). Call after a purchase and at startup; 6 a minute. */
	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Store", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", AdvancedDisplay = "AppId", DisplayName = "Sync Meta Purchases (SplatterVault)"))
	static USvMetaSyncAsyncAction* SyncMetaPurchases(UObject* WorldContextObject, const FString& AppId);

protected:
	virtual void Start(FSplatterVaultClient& Client) override;
	virtual void FailNow(const FSvError& Error) override;

private:
	FString AppId;
};

UCLASS()
class SPLATTERVAULT_API USvResumeAsyncAction : public USvAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FSvResumePin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FSvResumePin OnFailure;

	/** Sends again the signed-in player's store changes that got no answer (each with its own Idempotency-Key). */
	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Store", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Resume Pending Store Changes (SplatterVault)"))
	static USvResumeAsyncAction* ResumePendingStoreChanges(UObject* WorldContextObject);

protected:
	virtual void Start(FSplatterVaultClient& Client) override;
	virtual void FailNow(const FSvError& Error) override;
};
