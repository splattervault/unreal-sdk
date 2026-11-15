// Copyright (c) SplatterVault. Licensed under the MIT License.

#include "GenericPlatform/GenericPlatformHttp.h"
#include "SplatterVaultClient.h"
#include "SvJson.h"
#include "SvPipeline.h"

namespace
{
	FSvError BadArgument(const FString& Why)
	{
		return FSvError(0, SvErrorCodes::InvalidArgument, Why);
	}

	FSvError Unreadable(const FSvHttpResponse& R)
	{
		return FSvError(R.Status, SvErrorCodes::InvalidResponse, TEXT("The answer could not be read."));
	}
}

FSvPlayerDataApi::FSvPlayerDataApi(FSplatterVaultClient& InOwner)
	: Owner(InOwner)
{
}

bool FSvPlayerDataApi::KeysParam(const TArray<FString>& Keys, FString& OutParam, FString& OutError)
{
	OutParam.Reset();
	TArray<FString> Distinct;
	for (const FString& K : Keys)
	{
		if (!Distinct.ContainsByPredicate([&K](const FString& D) { return D.Equals(K, ESearchCase::CaseSensitive); })) Distinct.Add(K);
	}
	if (Distinct.Num() == 0) return true;
	if (Distinct.Num() > MaxReadKeys)
	{
		OutError = FString::Printf(TEXT("A read names at most %d keys."), MaxReadKeys);
		return false;
	}
	TArray<FString> Escaped;
	for (const FString& K : Distinct)
	{
		if (!FSvDataOps::IsValidKey(K))
		{
			OutError = TEXT("Not a valid key: keys are 1 to 128 letters, digits, _ . : -.");
			return false;
		}
		Escaped.Add(FGenericPlatformHttp::UrlEncode(K));
	}
	OutParam = TEXT("keys=") + FString::Join(Escaped, TEXT(","));
	return true;
}

TSvFuture<FSvPlayerData> FSvPlayerDataApi::GetMine(const TArray<FString>& Keys, bool bKeysOnly, const TArray<FString>& Stores)
{
	TArray<FString> Query;
	if (Stores.Num() > 0)
	{
		for (const FString& S : Stores)
		{
			if (S != TEXT("player") && S != TEXT("readonly")) return SvFailed<FSvPlayerData>(BadArgument(TEXT("A player reads only the player and readonly stores.")));
		}
		Query.Add(TEXT("stores=") + FString::Join(Stores, TEXT(",")));
	}
	FString KeyParam;
	FString Error;
	if (!KeysParam(Keys, KeyParam, Error)) return SvFailed<FSvPlayerData>(BadArgument(Error));
	if (!KeyParam.IsEmpty()) Query.Add(KeyParam);
	if (bKeysOnly) Query.Add(TEXT("keysOnly=true"));
	const FString Path = TEXT("/player-data/me") + (Query.Num() > 0 ? TEXT("?") + FString::Join(Query, TEXT("&")) : FString());
	FSvAuth* Auth = &Owner.Auth();
	return SvAsync<FSvPlayerData>([Auth, Path](TSvCallback<FSvPlayerData> Done)
	{
		Auth->SendAsPlayer(FSvCall::Get(Path), [Done](const TSvResult<FSvHttpResponse>& R)
		{
			if (R.IsError()) return Done(TSvResult<FSvPlayerData>::Fail(R.GetError()));
			FSvPlayerData Data;
			if (!SvModels::ReadPlayerDataText(R.GetValue().Body, Data)) return Done(TSvResult<FSvPlayerData>::Fail(Unreadable(R.GetValue())));
			Done(TSvResult<FSvPlayerData>::Ok(Data));
		});
	});
}

TSvFuture<FSvDataWriteResult> FSvPlayerDataApi::Write(const FSvDataOps& Ops)
{
	if (!Ops.GetBuildError().IsEmpty()) return SvFailed<FSvDataWriteResult>(BadArgument(Ops.GetBuildError()));
	if (Ops.Num() == 0) return SvFailed<FSvDataWriteResult>(BadArgument(TEXT("Add at least one op.")));
	FSvCall Call;
	Call.Method = TEXT("PATCH");
	Call.Path = TEXT("/player-data/me");
	Call.Body = Ops.ToJson();
	// No Idempotency-Key for data writes. A write whose every op has ifVersion is sent again only after an answer that
	// proves nothing was applied (408, 429, 503; and the 401 token_expired renewal). Any other write is sent once.
	Call.Retry = Ops.AllHaveIfVersion() ? ESvRetry::IfVersionWrite : ESvRetry::Never;
	FSvAuth* Auth = &Owner.Auth();
	return SvAsync<FSvDataWriteResult>([Auth, Call](TSvCallback<FSvDataWriteResult> Done)
	{
		Auth->SendAsPlayer(Call, [Done](const TSvResult<FSvHttpResponse>& R)
		{
			if (R.IsError()) return Done(TSvResult<FSvDataWriteResult>::Fail(FSvPlayerDataApi::MarkWriteOutcome(R.GetError())));
			FSvDataWriteResult Result;
			if (!SvModels::ReadText(R.GetValue().Body, Result)) return Done(TSvResult<FSvDataWriteResult>::Fail(Unreadable(R.GetValue())));
			Done(TSvResult<FSvDataWriteResult>::Ok(Result));
		});
	});
}

