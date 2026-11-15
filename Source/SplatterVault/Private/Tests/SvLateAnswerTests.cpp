// Copyright (c) SplatterVault. Licensed under the MIT License.

// Coordinator rules 14-16: epoch checks after every await (refresh attempts, the device fallback, the ask-game path,
// erase cleanup, account-change tokens; game sign-ins start a new sign-in before they send), resume never joins a
// send in flight, and every store-change error says whether the change is still kept.

#include "Tests/SvTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

using namespace SvTest;

namespace
{
	const TCHAR* Ann = TEXT("3b6a1d22-0000-4000-8000-0000000000a1");
	const TCHAR* Bob = TEXT("3b6a1d22-0000-4000-8000-0000000000b2");

	FString Answer(const FString& Token, const FString& PlayerId, const FString& Refresh = FString())
	{
		const FString Extra = Refresh.IsEmpty() ? FString()
			: FString::Printf(TEXT(",\"refreshToken\":\"%s\",\"refreshExpiresAt\":\"2026-12-15T12:00:00.000Z\""), *Refresh);
		return FString::Printf(TEXT("{\"token\":\"%s\",\"expiresAt\":\"2026-11-15T13:00:00.000Z\",\"player\":{\"id\":\"%s\",\"displayName\":null,\"createdAt\":\"2026-11-15T12:00:00.000Z\",\"newPlayer\":false},\"identities\":[]%s}"),
			*Token, *PlayerId, *Extra);
	}

	FString Jwt(FHarness& H, const FString& Player, const FString& Unique, const FString& Amr)
	{
		const int64 Iat = Unix(H.Clock->Wall);
		return MakeToken(Player, Iat, Iat + 3600, Amr, Unique);
	}

	const TCHAR* Txn = TEXT("{\"txn\":{\"id\":\"11111111-2222-4333-8444-555555555555\",\"kind\":\"purchase\",\"createdAt\":\"2026-11-15T12:00:00.000Z\",\"reason\":null,\"reverses\":null},\"changes\":[],\"balances\":[],\"items\":[],\"shortfall\":[],\"replayed\":false}");
	const TCHAR* Profile = TEXT("{\"player\":{\"id\":\"3b6a1d22-0000-4000-8000-0000000000a1\",\"displayName\":null,\"createdAt\":\"2026-11-15T12:00:00.000Z\",\"lastSignedInAt\":null},\"game\":{\"gameKey\":null},\"identities\":[]}");

	FString StoredRefresh(FHarness& H)
	{
		FString Json, Why;
		if (H.Store->Get(H.Client->GetStoragePrefix() + TEXT("refresh_token"), Json, Why) != ESvStoreRead::Found) return FString();
		return SvJson::GetString(SvJson::ParseObject(Json), TEXT("token"));
	}

	TFuture<bool> Answered(bool bValue)
	{
		TPromise<bool> P;
		TFuture<bool> F = P.GetFuture();
		P.SetValue(bValue);
		return F;
	}

	/** Email sign-ins for ann@ / bob@ (each sign-in of a player gets a new refresh token), recording what is sent. */
	struct FEmailWorld
	{
		TArray<FString> RefreshSent;
		TArray<FString> SignedOut;
		int32 AnnSignIns = 0;
		FString RefreshAnswer = TEXT("invalid"); // "invalid": refresh_invalid; else the rotated token
		int32 DeviceSends = 0;

