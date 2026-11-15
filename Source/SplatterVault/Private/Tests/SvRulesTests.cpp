// Copyright (c) SplatterVault. Licensed under the MIT License.

// Tests for the cross-SDK rules (unknown outcomes, idempotency keys on errors, per-player pending changes, no
// redirects, option validation, case-sensitive comparisons).

#include "Tests/SvTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "SplatterVaultBlueprintLibrary.h"
#include "SvPipeline.h"

#include <limits>

using namespace SvTest;

namespace
{
	const TCHAR* PlayerOne = TEXT("1d0c6a3e-0000-4000-8000-00000000000a");
	const TCHAR* PlayerTwo = TEXT("1d0c6a3e-0000-4000-8000-00000000000b");

	FString SignedIn(const FString& Token, const FString& PlayerId, const FString& Extra = FString())
	{
		return FString::Printf(TEXT("{\"token\":\"%s\",\"expiresAt\":\"2026-11-15T13:00:00.000Z\",\"player\":{\"id\":\"%s\",\"displayName\":null,\"createdAt\":\"2026-11-15T12:00:00.000Z\",\"newPlayer\":false},\"identities\":[]%s}"),
			*Token, *PlayerId, *Extra);
	}

	FString TxnBody(bool bReplayed)
	{
		return FString::Printf(TEXT("{\"txn\":{\"id\":\"11111111-2222-4333-8444-555555555555\",\"kind\":\"purchase\",\"createdAt\":\"2026-11-15T12:00:00.000Z\",\"reason\":null,\"reverses\":null},\"changes\":[],\"balances\":[],\"items\":[],\"shortfall\":[],\"replayed\":%s}"),
			bReplayed ? TEXT("true") : TEXT("false"));
	}

	const TCHAR* WriteOk = TEXT("{\"results\":[{\"store\":\"player\",\"key\":\"score\",\"version\":8}]}");

	FString Token(FHarness& H, const FString& Player, const FString& Unique, const FString& Amr = TEXT("device"))
	{
		const int64 Iat = Unix(H.Clock->Wall);
		return MakeToken(Player, Iat, Iat + 3600, Amr, Unique);
	}

	/** The same text with the case of every ASCII letter swapped. */
	FString SwapCase(const FString& S)
	{
		FString Out = S;
		for (int32 i = 0; i < Out.Len(); ++i)
		{
			const TCHAR C = Out[i];
			if (C >= TEXT('a') && C <= TEXT('z')) Out[i] = C - 32;
			else if (C >= TEXT('A') && C <= TEXT('Z')) Out[i] = C + 32;
		}
		return Out;
	}

	/** Writes the kept pending list directly (to set up several players' changes). */
	/**
	 * Changes kept by an earlier run of the game. Written straight to the store: the client reads the list once and then
	 * keeps it in memory, so call this before the client first reads it (or Restart the harness after it).
	 */
	void KeepPending(FHarness& H, const TArray<FSvPendingStoreChange>& Changes)
	{
		TArray<TSharedPtr<FJsonValue>> A;
		for (const FSvPendingStoreChange& C : Changes) A.Add(MakeShared<FJsonValueObject>(SvModels::Write(C)));
		FString Why;
		H.Store->Set(H.Client->GetStoragePrefix() + TEXT("pending_store_changes"), SvJson::WriteValue(MakeShared<FJsonValueArray>(A)), Why);
	}

	FSvPendingStoreChange Change(const FString& Key, const FString& Player, const FString& Sku = TEXT("potion_small"))
	{
		FSvPendingStoreChange C;
		C.IdempotencyKey = Key;
		C.Kind = TEXT("purchase");
		C.Path = TEXT("/store/me/purchases");
		C.Body = FString::Printf(TEXT("{\"sku\":\"%s\",\"currency\":\"COINS\",\"quantity\":1,\"expectedPrice\":20}"), *Sku);
		C.PlayerId = Player;
		C.CreatedAt = TEXT("2026-11-15T12:00:00.000Z");
		return C;
	}

	/** All kept changes, of every player. */
	TArray<FSvPendingStoreChange> AllKept(FHarness& H)
	{
		TArray<FSvPendingStoreChange> Out;
		FString Json, Why;
		if (H.Store->Get(H.Client->GetStoragePrefix() + TEXT("pending_store_changes"), Json, Why) != ESvStoreRead::Found) return Out;
		const TSharedPtr<FJsonValue> V = SvJson::ParseValue(Json);
		if (!V.IsValid() || V->Type != EJson::Array) return Out;
		for (const TSharedPtr<FJsonValue>& E : V->AsArray())
		{
			FSvPendingStoreChange C;
			if (SvModels::Read(E->AsObject(), C)) Out.Add(C);
		}
		return Out;
	}
}

