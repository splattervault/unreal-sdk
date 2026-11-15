// Copyright (c) SplatterVault. Licensed under the MIT License.

#include "SplatterVaultSubsystem.h"

#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "SplatterVaultClient.h"
#include "SplatterVaultModule.h"
#include "SplatterVaultSettings.h"

void USplatterVaultSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	const USplatterVaultSettings* Settings = GetDefault<USplatterVaultSettings>();
#if WITH_EDITOR
	if (GIsEditor && !Settings->bEnableInEditor)
	{
		ConfigError = TEXT("SplatterVault is turned off in the editor (Project Settings > Plugins > SplatterVault > Enable In Editor).");
		return;
	}
#endif
	if (Settings->GameId == 0 && Settings->GameKey.IsEmpty())
	{
		ConfigError = TEXT("Set your Game Id in Project Settings > Plugins > SplatterVault (or call Configure).");
		return;
	}
	FString Error;
	if (!ConfigureFromSettings(Error))
	{
		UE_LOG(LogSplatterVault, Warning, TEXT("[SplatterVault] Not configured: %s"), *Error);
	}
}

void USplatterVaultSubsystem::Deinitialize()
{
	DropClient(false);
	Super::Deinitialize();
}

bool USplatterVaultSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	// Game clients only: a dedicated server has no player to sign in (and must never hold player secrets).
	return !IsRunningDedicatedServer() && Super::ShouldCreateSubsystem(Outer);
}

FString USplatterVaultSubsystem::StorageNamespaceFor(const FString& NormalizedBaseUrl, int32 GameId, const FString& GameKey, int32 PieInstance,
	const TCHAR* CommandLine)
{
	FString Namespace = FSplatterVaultClient::DefaultStorageNamespace(NormalizedBaseUrl, GameId, GameKey);
	// A second Play In Editor window is another client: its own secrets, or two clients would rotate one refresh
	// token (ending the sign-in) and make two guests over one device secret.
	if (PieInstance > 0) Namespace += FString::Printf(TEXT("|pie:%d"), PieInstance);
	// Two game processes on one machine: give each its own store with -SvStorageNamespace=<name>.
	FString Extra;
	if (CommandLine && FParse::Value(CommandLine, TEXT("SvStorageNamespace="), Extra) && !Extra.IsEmpty()) Namespace += TEXT("|") + Extra;
	return Namespace;
}

USplatterVaultSubsystem* USplatterVaultSubsystem::Get(const UObject* WorldContext)
{
	if (!WorldContext || !GEngine) return nullptr;
	const UWorld* World = GEngine->GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::ReturnNull);
	const UGameInstance* GameInstance = World ? World->GetGameInstance() : Cast<UGameInstance>(WorldContext);
	return GameInstance ? GameInstance->GetSubsystem<USplatterVaultSubsystem>() : nullptr;
}

TSharedPtr<FSplatterVaultClient> USplatterVaultSubsystem::GetClientFor(const UObject* WorldContext)
{
	const USplatterVaultSubsystem* Subsystem = Get(WorldContext);
	return Subsystem ? Subsystem->GetClient() : TSharedPtr<FSplatterVaultClient>();
}

bool USplatterVaultSubsystem::ConfigureFromSettings(FString& OutError)
{
	const USplatterVaultSettings* S = GetDefault<USplatterVaultSettings>();
	const bool bOk = CreateClient(S->GameId, S->GameId != 0 ? FString() : S->GameKey, S->BaseUrlOverride, S->bAllowInsecureHttpForDevelopment, S->RequestTimeoutSeconds,
		S->MaxAttempts, OutError);
	if (bOk && S->bRestoreOnStartup) StartRestore();
	return bOk;
}

bool USplatterVaultSubsystem::Configure(int32 GameId, const FString& GameKey, const FString& BaseUrl, FString& OutError)
{
	const USplatterVaultSettings* S = GetDefault<USplatterVaultSettings>();
	return CreateClient(GameId, GameId != 0 ? FString() : GameKey, BaseUrl, S->bAllowInsecureHttpForDevelopment, S->RequestTimeoutSeconds, S->MaxAttempts, OutError);
}

