// Copyright (c) SplatterVault. Licensed under the MIT License.

// Integration tests against the studio API mock in tools/sdk-mock (node tools/sdk-mock/server.mjs --port 8787). They
// run only when the mock is named: set SV_MOCK_URL (for example http://127.0.0.1:8787/rest). Without it each test logs
// a warning and passes.

#include "Tests/SvTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformMisc.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/Paths.h"
#include "SplatterVaultBlueprintLibrary.h"

// The quick start sample shipped in Docs/Samples (compiled and run here so it stays correct).
#if __has_include("../../../../Docs/Samples/SvQuickStartSample.inl")
#include "../../../../Docs/Samples/SvQuickStartSample.inl"
#define SV_HAS_QUICKSTART_SAMPLE 1
#else
#define SV_HAS_QUICKSTART_SAMPLE 0
#endif

using namespace SvTest;

namespace
{
	FString RedirectorBase()
	{
		FString Url = FPlatformMisc::GetEnvironmentVariable(TEXT("SV_REDIRECT_URL"));
		while (Url.EndsWith(TEXT("/"))) Url.LeftChopInline(1);
		return Url;
	}

	FString MockBase()
	{
		FString Url = FPlatformMisc::GetEnvironmentVariable(TEXT("SV_MOCK_URL"));
		while (Url.EndsWith(TEXT("/"))) Url.LeftChopInline(1);
		return Url;
	}

	FString ControlBase()
	{
		FString Url = MockBase();
		if (Url.EndsWith(TEXT("/rest"))) Url.LeftChopInline(5);
		return Url;
	}

	/** Calls a mock control (/__mock/...): never through the SDK. */
	void Control(const FString& Method, const FString& Path, const FString& Body, TFunction<void(int32, TSharedPtr<FJsonObject>)> Done)
	{
		const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Req = FHttpModule::Get().CreateRequest();
		Req->SetURL(ControlBase() + Path);
		Req->SetVerb(Method);
		Req->SetHeader(TEXT("Accept"), TEXT("application/json"));
		if (!Body.IsEmpty())
		{
			Req->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
			Req->SetContentAsString(Body);
		}
		Req->SetTimeout(30.f);
		Req->OnProcessRequestComplete().BindLambda([Done](FHttpRequestPtr, FHttpResponsePtr Resp, bool bOk)
		{
			if (!bOk || !Resp.IsValid()) return Done(0, nullptr);
			Done(Resp->GetResponseCode(), SvJson::ParseObject(Resp->GetContentAsString()));
		});
		Req->ProcessRequest();
	}

	/** The request log's last sequence number. */
	void LastSeq(TFunction<void(int64)> Done)
	{
		Control(TEXT("GET"), TEXT("/__mock/requests?since=999999999"), FString(), [Done](int32, TSharedPtr<FJsonObject> J)
		{
			int64 Last = -1;
			SvJson::GetInt64(J, TEXT("last"), Last);
			Done(Last);
		});
	}

	/** Logged requests after Since whose path contains PathPart. */
	void RequestsSince(int64 Since, const FString& PathPart, TFunction<void(TArray<TSharedPtr<FJsonObject>>)> Done)
	{
		Control(TEXT("GET"), FString::Printf(TEXT("/__mock/requests?since=%lld&path=%s"), (long long)Since, *PathPart), FString(),
			[Done](int32, TSharedPtr<FJsonObject> J)
			{
				TArray<TSharedPtr<FJsonObject>> Out;
				const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
				if (J.IsValid() && J->TryGetArrayField(TEXT("requests"), List) && List)
				{
					for (const TSharedPtr<FJsonValue>& V : *List)
					{
						if (V.IsValid() && V->Type == EJson::Object) Out.Add(V->AsObject());
					}
				}
				Done(Out);
			});
	}

	/** A client for the mock with the real HTTP stack, real clocks and the given store. */
	TSharedPtr<FSplatterVaultClient> MockClient(TSharedPtr<ISvSecureStore> Store, TSharedRef<TArray<FString>> Logs)
	{
		FSvClientOptions O;
		O.GameId = 4242;
		O.BaseUrl = MockBase();
		O.SecureStore = Store;
		O.RequestTimeoutSeconds = 20;
		O.Logger = [Logs](ELogVerbosity::Type, const FString& Line) { Logs->Add(Line); };
		FString Error;
		return FSplatterVaultClient::Create(O, Error);
	}

	bool SkipWithoutMock(FAutomationTestBase* Test)
	{
		if (!MockBase().IsEmpty()) return false;
		Test->AddWarning(TEXT("SV_MOCK_URL is not set: the integration test did not run."));
		return true;
	}

	template <typename T>
	bool Ok(FAutomationTestBase* Test, const TCHAR* What, const TSvResult<T>& R)
	{
		if (R.IsOk()) return true;
		Test->AddError(FString::Printf(TEXT("%s failed: %s"), What, *R.GetError().ToString()));
		return false;
	}

	struct FFlowState
	{
		TSharedPtr<ISvSecureStore> Store;
		TSharedPtr<FSplatterVaultClient> Client;
		TSharedPtr<FSplatterVaultClient> Second;
		TSharedRef<TArray<FString>> Logs = MakeShared<TArray<FString>>();
		FString PlayerId;
		FString Token;
		FString IdempotencyKey;
		int64 ScoreVersion = 0;
		int64 Seq = 0;
		FString ETag;
		double Started = 0;
		FString Email;
	};
}