// ─── rule 2: ifVersion writes re-sent only after an answer that proves nothing was applied ───────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSvDataWriteOutcomeTest, "SplatterVault.Unit.DataWriteOutcome", SV_TEST_FLAGS)
bool FSvDataWriteOutcomeTest::RunTest(const FString& Parameters)
{
	FHarness H;
	const FString T1 = Token(H, PlayerOne, TEXT("1"));
	const FString T2 = Token(H, PlayerOne, TEXT("2"));
	TArray<FSvHttpResponse> Script;
	int32 Writes = 0;
	H.Transport->Handler = [&](const FSvHttpRequest& R)
	{
		if (R.Url.EndsWith(TEXT("/player-auth/device/new"))) return JsonReply(200, SignedIn(T1, PlayerOne, TEXT(",\"deviceSecret\":\"svpd1_X\"")));
		if (R.Url.EndsWith(TEXT("/player-auth/device"))) return JsonReply(200, SignedIn(T2, PlayerOne));
		++Writes;
		if (Script.Num() == 0) return JsonReply(200, WriteOk);
		const FSvHttpResponse Next = Script[0];
		Script.RemoveAt(0);
		return Next;
	};
	TestTrue(TEXT("signed in"), Now(H.Client->Auth().SignInAsGuest()).IsOk());

	FSvDataOps Guarded;
	Guarded.SetNumber(TEXT("score"), 10, 7);
	auto Run = [&](TArray<FSvHttpResponse> Answers) -> TSvResult<FSvDataWriteResult>
	{
		Script = MoveTemp(Answers);
		Writes = 0;
		H.Clock->Delays.Reset();
		return Now(H.Client->Data().Write(Guarded));
	};

	for (const ESvTransportFailure F : {ESvTransportFailure::Network, ESvTransportFailure::Timeout})
	{
		const TSvResult<FSvDataWriteResult> R = Run({FSvHttpResponse::Failed(F)});
		TestTrue(TEXT("no answer: not sent again"), R.IsError() && Writes == 1);
		TestTrue(TEXT("no answer: outcome unknown"), R.GetError().OutcomeUnknown && R.GetError().Attempts == 1);
	}
	for (const int32 Status : {500, 502, 504})
	{
		const TSvResult<FSvDataWriteResult> R = Run({ErrorReply(Status, TEXT("internal_error"))});
		TestTrue(FString::Printf(TEXT("%d: not sent again, outcome unknown"), Status), R.IsError() && Writes == 1 && R.GetError().OutcomeUnknown);
	}
	for (const int32 Status : {408, 429, 503})
	{
		const TSvResult<FSvDataWriteResult> R = Run({ErrorReply(Status, TEXT("data_busy"), TEXT("1"))});
		TestTrue(FString::Printf(TEXT("%d proves nothing applied: sent again, succeeds"), Status), R.IsOk() && Writes == 2);
	}
	{
		const TSvResult<FSvDataWriteResult> R = Run({ErrorReply(503, TEXT("data_busy")), ErrorReply(409, TEXT("version_conflict"))});
		TestTrue(TEXT("conflict after a resend: outcome unknown, not a plain conflict"),
			R.IsError() && R.GetError().Is(TEXT("version_conflict")) && R.GetError().OutcomeUnknown && R.GetError().Attempts == 2);
	}
	{
		const TSvResult<FSvDataWriteResult> R = Run({ErrorReply(409, TEXT("version_conflict"))});
		TestTrue(TEXT("first-send conflict is a plain conflict"), R.IsError() && !R.GetError().OutcomeUnknown && R.GetError().Attempts == 1);
		TestFalse(TEXT("Blueprint: not outcome unknown"), USplatterVaultBlueprintLibrary::IsOutcomeUnknown(R.GetError()));
	}
	{
		// The 401 token_expired renewal path counts too: a conflict on the resend may be our own earlier send.
		const TSvResult<FSvDataWriteResult> R = Run({JsonReply(401, TEXT("{\"error\":\"token_expired\",\"message\":\"Token expired\"}")), ErrorReply(409, TEXT("version_conflict"))});
		TestTrue(TEXT("renewed and sent again"), Writes == 2);
		TestTrue(TEXT("conflict after the renewal resend: outcome unknown"), R.IsError() && R.GetError().OutcomeUnknown && R.GetError().Attempts == 2);
	}
	{
		// A write without ifVersion is never sent again; a 503 proves nothing applied, no answer leaves it unknown.
		FSvDataOps Blind;
		Blind.Inc(TEXT("matches"), 1);
		Script = {ErrorReply(503, TEXT("data_busy"))};
		Writes = 0;
		const TSvResult<FSvDataWriteResult> Busy = Now(H.Client->Data().Write(Blind));
		TestTrue(TEXT("no ifVersion: 503 not sent again, not unknown"), Busy.IsError() && Writes == 1 && !Busy.GetError().OutcomeUnknown);
		Script = {FSvHttpResponse::Failed(ESvTransportFailure::Timeout)};
		Writes = 0;
		const TSvResult<FSvDataWriteResult> Lost = Now(H.Client->Data().Write(Blind));
		TestTrue(TEXT("no ifVersion: timeout is outcome unknown"), Lost.IsError() && Writes == 1 && Lost.GetError().OutcomeUnknown);
		TestTrue(TEXT("Blueprint helper"), USplatterVaultBlueprintLibrary::IsOutcomeUnknown(Lost.GetError()));
	}
	return true;
}

