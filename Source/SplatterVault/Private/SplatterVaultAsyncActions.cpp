// Copyright (c) SplatterVault. Licensed under the MIT License.

#include "SplatterVaultAsyncActions.h"

#include "SplatterVaultClient.h"
#include "SplatterVaultSubsystem.h"

namespace
{
	/** Broadcasts the future's outcome on the action's pins, then lets the action go. */
	template <typename TAction, typename T>
	void Bind(TAction* Action, TSvFuture<T>&& Future)
	{
		TWeakObjectPtr<TAction> Weak(Action);
		Future.Next([Weak](const TSvResult<T>& R)
		{
			TAction* A = Weak.Get();
			if (!A) return;
			if (R.IsOk()) A->OnSuccess.Broadcast(R.GetValue(), FSvError());
			else A->OnFailure.Broadcast(T(), R.GetError());
			A->SetReadyToDestroy();
		});
	}

	/** An operation that answers nothing: the pins carry true / false. */
	template <typename TAction>
	void BindVoid(TAction* Action, TSvFuture<FSvVoid>&& Future)
	{
		TWeakObjectPtr<TAction> Weak(Action);
		Future.Next([Weak](const TSvResult<FSvVoid>& R)
		{
			TAction* A = Weak.Get();
			if (!A) return;
			if (R.IsOk()) A->OnSuccess.Broadcast(true, FSvError());
			else A->OnFailure.Broadcast(false, R.GetError());
			A->SetReadyToDestroy();
		});
	}

	template <typename TAction, typename T>
	void Fail(TAction* Action, const FSvError& Error)
	{
		Action->OnFailure.Broadcast(T(), Error);
		Action->SetReadyToDestroy();
	}
}

// ─── base ─────────────────────────────────────────────────────────────────────────────────────────────────────────

void USvAsyncActionBase::Init(UObject* WorldContextObject)
{
	WorldContext = WorldContextObject;
	RegisterWithGameInstance(WorldContextObject);
}

void USvAsyncActionBase::Activate()
{
	USplatterVaultSubsystem* Subsystem = USplatterVaultSubsystem::Get(WorldContext.Get());
	const TSharedPtr<FSplatterVaultClient> Client = Subsystem ? Subsystem->GetClient() : nullptr;
	if (!Client.IsValid())
	{
		const FString Why = Subsystem ? Subsystem->GetConfigurationError() : FString(TEXT("No game instance: SplatterVault nodes run in a game (or Play In Editor)."));
		FailNow(FSvError(0, SvErrorCodes::NotConfigured, Why.IsEmpty() ? FString(TEXT("SplatterVault is not configured.")) : Why));
		return;
	}
	Start(*Client);
}

// ─── sign-in ──────────────────────────────────────────────────────────────────────────────────────────────────────

namespace
{
	enum class ESignIn : uint8
	{
		Guest,
		CreateGuest,
		Device,
		EmailCode,
		Password,
		Meta,
		Steam,
		Xsolla,
		PlayFab,
		Supabase,
		Refresh
	};
}

#define SV_SIGNIN_ACTION(KindValue, ...) \
	USvSignInAsyncAction* Action = NewObject<USvSignInAsyncAction>(); \
	Action->Init(WorldContextObject); \
	Action->Kind = (uint8)(KindValue); \
	__VA_ARGS__; \
	return Action;

USvSignInAsyncAction* USvSignInAsyncAction::SignInAsGuest(UObject* WorldContextObject, const FString& DisplayName)
{
	SV_SIGNIN_ACTION(ESignIn::Guest, Action->A1 = DisplayName)
}

USvSignInAsyncAction* USvSignInAsyncAction::CreateGuest(UObject* WorldContextObject, const FString& DisplayName, bool bReplaceExistingGuest)
{
	SV_SIGNIN_ACTION(ESignIn::CreateGuest, Action->A1 = DisplayName, Action->bFlag = bReplaceExistingGuest)
}

USvSignInAsyncAction* USvSignInAsyncAction::SignInWithDevice(UObject* WorldContextObject)
{
	SV_SIGNIN_ACTION(ESignIn::Device, (void)0)
}

USvSignInAsyncAction* USvSignInAsyncAction::VerifyEmailCode(UObject* WorldContextObject, const FString& Email, const FString& Code, const FString& NewPassword,
	const FString& DisplayName)
{
	SV_SIGNIN_ACTION(ESignIn::EmailCode, Action->A1 = Email, Action->A2 = Code, Action->A3 = NewPassword, Action->A4 = DisplayName)
}