// ─── guest → data → store → expiry → faults ───────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSvIntegrationFlowTest, "SplatterVault.Integration.GuestDataStoreFlow", SV_TEST_FLAGS)
bool FSvIntegrationFlowTest::RunTest(const FString& Parameters)
{
	if (SkipWithoutMock(this)) return true;
	FAutomationTestBase* T = this;
	TSharedRef<FFlowState> S = MakeShared<FFlowState>();
	S->Store = MakeShared<FSvMemorySecureStore>();
	S->Client = MockClient(S->Store, S->Logs);
	if (!TestTrue(TEXT("client"), S->Client.IsValid())) return false;
	TSharedRef<FSteps> Steps = MakeShared<FSteps>(this);

	Steps->Add(TEXT("mock health"), [T](FSteps::FNext Next)
	{
		Control(TEXT("GET"), TEXT("/__mock/health"), FString(), [T, Next](int32 Status, TSharedPtr<FJsonObject> J)
		{
			Next(T->TestEqual(TEXT("mock answers"), Status, 200));
		});
	});
	Steps->Add(TEXT("create guest"), [T, S](FSteps::FNext Next)
	{
		S->Client->Auth().SignInAsGuest(TEXT("UE Tester")).Next([T, S, Next](const TSvResult<FSvSignInResult>& R)
		{
			if (!Ok(T, TEXT("SignInAsGuest"), R)) return Next(false);
			T->TestTrue(TEXT("new player"), R.GetValue().Player.NewPlayer);
			T->TestTrue(TEXT("renewable, saved"), R.GetValue().Renewable && R.GetValue().SavedToDevice);
			T->TestEqual(TEXT("method device"), R.GetValue().Method, FString(SvProviders::Device));
			S->PlayerId = R.GetValue().Player.Id;
			S->Token = S->Client->Auth().GetPlayerToken();
			Next(!S->PlayerId.IsEmpty() && !S->Token.IsEmpty());
		});
	});
	Steps->Add(TEXT("sign in again (second launch restores the guest)"), [T, S](FSteps::FNext Next)
	{
		S->Second = MockClient(S->Store, S->Logs);
		S->Second->Auth().Restore().Next([T, S, Next](const TSvResult<bool>& R)
		{
			if (!Ok(T, TEXT("Restore"), R)) return Next(false);
			T->TestTrue(TEXT("restored"), R.GetValue());
			T->TestEqual(TEXT("same guest"), S->Second->Auth().GetPlayerId(), S->PlayerId);
			Next(R.GetValue());
		});
	});
	Steps->Add(TEXT("me"), [T, S](FSteps::FNext Next)
	{
		S->Client->Auth().GetMe().Next([T, S, Next](const TSvResult<FSvPlayerProfile>& R)
		{
			if (!Ok(T, TEXT("GetMe"), R)) return Next(false);
			T->TestEqual(TEXT("me id"), R.GetValue().Player.Id, S->PlayerId);
			T->TestTrue(TEXT("device identity"), R.GetValue().HasIdentity(SvProviders::Device));
			Next(true);
		});
	});
	Steps->Add(TEXT("data write"), [T, S](FSteps::FNext Next)
	{
		FSvDataOps Ops;
		Ops.SetNumber(TEXT("score"), 10, 0).SetJson(TEXT("settings"), TEXT("{\"fov\":90}"), 0).Inc(TEXT("matches"), 1, FString(), 0, 100, 0);
		S->Client->Data().Write(Ops).Next([T, S, Next](const TSvResult<FSvDataWriteResult>& R)
		{
			if (!Ok(T, TEXT("Write"), R)) return Next(false);
			T->TestEqual(TEXT("three results"), R.GetValue().Results.Num(), 3);
			if (R.GetValue().Results.Num() == 3)
			{
				S->ScoreVersion = R.GetValue().Results[0].Version;
				T->TestTrue(TEXT("score has a version"), R.GetValue().Results[0].HasVersion && S->ScoreVersion > 0);
				T->TestEqual(TEXT("inc answers the value"), R.GetValue().Results[2].ValueJson, FString(TEXT("1")));
			}
			Next(true);
		});
	});
	Steps->Add(TEXT("data read"), [T, S](FSteps::FNext Next)
	{
		S->Client->Data().GetMine().Next([T, S, Next](const TSvResult<FSvPlayerData>& R)
		{
			if (!Ok(T, TEXT("GetMine"), R)) return Next(false);
			const FSvDataEntry* Score = R.GetValue().Player.Find(TEXT("score"));
			T->TestTrue(TEXT("score read back"), Score && USplatterVaultBlueprintLibrary::GetDataInt(*Score, -1) == 10 && Score->Version == S->ScoreVersion);
			const FSvDataEntry* Settings = R.GetValue().Player.Find(TEXT("settings"));
			T->TestTrue(TEXT("settings object"), Settings && USplatterVaultBlueprintLibrary::GetDataObjectInt(*Settings, TEXT("fov"), 0) == 90);
			const FSvDataEntry* Tier = R.GetValue().Readonly.Find(TEXT("profile.tier"));
			T->TestTrue(TEXT("readonly tier"), Tier && USplatterVaultBlueprintLibrary::GetDataString(*Tier, TEXT("")) == TEXT("bronze"));
			Next(true);
		});
	});
	Steps->Add(TEXT("data ifVersion conflict"), [T, S](FSteps::FNext Next)
	{
		FSvDataOps Ops;
		Ops.SetNumber(TEXT("score"), 99, S->ScoreVersion + 1000);
		S->Client->Data().Write(Ops).Next([T, S, Next](const TSvResult<FSvDataWriteResult>& R)
		{
			if (!T->TestTrue(TEXT("conflict refused"), R.IsError())) return Next(false);
			T->TestEqual(TEXT("409"), R.GetError().Status, 409);
			T->TestEqual(TEXT("version_conflict"), R.GetError().Code, FString(TEXT("version_conflict")));
			const TArray<FSvVersionInfo> V = USplatterVaultBlueprintLibrary::GetVersionConflicts(R.GetError());
			T->TestTrue(TEXT("details carry the current version"), V.Num() == 1 && V[0].Key == TEXT("score") && V[0].Version == S->ScoreVersion);
			Next(true);
		});
	});
	Steps->Add(TEXT("game data + 304"), [T, S](FSteps::FNext Next)
	{
		S->Client->Data().GetGameData().Next([T, S, Next](const TSvResult<FSvGameData>& First)
		{
			if (!Ok(T, TEXT("GetGameData"), First)) return Next(false);
			T->TestTrue(TEXT("game data ETag"), !First.GetValue().ETag.IsEmpty() && First.GetValue().Data.Find(TEXT("motd")) != nullptr);
			S->Client->Data().GetGameData().Next([T, Next](const TSvResult<FSvGameData>& Second)
			{
				if (!Ok(T, TEXT("GetGameData again"), Second)) return Next(false);
				T->TestTrue(TEXT("game data 304"), Second.GetValue().NotModified && Second.GetValue().Data.Find(TEXT("motd")) != nullptr);
				Next(true);
			});
		});
	});
	Steps->Add(TEXT("catalog + 304"), [T, S](FSteps::FNext Next)
	{
		S->Client->Store().GetCatalog().Next([T, S, Next](const TSvResult<FSvStoreCatalog>& First)
		{
			if (!Ok(T, TEXT("GetCatalog"), First)) return Next(false);
			const FSvCatalogItem* Potion = First.GetValue().Find(TEXT("potion_small"));
			int64 Price = 0;
			T->TestTrue(TEXT("potion 20 COINS"), Potion && Potion->PriceIn(TEXT("COINS"), Price) && Price == 20 && Potion->Purchasable);
			T->TestTrue(TEXT("catalog ETag"), !First.GetValue().ETag.IsEmpty() && !First.GetValue().NotModified);
			S->Client->Store().GetCatalog().Next([T, Next](const TSvResult<FSvStoreCatalog>& Second)
			{
				if (!Ok(T, TEXT("GetCatalog again"), Second)) return Next(false);
				T->TestTrue(TEXT("catalog 304 keeps the items"), Second.GetValue().NotModified && Second.GetValue().Find(TEXT("potion_small")) != nullptr);
				Next(true);
			});
		});
	});
	Steps->Add(TEXT("purchase"), [T, S](FSteps::FNext Next)
	{
		S->IdempotencyKey = FSvStoreApi::NewIdempotencyKey();
		S->Client->Store().Purchase(TEXT("potion_small"), TEXT("COINS"), 20, 2, S->IdempotencyKey).Next([T, S, Next](const TSvResult<FSvStoreTxnResult>& R)
		{
			if (!Ok(T, TEXT("Purchase"), R)) return Next(false);
			T->TestFalse(TEXT("first purchase not replayed"), R.GetValue().Replayed);
			T->TestEqual(TEXT("key"), R.GetValue().IdempotencyKey, S->IdempotencyKey);
			const FSvBalance* Coins = R.GetValue().Balances.FindByPredicate([](const FSvBalance& B) { return B.Currency == TEXT("COINS"); });
			T->TestTrue(TEXT("1000 - 40 COINS"), Coins && Coins->Amount == 960);
			T->TestEqual(TEXT("nothing pending"), S->Client->Store().GetPending().GetValue().Num(), 0);
			Next(true);
		});
	});
	Steps->Add(TEXT("purchase replay (same key)"), [T, S](FSteps::FNext Next)
	{
		S->Client->Store().Purchase(TEXT("potion_small"), TEXT("COINS"), 20, 2, S->IdempotencyKey).Next([T, Next](const TSvResult<FSvStoreTxnResult>& R)
		{
			if (!Ok(T, TEXT("Purchase replay"), R)) return Next(false);
			T->TestTrue(TEXT("replayed"), R.GetValue().Replayed);
			Next(true);
		});
	});
	Steps->Add(TEXT("price mismatch"), [T, S](FSteps::FNext Next)
	{
		S->Client->Store().Purchase(TEXT("potion_small"), TEXT("COINS"), 19).Next([T, S, Next](const TSvResult<FSvStoreTxnResult>& R)
		{
			T->TestTrue(TEXT("price_changed"), R.IsError() && R.GetError().Status == 409 && R.GetError().Is(TEXT("price_changed")));
			T->TestEqual(TEXT("refusal not pending"), S->Client->Store().GetPending().GetValue().Num(), 0);
			Next(R.IsError());
		});
	});
	Steps->Add(TEXT("holdings"), [T, S](FSteps::FNext Next)
	{
		S->Client->Store().GetHoldings().Next([T, S, Next](const TSvResult<FSvStoreHoldings>& R)
		{
			if (!Ok(T, TEXT("GetHoldings"), R)) return Next(false);
			T->TestEqual(TEXT("two potions (replay bought nothing)"), R.GetValue().CountOf(TEXT("potion_small")), (int64)2);
			T->TestEqual(TEXT("960 COINS"), R.GetValue().BalanceOf(TEXT("COINS")), (int64)960);
			T->TestEqual(TEXT("holdings player"), R.GetValue().PlayerId, S->PlayerId);
			Next(true);
		});
	});
	Steps->Add(TEXT("consume"), [T, S](FSteps::FNext Next)
	{
		S->Client->Store().ConsumeSku(TEXT("potion_small"), 1).Next([T, Next](const TSvResult<FSvStoreTxnResult>& R)
		{
			if (!Ok(T, TEXT("ConsumeSku"), R)) return Next(false);
			T->TestEqual(TEXT("consume txn"), R.GetValue().Txn.Kind, FString(TEXT("consume")));
			T->TestTrue(TEXT("one potion left"), R.GetValue().Items.Num() == 1 && R.GetValue().Items[0].Quantity == 1);
			Next(true);
		});
	});
	Steps->Add(TEXT("history"), [T, S](FSteps::FNext Next)
	{
		S->Client->Store().GetHistory().Next([T, Next](const TSvResult<FSvStoreHistoryPage>& R)
		{
			if (!Ok(T, TEXT("GetHistory"), R)) return Next(false);
			TArray<FString> Kinds;
			for (const FSvHistoryEntry& E : R.GetValue().Txns) Kinds.Add(E.Kind);
			T->TestEqual(TEXT("consume, purchase, grant (newest first)"), FString::Join(Kinds, TEXT(",")), FString(TEXT("consume,purchase,grant")));
			T->TestTrue(TEXT("one page"), R.GetValue().Next.IsEmpty());
			Next(true);
		});
	});
	Steps->Add(TEXT("forced expiry -> automatic renewal"), [T, S](FSteps::FNext Next)
	{
		LastSeq([T, S, Next](int64 Seq)
		{
			S->Seq = Seq;
			Control(TEXT("POST"), TEXT("/__mock/expire-tokens"), TEXT("{}"), [T, S, Next](int32 Status, TSharedPtr<FJsonObject>)
			{
				if (!T->TestEqual(TEXT("expire-tokens"), Status, 200)) return Next(false);
				const FString OldToken = S->Client->Auth().GetPlayerToken();
				S->Client->Store().GetHoldings().Next([T, S, OldToken, Next](const TSvResult<FSvStoreHoldings>& R)
				{
					if (!Ok(T, TEXT("GetHoldings after expiry"), R)) return Next(false);
					T->TestNotEqual(TEXT("a new token"), S->Client->Auth().GetPlayerToken(), OldToken);
					RequestsSince(S->Seq, TEXT("/rest/"), [T, Next](TArray<TSharedPtr<FJsonObject>> Requests)
					{
						TArray<FString> Seen;
						for (const TSharedPtr<FJsonObject>& Q : Requests)
						{
							int64 Status = 0;
							SvJson::GetInt64(Q, TEXT("status"), Status);
							Seen.Add(FString::Printf(TEXT("%s %s %lld"), *SvJson::GetString(Q, TEXT("method")), *SvJson::GetString(Q, TEXT("path")), (long long)Status));
						}
						T->TestEqual(TEXT("401, guest renewal, resend"), FString::Join(Seen, TEXT(" | ")),
							FString(TEXT("GET /rest/store/me 401 | POST /rest/player-auth/device 200 | GET /rest/store/me 200")));
						Next(true);
					});
				});
			});
		});
	});
	Steps->Add(TEXT("injected 503 + Retry-After"), [T, S](FSteps::FNext Next)
	{
		Control(TEXT("POST"), TEXT("/__mock/fault"), TEXT("{\"match\":\"/store/me\",\"method\":\"GET\",\"status\":503,\"retryAfter\":1,\"times\":1}"),
			[T, S, Next](int32 Status, TSharedPtr<FJsonObject>)
			{
				if (!T->TestEqual(TEXT("fault queued"), Status, 200)) return Next(false);
				S->Started = FPlatformTime::Seconds();
				S->Client->Store().GetHoldings().Next([T, S, Next](const TSvResult<FSvStoreHoldings>& R)
				{
					const double Waited = FPlatformTime::Seconds() - S->Started;
					if (!Ok(T, TEXT("GetHoldings through a 503"), R)) return Next(false);
					T->TestTrue(FString::Printf(TEXT("waited out Retry-After (%.2f s)"), Waited), Waited >= 0.95);
					Next(true);
				});
			});
	});
	Steps->Add(TEXT("injected 503 with a long Retry-After is handed to the game"), [T, S](FSteps::FNext Next)
	{
		Control(TEXT("POST"), TEXT("/__mock/fault"), TEXT("{\"match\":\"/store/me\",\"method\":\"GET\",\"status\":503,\"retryAfter\":30,\"times\":1}"),
			[T, S, Next](int32, TSharedPtr<FJsonObject>)
			{
				S->Client->Store().GetHoldings().Next([T, Next](const TSvResult<FSvStoreHoldings>& R)
				{
					T->TestTrue(TEXT("503 surfaced"), R.IsError() && R.GetError().Status == 503);
					T->TestTrue(TEXT("Retry-After 30 carried"), R.GetError().HasRetryAfter && FMath::IsNearlyEqual(R.GetError().RetryAfterSeconds, 30.f));
					T->TestEqual(TEXT("code"), R.GetError().Code, FString(TEXT("player_sign_in_disabled")));
					Next(true);
				});
			});
	});
	Steps->Add(TEXT("lost purchase answer: retried with the same key, replayed"), [T, S](FSteps::FNext Next)
	{
		LastSeq([T, S, Next](int64 Seq)
		{
			S->Seq = Seq;
			Control(TEXT("POST"), TEXT("/__mock/fault"), TEXT("{\"match\":\"/store/me/purchases\",\"method\":\"POST\",\"drop\":true,\"after\":true,\"times\":1}"),
				[T, S, Next](int32, TSharedPtr<FJsonObject>)
				{
					S->Client->Store().Purchase(TEXT("raffle_ticket"), TEXT("COINS"), 50).Next([T, S, Next](const TSvResult<FSvStoreTxnResult>& R)
					{
						if (!Ok(T, TEXT("Purchase through a lost answer"), R)) return Next(false);
						T->TestTrue(TEXT("the retry replayed the first (applied) purchase"), R.GetValue().Replayed);
						RequestsSince(S->Seq, TEXT("/store/me/purchases"), [T, Next](TArray<TSharedPtr<FJsonObject>> Requests)
						{
							T->TestEqual(TEXT("two sends"), Requests.Num(), 2);
							if (Requests.Num() == 2)
							{
								const FString K1 = SvJson::GetString(SvJson::GetObj(Requests[0], TEXT("headers")), TEXT("idempotencyKey"));
								const FString K2 = SvJson::GetString(SvJson::GetObj(Requests[1], TEXT("headers")), TEXT("idempotencyKey"));
								T->TestTrue(TEXT("same Idempotency-Key"), !K1.IsEmpty() && K1 == K2);
							}
							Next(true);
						});
					});
				});
		});
	});
	Steps->Add(TEXT("no token or secret in the SDK's logs"), [T, S](FSteps::FNext Next)
	{
		bool bClean = true;
		const FString Current = S->Client->Auth().GetPlayerToken();
		for (const FString& L : *S->Logs)
		{
			if (L.Contains(S->Token) || (!Current.IsEmpty() && L.Contains(Current)) || L.Contains(TEXT("svpd1_")) || L.Contains(TEXT("svpr1_"))) bClean = false;
		}
		Next(T->TestTrue(TEXT("logs are clean"), bClean));
	});

	Steps->Start();
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForSteps(Steps, this, 180.0));
	return true;
}

