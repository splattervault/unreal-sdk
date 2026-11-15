// Copyright (c) SplatterVault. Licensed under the MIT License.

#include "GenericPlatform/GenericPlatformHttp.h"
#include "Misc/Guid.h"
#include "SplatterVaultClient.h"
#include "SvCrypto.h"
#include "SvJson.h"
#include "SvPipeline.h"

namespace
{
	const TCHAR* PendingItem = TEXT("pending_store_changes");

	FSvError BadArgument(const FString& Why)
	{
		return FSvError(0, SvErrorCodes::InvalidArgument, Why);
	}

	FSvError Unreadable(const FSvHttpResponse& R)
	{
		return FSvError(R.Status, SvErrorCodes::InvalidResponse, TEXT("The answer could not be read."));
	}

}

FSvStoreApi::FSvStoreApi(FSplatterVaultClient& InOwner)
	: Owner(InOwner)
{
}

FString FSvStoreApi::PendingKey() const
{
	return Owner.GetStoragePrefix() + TEXT("pending_store_changes");
}

FString FSvStoreApi::NewIdempotencyKey()
{
	TArray<uint8> Bytes;
	if (SvCrypto::RandomBytes(16, Bytes)) return SvCrypto::Hex(Bytes);
	return FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
}

bool FSvStoreApi::IsValidIdempotencyKey(const FString& Key)
{
	if (Key.IsEmpty() || Key.Len() > 128) return false;
	for (const TCHAR C : Key)
	{
		if (C < TEXT('!') || C > TEXT('~')) return false;
	}
	return true;
}

void FSvStoreApi::ClearCatalogCache()
{
	CatalogCache.Reset();
}

TSvFuture<FSvStoreCatalog> FSvStoreApi::GetCatalog()
{
	FSvStoreApi* Self = this;
	return SvAsync<FSvStoreCatalog>([Self](TSvCallback<FSvStoreCatalog> Done)
	{
		// The catalog carries this player's purchasable/consumable flags: kept per player.
		const FString CacheKey = Self->Owner.Auth().GetPlayerId() + TEXT("|/store/catalog");
		const FCachedCatalog* Cached = Self->CatalogCache.Find(CacheKey);
		FSvCall Call = FSvCall::Get(TEXT("/store/catalog"));
		Call.bAllow304 = Cached != nullptr;
		if (Cached && !Cached->ETag.IsEmpty()) Call.Headers.Add(TEXT("If-None-Match"), Cached->ETag);
		Self->Owner.Auth().SendAsPlayer(Call, [Self, CacheKey, Done](const TSvResult<FSvHttpResponse>& R)
		{
			if (R.IsError()) return Done(TSvResult<FSvStoreCatalog>::Fail(R.GetError()));
			const FSvHttpResponse& Response = R.GetValue();
			const FCachedCatalog* Kept = Self->CatalogCache.Find(CacheKey);
			if (Response.Status == 304 && Kept)
			{
				FSvStoreCatalog Same = Kept->Catalog;
				Same.ETag = Kept->ETag;
				Same.NotModified = true;
				return Done(TSvResult<FSvStoreCatalog>::Ok(Same));
			}
			FSvStoreCatalog Catalog;
			if (!SvModels::ReadText(Response.Body, Catalog)) return Done(TSvResult<FSvStoreCatalog>::Fail(Unreadable(Response)));
			Catalog.ETag = Response.GetHeader(TEXT("ETag"));
			Catalog.NotModified = false;
			if (!Catalog.ETag.IsEmpty())
			{
				FCachedCatalog Entry;
				Entry.ETag = Catalog.ETag;
				Entry.Catalog = Catalog;
				Self->CatalogCache.Add(CacheKey, Entry);
			}
			else
			{
				Self->CatalogCache.Remove(CacheKey);
			}
			Done(TSvResult<FSvStoreCatalog>::Ok(Catalog));
		});
	});
}

