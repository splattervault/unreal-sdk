// Copyright (c) SplatterVault. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "SvTypes.h"
#include "SplatterVaultSubsystem.generated.h"

class FSplatterVaultClient;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSvSignedInEvent, const FSvSignInResult&, Result);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FSvSignedOutEvent);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSvSignInRequiredEvent, const FSvSignInRequired&, Info);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSvStorageFailedEvent, const FSvStorageFailure&, Failure);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FSvRestoreFinishedEvent, bool, bSignedIn, const FSvError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSvPendingResumedEvent, const FSvResumeResult&, Result);

/**
 * SplatterVault for one game instance: player sign-in and accounts, player data and the store.
 *
 * Configured from Project Settings > Plugins > SplatterVault (or Configure at runtime). Blueprint: use the async
 * nodes in the SplatterVault category (Sign In As Guest, Get Catalog, Purchase, ...) and the events below. C++: call
 * GetClient() and use FSplatterVaultClient (Auth(), Data(), Store()), whose calls return TFuture<TSvResult<T>>.
 *
 * It never holds an API key: players sign in with a platform proof, an email account or a guest on the device.
 */
UCLASS()
class SPLATTERVAULT_API USplatterVaultSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	/** Not made on a dedicated server. */
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;

	/**
	 * Where this game instance keeps its secrets: the API origin and the game, plus "|pie:N" for a second Play In
	 * Editor window and "|<name>" when the command line has -SvStorageNamespace=<name> (two game processes on one PC).
	 */
	static FString StorageNamespaceFor(const FString& NormalizedBaseUrl, int32 GameId, const FString& GameKey, int32 PieInstance, const TCHAR* CommandLine);

	/** The subsystem of WorldContext's game instance, or null. */
	static USplatterVaultSubsystem* Get(const UObject* WorldContext);

	/**
	 * The client of WorldContext's game instance (any actor, component, widget or the game instance itself), or null
	 * when there is none or the SDK is not configured (GetConfigurationError says why). Capture the result in your
	 * continuations: after the game instance ends, every call on it answers client_shutdown.
	 */
	static TSharedPtr<FSplatterVaultClient> GetClientFor(const UObject* WorldContext);

	/** The C++ client; null when the SDK is not configured (see GetConfigurationError). */
	TSharedPtr<FSplatterVaultClient> GetClient() const { return Client; }

	/**
	 * Configures (or reconfigures) the SDK from code: GameId (preferred) or GameKey, and BaseUrl (empty: the
	 * production API). The same game and API again changes nothing. Another game or API drops the sign-in in memory;
	 * what is stored on the device stays. False and OutError when refused.
	 */
	UFUNCTION(BlueprintCallable, Category = "SplatterVault", meta = (AdvancedDisplay = "BaseUrl"))
	bool Configure(int32 GameId, const FString& GameKey, const FString& BaseUrl, FString& OutError);

	/** Configures from Project Settings (done at startup). */
	bool ConfigureFromSettings(FString& OutError);

	/** The SDK has a game and an API to talk to. */
	UFUNCTION(BlueprintPure, Category = "SplatterVault")
	bool IsConfigured() const;

	/** Why the SDK is not configured, or empty. */
	UFUNCTION(BlueprintPure, Category = "SplatterVault")
	FString GetConfigurationError() const { return ConfigError; }

	/** A player is signed in (their token may still need renewing; the SDK does that by itself). */
	UFUNCTION(BlueprintPure, Category = "SplatterVault|Auth")
	bool IsSignedIn() const;

	/** The SplatterVault player id, or empty. */
	UFUNCTION(BlueprintPure, Category = "SplatterVault|Auth")
	FString GetPlayerId() const;

	/** The signed-in player. */
	UFUNCTION(BlueprintPure, Category = "SplatterVault|Auth")
	FSvSignedInPlayer GetPlayer() const;

	/** How the player signed in: device, email, meta, steam, xsolla, playfab, supabase or studio; empty when nobody is. */
	UFUNCTION(BlueprintPure, Category = "SplatterVault|Auth")
	FString GetSignInMethod() const;

	/** The ways the player can sign in, as last answered. */
	UFUNCTION(BlueprintPure, Category = "SplatterVault|Auth")
	TArray<FSvIdentity> GetIdentities() const;

	/**
	 * The player token, to hand to YOUR game server (it checks it offline with GET /rest/player-auth/keys). Never log
	 * it, print it, or save it.
	 */
	UFUNCTION(BlueprintPure, Category = "SplatterVault|Auth")
	FString GetPlayerToken() const;

	/** How the player last signed in on this device, or empty (nobody, or signed out). Use it at startup to pick a platform sign-in. */
	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Auth")
	FString GetLastSignInMethod();

	/** This device holds a guest secret. */
	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Auth")
	bool HasGuestOnDevice();

	/** Forgets this device's guest secret: the guest is lost unless it has another way to sign in. */
	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Auth")
	bool ForgetGuestOnDevice();

	/** Saves again a refresh token or guest secret that secure storage could not save earlier. */
	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Auth")
	bool RetrySavingSecrets();

	/** How this device's secrets are protected: dpapi, android-keystore, obfuscated or memory. */
	UFUNCTION(BlueprintPure, Category = "SplatterVault|Auth")
	FString GetStorageProtection() const;

	/** The signed-in player's store changes that got no answer yet. */
	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Store")
	TArray<FSvPendingStoreChange> GetPendingStoreChanges();

	/** Forgets one of the signed-in player's unanswered store changes (it may still have happened: check the history). */
	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Store")
	bool DiscardPendingStoreChange(const FString& IdempotencyKey);

	/** A player signed in (any way, renewals included). */
	UPROPERTY(BlueprintAssignable, Category = "SplatterVault|Auth")
	FSvSignedInEvent OnSignedIn;

	/** The player signed out, or the sign-in ended. */
	UPROPERTY(BlueprintAssignable, Category = "SplatterVault|Auth")
	FSvSignedOutEvent OnSignedOut;

	/** The player must sign in again with a fresh proof (a Meta, Steam... token ran out, or the sign-in ended). */
	UPROPERTY(BlueprintAssignable, Category = "SplatterVault|Auth")
	FSvSignInRequiredEvent OnSignInRequired;

	/** Secure storage failed (a new guest may exist in memory only: see Retry Saving Secrets). */
	UPROPERTY(BlueprintAssignable, Category = "SplatterVault|Auth")
	FSvStorageFailedEvent OnStorageFailed;

	/** The startup restore finished (Project Settings: Restore On Startup). Error is set when it could not tell (offline). */
	UPROPERTY(BlueprintAssignable, Category = "SplatterVault|Auth")
	FSvRestoreFinishedEvent OnRestoreFinished;

	/** Store changes left from an earlier run were sent again after a sign-in. */
	UPROPERTY(BlueprintAssignable, Category = "SplatterVault|Store")
	FSvPendingResumedEvent OnPendingStoreChangesResumed;

	/** The startup restore ran and finished. */
	UFUNCTION(BlueprintPure, Category = "SplatterVault|Auth")
	bool HasRestoreFinished() const { return bRestoreFinished; }

private:
	bool CreateClient(int32 GameId, const FString& GameKey, const FString& BaseUrl, bool bAllowInsecureHttpForDevelopment, double Timeout, int32 MaxAttempts,
		FString& OutError);
	/** Shuts the client down; with bReleaseNextTick it is released on the next tick (safe from inside its events). */
	void DropClient(bool bReleaseNextTick);
	void StartRestore();
	void ResumePending();

	TSharedPtr<FSplatterVaultClient> Client;
	FString ConfigError;
	bool bRestoreFinished = false;
	bool bResuming = false;
	FDelegateHandle SignedInHandle;
};