bool USplatterVaultSubsystem::CreateClient(int32 GameId, const FString& GameKey, const FString& BaseUrl, bool bAllowInsecureHttpForDevelopment, double Timeout,
	int32 MaxAttempts, FString& OutError)
{
	// Configuring the same game and API again keeps the client: dropping it during a refresh would lose the sign-in
	// (the old refresh token is spent once the API reads it).
	FString UrlError;
	const FString Url = FSplatterVaultClient::CheckBaseUrl(BaseUrl, bAllowInsecureHttpForDevelopment, UrlError);
	if (Client.IsValid() && !Url.IsEmpty() && Client->GetGameId() == GameId && Client->GetGameKey().Equals(GameKey, ESearchCase::CaseSensitive) &&
		Client->GetBaseUrl().Equals(Url, ESearchCase::CaseSensitive))
	{
		ConfigError.Reset();
		return true;
	}
	DropClient(true);
	FSvClientOptions Options;
	Options.GameId = GameId;
	Options.GameKey = GameKey;
	Options.BaseUrl = BaseUrl;
	Options.bAllowInsecureHttpForDevelopment = bAllowInsecureHttpForDevelopment;
	Options.RequestTimeoutSeconds = Timeout; // validated by FSplatterVaultClient::Create (refused, not fixed)
	Options.RetryPolicy.MaxAttempts = MaxAttempts;
	if (!Url.IsEmpty())
	{
		int32 PieInstance = INDEX_NONE;
#if WITH_EDITOR
		const UGameInstance* GameInstance = GetGameInstance();
		const FWorldContext* Context = GameInstance ? GameInstance->GetWorldContext() : nullptr;
		if (Context) PieInstance = Context->PIEInstance;
#endif
		Options.StorageNamespace = StorageNamespaceFor(Url, GameId, GameKey, PieInstance, FCommandLine::Get());
	}
	Client = FSplatterVaultClient::Create(Options, OutError);
	if (!Client.IsValid())
	{
		ConfigError = OutError;
		return false;
	}
	ConfigError.Reset();

	TWeakObjectPtr<USplatterVaultSubsystem> Weak(this);
	FSvAuth& Auth = Client->Auth();
	SignedInHandle = Auth.OnSignedIn.AddLambda([Weak](const FSvSignInResult& R)
	{
		if (USplatterVaultSubsystem* Self = Weak.Get())
		{
			Self->OnSignedIn.Broadcast(R);
			if (GetDefault<USplatterVaultSettings>()->bResumePendingAfterSignIn) Self->ResumePending();
		}
	});
	Auth.OnSignedOut.AddLambda([Weak]()
	{
		if (USplatterVaultSubsystem* Self = Weak.Get()) Self->OnSignedOut.Broadcast();
	});
	Auth.OnSignInRequired.AddLambda([Weak](const FSvSignInRequired& Info)
	{
		if (USplatterVaultSubsystem* Self = Weak.Get()) Self->OnSignInRequired.Broadcast(Info);
	});
	Auth.OnStorageFailed.AddLambda([Weak](const FSvStorageFailure& F)
	{
		if (USplatterVaultSubsystem* Self = Weak.Get()) Self->OnStorageFailed.Broadcast(F);
	});
	return true;
}

void USplatterVaultSubsystem::DropClient(bool bReleaseNextTick)
{
	if (!Client.IsValid()) return;
	FSvAuth& Auth = Client->Auth();
	Auth.OnSignedIn.Clear();
	Auth.OnSignedOut.Clear();
	Auth.OnSignInRequired.Clear();
	Auth.OnStorageFailed.Clear();
	Client->Shutdown();
	if (bReleaseNextTick)
	{
		// Configure can be called from inside one of the client's own events: the client is released after it returns.
		TSharedPtr<FSplatterVaultClient> Old = MoveTemp(Client);
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Old](float) mutable
		{
			Old.Reset();
			return false;
		}));
	}
	Client.Reset();
}