template <typename T>
void FSvStoreApi::GetModel(FSvAuth& Auth, const FString& Path, TSvCallback<T> Done)
{
	{
		Auth.SendAsPlayer(FSvCall::Get(Path), [Done](const TSvResult<FSvHttpResponse>& R)
		{
			if (R.IsError()) return Done(TSvResult<T>::Fail(R.GetError()));
			T Value;
			if (!SvModels::ReadText(R.GetValue().Body, Value)) return Done(TSvResult<T>::Fail(Unreadable(R.GetValue())));
			Done(TSvResult<T>::Ok(Value));
		});
	}
}

TSvFuture<FSvStoreHoldings> FSvStoreApi::GetHoldings()
{
	FSvAuth* Auth = &Owner.Auth();
	return SvAsync<FSvStoreHoldings>([Auth](TSvCallback<FSvStoreHoldings> Done) { GetModel<FSvStoreHoldings>(*Auth, TEXT("/store/me"), Done); });
}

TSvFuture<FSvStoreHistoryPage> FSvStoreApi::GetHistory(const FString& Before)
{
	const FString Path = TEXT("/store/me/history") + (Before.IsEmpty() ? FString() : TEXT("?before=") + FGenericPlatformHttp::UrlEncode(Before));
	FSvAuth* Auth = &Owner.Auth();
	return SvAsync<FSvStoreHistoryPage>([Auth, Path](TSvCallback<FSvStoreHistoryPage> Done) { GetModel<FSvStoreHistoryPage>(*Auth, Path, Done); });
}

TSvFuture<FSvStoreTxnResult> FSvStoreApi::Purchase(const FString& Sku, const FString& Currency, int64 ExpectedPrice, int32 Quantity, const FString& IdempotencyKey)
{
	if (Sku.IsEmpty()) return SvFailed<FSvStoreTxnResult>(BadArgument(TEXT("Sku is required.")));
	if (Currency.IsEmpty()) return SvFailed<FSvStoreTxnResult>(BadArgument(TEXT("Currency is required.")));
	if (ExpectedPrice < 0) return SvFailed<FSvStoreTxnResult>(BadArgument(TEXT("ExpectedPrice is 0 or more.")));
	if (Quantity < 1 || Quantity > MaxPurchaseQuantity)
	{
		return SvFailed<FSvStoreTxnResult>(BadArgument(FString::Printf(TEXT("Quantity is 1 to %d."), MaxPurchaseQuantity)));
	}
	TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("sku"), Sku);
	Body->SetStringField(TEXT("currency"), Currency);
	Body->SetNumberField(TEXT("quantity"), Quantity);
	Body->SetField(TEXT("expectedPrice"), SvJson::Int(ExpectedPrice));
	return Change(TEXT("purchase"), TEXT("/store/me/purchases"), SvJson::Write(Body), IdempotencyKey);
}

TSvFuture<FSvStoreTxnResult> FSvStoreApi::ConsumeInstance(const FString& InstanceId, int64 Quantity, const FString& IdempotencyKey)
{
	if (InstanceId.IsEmpty()) return SvFailed<FSvStoreTxnResult>(BadArgument(TEXT("InstanceId is required.")));
	if (Quantity < 0) return SvFailed<FSvStoreTxnResult>(BadArgument(TEXT("Quantity is 1 or more (0: all of a one-of-a-kind item).")));
	TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("instanceId"), InstanceId);
	if (Quantity > 0) Body->SetField(TEXT("quantity"), SvJson::Int(Quantity));
	return Change(TEXT("consume"), TEXT("/store/me/consume"), SvJson::Write(Body), IdempotencyKey);
}