		TFunction<FSvHttpResponse(const FSvHttpRequest&)> Handler(FHarness& H)
		{
			return [this, &H](const FSvHttpRequest& R)
			{
				if (R.Url.EndsWith(TEXT("/player-auth/email/password")))
				{
					if (SvJson::GetString(BodyOf(R), TEXT("email")) == TEXT("bob@example.com"))
					{
						return JsonReply(200, Answer(Jwt(H, Bob, TEXT("b"), TEXT("email")), Bob, TEXT("svpr1_BOB")));
					}
					++AnnSignIns;
					return JsonReply(200, Answer(Jwt(H, Ann, FString::FromInt(AnnSignIns), TEXT("email")), Ann, FString::Printf(TEXT("svpr1_ANN%d"), AnnSignIns)));
				}
				if (R.Url.EndsWith(TEXT("/player-auth/refresh")))
				{
					RefreshSent.Add(SvJson::GetString(BodyOf(R), TEXT("refreshToken")));
					if (RefreshAnswer == TEXT("invalid")) return ErrorReply(401, TEXT("refresh_invalid"));
					return JsonReply(200, Answer(Jwt(H, Ann, TEXT("r"), TEXT("email")), Ann, RefreshAnswer));
				}
				if (R.Url.EndsWith(TEXT("/player-auth/sign-out")))
				{
					SignedOut.Add(SvJson::GetString(BodyOf(R), TEXT("refreshToken")));
					return FSvHttpResponse::Make(204, FString());
				}
				if (R.Url.EndsWith(TEXT("/player-auth/steam"))) return JsonReply(200, Answer(Jwt(H, Bob, TEXT("s"), TEXT("steam")), Bob));
				if (R.Url.EndsWith(TEXT("/player-auth/device"))) { ++DeviceSends; return JsonReply(200, Answer(Jwt(H, Ann, TEXT("d"), TEXT("device")), Ann)); }
				if (R.Url.EndsWith(TEXT("/player-auth/me/password"))) return JsonReply(200, TEXT("{\"refreshToken\":\"svpr1_CHANGED\",\"refreshExpiresAt\":\"2026-12-15T12:00:00.000Z\"}"));
				if (R.Method == TEXT("DELETE")) return FSvHttpResponse::Make(204, FString());
				return JsonReply(200, Profile);
			};
		}
	};
}