// ─── rule 3: store errors carry the Idempotency-Key; in-progress conflicts stay pending ───────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSvStoreErrorKeyTest, "SplatterVault.Unit.StoreErrorKey", SV_TEST_FLAGS)
bool FSvStoreErrorKeyTest::RunTest(const FString& Parameters)
{
	FHarness H;
	const FString T1 = Token(H, PlayerOne, TEXT("1"));
	const FString T2 = Token(H, PlayerOne, TEXT("2"));
	TArray<FSvHttpResponse> Script;
	TArray<FString> Keys;
	TArray<FString> Bodies;
	H.Transport->Handler = [&](const FSvHttpRequest& R)
	{
		if (R.Url.EndsWith(TEXT("/player-auth/device/new"))) return JsonReply(200, SignedIn(T1, PlayerOne, TEXT(",\"deviceSecret\":\"svpd1_X\"")));
		if (R.Url.EndsWith(TEXT("/player-auth/device"))) return JsonReply(200, SignedIn(T2, PlayerOne));
		Keys.Add(R.Headers.FindRef(TEXT("Idempotency-Key")));
		Bodies.Add(R.Body);
		if (Script.Num() == 0) return JsonReply(201, TxnBody(false));
		const FSvHttpResponse Next = Script[0];
		Script.RemoveAt(0);
		return Next;
	};
	TestTrue(TEXT("signed in"), Now(H.Client->Auth().SignInAsGuest()).IsOk());

	// No answer at all: the error names the key, the change stays pending; the game sends it again with that key.
	Script = {FSvHttpResponse::Failed(ESvTransportFailure::Timeout), FSvHttpResponse::Failed(ESvTransportFailure::Timeout),
		FSvHttpResponse::Failed(ESvTransportFailure::Timeout)};
	const TSvResult<FSvStoreTxnResult> Lost = Now(H.Client->Store().Purchase(TEXT("potion_small"), TEXT("COINS"), 20));
	TestTrue(TEXT("lost: error"), Lost.IsError() && Lost.GetError().IsNetworkError());
	TestTrue(TEXT("lost: outcome unknown"), Lost.GetError().OutcomeUnknown);
	const FString Key = Lost.GetError().IdempotencyKey;
	TestTrue(TEXT("lost: the error carries the key that was sent"), Key.Len() == 32 && Keys.Num() == 3 && Keys[0] == Key && Keys[2] == Key);
	const TArray<FSvPendingStoreChange> Pending = H.Client->Store().GetPending().GetValue();
	TestTrue(TEXT("lost: pending under that key"), Pending.Num() == 1 && Pending[0].IdempotencyKey == Key);
	Keys.Reset();
	Bodies.Reset();
	Script = {JsonReply(200, TxnBody(true))};
	const TSvResult<FSvStoreTxnResult> Again = Now(H.Client->Store().Purchase(TEXT("potion_small"), TEXT("COINS"), 20, 1, Key));
	TestTrue(TEXT("sent again with the same key: replayed"), Again.IsOk() && Again.GetValue().Replayed && Keys.Num() == 1 && Keys[0] == Key);
	TestTrue(TEXT("byte-identical body"), Bodies.Num() == 1 && Bodies[0] == Pending[0].Body);
	TestEqual(TEXT("nothing pending"), H.Client->Store().GetPending().GetValue().Num(), 0);

	// idempotency_conflict after a resend: the first send is still being applied; it stays pending, outcome unknown.
	Script = {FSvHttpResponse::Failed(ESvTransportFailure::Timeout), ErrorReply(409, TEXT("idempotency_conflict"))};
	const TSvResult<FSvStoreTxnResult> Busy = Now(H.Client->Store().Purchase(TEXT("sword_basic"), TEXT("COINS"), 250));
	TestTrue(TEXT("in progress: outcome unknown, 2 sends"), Busy.IsError() && Busy.GetError().OutcomeUnknown && Busy.GetError().Attempts == 2);
	TestEqual(TEXT("in progress: still pending"), H.Client->Store().GetPending().GetValue().Num(), 1);
	H.Client->Store().DiscardPending(Busy.GetError().IdempotencyKey);

	// After a 401 token_expired renewal the resend counts as a second send too.
	Script = {JsonReply(401, TEXT("{\"error\":\"token_expired\",\"message\":\"Token expired\"}")), ErrorReply(409, TEXT("idempotency_conflict"))};
	const TSvResult<FSvStoreTxnResult> Renewed = Now(H.Client->Store().Purchase(TEXT("hat_red"), TEXT("COINS"), 300));
	TestTrue(TEXT("renewal resend conflict: attempts 2, stays pending"), Renewed.IsError() && Renewed.GetError().Attempts == 2 &&
		H.Client->Store().GetPending().GetValue().Num() == 1);
	H.Client->Store().DiscardPending(Renewed.GetError().IdempotencyKey);

	// idempotency_conflict on the first send: a refusal (the key was used for another request); nothing kept.
	Script = {ErrorReply(409, TEXT("idempotency_conflict"))};
	const TSvResult<FSvStoreTxnResult> Refused = Now(H.Client->Store().Purchase(TEXT("raffle_ticket"), TEXT("COINS"), 50, 1, TEXT("reused-key")));
	TestTrue(TEXT("first-send conflict: refusal, key on the error"), Refused.IsError() && !Refused.GetError().OutcomeUnknown &&
		Refused.GetError().IdempotencyKey == TEXT("reused-key"));
	TestEqual(TEXT("first-send conflict: not pending"), H.Client->Store().GetPending().GetValue().Num(), 0);
	return true;
}