TSvFuture<FSvStoreTxnResult> FSvStoreApi::ConsumeSku(const FString& Sku, int64 Quantity, const FString& IdempotencyKey)
{
	if (Sku.IsEmpty()) return SvFailed<FSvStoreTxnResult>(BadArgument(TEXT("Sku is required.")));
	if (Quantity < 1) return SvFailed<FSvStoreTxnResult>(BadArgument(TEXT("Quantity is 1 or more.")));
	TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("sku"), Sku);
	Body->SetField(TEXT("quantity"), SvJson::Int(Quantity));
	return Change(TEXT("consume"), TEXT("/store/me/consume"), SvJson::Write(Body), IdempotencyKey);
}

TSvFuture<FSvMetaSyncResult> FSvStoreApi::SyncMetaPurchases(const FString& AppId)
{
	TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	if (!AppId.IsEmpty()) Body->SetStringField(TEXT("appId"), AppId);
	// Not repeated by the SDK: each check asks Meta and spends one of the player's 6 a minute.
	const FSvCall Call = FSvCall::Post(TEXT("/store/me/platform-purchases/meta"), SvJson::Write(Body), ESvRetry::Never);
	FSvAuth* Auth = &Owner.Auth();
	return SvAsync<FSvMetaSyncResult>([Auth, Call](TSvCallback<FSvMetaSyncResult> Done)
	{
		Auth->SendAsPlayer(Call, [Done](const TSvResult<FSvHttpResponse>& R)
		{
			if (R.IsError()) return Done(TSvResult<FSvMetaSyncResult>::Fail(R.GetError()));
			FSvMetaSyncResult Result;
			if (!SvModels::ReadText(R.GetValue().Body, Result)) return Done(TSvResult<FSvMetaSyncResult>::Fail(Unreadable(R.GetValue())));
			Done(TSvResult<FSvMetaSyncResult>::Ok(Result));
		});
	});
}

namespace
{
	bool SameChange(const FSvPendingStoreChange& P, const FString& Key, const FString& PlayerId)
	{
		return SvSame(P.IdempotencyKey, Key) && SvSame(P.PlayerId, PlayerId);
	}

	FString InFlightId(const FString& PlayerId, const FString& Key)
	{
		return PlayerId + TEXT("\n") + Key;
	}

	/** No answer, or 500 / 502 / 504: the change may or may not have happened (the same key finishes it at most once). */
	bool NoProofEitherWay(const FSvError& E)
	{
		return E.IsNetworkError() || E.Status == 500 || E.Status == 502 || E.Status == 504;
	}
}