FSvError FSvPlayerDataApi::MarkWriteOutcome(const FSvError& In)
{
	FSvError E = In;
	if (E.IsNetworkError() || E.Status == 500 || E.Status == 502 || E.Status == 504)
	{
		// No answer, or a server failure that does not say whether the write ran.
		E.OutcomeUnknown = true;
	}
	else if (E.Is(TEXT("version_conflict")) && E.Attempts > 1)
	{
		// The write was sent more than once: the conflict may be with our own earlier send that did apply.
		E.OutcomeUnknown = true;
		E.Message = TEXT("version_conflict after the write was sent again: an earlier send may have applied it. Read again before writing.");
	}
	return E;
}

TSvFuture<FSvGameData> FSvPlayerDataApi::GetGameData(const TArray<FString>& Keys)
{
	FString KeyParam;
	FString Error;
	if (!KeysParam(Keys, KeyParam, Error)) return SvFailed<FSvGameData>(BadArgument(Error));
	return GetCached(TEXT("/player-data/game") + (KeyParam.IsEmpty() ? FString() : TEXT("?") + KeyParam), true);
}

TSvFuture<FSvGameData> FSvPlayerDataApi::GetPublic(const FString& GameIdOrKey, const TArray<FString>& Keys)
{
	const FString Game = !GameIdOrKey.IsEmpty() ? GameIdOrKey : (Owner.GetGameId() != 0 ? FString::FromInt(Owner.GetGameId()) : Owner.GetGameKey());
	if (Game.IsEmpty()) return SvFailed<FSvGameData>(BadArgument(TEXT("Name the game.")));
	if (Game == TEXT(".") || Game == TEXT("..")) return SvFailed<FSvGameData>(BadArgument(TEXT("Not a game id or Game Key.")));
	FString KeyParam;
	FString Error;
	if (!KeysParam(Keys, KeyParam, Error)) return SvFailed<FSvGameData>(BadArgument(Error));
	return GetCached(TEXT("/player-data/games/") + FGenericPlatformHttp::UrlEncode(Game) + TEXT("/public") + (KeyParam.IsEmpty() ? FString() : TEXT("?") + KeyParam),
		false);
}

void FSvPlayerDataApi::ClearCache()
{
	Cache.Reset();
}

TSvFuture<FSvGameData> FSvPlayerDataApi::GetCached(const FString& Path, bool bAsPlayer)
{
	FSvPlayerDataApi* Self = this;
	return SvAsync<FSvGameData>([Self, Path, bAsPlayer](TSvCallback<FSvGameData> Done)
	{
		const FCached* Cached = Self->Cache.Find(Path);
		FSvCall Call = FSvCall::Get(Path);
		Call.bAllow304 = Cached != nullptr;
		if (Cached && !Cached->ETag.IsEmpty()) Call.Headers.Add(TEXT("If-None-Match"), Cached->ETag);
		auto OnAnswer = [Self, Path, Done](const TSvResult<FSvHttpResponse>& R)
		{
			if (R.IsError()) return Done(TSvResult<FSvGameData>::Fail(R.GetError()));
			const FSvHttpResponse& Response = R.GetValue();
			const FCached* Kept = Self->Cache.Find(Path);
			if (Response.Status == 304 && Kept)
			{
				FSvGameData Same;
				Same.Data = Kept->Data;
				Same.ETag = Kept->ETag;
				Same.NotModified = true;
				return Done(TSvResult<FSvGameData>::Ok(Same));
			}
			FSvGameData Fresh;
			if (!SvModels::ReadGameDataText(Response.Body, Fresh.Data)) return Done(TSvResult<FSvGameData>::Fail(Unreadable(Response)));
			Fresh.ETag = Response.GetHeader(TEXT("ETag"));
			if (!Fresh.ETag.IsEmpty())
			{
				FCached Entry;
				Entry.ETag = Fresh.ETag;
				Entry.Data = Fresh.Data;
				Self->Cache.Add(Path, Entry);
			}
			else
			{
				Self->Cache.Remove(Path);
			}
			Done(TSvResult<FSvGameData>::Ok(Fresh));
		};
		if (bAsPlayer) Self->Owner.Auth().SendAsPlayer(Call, OnAnswer);
		else Self->Owner.GetPipeline().Send(Call, FString(), OnAnswer);
	});
}