// ─── rule 4: pending changes belong to their player ───────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSvPendingPerPlayerTest, "SplatterVault.Unit.PendingPerPlayer", SV_TEST_FLAGS)
bool FSvPendingPerPlayerTest::RunTest(const FString& Parameters)
{
	FHarness H;
	const FString TokenOne = Token(H, PlayerOne, TEXT("one"), TEXT("steam"));
	const FString TokenTwo = Token(H, PlayerTwo, TEXT("two"), TEXT("steam"));
	TArray<FString> SentKeys;
	TArray<FString> SentTokens;
	TFunction<void(const FSvHttpRequest&)> OnPurchase;
	H.Transport->Handler = [&](const FSvHttpRequest& R)
	{
		if (R.Url.EndsWith(TEXT("/player-auth/steam")))
		{
			const bool bTwo = SvJson::GetString(BodyOf(R), TEXT("ticket")) == TEXT("bbbb");
			return JsonReply(200, SignedIn(bTwo ? TokenTwo : TokenOne, bTwo ? PlayerTwo : PlayerOne));
		}
		SentKeys.Add(R.Headers.FindRef(TEXT("Idempotency-Key")));
		SentTokens.Add(R.Headers.FindRef(TEXT("Authorization")));
		if (OnPurchase) OnPurchase(R);
		return JsonReply(201, TxnBody(false));
	};

	// The same key kept for two players: each player's resume sends only its own, and removes only its own.
	KeepPending(H, {Change(TEXT("k1"), PlayerOne), Change(TEXT("k1"), PlayerTwo), Change(TEXT("k2"), PlayerOne)});
	TestTrue(TEXT("player two signed in"), Now(H.Client->Auth().SignInWithSteam(TEXT("bbbb"))).IsOk());
	TestEqual(TEXT("player two sees one change"), H.Client->Store().GetPending().GetValue().Num(), 1);
	const TSvResult<FSvResumeResult> Two = Now(H.Client->Store().ResumePending());
	TestTrue(TEXT("player two: one completed"), Two.IsOk() && Two.GetValue().Completed.Num() == 1 && !Two.GetValue().PlayerChanged);
	TestTrue(TEXT("player two: sent with player two's token"), SentTokens.Num() == 1 && SentTokens[0] == TEXT("Bearer ") + TokenTwo);
	TArray<FSvPendingStoreChange> Kept = AllKept(H);
	TestTrue(TEXT("player one's k1 untouched (removed by key AND player)"), Kept.Num() == 2 &&
		Kept.ContainsByPredicate([](const FSvPendingStoreChange& C) { return C.IdempotencyKey == TEXT("k1") && C.PlayerId == PlayerOne; }));

	// Player one resumes, and player two signs in while k1 is in flight: k2 is not sent under player two's token.
	SentKeys.Reset();
	SentTokens.Reset();
	TestTrue(TEXT("player one signed in"), Now(H.Client->Auth().SignInWithSteam(TEXT("aaaa"))).IsOk());
	FSplatterVaultClient* Client = H.Client.Get();
	OnPurchase = [Client](const FSvHttpRequest&)
	{
		Client->Auth().SignInWithSteam(TEXT("bbbb")); // answered at once by the fake transport
	};
	const TSvResult<FSvResumeResult> One = Now(H.Client->Store().ResumePending());
	OnPurchase = nullptr;
	TestTrue(TEXT("stopped: player changed"), One.IsOk() && One.GetValue().PlayerChanged && One.GetValue().Completed.Num() == 1 &&
		One.GetValue().StillPending == 1);
	TestTrue(TEXT("only k1 was sent, with player one's token"), SentKeys.Num() == 1 && SentKeys[0] == TEXT("k1") && SentTokens[0] == TEXT("Bearer ") + TokenOne);
	Kept = AllKept(H);
	TestTrue(TEXT("player one's k2 still kept"), Kept.Num() == 1 && Kept[0].IdempotencyKey == TEXT("k2") && Kept[0].PlayerId == PlayerOne);

	// A change finished or discarded while resuming is not sent (kept by an earlier run: the game starts again).
	SentKeys.Reset();
	KeepPending(H, {Change(TEXT("k3"), PlayerOne), Change(TEXT("k4"), PlayerOne)});
	H.Restart();
	Client = H.Client.Get();
	TestTrue(TEXT("player one again"), Now(H.Client->Auth().SignInWithSteam(TEXT("aaaa"))).IsOk());
	OnPurchase = [Client](const FSvHttpRequest&) { Client->Store().DiscardPending(TEXT("k4")); };
	const TSvResult<FSvResumeResult> Gone = Now(H.Client->Store().ResumePending());
	OnPurchase = nullptr;
	TestTrue(TEXT("k4 discarded meanwhile: not sent"), Gone.IsOk() && SentKeys.Num() == 1 && SentKeys[0] == TEXT("k3"));

	// DiscardPending matches key AND player.
	KeepPending(H, {Change(TEXT("k5"), PlayerOne), Change(TEXT("k5"), PlayerTwo)});
	H.Restart();
	TestTrue(TEXT("player one after the restart"), Now(H.Client->Auth().SignInWithSteam(TEXT("aaaa"))).IsOk());
	TestTrue(TEXT("discard player one's k5"), H.Client->Store().DiscardPending(TEXT("k5")));
	Kept = AllKept(H);
	TestTrue(TEXT("player two's k5 kept"), Kept.Num() == 1 && Kept[0].PlayerId == PlayerTwo);

	// A store change whose token renewal signs in another player is not sent under that player (player_changed).
	FHarness R2;
	const FString OldToken = Token(R2, PlayerOne, TEXT("old"), TEXT("steam"));
	const FString OtherToken = Token(R2, PlayerTwo, TEXT("other"), TEXT("steam"));
	int32 Purchases = 0;
	R2.Transport->Handler = [&](const FSvHttpRequest& R)
	{
		if (R.Url.EndsWith(TEXT("/player-auth/steam")))
		{
			const bool bTwo = SvJson::GetString(BodyOf(R), TEXT("ticket")) == TEXT("bbbb");
			return JsonReply(200, SignedIn(bTwo ? OtherToken : OldToken, bTwo ? PlayerTwo : PlayerOne));
		}
		++Purchases;
		return JsonReply(401, TEXT("{\"error\":\"token_expired\",\"message\":\"Token expired\"}"));
	};
	TestTrue(TEXT("player one (renewal test)"), Now(R2.Client->Auth().SignInWithSteam(TEXT("aaaa"))).IsOk());
	FSvAuth* Auth2 = &R2.Client->Auth();
	Auth2->SetReauthenticate([Auth2](const FSvSignInRequired&)
	{
		return Auth2->SignInWithSteam(TEXT("bbbb")).Next([](const TSvResult<FSvSignInResult>& S) { return S.IsOk(); });
	});
	const TSvResult<FSvStoreTxnResult> Switched = Now(R2.Client->Store().Purchase(TEXT("potion_small"), TEXT("COINS"), 20));
	TestTrue(TEXT("renewal switched players: player_changed"), Switched.IsError() && Switched.GetError().Is(SvErrorCodes::PlayerChanged));
	TestEqual(TEXT("never sent under player two"), Purchases, 1);
	const TArray<FSvPendingStoreChange> KeptForOne = AllKept(R2);
	TestTrue(TEXT("kept for player one"), KeptForOne.Num() == 1 && KeptForOne[0].PlayerId == PlayerOne);
	return true;
}