USvSignInAsyncAction* USvSignInAsyncAction::SignInWithPassword(UObject* WorldContextObject, const FString& Email, const FString& Password)
{
	SV_SIGNIN_ACTION(ESignIn::Password, Action->A1 = Email, Action->A2 = Password)
}

USvSignInAsyncAction* USvSignInAsyncAction::SignInWithMeta(UObject* WorldContextObject, const FString& UserId, const FString& Nonce, const FString& AppId)
{
	SV_SIGNIN_ACTION(ESignIn::Meta, Action->A1 = UserId, Action->A2 = Nonce, Action->A3 = AppId)
}

USvSignInAsyncAction* USvSignInAsyncAction::SignInWithSteam(UObject* WorldContextObject, const FString& TicketHex, const FString& AppId)
{
	SV_SIGNIN_ACTION(ESignIn::Steam, Action->A1 = TicketHex, Action->A3 = AppId)
}

USvSignInAsyncAction* USvSignInAsyncAction::SignInWithSteamTicket(UObject* WorldContextObject, const TArray<uint8>& Ticket, const FString& AppId)
{
	SV_SIGNIN_ACTION(ESignIn::Steam, Action->A1 = FSvAuth::ToHex(Ticket), Action->A3 = AppId)
}

USvSignInAsyncAction* USvSignInAsyncAction::SignInWithXsolla(UObject* WorldContextObject, const FString& Token, const FString& AppId)
{
	SV_SIGNIN_ACTION(ESignIn::Xsolla, Action->A1 = Token, Action->A3 = AppId)
}

USvSignInAsyncAction* USvSignInAsyncAction::SignInWithPlayFab(UObject* WorldContextObject, const FString& SessionTicket, const FString& AppId)
{
	SV_SIGNIN_ACTION(ESignIn::PlayFab, Action->A1 = SessionTicket, Action->A3 = AppId)
}

USvSignInAsyncAction* USvSignInAsyncAction::SignInWithSupabase(UObject* WorldContextObject, const FString& AccessToken, const FString& AppId)
{
	SV_SIGNIN_ACTION(ESignIn::Supabase, Action->A1 = AccessToken, Action->A3 = AppId)
}

USvSignInAsyncAction* USvSignInAsyncAction::RefreshSignIn(UObject* WorldContextObject)
{
	SV_SIGNIN_ACTION(ESignIn::Refresh, (void)0)
}

#undef SV_SIGNIN_ACTION

void USvSignInAsyncAction::Start(FSplatterVaultClient& Client)
{
	FSvAuth& Auth = Client.Auth();
	switch ((ESignIn)Kind)
	{
	case ESignIn::Guest: return Bind(this, Auth.SignInAsGuest(A1));
	case ESignIn::CreateGuest: return Bind(this, Auth.CreateGuest(A1, bFlag));
	case ESignIn::Device: return Bind(this, Auth.SignInWithDevice());
	case ESignIn::EmailCode: return Bind(this, Auth.VerifyEmailCode(A1, A2, A3, A4));
	case ESignIn::Password: return Bind(this, Auth.SignInWithPassword(A1, A2));
	case ESignIn::Meta: return Bind(this, Auth.SignInWithMeta(A1, A2, A3));
	case ESignIn::Steam: return Bind(this, Auth.SignInWithSteam(A1, A3));
	case ESignIn::Xsolla: return Bind(this, Auth.SignInWithXsolla(A1, A3));
	case ESignIn::PlayFab: return Bind(this, Auth.SignInWithPlayFab(A1, A3));
	case ESignIn::Supabase: return Bind(this, Auth.SignInWithSupabase(A1, A3));
	case ESignIn::Refresh:
	default: return Bind(this, Auth.Refresh());
	}
}

void USvSignInAsyncAction::FailNow(const FSvError& Error)
{
	Fail<USvSignInAsyncAction, FSvSignInResult>(this, Error);
}

// ─── the player ───────────────────────────────────────────────────────────────────────────────────────────────────

namespace
{
	enum class EProfile : uint8
	{
		Me,
		Studio,
		LinkMeta,
		LinkSteam,
		LinkXsolla,
		LinkPlayFab,
		LinkSupabase,
		Unlink,
		AddEmail,
		ChangeEmail,
		RemoveEmail
	};
}