// ─── email: code sign-up, refresh after expiry (single flight), restore, password, sign-out ───────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSvIntegrationEmailTest, "SplatterVault.Integration.EmailRefresh", SV_TEST_FLAGS)
bool FSvIntegrationEmailTest::RunTest(const FString& Parameters)
{
	if (SkipWithoutMock(this)) return true;
	FAutomationTestBase* T = this;
	TSharedRef<FFlowState> S = MakeShared<FFlowState>();
#if PLATFORM_WINDOWS
	// The real DPAPI store, in a throwaway directory.
	S->Store = MakeShared<FSvProtectedFileStore>(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("SplatterVault"), FGuid::NewGuid().ToString()),
		MakeShared<FSvDpapiProtector>());
#else
	S->Store = MakeShared<FSvMemorySecureStore>();
#endif
	S->Client = MockClient(S->Store, S->Logs);
	S->Email = FString::Printf(TEXT("ue-%s@example.com"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12).ToLower());
	TSharedRef<FSteps> Steps = MakeShared<FSteps>(this);
	const FString Password = TEXT("correct horse battery");

	Steps->Add(TEXT("request email code"), [T, S](FSteps::FNext Next)
	{
		S->Client->Auth().RequestEmailCode(S->Email).Next([T, Next](const TSvResult<FSvCodeSent>& R)
		{
			if (!Ok(T, TEXT("RequestEmailCode"), R)) return Next(false);
			T->TestTrue(TEXT("code lasts and resend wait"), R.GetValue().ExpiresInSeconds > 0 && R.GetValue().ResendAfterSeconds > 0);
			Next(true);
		});
	});
	Steps->Add(TEXT("verify code (sign up with a password)"), [T, S, Password](FSteps::FNext Next)
	{
		S->Client->Auth().VerifyEmailCode(S->Email, TEXT("12345678"), Password, TEXT("Email Tester")).Next([T, S, Next](const TSvResult<FSvSignInResult>& R)
		{
			if (!Ok(T, TEXT("VerifyEmailCode"), R)) return Next(false);
			T->TestTrue(TEXT("renewable (refresh token stored)"), R.GetValue().Renewable && R.GetValue().SavedToDevice);
			T->TestEqual(TEXT("method email"), R.GetValue().Method, FString(SvProviders::Email));
			S->PlayerId = R.GetValue().Player.Id;
			Next(true);
		});
	});
	Steps->Add(TEXT("forced expiry: two requests share one refresh"), [T, S](FSteps::FNext Next)
	{
		LastSeq([T, S, Next](int64 Seq)
		{
			S->Seq = Seq;
			Control(TEXT("POST"), TEXT("/__mock/expire-tokens"), TEXT("{}"), [T, S, Next](int32, TSharedPtr<FJsonObject>)
			{
				TSharedRef<int32> Left = MakeShared<int32>(2);
				TSharedRef<bool> AllOk = MakeShared<bool>(true);
				auto One = [T, S, Left, AllOk, Next](const TSvResult<FSvPlayerProfile>& R)
				{
					*AllOk &= Ok(T, TEXT("GetMe after expiry"), R);
					if (--(*Left) > 0) return;
					RequestsSince(S->Seq, TEXT("/player-auth/refresh"), [T, AllOk, Next](TArray<TSharedPtr<FJsonObject>> Requests)
					{
						T->TestEqual(TEXT("exactly one refresh"), Requests.Num(), 1);
						Next(*AllOk);
					});
				};
				S->Client->Auth().GetMe().Next(One);
				S->Client->Auth().GetMe().Next(One);
			});
		});
	});
	Steps->Add(TEXT("next launch restores with the refresh token"), [T, S](FSteps::FNext Next)
	{
		S->Second = MockClient(S->Store, S->Logs);
		S->Second->Auth().Restore().Next([T, S, Next](const TSvResult<bool>& R)
		{
			if (!Ok(T, TEXT("Restore (email)"), R)) return Next(false);
			T->TestTrue(TEXT("restored"), R.GetValue());
			T->TestEqual(TEXT("same player"), S->Second->Auth().GetPlayerId(), S->PlayerId);
			Next(R.GetValue());
		});
	});
	Steps->Add(TEXT("password sign-in"), [T, S, Password](FSteps::FNext Next)
	{
		S->Second->Auth().SignInWithPassword(S->Email, Password).Next([T, S, Next](const TSvResult<FSvSignInResult>& R)
		{
			if (!Ok(T, TEXT("SignInWithPassword"), R)) return Next(false);
			T->TestEqual(TEXT("same player"), R.GetValue().Player.Id, S->PlayerId);
			Next(true);
		});
	});
	Steps->Add(TEXT("wrong password"), [T, S](FSteps::FNext Next)
	{
		S->Second->Auth().SignInWithPassword(S->Email, TEXT("wrong password!")).Next([T, Next](const TSvResult<FSvSignInResult>& R)
		{
			T->TestTrue(TEXT("invalid_credentials"), R.IsError() && R.GetError().Status == 401 && R.GetError().Is(TEXT("invalid_credentials")));
			Next(true);
		});
	});
	Steps->Add(TEXT("sign out: restore answers false"), [T, S](FSteps::FNext Next)
	{
		S->Second->Auth().SignOut().Next([T, S, Next](const TSvResult<FSvVoid>&)
		{
			TSharedPtr<FSplatterVaultClient> Third = MockClient(S->Store, S->Logs);
			S->Client = Third;
			Third->Auth().Restore().Next([T, Next](const TSvResult<bool>& R)
			{
				T->TestTrue(TEXT("signed out stays signed out"), R.IsOk() && !R.GetValue());
				Next(true);
			});
		});
	});

	Steps->Start();
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForSteps(Steps, this, 180.0));
	return true;
}