// ─── rule 5: no redirects, no plain http off loopback ─────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSvNoRedirectsTest, "SplatterVault.Unit.NoRedirects", SV_TEST_FLAGS)
bool FSvNoRedirectsTest::RunTest(const FString& Parameters)
{
	FHarness H;
	int32 Sends = 0;
	FSvHttpResponse Answer;
	H.Transport->Handler = [&](const FSvHttpRequest&)
	{
		++Sends;
		return Answer;
	};
	FSvPipeline& P = H.Client->GetPipeline();
	for (const int32 Status : {301, 302, 303, 307, 308})
	{
		TMap<FString, FString> Location;
		Location.Add(TEXT("Location"), TEXT("https://elsewhere.example/steal"));
		Answer = FSvHttpResponse::Make(Status, FString(), Location);
		Sends = 0;
		FSvError Seen;
		P.Send(FSvCall::Get(TEXT("/store/me")), TEXT("tok"), [&Seen](const TSvResult<FSvHttpResponse>& R) { Seen = R.GetError(); });
		TestTrue(FString::Printf(TEXT("%d: redirect_refused, sent once"), Status), Seen.Is(SvErrorCodes::RedirectRefused) && Seen.Status == Status && Sends == 1);
	}
	// An answer the HTTP stack got by following a redirect is refused too.
	Answer = JsonReply(200, TEXT("{}"));
	Answer.bRedirected = true;
	Sends = 0;
	FSvError Followed;
	bool bOk = true;
	P.Send(FSvCall::Get(TEXT("/store/me")), TEXT("tok"), [&](const TSvResult<FSvHttpResponse>& R)
	{
		bOk = R.IsOk();
		Followed = R.GetError();
	});
	TestTrue(TEXT("followed redirect: refused, not retried"), !bOk && Followed.Is(SvErrorCodes::RedirectRefused) && Sends == 1);

	TestFalse(TEXT("same URL"), FSvHttpResponse::IsRedirectOf(TEXT("https://api.test/rest/store/me?a=1"), TEXT("https://api.test/rest/store/me?a=1")));
	TestFalse(TEXT("default port spelled out"), FSvHttpResponse::IsRedirectOf(TEXT("https://api.test/rest/x"), TEXT("https://API.test:443/rest/x")));
	TestFalse(TEXT("unknown effective URL"), FSvHttpResponse::IsRedirectOf(TEXT("https://api.test/rest/x"), FString()));
	TestTrue(TEXT("other host"), FSvHttpResponse::IsRedirectOf(TEXT("https://api.test/rest/x"), TEXT("https://evil.test/rest/x")));
	TestTrue(TEXT("other scheme"), FSvHttpResponse::IsRedirectOf(TEXT("https://api.test/rest/x"), TEXT("http://api.test/rest/x")));
	TestTrue(TEXT("other port"), FSvHttpResponse::IsRedirectOf(TEXT("http://127.0.0.1:8787/rest/x"), TEXT("http://127.0.0.1:8788/rest/x")));
	TestTrue(TEXT("other path"), FSvHttpResponse::IsRedirectOf(TEXT("https://api.test/rest/x"), TEXT("https://api.test/rest/y")));

	FString Error;
	TestFalse(TEXT("http to a LAN host refused"), FSplatterVaultClient::CheckBaseUrl(TEXT("http://192.168.1.20/rest"), false, Error).Len() > 0);
	TestFalse(TEXT("127.evil.test is not loopback"), FSplatterVaultClient::CheckBaseUrl(TEXT("http://127.evil.test/rest"), false, Error).Len() > 0);
	TestFalse(TEXT("127.0.0.1.evil.test is not loopback"), FSplatterVaultClient::CheckBaseUrl(TEXT("http://127.0.0.1.evil.test/rest"), false, Error).Len() > 0);
	TestTrue(TEXT("127.0.0.1 allowed"), FSplatterVaultClient::CheckBaseUrl(TEXT("http://127.0.0.1:8787/rest"), false, Error).Len() > 0);
	TestTrue(TEXT("127.1.2.3 allowed"), FSplatterVaultClient::CheckBaseUrl(TEXT("http://127.1.2.3/rest"), false, Error).Len() > 0);
	TestTrue(TEXT("[::1] allowed"), FSplatterVaultClient::CheckBaseUrl(TEXT("http://[::1]:8787/rest"), false, Error).Len() > 0);
	TestTrue(TEXT("localhost allowed"), FSplatterVaultClient::CheckBaseUrl(TEXT("http://localhost:8787/rest"), false, Error).Len() > 0);
	TestTrue(TEXT("https anywhere"), FSplatterVaultClient::CheckBaseUrl(TEXT("https://staging.example.com/rest"), false, Error).Len() > 0);
#if !UE_BUILD_SHIPPING
	TestTrue(TEXT("LAN http with the development opt-in"), FSplatterVaultClient::CheckBaseUrl(TEXT("http://192.168.1.20/rest"), true, Error).Len() > 0);
#endif
	return true;
}