#define SV_PROFILE_ACTION(KindValue, ...) \
	USvProfileAsyncAction* Action = NewObject<USvProfileAsyncAction>(); \
	Action->Init(WorldContextObject); \
	Action->Kind = (uint8)(KindValue); \
	__VA_ARGS__; \
	return Action;

USvProfileAsyncAction* USvProfileAsyncAction::GetMe(UObject* WorldContextObject)
{
	SV_PROFILE_ACTION(EProfile::Me, (void)0)
}

USvProfileAsyncAction* USvProfileAsyncAction::SignInWithStudioToken(UObject* WorldContextObject, const FString& PlayerToken, const FString& ExpiresAt)
{
	SV_PROFILE_ACTION(EProfile::Studio, Action->A1 = PlayerToken, Action->A2 = ExpiresAt)
}

USvProfileAsyncAction* USvProfileAsyncAction::LinkMeta(UObject* WorldContextObject, const FString& UserId, const FString& Nonce, const FString& AppId)
{
	SV_PROFILE_ACTION(EProfile::LinkMeta, Action->A1 = UserId, Action->A2 = Nonce, Action->A3 = AppId)
}

USvProfileAsyncAction* USvProfileAsyncAction::LinkSteam(UObject* WorldContextObject, const FString& TicketHex, const FString& AppId)
{
	SV_PROFILE_ACTION(EProfile::LinkSteam, Action->A1 = TicketHex, Action->A3 = AppId)
}

USvProfileAsyncAction* USvProfileAsyncAction::LinkSteamTicket(UObject* WorldContextObject, const TArray<uint8>& Ticket, const FString& AppId)
{
	SV_PROFILE_ACTION(EProfile::LinkSteam, Action->A1 = FSvAuth::ToHex(Ticket), Action->A3 = AppId)
}

USvProfileAsyncAction* USvProfileAsyncAction::LinkXsolla(UObject* WorldContextObject, const FString& Token, const FString& AppId)
{
	SV_PROFILE_ACTION(EProfile::LinkXsolla, Action->A1 = Token, Action->A3 = AppId)
}

USvProfileAsyncAction* USvProfileAsyncAction::LinkPlayFab(UObject* WorldContextObject, const FString& SessionTicket, const FString& AppId)
{
	SV_PROFILE_ACTION(EProfile::LinkPlayFab, Action->A1 = SessionTicket, Action->A3 = AppId)
}

USvProfileAsyncAction* USvProfileAsyncAction::LinkSupabase(UObject* WorldContextObject, const FString& AccessToken, const FString& AppId)
{
	SV_PROFILE_ACTION(EProfile::LinkSupabase, Action->A1 = AccessToken, Action->A3 = AppId)
}

USvProfileAsyncAction* USvProfileAsyncAction::Unlink(UObject* WorldContextObject, const FString& Provider)
{
	SV_PROFILE_ACTION(EProfile::Unlink, Action->A1 = Provider)
}

USvProfileAsyncAction* USvProfileAsyncAction::AddEmail(UObject* WorldContextObject, const FString& Email, const FString& Code, const FString& Password)
{
	SV_PROFILE_ACTION(EProfile::AddEmail, Action->A1 = Email, Action->A2 = Code, Action->A3 = Password)
}

USvProfileAsyncAction* USvProfileAsyncAction::ChangeEmail(UObject* WorldContextObject, const FString& NewEmail, const FString& Code, const FString& CurrentPassword,
	const FString& OldCode)
{
	SV_PROFILE_ACTION(EProfile::ChangeEmail, Action->A1 = NewEmail, Action->A2 = Code, Action->A3 = CurrentPassword, Action->A4 = OldCode)
}

USvProfileAsyncAction* USvProfileAsyncAction::RemoveEmail(UObject* WorldContextObject, const FString& CurrentPassword, const FString& OldCode)
{
	SV_PROFILE_ACTION(EProfile::RemoveEmail, Action->A1 = CurrentPassword, Action->A2 = OldCode)
}

#undef SV_PROFILE_ACTION