// ─── platform sign-ins, studio token, Meta purchase sync, link / unlink, pending resume ───────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSvIntegrationPlatformsTest, "SplatterVault.Integration.PlatformsAndPending", SV_TEST_FLAGS)
bool FSvIntegrationPlatformsTest::RunTest(const FString& Parameters)
{
	if (SkipWithoutMock(this)) return true;
	FAutomationTestBase* T = this;
	TSharedRef<FFlowState> S = MakeShared<FFlowState>();
	S->Store = MakeShared<FSvMemorySecureStore>();
	S->Client = MockClient(S->Store, S->Logs);
	TSharedRef<FSteps> Steps = MakeShared<FSteps>(this);
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(10).ToLower();

	auto SignInStep = [T, S](const FString& Name, TFunction<TSvFuture<FSvSignInResult>(FSvAuth&)> Call, const FString& Method)
	{
		return [T, S, Name, Call, Method](FSteps::FNext Next)
		{
			Call(S->Client->Auth()).Next([T, Name, Method, Next](const TSvResult<FSvSignInResult>& R)
			{
				if (!Ok(T, *Name, R)) return Next(false);
				T->TestEqual(*(Name + TEXT(" method")), R.GetValue().Method, Method);
				T->TestFalse(*(Name + TEXT(" not renewable without a fresh proof")), R.GetValue().Renewable);
				Next(true);
			});
		};
	};
	Steps->Add(TEXT("steam"), SignInStep(TEXT("SignInWithSteam"), [Suffix](FSvAuth& A) { return A.SignInWithSteam(TEXT("deadbeef") + FString::Printf(TEXT("%08x"), GetTypeHash(Suffix))); }, SvProviders::Steam));
	Steps->Add(TEXT("xsolla"), SignInStep(TEXT("SignInWithXsolla"), [Suffix](FSvAuth& A) { return A.SignInWithXsolla(TEXT("test-token:") + Suffix); }, SvProviders::Xsolla));
	Steps->Add(TEXT("playfab"), SignInStep(TEXT("SignInWithPlayFab"), [Suffix](FSvAuth& A) { return A.SignInWithPlayFab(TEXT("test-token:") + Suffix, TEXT("MOCK1")); }, SvProviders::PlayFab));
	Steps->Add(TEXT("supabase"), SignInStep(TEXT("SignInWithSupabase"), [Suffix](FSvAuth& A) { return A.SignInWithSupabase(TEXT("test-token:") + Suffix); }, SvProviders::Supabase));
	Steps->Add(TEXT("meta"), SignInStep(TEXT("SignInWithMeta"), [](FSvAuth& A) { return A.SignInWithMeta(FString::Printf(TEXT("%llu"), (unsigned long long)(FDateTime::UtcNow().GetTicks() % 100000000000000ull)), TEXT("test-nonce")); }, SvProviders::Meta));
	Steps->Add(TEXT("meta purchase sync"), [T, S](FSteps::FNext Next)
	{
		S->Client->Store().SyncMetaPurchases().Next([T, S, Next](const TSvResult<FSvMetaSyncResult>& R)
		{
			if (!Ok(T, TEXT("SyncMetaPurchases"), R)) return Next(false);
			T->TestEqual(TEXT("granted 1"), R.GetValue().Granted, 1);
			T->TestEqual(TEXT("unmapped 1"), R.GetValue().Unmapped, 1);
			S->Client->Store().GetHoldings().Next([T, Next](const TSvResult<FSvStoreHoldings>& H)
			{
				if (!Ok(T, TEXT("GetHoldings after sync"), H)) return Next(false);
				T->TestEqual(TEXT("10 potions granted"), H.GetValue().CountOf(TEXT("potion_small")), (int64)10);
				Next(true);
			});
		});
	});
	Steps->Add(TEXT("link steam, then unlink it"), [T, S, Suffix](FSteps::FNext Next)
	{
		S->Client->Auth().LinkSteam(TEXT("deadbeef") + FString::Printf(TEXT("%08x"), GetTypeHash(Suffix + TEXT("link")))).Next([T, S, Next](const TSvResult<FSvPlayerProfile>& R)
		{
			if (!Ok(T, TEXT("LinkSteam"), R)) return Next(false);
			T->TestTrue(TEXT("steam linked"), R.GetValue().HasIdentity(SvProviders::Steam) && R.GetValue().HasIdentity(SvProviders::Meta));
			S->Client->Auth().Unlink(SvProviders::Steam).Next([T, Next](const TSvResult<FSvPlayerProfile>& U)
			{
				if (!Ok(T, TEXT("Unlink"), U)) return Next(false);
				T->TestFalse(TEXT("steam unlinked"), U.GetValue().HasIdentity(SvProviders::Steam));
				Next(true);
			});
		});
	});
	Steps->Add(TEXT("studio-issued token"), [T, S, Suffix](FSteps::FNext Next)
	{
		Control(TEXT("POST"), TEXT("/__mock/studio-token"), FString::Printf(TEXT("{\"externalId\":\"ue-%s\",\"displayName\":\"Studio Player\"}"), *Suffix),
			[T, S, Next](int32 Status, TSharedPtr<FJsonObject> J)
			{
				const FString Token = SvJson::GetString(J, TEXT("token"));
				if (!T->TestTrue(TEXT("studio token issued"), Status == 200 && !Token.IsEmpty())) return Next(false);
				S->Client->Auth().SignInWithStudioToken(Token, SvJson::GetString(J, TEXT("expiresAt"))).Next([T, S, Next](const TSvResult<FSvPlayerProfile>& R)
				{
					if (!Ok(T, TEXT("SignInWithStudioToken"), R)) return Next(false);
					T->TestEqual(TEXT("studio method"), S->Client->Auth().GetMethod(), FString(SvProviders::Studio));
					T->TestEqual(TEXT("studio player"), R.GetValue().Player.DisplayName, FString(TEXT("Studio Player")));
					T->TestTrue(TEXT("expiry known"), S->Client->Auth().GetTokenExpiresAtUtc().IsSet());
					Next(true);
				});
			});
	});
	Steps->Add(TEXT("refused studio token changes nothing"), [T, S](FSteps::FNext Next)
	{
		const FString Before = S->Client->Auth().GetPlayerId();
		S->Client->Auth().SignInWithStudioToken(TEXT("not.a.token")).Next([T, S, Before, Next](const TSvResult<FSvPlayerProfile>& R)
		{
			T->TestTrue(TEXT("token_invalid"), R.IsError() && R.GetError().Is(TEXT("token_invalid")));
			T->TestEqual(TEXT("still the studio player"), S->Client->Auth().GetPlayerId(), Before);
			Next(true);
		});
	});
	Steps->Add(TEXT("pending purchase survives a dead network, then resumes"), [T, S](FSteps::FNext Next)
	{
		// Every send of the purchase is applied once by the mock, then its answer is dropped (3 tries).
		Control(TEXT("POST"), TEXT("/__mock/fault"), TEXT("{\"match\":\"/store/me/purchases\",\"method\":\"POST\",\"drop\":true,\"after\":true,\"times\":3}"),
			[T, S, Next](int32, TSharedPtr<FJsonObject>)
			{
				S->Client->Store().Purchase(TEXT("hat_red"), TEXT("GEMS"), 25, 1, TEXT("ue-hat-") + S->Client->Auth().GetPlayerId().Left(8)).Next(
					[T, S, Next](const TSvResult<FSvStoreTxnResult>& R)
					{
						// The studio player has no GEMS: the first (applied) send was refused, and its answer lost. Whatever
						// the API said, the change must stay pending until an answer arrives.
						T->TestTrue(TEXT("no answer"), R.IsError() && R.GetError().IsNetworkError());
						T->TestTrue(TEXT("outcome unknown, key on the error"), R.GetError().OutcomeUnknown && !R.GetError().IdempotencyKey.IsEmpty());
						const TArray<FSvPendingStoreChange> Pending = S->Client->Store().GetPending().GetValue();
						T->TestEqual(TEXT("one pending"), Pending.Num(), 1);
						S->Client->Store().ResumePending().Next([T, S, Next](const TSvResult<FSvResumeResult>& Resumed)
						{
							if (!Ok(T, TEXT("ResumePending"), Resumed)) return Next(false);
							T->TestEqual(TEXT("answered now (refused: no GEMS)"), Resumed.GetValue().Refused.Num(), 1);
							if (Resumed.GetValue().Refused.Num() == 1)
							{
								T->TestEqual(TEXT("insufficient_funds"), Resumed.GetValue().Refused[0].Error.Code, FString(TEXT("insufficient_funds")));
							}
							T->TestEqual(TEXT("queue empty"), S->Client->Store().GetPending().GetValue().Num(), 0);
							Next(true);
						});
					});
			});
	});
	Steps->Add(TEXT("erase the player"), [T, S](FSteps::FNext Next)
	{
		S->Client->Auth().EraseMe().Next([T, S, Next](const TSvResult<FSvVoid>& R)
		{
			if (!Ok(T, TEXT("EraseMe"), R)) return Next(false);
			T->TestFalse(TEXT("signed out after erase"), S->Client->Auth().IsSignedIn());
			Next(true);
		});
	});

	Steps->Start();
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForSteps(Steps, this, 180.0));
	return true;
}