// ─── rule 6: numeric options are validated at configuration time ──────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSvOptionsValidationTest, "SplatterVault.Unit.OptionsValidation", SV_TEST_FLAGS)
bool FSvOptionsValidationTest::RunTest(const FString& Parameters)
{
	auto Base = []()
	{
		FSvClientOptions O;
		O.GameId = 1;
		O.SecureStore = MakeShared<FSvMemorySecureStore>();
		O.Transport = MakeShared<FFakeTransport>();
		return O;
	};
	FString Error;
	TestTrue(TEXT("defaults pass"), FSplatterVaultClient::Create(Base(), Error).IsValid());
	const double NaN = std::numeric_limits<double>::quiet_NaN();
	const double Inf = std::numeric_limits<double>::infinity();
	struct FCase
	{
		const TCHAR* What;
		TFunction<void(FSvClientOptions&)> Set;
	};
	const FCase Cases[] = {
		{TEXT("timeout NaN"), [NaN](FSvClientOptions& O) { O.RequestTimeoutSeconds = NaN; }},
		{TEXT("timeout infinite"), [Inf](FSvClientOptions& O) { O.RequestTimeoutSeconds = Inf; }},
		{TEXT("timeout 0"), [](FSvClientOptions& O) { O.RequestTimeoutSeconds = 0; }},
		{TEXT("timeout negative"), [](FSvClientOptions& O) { O.RequestTimeoutSeconds = -5; }},
		{TEXT("timeout over 2^31-1 ms"), [](FSvClientOptions& O) { O.RequestTimeoutSeconds = 2147484.0; }},
		{TEXT("refresh timeout 11"), [](FSvClientOptions& O) { O.RefreshTimeoutSeconds = 11; }},
		{TEXT("refresh timeout NaN"), [NaN](FSvClientOptions& O) { O.RefreshTimeoutSeconds = NaN; }},
		{TEXT("renew lead 3601"), [](FSvClientOptions& O) { O.RenewBeforeExpirySeconds = 3601; }},
		{TEXT("renew lead negative"), [](FSvClientOptions& O) { O.RenewBeforeExpirySeconds = -1; }},
		{TEXT("max attempts 0"), [](FSvClientOptions& O) { O.RetryPolicy.MaxAttempts = 0; }},
		{TEXT("max attempts 11"), [](FSvClientOptions& O) { O.RetryPolicy.MaxAttempts = 11; }},
		{TEXT("base delay NaN"), [NaN](FSvClientOptions& O) { O.RetryPolicy.BaseDelaySeconds = NaN; }},
		{TEXT("max backoff infinite"), [Inf](FSvClientOptions& O) { O.RetryPolicy.MaxBackoffSeconds = Inf; }},
		{TEXT("max retry-after NaN"), [NaN](FSvClientOptions& O) { O.RetryPolicy.MaxRetryAfterSeconds = NaN; }},
		{TEXT("max retry-after negative"), [](FSvClientOptions& O) { O.RetryPolicy.MaxRetryAfterSeconds = -1; }},
		{TEXT("jitter 1.5"), [](FSvClientOptions& O) { O.RetryPolicy.Jitter = 1.5; }},
		{TEXT("jitter NaN"), [NaN](FSvClientOptions& O) { O.RetryPolicy.Jitter = NaN; }},
	};
	for (const FCase& C : Cases)
	{
		FSvClientOptions O = Base();
		C.Set(O);
		Error.Reset();
		TestFalse(FString::Printf(TEXT("refused: %s"), C.What), FSplatterVaultClient::Create(O, Error).IsValid());
		TestFalse(FString::Printf(TEXT("explained: %s"), C.What), Error.IsEmpty());
	}
	FSvClientOptions Edge = Base();
	Edge.RequestTimeoutSeconds = 2147483.0;
	Edge.RefreshTimeoutSeconds = 10;
	Edge.RenewBeforeExpirySeconds = 0;
	Edge.RetryPolicy.MaxAttempts = 1;
	Edge.RetryPolicy.Jitter = 0;
	TestTrue(TEXT("edges accepted"), FSplatterVaultClient::Create(Edge, Error).IsValid());
	return true;
}