void USvProfileAsyncAction::Start(FSplatterVaultClient& Client)
{
	FSvAuth& Auth = Client.Auth();
	switch ((EProfile)Kind)
	{
	case EProfile::Studio: return Bind(this, Auth.SignInWithStudioToken(A1, A2));
	case EProfile::LinkMeta: return Bind(this, Auth.LinkMeta(A1, A2, A3));
	case EProfile::LinkSteam: return Bind(this, Auth.LinkSteam(A1, A3));
	case EProfile::LinkXsolla: return Bind(this, Auth.LinkXsolla(A1, A3));
	case EProfile::LinkPlayFab: return Bind(this, Auth.LinkPlayFab(A1, A3));
	case EProfile::LinkSupabase: return Bind(this, Auth.LinkSupabase(A1, A3));
	case EProfile::Unlink: return Bind(this, Auth.Unlink(A1));
	case EProfile::AddEmail: return Bind(this, Auth.AddEmail(A1, A2, A3));
	case EProfile::ChangeEmail: return Bind(this, Auth.ChangeEmail(A1, A2, A3, A4));
	case EProfile::RemoveEmail: return Bind(this, Auth.RemoveEmail(A1, A2));
	case EProfile::Me:
	default: return Bind(this, Auth.GetMe());
	}
}

void USvProfileAsyncAction::FailNow(const FSvError& Error)
{
	Fail<USvProfileAsyncAction, FSvPlayerProfile>(this, Error);
}

// ─── email codes ──────────────────────────────────────────────────────────────────────────────────────────────────

USvCodeSentAsyncAction* USvCodeSentAsyncAction::RequestEmailCode(UObject* WorldContextObject, const FString& Email)
{
	USvCodeSentAsyncAction* Action = NewObject<USvCodeSentAsyncAction>();
	Action->Init(WorldContextObject);
	Action->Kind = 0;
	Action->A1 = Email;
	return Action;
}

USvCodeSentAsyncAction* USvCodeSentAsyncAction::RequestLinkEmailCode(UObject* WorldContextObject, const FString& Email)
{
	USvCodeSentAsyncAction* Action = NewObject<USvCodeSentAsyncAction>();
	Action->Init(WorldContextObject);
	Action->Kind = 1;
	Action->A1 = Email;
	return Action;
}

USvCodeSentAsyncAction* USvCodeSentAsyncAction::RequestEmailRemovalCode(UObject* WorldContextObject)
{
	USvCodeSentAsyncAction* Action = NewObject<USvCodeSentAsyncAction>();
	Action->Init(WorldContextObject);
	Action->Kind = 2;
	return Action;
}

void USvCodeSentAsyncAction::Start(FSplatterVaultClient& Client)
{
	if (Kind == 0) return Bind(this, Client.Auth().RequestEmailCode(A1));
	if (Kind == 1) return Bind(this, Client.Auth().RequestLinkEmailCode(A1));
	Bind(this, Client.Auth().RequestEmailRemovalCode());
}

void USvCodeSentAsyncAction::FailNow(const FSvError& Error)
{
	Fail<USvCodeSentAsyncAction, FSvCodeSent>(this, Error);
}

// ─── yes / no ─────────────────────────────────────────────────────────────────────────────────────────────────────

USvBoolAsyncAction* USvBoolAsyncAction::RestoreSignIn(UObject* WorldContextObject)
{
	USvBoolAsyncAction* Action = NewObject<USvBoolAsyncAction>();
	Action->Init(WorldContextObject);
	Action->Kind = 0;
	return Action;
}

USvBoolAsyncAction* USvBoolAsyncAction::SignOut(UObject* WorldContextObject, bool bForgetGuest)
{
	USvBoolAsyncAction* Action = NewObject<USvBoolAsyncAction>();
	Action->Init(WorldContextObject);
	Action->Kind = 1;
	Action->bFlag = bForgetGuest;
	return Action;
}

USvBoolAsyncAction* USvBoolAsyncAction::EraseMe(UObject* WorldContextObject)
{
	USvBoolAsyncAction* Action = NewObject<USvBoolAsyncAction>();
	Action->Init(WorldContextObject);
	Action->Kind = 2;
	return Action;
}

USvBoolAsyncAction* USvBoolAsyncAction::ChangePassword(UObject* WorldContextObject, const FString& CurrentPassword, const FString& NewPassword)
{
	USvBoolAsyncAction* Action = NewObject<USvBoolAsyncAction>();
	Action->Init(WorldContextObject);
	Action->Kind = 3;
	Action->A1 = CurrentPassword;
	Action->A2 = NewPassword;
	return Action;
}