// ─── unknown outcomes, idempotency keys on errors, redirects (cross-SDK rules 2, 3, 5) ──────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSvIntegrationRulesTest, "SplatterVault.Integration.OutcomesAndRedirects", SV_TEST_FLAGS)
bool FSvIntegrationRulesTest::RunTest(const FString& Parameters)
{
	if (SkipWithoutMock(this)) return true;
	FAutomationTestBase* T = this;
	TSharedRef<FFlowState> S = MakeShared<FFlowState>();
	S->Store = MakeShared<FSvMemorySecureStore>();
	S->Client = MockClient(S->Store, S->Logs);
	TSharedRef<FSteps> Steps = MakeShared<FSteps>(this);

	Steps->Add(TEXT("guest"), [T, S](FSteps::FNext Next)
	{
		S->Client->Auth().SignInAsGuest().Next([T, S, Next](const TSvResult<FSvSignInResult>& R)
		{
			if (!Ok(T, TEXT("SignInAsGuest"), R)) return Next(false);
			FSvDataOps Ops;
			Ops.SetNumber(TEXT("level"), 1, 0);
			S->Client->Data().Write(Ops).Next([T, S, Next](const TSvResult<FSvDataWriteResult>& W)
			{
				if (!Ok(T, TEXT("first write"), W)) return Next(false);
				S->ScoreVersion = W.GetValue().Results[0].Version;
				Next(true);
			});
		});
	});
	Steps->Add(TEXT("ifVersion write whose answer is lost: not sent again, outcome unknown, recoverable"), [T, S](FSteps::FNext Next)
	{
		LastSeq([T, S, Next](int64 Seq)
		{
			S->Seq = Seq;
			Control(TEXT("POST"), TEXT("/__mock/fault"), TEXT("{\"match\":\"/player-data/me\",\"method\":\"PATCH\",\"drop\":true,\"after\":true,\"times\":1}"),
				[T, S, Next](int32, TSharedPtr<FJsonObject>)
				{
					FSvDataOps Ops;
					Ops.SetNumber(TEXT("level"), 2, S->ScoreVersion);
					S->Client->Data().Write(Ops).Next([T, S, Next](const TSvResult<FSvDataWriteResult>& W)
					{
						T->TestTrue(TEXT("lost answer: error, outcome unknown"), W.IsError() && W.GetError().OutcomeUnknown && W.GetError().Attempts == 1);
						RequestsSince(S->Seq, TEXT("/player-data/me"), [T, S, Next](TArray<TSharedPtr<FJsonObject>> Requests)
						{
							T->TestEqual(TEXT("sent once"), Requests.Num(), 1);
							// Recovery: read again. The version moved and the value is ours: it applied; do not write again.
							S->Client->Data().GetMine({TEXT("level")}).Next([T, S, Next](const TSvResult<FSvPlayerData>& D)
							{
								if (!Ok(T, TEXT("re-read"), D)) return Next(false);
								const FSvDataEntry* Level = D.GetValue().Player.Find(TEXT("level"));
								T->TestTrue(TEXT("the lost write did apply (version moved, value 2)"), Level && Level->Version > S->ScoreVersion &&
									USplatterVaultBlueprintLibrary::GetDataInt(*Level, 0) == 2);
								if (Level) S->ScoreVersion = Level->Version;
								Next(true);
							});
						});
					});
				});
		});
	});
	Steps->Add(TEXT("ifVersion write after 503: sent again"), [T, S](FSteps::FNext Next)
	{
		LastSeq([T, S, Next](int64 Seq)
		{
			S->Seq = Seq;
			Control(TEXT("POST"), TEXT("/__mock/fault"), TEXT("{\"match\":\"/player-data/me\",\"method\":\"PATCH\",\"status\":503,\"retryAfter\":0,\"times\":1}"),
				[T, S, Next](int32, TSharedPtr<FJsonObject>)
				{
					FSvDataOps Ops;
					Ops.SetNumber(TEXT("level"), 3, S->ScoreVersion);
					S->Client->Data().Write(Ops).Next([T, S, Next](const TSvResult<FSvDataWriteResult>& W)
					{
						if (!Ok(T, TEXT("write through a 503"), W)) return Next(false);
						RequestsSince(S->Seq, TEXT("/player-data/me"), [T, Next](TArray<TSharedPtr<FJsonObject>> Requests)
						{
							T->TestEqual(TEXT("two sends"), Requests.Num(), 2);
							Next(true);
						});
					});
				});
		});
	});
	Steps->Add(TEXT("purchase with no answer: the key on the error finishes it once"), [T, S](FSteps::FNext Next)
	{
		Control(TEXT("POST"), TEXT("/__mock/fault"), TEXT("{\"match\":\"/store/me/purchases\",\"method\":\"POST\",\"drop\":true,\"after\":true,\"times\":3}"),
			[T, S, Next](int32, TSharedPtr<FJsonObject>)
			{
				S->Client->Store().Purchase(TEXT("potion_small"), TEXT("COINS"), 20).Next([T, S, Next](const TSvResult<FSvStoreTxnResult>& R)
				{
					T->TestTrue(TEXT("no answer: outcome unknown"), R.IsError() && R.GetError().OutcomeUnknown);
					const FString Key = R.GetError().IdempotencyKey;
					T->TestTrue(TEXT("the key is on the error"), !Key.IsEmpty());
					S->Client->Store().Purchase(TEXT("potion_small"), TEXT("COINS"), 20, 1, Key).Next([T, S, Next](const TSvResult<FSvStoreTxnResult>& Again)
					{
						if (!Ok(T, TEXT("purchase again with the key"), Again)) return Next(false);
						T->TestTrue(TEXT("replayed: bought once"), Again.GetValue().Replayed);
						S->Client->Store().GetHoldings().Next([T, Next](const TSvResult<FSvStoreHoldings>& H)
						{
							T->TestTrue(TEXT("one potion, 980 COINS"), H.IsOk() && H.GetValue().CountOf(TEXT("potion_small")) == 1 && H.GetValue().BalanceOf(TEXT("COINS")) == 980);
							Next(true);
						});
					});
				});
			});
	});
	Steps->Add(TEXT("a 3xx answer is refused and not sent again"), [T, S](FSteps::FNext Next)
	{
		LastSeq([T, S, Next](int64 Seq)
		{
			S->Seq = Seq;
			Control(TEXT("POST"), TEXT("/__mock/fault"), TEXT("{\"match\":\"/store/me\",\"method\":\"GET\",\"status\":302,\"times\":1}"), [T, S, Next](int32, TSharedPtr<FJsonObject>)
			{
				S->Client->Store().GetHoldings().Next([T, S, Next](const TSvResult<FSvStoreHoldings>& R)
				{
					T->TestTrue(TEXT("redirect_refused"), R.IsError() && R.GetError().Is(SvErrorCodes::RedirectRefused) && R.GetError().Status == 302);
					RequestsSince(S->Seq, TEXT("/store/me"), [T, Next](TArray<TSharedPtr<FJsonObject>> Requests)
					{
						T->TestEqual(TEXT("sent once"), Requests.Num(), 1);
						Next(true);
					});
				});
			});
		});
	});
	Steps->Add(TEXT("a redirect the HTTP stack followed is refused (UE 5.5+)"), [T](FSteps::FNext Next)
	{
		if (RedirectorBase().IsEmpty())
		{
			T->AddWarning(TEXT("SV_REDIRECT_URL is not set: the followed-redirect check did not run."));
			return Next(true);
		}
		FSvClientOptions O;
		O.GameId = 4242;
		O.BaseUrl = RedirectorBase();
		O.SecureStore = MakeShared<FSvMemorySecureStore>();
		FString Error;
		TSharedPtr<FSplatterVaultClient> Via = FSplatterVaultClient::Create(O, Error);
		if (!Via.IsValid()) return Next(false);
		Via->Data().GetPublic().Next([T, Via, Next](const TSvResult<FSvGameData>& R)
		{
#if !UE_VERSION_OLDER_THAN(5, 5, 0)
			T->TestTrue(TEXT("followed redirect refused"), R.IsError() && R.GetError().Is(SvErrorCodes::RedirectRefused));
#else
			T->AddInfo(FString::Printf(TEXT("UE 5.3-5.4 cannot tell a followed redirect: %s"), R.IsOk() ? TEXT("answer used") : *R.GetError().ToString()));
#endif
			Next(true);
		});
	});

	Steps->Start();
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForSteps(Steps, this, 180.0));
	return true;
}