TSvFuture<FSvStoreTxnResult> FSvStoreApi::Change(const FString& Kind, const FString& Path, const FString& BodyJson, const FString& InKey)
{
	const TSharedPtr<FSplatterVaultClient> Pinned = SvPin(Owner);
	const FString Key = InKey.IsEmpty() ? NewIdempotencyKey() : InKey;
	// Every store-change error carries the key and whether the change is still kept (here: nothing was kept or sent).
	auto Refuse = [&Key](FSvError E)
	{
		E.IdempotencyKey = Key;
		E.Pending = false;
		return SvFailed<FSvStoreTxnResult>(E);
	};
	auto Different = []()
	{
		return FSvError(0, SvErrorCodes::ChangeInProgress,
			TEXT("Another change is kept or being sent under this Idempotency-Key: nothing was sent. Finish it (ResumePending) or discard it first, or use a new key."));
	};
	if (!IsValidIdempotencyKey(Key)) return Refuse(BadArgument(TEXT("An Idempotency-Key is 1 to 128 visible ASCII characters.")));
	// A shut-down client keeps nothing new: the change is refused (not kept, not sent).
	if (Owner.IsShutdown()) return Refuse(FSvError(0, SvErrorCodes::Shutdown, TEXT("The SplatterVault client was shut down: nothing was sent.")));
	const FString Player = Owner.Auth().GetPlayerId();
	if (Player.IsEmpty()) return Refuse(FSvError(0, SvErrorCodes::NotSignedIn, FString()));
	const FString Request = Path + TEXT("\n") + BodyJson;

	// One send per (player, key) at a time: the same change already being sent is joined (it gets that send's answer);
	// a different request under the same key is refused locally (nothing sent, the kept entry untouched).
	const FString Flight = InFlightId(Player, Key);
	if (const FInFlightChange* Running = InFlight.Find(Flight))
	{
		if (!SvSame(Running->Request, Request)) return Refuse(Different());
		FSvStoreApi* Joiner = this;
		return SvAsyncOr<FSvStoreTxnResult>(DroppedError(Key, true), [Joiner, Flight](TSvCallback<FSvStoreTxnResult> Done)
		{
			if (FInFlightChange* Entry = Joiner->InFlight.Find(Flight)) Entry->Joined.Add(MoveTemp(Done));
		});
	}

	// A change already kept under this key (an earlier send got no answer): the same change finishes it as it was
	// kept; a different one is refused and the kept entry is left exactly as it is.
	FSvPendingStoreChange Pending;
	bool bKept = false;
	TArray<FSvPendingStoreChange> Kept;
	FSvError LoadError;
	if (LoadPending(Kept, LoadError))
	{
		if (const FSvPendingStoreChange* Existing = Kept.FindByPredicate([&](const FSvPendingStoreChange& P) { return SameChange(P, Key, Player); }))
		{
			if (!SvSame(Existing->Path, Path) || !SvSame(Existing->Body, BodyJson)) return Refuse(Different());
			Pending = *Existing;
			bKept = true;
		}
	}
	if (!bKept)
	{
		Pending.IdempotencyKey = Key;
		Pending.Kind = Kind;
		Pending.Path = Path;
		Pending.Body = BodyJson;
		Pending.PlayerId = Player;
		Pending.CreatedAt = Owner.GetPipeline().Now().ToIso8601();
		// Kept BEFORE the first send: a crash or a lost answer can then be finished with the same key. When storage
		// fails the change is still sent (with its key, so this call's own retries stay safe); it just cannot be finished
		// after a restart (OnStorageFailed was raised, and its errors say Pending false).
		bKept = UpdatePending([&](TArray<FSvPendingStoreChange>& List) { List.Add(Pending); });
	}
	FSvStoreApi* Self = this;
	return SvAsyncOr<FSvStoreTxnResult>(DroppedError(Key, bKept),
		[Self, Pending, bKept](TSvCallback<FSvStoreTxnResult> Done) { Self->SendChange(Pending, bKept, Done); });
}

void FSvStoreApi::Abandon()
{
	// Dropping the joined callbacks completes their futures with DroppedError (their key, still pending).
	TMap<FString, FInFlightChange, FDefaultSetAllocator, TSvCaseSensitiveKeyFuncs<FInFlightChange>> Dropped = MoveTemp(InFlight);
	InFlight.Reset();
}

FSvError FSvStoreApi::DroppedError(const FString& Key, bool bKept)
{
	FSvError E(0, SvErrorCodes::Shutdown, TEXT("The SplatterVault client was shut down before the store change was answered."));
	E.IdempotencyKey = Key;
	E.Pending = bKept;
	E.OutcomeUnknown = true;
	return E;
}

bool FSvStoreApi::StaysPending(const FSvError& Error)
{
	// No answer, a transient one, a sign-in problem (the change was not looked at), or nothing sent (status 0,
	// player_changed included): send it again later with the same key.
	return Error.Status == 0 || Error.IsTransient() || Error.Status == 401;
}