// ─── rule 14: every attempt and every cleanup belongs to the sign-in it started under ────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSvRefreshAttemptsEpochTest, "SplatterVault.Unit.RefreshAttemptsEpoch", SV_TEST_FLAGS)
bool FSvRefreshAttemptsEpochTest::RunTest(const FString& Parameters)
{
	// A refresh_invalid answered after a sign-out and another player's sign-in: the second attempt neither deletes the
	// new player's refresh token (as "another player's leftover") nor runs at all.
	for (const bool bSamePlayer : {false, true})
	{
		FHarness H;
		FEmailWorld W;
		H.Transport->Handler = W.Handler(H);
		Now(H.Client->Auth().SignInWithPassword(TEXT("ann@example.com"), TEXT("password1")));
		H.Transport->bDeferred = true;
		TSvFuture<FSvSignInResult> Renew = H.Client->Auth().Refresh();
		H.Client->Auth().SignOut();
		H.Client->Auth().SignInWithPassword(bSamePlayer ? TEXT("ann@example.com") : TEXT("bob@example.com"), TEXT("password1"));
		H.Transport->PumpOne(TEXT("/player-auth/email/password")); // the new sign-in lands first
		H.Transport->Pump();
		const FString Expected = bSamePlayer ? TEXT("svpr1_ANN2") : TEXT("svpr1_BOB");
		TestEqual(TEXT("one refresh attempt only"), W.RefreshSent.Num(), 1);
		TestTrue(TEXT("the late refusal is reported as sign_in_changed"), Renew.IsReady() && Renew.Get().GetError().Is(SvErrorCodes::SignInChanged));
		TestEqual(TEXT("the new sign-in's refresh token is kept (not deleted, not spent)"), StoredRefresh(H), Expected);
		TestEqual(TEXT("the new player stays signed in"), H.Client->Auth().GetPlayerId(), FString(bSamePlayer ? Ann : Bob));
		TestFalse(TEXT("nobody signs out the new sign-in"), W.SignedOut.Contains(Expected));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSvLateExpiredAfterSignOutTest, "SplatterVault.Unit.LateExpiredAfterSignOut", SV_TEST_FLAGS)
bool FSvLateExpiredAfterSignOutTest::RunTest(const FString& Parameters)
{
	// A guest's request answered 401 token_expired after a sign-out: the guest is not renewed with the device secret.
	{
		FHarness H;
		FEmailWorld W;
		const FString G = Jwt(H, Ann, TEXT("g"), TEXT("device"));
		H.Transport->Handler = [&](const FSvHttpRequest& R)
		{
			if (R.Url.EndsWith(TEXT("/player-auth/device/new"))) return JsonReply(200, Answer(G, Ann).Replace(TEXT("\"identities\":[]"), TEXT("\"identities\":[],\"deviceSecret\":\"svpd1_G\""), ESearchCase::CaseSensitive));
			if (R.Url.EndsWith(TEXT("/player-auth/device"))) { ++W.DeviceSends; return JsonReply(200, Answer(G, Ann)); }
			return ErrorReply(401, TEXT("token_expired"));
		};
		Now(H.Client->Auth().SignInAsGuest());
		int32 Required = 0;
		H.Client->Auth().OnSignInRequired.AddLambda([&Required](const FSvSignInRequired&) { ++Required; });
		H.Transport->bDeferred = true;
		TSvFuture<FSvPlayerProfile> Me = H.Client->Auth().GetMe();
		H.Client->Auth().SignOut();
		H.Transport->Pump();
		TestEqual(TEXT("guest: no device renewal"), W.DeviceSends, 0);
		TestEqual(TEXT("guest: nobody told to sign in"), Required, 0);
		TestFalse(TEXT("guest: still signed out"), H.Client->Auth().IsSignedIn());
		TestTrue(TEXT("guest: the request fails with its 401"), Me.IsReady() && Me.Get().GetError().Is(TEXT("token_expired")));
	}
	// A Steam player's request answered 401 token_expired after a sign-out: the game's reauthenticate is not called.
	{
		FHarness H;
		const FString S = Jwt(H, Bob, TEXT("s"), TEXT("steam"));
		H.Transport->Handler = [&](const FSvHttpRequest& R)
		{
			if (R.Url.EndsWith(TEXT("/player-auth/steam"))) return JsonReply(200, Answer(S, Bob));
			return ErrorReply(401, TEXT("token_expired"));
		};
		Now(H.Client->Auth().SignInWithSteam(TEXT("abcd")));
		int32 Reauth = 0;
		int32 Required = 0;
		H.Client->Auth().SetReauthenticate([&Reauth](const FSvSignInRequired&) { ++Reauth; return Answered(false); });
		H.Client->Auth().OnSignInRequired.AddLambda([&Required](const FSvSignInRequired&) { ++Required; });
		H.Transport->bDeferred = true;
		TSvFuture<FSvPlayerProfile> Me = H.Client->Auth().GetMe();
		H.Client->Auth().SignOut();
		H.Transport->Pump();
		TestEqual(TEXT("steam: reauthenticate not called"), Reauth, 0);
		TestEqual(TEXT("steam: nobody told to sign in"), Required, 0);
		TestFalse(TEXT("steam: still signed out"), H.Client->Auth().IsSignedIn());
		TestTrue(TEXT("steam: the request fails with its 401"), Me.IsReady() && Me.Get().GetError().Is(TEXT("token_expired")));
		// Signed in: a 401 token_expired still asks the game (the path is only closed for a sign-in that ended).
		H.Transport->bDeferred = false;
		Now(H.Client->Auth().SignInWithSteam(TEXT("abcd")));
		Now(H.Client->Auth().GetMe());
		TestEqual(TEXT("steam, signed in: reauthenticate called"), Reauth, 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSvGameSignInEpochTest, "SplatterVault.Unit.GameSignInEpoch", SV_TEST_FLAGS)
bool FSvGameSignInEpochTest::RunTest(const FString& Parameters)
{
	// A renewal in flight when the game starts a sign-in is not used, even when it answers first.
	{
		FHarness H;
		FEmailWorld W;
		W.RefreshAnswer = TEXT("svpr1_ROTATED");
		H.Transport->Handler = W.Handler(H);
		Now(H.Client->Auth().SignInWithPassword(TEXT("ann@example.com"), TEXT("password1")));
		const FString AnnToken = H.Client->Auth().GetPlayerToken();
		H.Transport->bDeferred = true;
		TSvFuture<FSvSignInResult> Renew = H.Client->Auth().Refresh();
		TSvFuture<FSvSignInResult> Steam = H.Client->Auth().SignInWithSteam(TEXT("abcd"));
		H.Transport->PumpOne(TEXT("/player-auth/refresh"));
		TestTrue(TEXT("renewal answered first: not used"), Renew.IsReady() && Renew.Get().GetError().Is(SvErrorCodes::SignInChanged));
		TestEqual(TEXT("the session is not replaced by it"), H.Client->Auth().GetPlayerToken(), AnnToken);
		H.Transport->Pump();
		TestTrue(TEXT("the game's sign-in is used"), Steam.IsReady() && Steam.Get().IsOk() && H.Client->Auth().GetPlayerId() == Bob);
		TestTrue(TEXT("the renewal's new refresh token is signed out"), W.SignedOut.Contains(TEXT("svpr1_ROTATED")));
	}
	// A background renewal that starts after the game's sign-in does not cancel it.
	{
		FHarness H;
		FEmailWorld W;
		W.RefreshAnswer = TEXT("svpr1_ROTATED");
		H.Transport->Handler = W.Handler(H);
		Now(H.Client->Auth().SignInWithPassword(TEXT("ann@example.com"), TEXT("password1")));
		H.Transport->bDeferred = true;
		TSvFuture<FSvSignInResult> Steam = H.Client->Auth().SignInWithSteam(TEXT("abcd"));
		H.Clock->Advance(4000);
		TSvFuture<FSvPlayerProfile> Me = H.Client->Auth().GetMe(); // due: renewed first
		H.Transport->PumpOne(TEXT("/player-auth/refresh"));
		H.Transport->Pump();
		TestTrue(TEXT("the game's sign-in still wins"), Steam.IsReady() && Steam.Get().IsOk() && H.Client->Auth().GetPlayerId() == Bob);
	}
	// Two game sign-ins: the later one wins, whatever order they answer in.
	{
		FHarness H;
		FEmailWorld W;
		H.Transport->Handler = W.Handler(H);
		H.Transport->bDeferred = true;
		TSvFuture<FSvSignInResult> First = H.Client->Auth().SignInWithSteam(TEXT("abcd"));
		TSvFuture<FSvSignInResult> Second = H.Client->Auth().SignInWithPassword(TEXT("ann@example.com"), TEXT("password1"));
		H.Transport->PumpOne(TEXT("/player-auth/email/password"));
		H.Transport->Pump();
		TestTrue(TEXT("the earlier sign-in is not used"), First.IsReady() && First.Get().GetError().Is(SvErrorCodes::SignInChanged));
		TestTrue(TEXT("the later one is"), Second.IsReady() && Second.Get().IsOk() && H.Client->Auth().GetPlayerId() == Ann);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSvAccountChangeEpochTest, "SplatterVault.Unit.AccountChangeEpoch", SV_TEST_FLAGS)
bool FSvAccountChangeEpochTest::RunTest(const FString& Parameters)
{
	// An erase answered after another player signed in: the new player's sign-in and records stay.
	{
		FHarness H;
		FEmailWorld W;
		H.Transport->Handler = W.Handler(H);
		Now(H.Client->Auth().SignInWithPassword(TEXT("ann@example.com"), TEXT("password1")));
		H.Transport->bDeferred = true;
		TSvFuture<FSvVoid> Erase = H.Client->Auth().EraseMe();
		H.Client->Auth().SignOut();
		H.Client->Auth().SignInWithPassword(TEXT("bob@example.com"), TEXT("password1"));
		H.Transport->PumpOne(TEXT("/player-auth/email/password"));
		H.Transport->Pump();
		TestTrue(TEXT("erase: done"), Erase.IsReady() && Erase.Get().IsOk());
		TestEqual(TEXT("erase: the new player stays signed in"), H.Client->Auth().GetPlayerId(), FString(Bob));
		TestEqual(TEXT("erase: the new player's refresh token stays"), StoredRefresh(H), FString(TEXT("svpr1_BOB")));
		TestEqual(TEXT("erase: the new player's last sign-in stays"), H.Client->Auth().GetLastSignInMethod().GetValue(), FString(SvProviders::Email));
	}
	// A password change answered after a sign-out: its new refresh token is not kept (and is signed out).
	for (const bool bOtherSignedIn : {false, true})
	{
		FHarness H;
		FEmailWorld W;
		H.Transport->Handler = W.Handler(H);
		Now(H.Client->Auth().SignInWithPassword(TEXT("ann@example.com"), TEXT("password1")));
		H.Transport->bDeferred = true;
		TSvFuture<FSvVoid> Change = H.Client->Auth().ChangePassword(TEXT("password1"), TEXT("password2"));
		H.Client->Auth().SignOut();
		if (bOtherSignedIn)
		{
			H.Client->Auth().SignInWithPassword(TEXT("bob@example.com"), TEXT("password1"));
			H.Transport->PumpOne(TEXT("/player-auth/email/password"));
		}
		H.Transport->Pump();
		TestTrue(TEXT("password: done"), Change.IsReady() && Change.Get().IsOk());
		TestEqual(TEXT("password: not saved for anyone else"), StoredRefresh(H), FString(bOtherSignedIn ? TEXT("svpr1_BOB") : TEXT("")));
		TestTrue(TEXT("password: signed out on the server"), W.SignedOut.Contains(TEXT("svpr1_CHANGED")));
	}
	// Still signed in: the password change's token is kept.
	{
		FHarness H;
		FEmailWorld W;
		H.Transport->Handler = W.Handler(H);
		Now(H.Client->Auth().SignInWithPassword(TEXT("ann@example.com"), TEXT("password1")));
		Now(H.Client->Auth().ChangePassword(TEXT("password1"), TEXT("password2")));
		TestEqual(TEXT("password: kept for the same player"), StoredRefresh(H), FString(TEXT("svpr1_CHANGED")));
	}
	return true;
}

// ─── rule 15: resume skips a change in flight (no deadlock from inside reauthenticate) ───────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSvResumeInsideReauthTest, "SplatterVault.Unit.ResumeInsideReauthenticate", SV_TEST_FLAGS)
bool FSvResumeInsideReauthTest::RunTest(const FString& Parameters)
{
	FHarness H;
	int32 SteamSignIns = 0;
	int32 Purchases = 0;
	FString Old;
	H.Transport->Handler = [&](const FSvHttpRequest& R)
	{
		if (R.Url.EndsWith(TEXT("/player-auth/steam")))
		{
			++SteamSignIns;
			return JsonReply(200, Answer(Jwt(H, Ann, FString::FromInt(SteamSignIns), TEXT("steam")), Ann));
		}
		++Purchases;
		if (R.Headers.FindRef(TEXT("Authorization")) == TEXT("Bearer ") + Old) return ErrorReply(401, TEXT("token_expired"));
		return JsonReply(201, Txn);
	};
	Now(H.Client->Auth().SignInWithSteam(TEXT("abcd")));
	Old = H.Client->Auth().GetPlayerToken();
	FSplatterVaultClient* Client = H.Client.Get();
	TOptional<TSvResult<FSvResumeResult>> Resumed;
	H.Client->Auth().SetReauthenticate([Client, &Resumed](const FSvSignInRequired&)
	{
		// The documented pattern: sign in again, then finish pending store changes.
		const bool bOk = Now(Client->Auth().SignInWithSteam(TEXT("abcd"))).IsOk();
		TSvFuture<FSvResumeResult> R = Client->Store().ResumePending();
		if (R.IsReady()) Resumed = R.Get();
		return Answered(bOk);
	});
	const TSvResult<FSvStoreTxnResult> Bought = Now(H.Client->Store().Purchase(TEXT("potion_small"), TEXT("COINS"), 20));
	TestTrue(TEXT("resume inside reauthenticate completes at once"), Resumed.IsSet() && Resumed->IsOk());
	TestTrue(TEXT("it skipped the change in flight"), Resumed.IsSet() && Resumed->GetValue().StillPending == 1 && Resumed->GetValue().Completed.Num() == 0);
	TestTrue(TEXT("the purchase finished with the new token"), Bought.IsOk());
	TestEqual(TEXT("sent twice: 401, then once with the new token"), Purchases, 2);
	TestEqual(TEXT("nothing pending"), H.Client->Store().GetPending().GetValue().Num(), 0);
	return true;
}

// ─── rule 16: every store-change error says whether the change is still kept, and carries its key ────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSvStoreErrorPendingTest, "SplatterVault.Unit.StoreErrorPending", SV_TEST_FLAGS)
bool FSvStoreErrorPendingTest::RunTest(const FString& Parameters)
{
	FHarness H(nullptr, 1);
	TArray<FSvHttpResponse> Script;
	TArray<FString> Bodies;
	H.Transport->Handler = [&](const FSvHttpRequest& R)
	{
		if (R.Url.EndsWith(TEXT("/player-auth/steam"))) return JsonReply(200, Answer(Jwt(H, Ann, TEXT("s"), TEXT("steam")), Ann));
		Bodies.Add(R.Body);
		if (Script.Num() == 0) return JsonReply(201, Txn);
		FSvHttpResponse Next = Script[0];
		Script.RemoveAt(0);
		return Next;
	};
	Now(H.Client->Auth().SignInWithSteam(TEXT("abcd")));
	FSvStoreApi& Store = H.Client->Store();
	struct FCase
	{
		const TCHAR* Name;
		FSvHttpResponse Response;
		bool bPending;
		bool bUnknown;
	};
	const TArray<FCase> Cases = {
		{TEXT("503"), ErrorReply(503, TEXT("unavailable"), TEXT("60")), true, false},
		{TEXT("408"), ErrorReply(408, TEXT("timeout")), true, false},
		{TEXT("429"), ErrorReply(429, TEXT("rate_limited"), TEXT("60")), true, false},
		{TEXT("no answer"), FSvHttpResponse::Failed(ESvTransportFailure::Network), true, true},
		{TEXT("500"), ErrorReply(500, TEXT("internal")), true, true},
		{TEXT("refusal"), ErrorReply(409, TEXT("insufficient_funds")), false, false},
	};
	for (const FCase& C : Cases)
	{
		Script = {C.Response};
		const FString Key = FString(TEXT("key-")) + C.Name;
		const TSvResult<FSvStoreTxnResult> R = Now(Store.Purchase(TEXT("potion_small"), TEXT("COINS"), 20, 1, Key.Replace(TEXT(" "), TEXT("-"))));
		TestTrue(*FString::Printf(TEXT("%s: Pending %d, OutcomeUnknown %d, key on the error"), C.Name, C.bPending, C.bUnknown), R.IsError() &&
			R.GetError().Pending == C.bPending && R.GetError().OutcomeUnknown == C.bUnknown && R.GetError().IdempotencyKey == Key.Replace(TEXT(" "), TEXT("-")));
		TestEqual(*FString::Printf(TEXT("%s: kept iff Pending"), C.Name), Store.GetPending().GetValue().Num(), C.bPending ? 1 : 0);
		Store.DiscardPending(Key.Replace(TEXT(" "), TEXT("-")));
	}

	// A different change under a kept key is refused locally; the kept entry is left exactly as it was.
	Script = {FSvHttpResponse::Failed(ESvTransportFailure::Network)};
	Now(Store.Purchase(TEXT("potion_small"), TEXT("COINS"), 20, 1, TEXT("kept-key")));
	const FString KeptBody = Store.GetPending().GetValue()[0].Body;
	const int32 SentBefore = Bodies.Num();
	const TSvResult<FSvStoreTxnResult> Other = Now(Store.Purchase(TEXT("sword_basic"), TEXT("COINS"), 250, 1, TEXT("kept-key")));
	TestTrue(TEXT("different change under a kept key: refused, nothing kept for it"), Other.IsError() && Other.GetError().Is(SvErrorCodes::ChangeInProgress) &&
		!Other.GetError().Pending && !Other.GetError().OutcomeUnknown && Other.GetError().IdempotencyKey == TEXT("kept-key"));
	TestEqual(TEXT("nothing sent for it"), Bodies.Num(), SentBefore);
	TestTrue(TEXT("the kept entry is untouched"), Store.GetPending().GetValue().Num() == 1 && Store.GetPending().GetValue()[0].Body == KeptBody);
	// The same change under the kept key finishes it ("finish", not "buy again").
	const TSvResult<FSvStoreTxnResult> Finished = Now(Store.Purchase(TEXT("potion_small"), TEXT("COINS"), 20, 1, TEXT("kept-key")));
	TestTrue(TEXT("the same change finishes it"), Finished.IsOk() && Bodies.Last() == KeptBody && Store.GetPending().GetValue().Num() == 0);

	// A sign-in problem keeps the change (401 token_expired with nobody to sign in again).
	Script = {ErrorReply(401, TEXT("token_expired"))};
	const TSvResult<FSvStoreTxnResult> Expired = Now(Store.Purchase(TEXT("potion_small"), TEXT("COINS"), 20, 1, TEXT("expired-key")));
	TestTrue(TEXT("401: Pending, key"), Expired.IsError() && Expired.GetError().Pending && Expired.GetError().IdempotencyKey == TEXT("expired-key"));

	// The client shuts down before the answer: client_shutdown with the key, still pending.
	{
		FHarness S;
		S.Transport->Handler = [&](const FSvHttpRequest& R)
		{
			if (R.Url.EndsWith(TEXT("/player-auth/steam"))) return JsonReply(200, Answer(Jwt(S, Ann, TEXT("s"), TEXT("steam")), Ann));
			return JsonReply(201, Txn);
		};
		Now(S.Client->Auth().SignInWithSteam(TEXT("abcd")));
		S.Transport->bDeferred = true;
		TSvFuture<FSvStoreTxnResult> Waiting = S.Client->Store().Purchase(TEXT("potion_small"), TEXT("COINS"), 20, 1, TEXT("shutdown-key"));
		TSvFuture<FSvStoreTxnResult> Joined = S.Client->Store().Purchase(TEXT("potion_small"), TEXT("COINS"), 20, 1, TEXT("shutdown-key"));
		S.Client->Shutdown();
		S.Transport->Pump(); // the answer arrives after the shutdown and is dropped
		for (TSvFuture<FSvStoreTxnResult>* F : {&Waiting, &Joined})
		{
			TestTrue(TEXT("shutdown: client_shutdown, key, Pending"), F->IsReady() && F->Get().GetError().Is(SvErrorCodes::Shutdown) &&
				F->Get().GetError().IdempotencyKey == TEXT("shutdown-key") && F->Get().GetError().Pending);
		}
	}
	return true;
}

// ─── conveniences asked for by the website snippets: Inc options, Steam ticket bytes for linking ─────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSvConveniencesTest, "SplatterVault.Unit.Conveniences", SV_TEST_FLAGS)
bool FSvConveniencesTest::RunTest(const FString& Parameters)
{
	// Inc with an options struct: the same op as the long form, with only what is needed spelled out.
	FSvDataOps Short;
	// (Plugin sources stay C++17: a game on older build settings compiles them. C++20 games may also write
	// FSvIncOptions{.Max = 9999, .IfVersion = 3}.)
	Short.Inc(TEXT("coins"), 5, FSvIncOptions().WithMax(9999).WithIfVersion(3));
	Short.Inc(TEXT("stats"), -2, FSvIncOptions().WithField(TEXT("gold")).WithMin(0).WithIfVersion(0));
	FSvDataOps Long;
	Long.Inc(TEXT("coins"), 5, FString(), TOptional<int64>(), TOptional<int64>(9999), 3);
	Long.Inc(TEXT("stats"), -2, TEXT("gold"), TOptional<int64>(0), TOptional<int64>(), 0);
	TestEqual(TEXT("Inc options: same ops as the long form"), Short.ToJson(), Long.ToJson());
	TestTrue(TEXT("Inc options: ifVersion on every op"), Short.AllHaveIfVersion() && Short.GetBuildError().IsEmpty());
	FSvDataOps Plain;
	Plain.Inc(TEXT("coins"), 1, FSvIncOptions());
	TestTrue(TEXT("Inc options: defaults add no ifVersion"), !Plain.AllHaveIfVersion() && !Plain.ToJson().Contains(TEXT("ifVersion")));
	FSvDataOps Bad;
	Bad.Inc(TEXT("coins"), 1, FSvIncOptions().WithMin(10).WithMax(1));
	TestFalse(TEXT("Inc options: still validated"), Bad.GetBuildError().IsEmpty());

	// LinkSteamTicket: the ticket bytes are sent as the hex the API takes.
	FHarness H;
	FString SentTicket;
	H.Transport->Handler = [&](const FSvHttpRequest& R)
	{
		if (R.Url.EndsWith(TEXT("/player-auth/steam"))) return JsonReply(200, Answer(Jwt(H, Ann, TEXT("s"), TEXT("steam")), Ann));
		if (R.Url.EndsWith(TEXT("/player-auth/link/steam"))) SentTicket = SvJson::GetString(BodyOf(R), TEXT("ticket"));
		return JsonReply(200, Profile);
	};
	TestTrue(TEXT("signed in"), Now(H.Client->Auth().SignInWithSteamTicket({0x01, 0xab})).IsOk());
	TestTrue(TEXT("LinkSteamTicket answered"), Now(H.Client->Auth().LinkSteamTicket({0xde, 0xad, 0xbe, 0xef})).IsOk());
	TestEqual(TEXT("LinkSteamTicket sends hex"), SentTicket, FString(TEXT("deadbeef")));
	TestTrue(TEXT("LinkSteamTicket refuses no bytes"), Now(H.Client->Auth().LinkSteamTicket(TArray<uint8>())).GetError().Is(SvErrorCodes::InvalidArgument));
	return true;
}

#endif