void USvBoolAsyncAction::Start(FSplatterVaultClient& Client)
{
	switch (Kind)
	{
	case 0: return Bind(this, Client.Auth().Restore());
	case 1: return BindVoid(this, Client.Auth().SignOut(bFlag));
	case 2: return BindVoid(this, Client.Auth().EraseMe());
	default: return BindVoid(this, Client.Auth().ChangePassword(A1, A2));
	}
}

void USvBoolAsyncAction::FailNow(const FSvError& Error)
{
	Fail<USvBoolAsyncAction, bool>(this, Error);
}

// ─── player data ──────────────────────────────────────────────────────────────────────────────────────────────────

USvPlayerDataAsyncAction* USvPlayerDataAsyncAction::GetMyData(UObject* WorldContextObject, const TArray<FString>& InKeys, bool bInKeysOnly)
{
	USvPlayerDataAsyncAction* Action = NewObject<USvPlayerDataAsyncAction>();
	Action->Init(WorldContextObject);
	Action->Keys = InKeys;
	Action->bKeysOnly = bInKeysOnly;
	return Action;
}

void USvPlayerDataAsyncAction::Start(FSplatterVaultClient& Client)
{
	Bind(this, Client.Data().GetMine(Keys, bKeysOnly));
}

void USvPlayerDataAsyncAction::FailNow(const FSvError& Error)
{
	Fail<USvPlayerDataAsyncAction, FSvPlayerData>(this, Error);
}

USvDataWriteAsyncAction* USvDataWriteAsyncAction::WriteMyData(UObject* WorldContextObject, const TArray<FSvDataOp>& InOps)
{
	USvDataWriteAsyncAction* Action = NewObject<USvDataWriteAsyncAction>();
	Action->Init(WorldContextObject);
	Action->Ops = InOps;
	return Action;
}

void USvDataWriteAsyncAction::Start(FSplatterVaultClient& Client)
{
	Bind(this, Client.Data().Write(FSvDataOps(Ops)));
}

void USvDataWriteAsyncAction::FailNow(const FSvError& Error)
{
	Fail<USvDataWriteAsyncAction, FSvDataWriteResult>(this, Error);
}

USvGameDataAsyncAction* USvGameDataAsyncAction::GetGameData(UObject* WorldContextObject, const TArray<FString>& InKeys)
{
	USvGameDataAsyncAction* Action = NewObject<USvGameDataAsyncAction>();
	Action->Init(WorldContextObject);
	Action->Keys = InKeys;
	return Action;
}

USvGameDataAsyncAction* USvGameDataAsyncAction::GetPublicGameData(UObject* WorldContextObject, const FString& GameIdOrKey, const TArray<FString>& InKeys)
{
	USvGameDataAsyncAction* Action = NewObject<USvGameDataAsyncAction>();
	Action->Init(WorldContextObject);
	Action->bPublic = true;
	Action->Game = GameIdOrKey;
	Action->Keys = InKeys;
	return Action;
}

void USvGameDataAsyncAction::Start(FSplatterVaultClient& Client)
{
	if (bPublic) return Bind(this, Client.Data().GetPublic(Game, Keys));
	Bind(this, Client.Data().GetGameData(Keys));
}

void USvGameDataAsyncAction::FailNow(const FSvError& Error)
{
	Fail<USvGameDataAsyncAction, FSvGameData>(this, Error);
}

// ─── store ────────────────────────────────────────────────────────────────────────────────────────────────────────

USvCatalogAsyncAction* USvCatalogAsyncAction::GetCatalog(UObject* WorldContextObject)
{
	USvCatalogAsyncAction* Action = NewObject<USvCatalogAsyncAction>();
	Action->Init(WorldContextObject);
	return Action;
}

void USvCatalogAsyncAction::Start(FSplatterVaultClient& Client)
{
	Bind(this, Client.Store().GetCatalog());
}

void USvCatalogAsyncAction::FailNow(const FSvError& Error)
{
	Fail<USvCatalogAsyncAction, FSvStoreCatalog>(this, Error);
}

USvHoldingsAsyncAction* USvHoldingsAsyncAction::GetHoldings(UObject* WorldContextObject)
{
	USvHoldingsAsyncAction* Action = NewObject<USvHoldingsAsyncAction>();
	Action->Init(WorldContextObject);
	return Action;
}

void USvHoldingsAsyncAction::Start(FSplatterVaultClient& Client)
{
	Bind(this, Client.Store().GetHoldings());
}