// ─── the quick start sample (Docs/Samples/SvQuickStartSample.inl) ─────────────────────────────────────────────────

#if SV_HAS_QUICKSTART_SAMPLE
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSvIntegrationSampleTest, "SplatterVault.Integration.QuickStartSample", SV_TEST_FLAGS)
bool FSvIntegrationSampleTest::RunTest(const FString& Parameters)
{
	if (SkipWithoutMock(this)) return true;
	FAutomationTestBase* T = this;
	TSharedRef<FFlowState> S = MakeShared<FFlowState>();
	S->Store = MakeShared<FSvMemorySecureStore>();
	S->Client = MockClient(S->Store, S->Logs);
	TSharedRef<FSteps> Steps = MakeShared<FSteps>(this);
	Steps->Add(TEXT("run the sample twice (new guest, then returning guest)"), [T, S](FSteps::FNext Next)
	{
		TSharedRef<TArray<FString>> Lines = MakeShared<TArray<FString>>();
		SvSample::RunQuickStart(S->Client.ToSharedRef(), [Lines](const FString& Line) { Lines->Add(Line); }, [T, S, Lines, Next](bool bOk)
		{
			T->TestTrue(TEXT("first run"), bOk);
			T->TestTrue(TEXT("launch number 1"), Lines->Contains(TEXT("Launch number 1")));
			SvSample::RunQuickStart(S->Client.ToSharedRef(), [Lines](const FString& Line) { Lines->Add(Line); }, [T, Lines, Next](bool bAgain)
			{
				T->TestTrue(TEXT("second run"), bAgain);
				T->TestTrue(TEXT("launch number 2"), Lines->Contains(TEXT("Launch number 2")));
				T->AddInfo(FString::Join(*Lines, TEXT(" / ")));
				Next(bAgain);
			});
		});
	});
	Steps->Start();
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForSteps(Steps, this, 120.0));
	return true;
}
#endif

#endif