void USplatterVaultSubsystem::StartRestore()
{
	if (!Client.IsValid()) return;
	bRestoreFinished = false;
	TWeakObjectPtr<USplatterVaultSubsystem> Weak(this);
	Client->Auth().Restore().Next([Weak](const TSvResult<bool>& R)
	{
		USplatterVaultSubsystem* Self = Weak.Get();
		if (!Self) return;
		Self->bRestoreFinished = true;
		if (R.IsOk()) Self->OnRestoreFinished.Broadcast(R.GetValue(), FSvError());
		else Self->OnRestoreFinished.Broadcast(false, R.GetError());
	});
}

void USplatterVaultSubsystem::ResumePending()
{
	if (!Client.IsValid() || bResuming) return;
	const TSvResult<TArray<FSvPendingStoreChange>> Pending = Client->Store().GetPending();
	if (Pending.IsError() || Pending.GetValue().Num() == 0) return;
	bResuming = true;
	TWeakObjectPtr<USplatterVaultSubsystem> Weak(this);
	Client->Store().ResumePending().Next([Weak](const TSvResult<FSvResumeResult>& R)
	{
		USplatterVaultSubsystem* Self = Weak.Get();
		if (!Self) return;
		Self->bResuming = false;
		if (R.IsOk() && (R.GetValue().Completed.Num() > 0 || R.GetValue().Refused.Num() > 0)) Self->OnPendingStoreChangesResumed.Broadcast(R.GetValue());
	});
}

bool USplatterVaultSubsystem::IsConfigured() const
{
	return Client.IsValid();
}

bool USplatterVaultSubsystem::IsSignedIn() const
{
	return Client.IsValid() && Client->Auth().IsSignedIn();
}

FString USplatterVaultSubsystem::GetPlayerId() const
{
	return Client.IsValid() ? Client->Auth().GetPlayerId() : FString();
}

FSvSignedInPlayer USplatterVaultSubsystem::GetPlayer() const
{
	return Client.IsValid() ? Client->Auth().GetPlayer() : FSvSignedInPlayer();
}

FString USplatterVaultSubsystem::GetSignInMethod() const
{
	return Client.IsValid() ? Client->Auth().GetMethod() : FString();
}

TArray<FSvIdentity> USplatterVaultSubsystem::GetIdentities() const
{
	return Client.IsValid() ? Client->Auth().GetIdentities() : TArray<FSvIdentity>();
}

FString USplatterVaultSubsystem::GetPlayerToken() const
{
	return Client.IsValid() ? Client->Auth().GetPlayerToken() : FString();
}

FString USplatterVaultSubsystem::GetLastSignInMethod()
{
	if (!Client.IsValid()) return FString();
	const TSvResult<FString> R = Client->Auth().GetLastSignInMethod();
	return R.IsOk() ? R.GetValue() : FString();
}

bool USplatterVaultSubsystem::HasGuestOnDevice()
{
	if (!Client.IsValid()) return false;
	const TSvResult<bool> R = Client->Auth().HasGuestOnDevice();
	return R.IsOk() && R.GetValue();
}

bool USplatterVaultSubsystem::ForgetGuestOnDevice()
{
	return Client.IsValid() && Client->Auth().ForgetGuestOnDevice();
}

bool USplatterVaultSubsystem::RetrySavingSecrets()
{
	return Client.IsValid() && Client->Auth().RetrySavingSecrets();
}

FString USplatterVaultSubsystem::GetStorageProtection() const
{
	return Client.IsValid() ? Client->GetSecureStore().GetProtection() : FString();
}

TArray<FSvPendingStoreChange> USplatterVaultSubsystem::GetPendingStoreChanges()
{
	if (!Client.IsValid()) return TArray<FSvPendingStoreChange>();
	const TSvResult<TArray<FSvPendingStoreChange>> R = Client->Store().GetPending();
	return R.IsOk() ? R.GetValue() : TArray<FSvPendingStoreChange>();
}

bool USplatterVaultSubsystem::DiscardPendingStoreChange(const FString& IdempotencyKey)
{
	return Client.IsValid() && Client->Store().DiscardPending(IdempotencyKey);
}