// ─── rule 8: tokens, keys and ids are compared case-sensitively ──────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSvCaseSensitiveTest, "SplatterVault.Unit.CaseSensitive", SV_TEST_FLAGS)
bool FSvCaseSensitiveTest::RunTest(const FString& Parameters)
{
	// A late refusal of an old token must not end a newer session whose token differs from it only in case.
	FHarness H;
	const FString Old = Token(H, PlayerOne, TEXT("abc"), TEXT("email"));
	const FString Fresh = SwapCase(Old);
	H.Transport->bDeferred = true;
	H.Transport->Handler = [&](const FSvHttpRequest& R)
	{
		const FString Extra = TEXT(",\"refreshToken\":\"svpr1_R\",\"refreshExpiresAt\":\"2026-12-15T12:00:00.000Z\"");
		if (R.Url.EndsWith(TEXT("/player-auth/email/password"))) return JsonReply(200, SignedIn(Old, PlayerOne, Extra));
		if (R.Url.EndsWith(TEXT("/player-auth/refresh"))) return JsonReply(200, SignedIn(Fresh, PlayerOne, Extra));
		return JsonReply(401, TEXT("{\"error\":\"token_invalid\",\"message\":\"The player token is not valid.\"}"));
	};
	int32 Required = 0;
	H.Client->Auth().OnSignInRequired.AddLambda([&Required](const FSvSignInRequired&) { ++Required; });
	TSvFuture<FSvSignInResult> SignIn = H.Client->Auth().SignInWithPassword(TEXT("a@example.com"), TEXT("password1"));
	H.Transport->Pump();
	TestTrue(TEXT("signed in"), SignIn.IsReady() && SignIn.Get().IsOk());
	TSvFuture<FSvSignInResult> Renew = H.Client->Auth().Refresh();      // sent first
	TSvFuture<FSvPlayerProfile> Late = H.Client->Auth().GetMe();        // sent with the old token
	H.Transport->Pump();                                                // refresh answers, then the old token is refused
	TestTrue(TEXT("renewed to a token that differs only in case"), Renew.IsReady() && Renew.Get().IsOk() &&
		H.Client->Auth().GetPlayerToken().Equals(Fresh, ESearchCase::CaseSensitive) && Fresh.Equals(Old, ESearchCase::IgnoreCase));
	TestTrue(TEXT("the old token's refusal"), Late.IsReady() && Late.Get().IsError() && Late.Get().GetError().Is(TEXT("token_invalid")));
	TestTrue(TEXT("the new session is kept"), H.Client->Auth().IsSignedIn() && Required == 0);

	// Player ids that differ only in case are different players; keys likewise.
	FHarness P;
	const FString Upper = FString(PlayerOne).ToUpper();
	P.Transport->Handler = [&](const FSvHttpRequest& R)
	{
		if (R.Url.EndsWith(TEXT("/player-auth/steam"))) return JsonReply(200, SignedIn(Token(P, PlayerOne, TEXT("p"), TEXT("steam")), PlayerOne));
		return JsonReply(201, TxnBody(false));
	};
	Now(P.Client->Auth().SignInWithSteam(TEXT("aaaa")));
	KeepPending(P, {Change(TEXT("key"), PlayerOne), Change(TEXT("KEY"), PlayerOne), Change(TEXT("key"), Upper)});
	TestEqual(TEXT("only this player's (exact id) changes"), P.Client->Store().GetPending().GetValue().Num(), 2);
	P.Client->Store().DiscardPending(TEXT("key"));
	const TArray<FSvPendingStoreChange> Kept = AllKept(P);
	TestTrue(TEXT("discard matched the exact key and id"), Kept.Num() == 2 && Kept.ContainsByPredicate([](const FSvPendingStoreChange& C)
		{ return C.IdempotencyKey.Equals(TEXT("KEY"), ESearchCase::CaseSensitive); }) &&
		Kept.ContainsByPredicate([&Upper](const FSvPendingStoreChange& C) { return C.PlayerId.Equals(Upper, ESearchCase::CaseSensitive); }));
	return true;
}

#endif