void FSvStoreApi::SendChange(const FSvPendingStoreChange& Pending, bool bKept, TSvCallback<FSvStoreTxnResult> Done)
{
	FSvCall Call = FSvCall::Post(Pending.Path, Pending.Body, ESvRetry::Safe);
	// Safe: every repeat carries the same key and the byte-identical body, so the API applies the change once.
	Call.Headers.Add(TEXT("Idempotency-Key"), Pending.IdempotencyKey);
	// Never sent under another player's token: that would charge the wrong player.
	Call.ExpectedPlayerId = Pending.PlayerId;
	FSvStoreApi* Self = this;
	const FString Key = Pending.IdempotencyKey;
	const FString Player = Pending.PlayerId;
	const FString Flight = InFlightId(Player, Key);
	FInFlightChange Entry;
	Entry.Request = Pending.Path + TEXT("\n") + Pending.Body;
	InFlight.Add(Flight, MoveTemp(Entry));
	// Every caller that joined this send gets the same answer.
	auto Finish = [Self, Flight, Done](const TSvResult<FSvStoreTxnResult>& Result)
	{
		FInFlightChange Ended;
		Self->InFlight.RemoveAndCopyValue(Flight, Ended);
		Done(Result);
		for (const TSvCallback<FSvStoreTxnResult>& Joined : Ended.Joined) Joined(Result);
	};
	Owner.Auth().SendAsPlayer(Call, [Self, Key, Player, bKept, Done = Finish](const TSvResult<FSvHttpResponse>& R)
	{
		if (R.IsError())
		{
			FSvError E = R.GetError();
			E.IdempotencyKey = Key;
			// The same key and the byte-identical body answered idempotency_conflict after more than one send: the
			// first send is still being applied. It stays pending (a later resume gets its stored answer).
			const bool bInProgress = E.Is(TEXT("idempotency_conflict")) && E.Attempts > 1;
			E.OutcomeUnknown = NoProofEitherWay(E) || bInProgress;
			bool bStillKept = bKept;
			if (!StaysPending(E) && !bInProgress && bKept)
			{
				// Answered with a refusal: nothing changed, nothing to finish. (Still kept only if the removal failed.)
				bStillKept = !Self->UpdatePending([&](TArray<FSvPendingStoreChange>& List) { List.RemoveAll([&](const FSvPendingStoreChange& P) { return SameChange(P, Key, Player); }); });
			}
			E.Pending = bStillKept;
			return Done(TSvResult<FSvStoreTxnResult>::Fail(E));
		}
		FSvStoreTxnResult Result;
		if (!SvModels::ReadText(R.GetValue().Body, Result))
		{
			// Applied (2xx) but not readable: still kept, so ResumePending gets the API's stored answer.
			FSvError E = Unreadable(R.GetValue());
			E.IdempotencyKey = Key;
			E.Pending = bKept;
			return Done(TSvResult<FSvStoreTxnResult>::Fail(E));
		}
		Result.IdempotencyKey = Key;
		if (bKept)
		{
			Self->UpdatePending([&](TArray<FSvPendingStoreChange>& List) { List.RemoveAll([&](const FSvPendingStoreChange& P) { return SameChange(P, Key, Player); }); });
		}
		Done(TSvResult<FSvStoreTxnResult>::Ok(Result));
	});
}

TSvResult<TArray<FSvPendingStoreChange>> FSvStoreApi::GetPending()
{
	const TSharedPtr<FSplatterVaultClient> Pinned = SvPin(Owner);
	const FString Player = Owner.Auth().GetPlayerId();
	TArray<FSvPendingStoreChange> All;
	FSvError Err;
	if (!LoadPending(All, Err)) return TSvResult<TArray<FSvPendingStoreChange>>::Fail(Err);
	TArray<FSvPendingStoreChange> Mine;
	if (!Player.IsEmpty())
	{
		for (const FSvPendingStoreChange& P : All)
		{
			if (SvSame(P.PlayerId, Player)) Mine.Add(P);
		}
	}
	return TSvResult<TArray<FSvPendingStoreChange>>::Ok(Mine);
}