void USvHoldingsAsyncAction::FailNow(const FSvError& Error)
{
	Fail<USvHoldingsAsyncAction, FSvStoreHoldings>(this, Error);
}

USvStoreTxnAsyncAction* USvStoreTxnAsyncAction::Purchase(UObject* WorldContextObject, const FString& Sku, const FString& Currency, int64 ExpectedPrice, int32 InQuantity,
	const FString& IdempotencyKey)
{
	USvStoreTxnAsyncAction* Action = NewObject<USvStoreTxnAsyncAction>();
	Action->Init(WorldContextObject);
	Action->Kind = 0;
	Action->A1 = Sku;
	Action->A2 = Currency;
	Action->Number = ExpectedPrice;
	Action->Quantity = InQuantity;
	Action->Key = IdempotencyKey;
	return Action;
}

USvStoreTxnAsyncAction* USvStoreTxnAsyncAction::ConsumeInstance(UObject* WorldContextObject, const FString& InstanceId, int64 InQuantity, const FString& IdempotencyKey)
{
	USvStoreTxnAsyncAction* Action = NewObject<USvStoreTxnAsyncAction>();
	Action->Init(WorldContextObject);
	Action->Kind = 1;
	Action->A1 = InstanceId;
	Action->Number = InQuantity;
	Action->Key = IdempotencyKey;
	return Action;
}

USvStoreTxnAsyncAction* USvStoreTxnAsyncAction::ConsumeSku(UObject* WorldContextObject, const FString& Sku, int64 InQuantity, const FString& IdempotencyKey)
{
	USvStoreTxnAsyncAction* Action = NewObject<USvStoreTxnAsyncAction>();
	Action->Init(WorldContextObject);
	Action->Kind = 2;
	Action->A1 = Sku;
	Action->Number = InQuantity;
	Action->Key = IdempotencyKey;
	return Action;
}

void USvStoreTxnAsyncAction::Start(FSplatterVaultClient& Client)
{
	if (Kind == 0) return Bind(this, Client.Store().Purchase(A1, A2, Number, Quantity, Key));
	if (Kind == 1) return Bind(this, Client.Store().ConsumeInstance(A1, Number, Key));
	Bind(this, Client.Store().ConsumeSku(A1, Number, Key));
}

void USvStoreTxnAsyncAction::FailNow(const FSvError& Error)
{
	Fail<USvStoreTxnAsyncAction, FSvStoreTxnResult>(this, Error);
}

USvHistoryAsyncAction* USvHistoryAsyncAction::GetHistory(UObject* WorldContextObject, const FString& InBefore)
{
	USvHistoryAsyncAction* Action = NewObject<USvHistoryAsyncAction>();
	Action->Init(WorldContextObject);
	Action->Before = InBefore;
	return Action;
}

void USvHistoryAsyncAction::Start(FSplatterVaultClient& Client)
{
	Bind(this, Client.Store().GetHistory(Before));
}

void USvHistoryAsyncAction::FailNow(const FSvError& Error)
{
	Fail<USvHistoryAsyncAction, FSvStoreHistoryPage>(this, Error);
}

USvMetaSyncAsyncAction* USvMetaSyncAsyncAction::SyncMetaPurchases(UObject* WorldContextObject, const FString& InAppId)
{
	USvMetaSyncAsyncAction* Action = NewObject<USvMetaSyncAsyncAction>();
	Action->Init(WorldContextObject);
	Action->AppId = InAppId;
	return Action;
}

void USvMetaSyncAsyncAction::Start(FSplatterVaultClient& Client)
{
	Bind(this, Client.Store().SyncMetaPurchases(AppId));
}

void USvMetaSyncAsyncAction::FailNow(const FSvError& Error)
{
	Fail<USvMetaSyncAsyncAction, FSvMetaSyncResult>(this, Error);
}

USvResumeAsyncAction* USvResumeAsyncAction::ResumePendingStoreChanges(UObject* WorldContextObject)
{
	USvResumeAsyncAction* Action = NewObject<USvResumeAsyncAction>();
	Action->Init(WorldContextObject);
	return Action;
}

void USvResumeAsyncAction::Start(FSplatterVaultClient& Client)
{
	Bind(this, Client.Store().ResumePending());
}

void USvResumeAsyncAction::FailNow(const FSvError& Error)
{
	Fail<USvResumeAsyncAction, FSvResumeResult>(this, Error);
}
