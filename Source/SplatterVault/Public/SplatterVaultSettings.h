// Copyright (c) SplatterVault. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "SplatterVaultSettings.generated.h"

/**
 * Project Settings > Plugins > SplatterVault. Saved to Config/DefaultGame.ini ([/Script/SplatterVault.SplatterVaultSettings]).
 *
 * Name your game with its Game Id (preferred: it never changes) or its public Game Key. Neither is a secret: both
 * ship in the game. Never put an organization API key here, or anywhere in a game build: it belongs on your servers.
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "SplatterVault"))
class SPLATTERVAULT_API USplatterVaultSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	USplatterVaultSettings();

	virtual FName GetCategoryName() const override;

	/** Your game id: the number on the game console's Sign-in tab. Set this or Game Key, not both. */
	UPROPERTY(Config, EditAnywhere, Category = "Game", meta = (ClampMin = "0"))
	int32 GameId = 0;

	/** The game's public Game Key (shown in the game console). Used only when Game Id is 0. Not a secret, not an API key. */
	UPROPERTY(Config, EditAnywhere, Category = "Game")
	FString GameKey;

	/** Another API base URL, /rest included (a local mock or staging). Empty: https://splattervault.com/rest. */
	UPROPERTY(Config, EditAnywhere, Category = "Connection", AdvancedDisplay)
	FString BaseUrlOverride;

	/**
	 * Allows an http:// base URL that is not a loopback host (127.0.0.1, ::1, localhost), for a development server on
	 * your network. Ignored in Shipping builds. Tokens and secrets travel on it in plain text.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Connection", AdvancedDisplay)
	bool bAllowInsecureHttpForDevelopment = false;

	/** Seconds one HTTP request may take. */
	UPROPERTY(Config, EditAnywhere, Category = "Connection", AdvancedDisplay, meta = (ClampMin = "1", ClampMax = "120"))
	float RequestTimeoutSeconds = 30.f;

	/** Sends in all for a request the SDK may repeat (reads, store changes with their Idempotency-Key). 1 turns retries off. */
	UPROPERTY(Config, EditAnywhere, Category = "Connection", AdvancedDisplay, meta = (ClampMin = "1", ClampMax = "6"))
	int32 MaxAttempts = 3;

	/** Sign the player back in at startup (email or guest sign-ins), and raise On Restore Finished. */
	UPROPERTY(Config, EditAnywhere, Category = "Behaviour")
	bool bRestoreOnStartup = true;

	/**
	 * After every sign-in, send again the store changes that got no answer (same Idempotency-Key). Off by default: a
	 * kept change may be one the API refused for a passing reason and never applied, and resuming applies it then.
	 * Turn it on only if your game expects that, or call Resume Pending Store Changes at a moment the player expects it.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Behaviour")
	bool bResumePendingAfterSignIn = false;

	/**
	 * Android: keep the SDK's Keystore-sealed secrets (files/splattervault/) out of Android Auto Backup and device
	 * transfer. A backup restored to another device cannot open them (the key stays on the old device) and they would
	 * read as storage_unreadable. When your manifest already names its own backup rules, they are left alone: add
	 * <exclude domain="file" path="splattervault/"/> to them.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Behaviour")
	bool bExcludeSecretsFromAndroidBackup = true;

	/** Start the client in the editor's game instances (Play In Editor). */
	UPROPERTY(Config, EditAnywhere, Category = "Behaviour")
	bool bEnableInEditor = true;
};