TSvFuture<FSvResumeResult> FSvStoreApi::ResumePending()
{
	const TSharedPtr<FSplatterVaultClient> Pinned = SvPin(Owner);
	const TSvResult<TArray<FSvPendingStoreChange>> Pending = GetPending();
	if (Pending.IsError()) return SvFailed<FSvResumeResult>(Pending.GetError());
	TSharedRef<TArray<FSvPendingStoreChange>> Queue = MakeShared<TArray<FSvPendingStoreChange>>(Pending.GetValue());
	TSharedRef<FSvResumeResult> Result = MakeShared<FSvResumeResult>();
	FSvStoreApi* Self = this;
	return SvAsync<FSvResumeResult>([Self, Queue, Result](TSvCallback<FSvResumeResult> Done) { Self->ResumeNext(Queue, 0, Result, Done); });
}

void FSvStoreApi::ResumeNext(TSharedRef<TArray<FSvPendingStoreChange>> Queue, int32 Index, TSharedRef<FSvResumeResult> Result, TSvCallback<FSvResumeResult> Done)
{
	if (Index >= Queue->Num()) return Done(TSvResult<FSvResumeResult>::Ok(*Result));
	const FSvPendingStoreChange Change = (*Queue)[Index];

	// Right before each send: the change's player must still be the signed-in player. If not, stop: the rest stays
	// pending for its own player (sending it now would charge the player who is signed in).
	if (!SvSame(Change.PlayerId, Owner.Auth().GetPlayerId()))
	{
		Result->PlayerChanged = true;
		Result->StillPending += Queue->Num() - Index;
		return Done(TSvResult<FSvResumeResult>::Ok(*Result));
	}
	// A send of this same change already running (the game's own call, with the same key) finishes it.
	if (InFlight.Contains(InFlightId(Change.PlayerId, Change.IdempotencyKey)))
	{
		Result->StillPending++;
		return ResumeNext(Queue, Index + 1, Result, Done);
	}
	// It must still be kept: a concurrent call may have finished or discarded it since the queue was read.
	TArray<FSvPendingStoreChange> Kept;
	FSvError LoadError;
	if (!LoadPending(Kept, LoadError))
	{
		Result->StillPending += Queue->Num() - Index;
		return Done(TSvResult<FSvResumeResult>::Ok(*Result));
	}
	if (!Kept.ContainsByPredicate([&Change](const FSvPendingStoreChange& P) { return SameChange(P, Change.IdempotencyKey, Change.PlayerId); }))
	{
		return ResumeNext(Queue, Index + 1, Result, Done);
	}

	FSvStoreApi* Self = this;
	SendChange(Change, true, [Self, Queue, Index, Result, Change, Done](const TSvResult<FSvStoreTxnResult>& R)
	{
		if (R.IsOk())
		{
			Result->Completed.Add(R.GetValue());
		}
		else if (StaysPending(R.GetError()) || R.GetError().OutcomeUnknown)
		{
			Result->StillPending++;
			if (R.GetError().Is(SvErrorCodes::PlayerChanged))
			{
				Result->PlayerChanged = true;
				Result->StillPending += Queue->Num() - Index - 1;
				return Done(TSvResult<FSvResumeResult>::Ok(*Result));
			}
		}
		else
		{
			FSvRefusedStoreChange Refused;
			Refused.Change = Change;
			Refused.Error = R.GetError();
			Result->Refused.Add(Refused);
		}
		Self->ResumeNext(Queue, Index + 1, Result, Done);
	});
}

bool FSvStoreApi::DiscardPending(const FString& IdempotencyKey, const FString& PlayerId)
{
	const TSharedPtr<FSplatterVaultClient> Pinned = SvPin(Owner);
	const FString Player = PlayerId.IsEmpty() ? Owner.Auth().GetPlayerId() : PlayerId;
	if (Player.IsEmpty()) return false;
	return UpdatePending([&](TArray<FSvPendingStoreChange>& List) { List.RemoveAll([&](const FSvPendingStoreChange& P) { return SameChange(P, IdempotencyKey, Player); }); },
		true);
}

bool FSvStoreApi::DiscardAllPending()
{
	const TSharedPtr<FSplatterVaultClient> Pinned = SvPin(Owner);
	return DeletePendingList();
}

void FSvStoreApi::ForgetPendingForPlayer(const FString& PlayerId)
{
	UpdatePending([&](TArray<FSvPendingStoreChange>& List) { List.RemoveAll([&](const FSvPendingStoreChange& P) { return SvSame(P.PlayerId, PlayerId); }); }, true);
}

bool FSvStoreApi::LoadPending(TArray<FSvPendingStoreChange>& OutList, FSvError& OutError)
{
	OutList.Reset();
	// Kept in memory once read (every write goes through it): each read of the store unseals a value (an Android
	// Keystore call), and a purchase reads the list several times.
	if (bPendingCached)
	{
		OutList = PendingCache;
		return true;
	}
	FString Json;
	FString Why;
	const ESvStoreRead R = Owner.GetSecureStore().Get(PendingKey(), Json, Why);
	if (R == ESvStoreRead::Error)
	{
		Owner.Auth().RaiseStorageFailed(PendingItem, ESvStorageOperation::Read, Why);
		OutError = FSvAuth::Unreadable(PendingItem);
		return false;
	}
	if (R == ESvStoreRead::NotFound || Json.IsEmpty())
	{
		PendingCache.Reset();
		bPendingCached = true;
		return true;
	}
	const TSharedPtr<FJsonValue> V = SvJson::ParseValue(Json);
	if (!V.IsValid() || V->Type != EJson::Array)
	{
		Owner.Auth().RaiseStorageFailed(PendingItem, ESvStorageOperation::Read, TEXT("The pending store changes are not in a known format."));
		OutError = FSvAuth::Unreadable(PendingItem);
		return false;
	}
	for (const TSharedPtr<FJsonValue>& E : V->AsArray())
	{
		FSvPendingStoreChange C;
		if (E.IsValid() && E->Type == EJson::Object && SvModels::Read(E->AsObject(), C)) OutList.Add(C);
	}
	PendingCache = OutList;
	bPendingCached = true;
	return true;
}

bool FSvStoreApi::UpdatePending(TFunctionRef<void(TArray<FSvPendingStoreChange>&)> Mutate, bool bDeleteIfUnreadable)
{
	TArray<FSvPendingStoreChange> List;
	FSvError Err;
	if (!LoadPending(List, Err))
	{
		// Kept changes are never overwritten by a shorter list; only removals may give up on an unreadable list.
		return bDeleteIfUnreadable && DeletePendingList();
	}
	Mutate(List);
	FString Why;
	bool bOk;
	if (List.Num() == 0)
	{
		bOk = Owner.GetSecureStore().Delete(PendingKey(), Why);
	}
	else
	{
		TArray<TSharedPtr<FJsonValue>> A;
		for (const FSvPendingStoreChange& C : List) A.Add(MakeShared<FJsonValueObject>(SvModels::Write(C)));
		bOk = Owner.GetSecureStore().Set(PendingKey(), SvJson::WriteValue(MakeShared<FJsonValueArray>(A)), Why);
	}
	if (bOk)
	{
		PendingCache = List;
		bPendingCached = true;
	}
	else
	{
		// What the store holds is no longer known for sure: read it again next time.
		bPendingCached = false;
		Owner.Auth().RaiseStorageFailed(PendingItem, ESvStorageOperation::Write, Why);
	}
	return bOk;
}

bool FSvStoreApi::DeletePendingList()
{
	FString Why;
	bPendingCached = false;
	if (Owner.GetSecureStore().Delete(PendingKey(), Why))
	{
		PendingCache.Reset();
		bPendingCached = true;
		return true;
	}
	Owner.Auth().RaiseStorageFailed(PendingItem, ESvStorageOperation::Delete, Why);
	return false;
}
