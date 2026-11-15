// Copyright (c) SplatterVault. Licensed under the MIT License.

#include "Tests/SvTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SplatterVaultBlueprintLibrary.h"
#include "SvCrypto.h"
#include "SvPipeline.h"

using namespace SvTest;

namespace
{
	TArray<uint8> FromHex(const FString& Hex)
	{
		TArray<uint8> Out;
		for (int32 i = 0; i + 1 < Hex.Len(); i += 2)
		{
			Out.Add((uint8)FParse::HexNumber(*Hex.Mid(i, 2)));
		}
		return Out;
	}

	FString SignInBody(const FString& Token, const FString& PlayerId, bool bNew, const FString& Extra = FString())
	{
		return FString::Printf(TEXT("{\"token\":\"%s\",\"expiresAt\":\"2026-11-15T13:00:00.000Z\",\"player\":{\"id\":\"%s\",\"displayName\":null,\"createdAt\":\"2026-11-15T12:00:00.000Z\",\"newPlayer\":%s},\"identities\":[{\"provider\":\"device\",\"linkedAt\":\"2026-11-15T12:00:00.000Z\"}]%s}"),
			*Token, *PlayerId, bNew ? TEXT("true") : TEXT("false"), *Extra);
	}

	const TCHAR* PlayerA = TEXT("0b6f2a8e-1c1d-4a59-9b3c-7f1f3e2a9c01");
	const TCHAR* ProfileBody = TEXT("{\"player\":{\"id\":\"0b6f2a8e-1c1d-4a59-9b3c-7f1f3e2a9c01\",\"displayName\":\"Ann\",\"createdAt\":\"2026-11-15T12:00:00.000Z\",\"lastSignedInAt\":null},\"game\":{\"gameKey\":null},\"identities\":[{\"provider\":\"device\",\"linkedAt\":\"2026-11-15T12:00:00.000Z\"}]}");
}

// ─── crypto ───────────────────────────────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSvCryptoTest, "SplatterVault.Unit.Crypto", SV_TEST_FLAGS)
bool FSvCryptoTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("sha256('')"), SvCrypto::Hex(SvCrypto::Sha256(SvCrypto::Utf8(TEXT("")))),
		FString(TEXT("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")));
	TestEqual(TEXT("sha256('abc')"), SvCrypto::Hex(SvCrypto::Sha256(SvCrypto::Utf8(TEXT("abc")))),
		FString(TEXT("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")));
	TestEqual(TEXT("sha256 two blocks"),
		SvCrypto::Hex(SvCrypto::Sha256(SvCrypto::Utf8(TEXT("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")))),
		FString(TEXT("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1")));
	// RFC 4231 test case 2.
	TestEqual(TEXT("hmac-sha256 rfc4231 #2"), SvCrypto::Hex(SvCrypto::HmacSha256(SvCrypto::Utf8(TEXT("Jefe")), SvCrypto::Utf8(TEXT("what do ya want for nothing?")))),
		FString(TEXT("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843")));
	// NIST SP 800-38A F.5.5 (CTR-AES256), first two blocks.
	const TArray<uint8> Key = FromHex(TEXT("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4"));
	const TArray<uint8> Iv = FromHex(TEXT("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff"));
	const TArray<uint8> Plain = FromHex(TEXT("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"));
	TArray<uint8> Cipher;
	Cipher.SetNumZeroed(Plain.Num());
	SvCrypto::Aes256Ctr(Key, Iv.GetData(), Plain.GetData(), Cipher.GetData(), Plain.Num());
	TestEqual(TEXT("aes-256-ctr nist"), SvCrypto::Hex(Cipher), FString(TEXT("601ec313775789a5b7a7f504bbf3d228f443e3ca4d62b59aca84e990cacaf5c5")));
	TArray<uint8> R1, R2;
	TestTrue(TEXT("random bytes"), SvCrypto::RandomBytes(32, R1) && SvCrypto::RandomBytes(32, R2));
	TestNotEqual(TEXT("random bytes differ"), SvCrypto::Hex(R1), SvCrypto::Hex(R2));
	const FString K1 = FSvStoreApi::NewIdempotencyKey();
	TestEqual(TEXT("idempotency key is 32 hex"), K1.Len(), 32);
	TestTrue(TEXT("idempotency key valid"), FSvStoreApi::IsValidIdempotencyKey(K1));
	TestFalse(TEXT("space not valid"), FSvStoreApi::IsValidIdempotencyKey(TEXT("a b")));
	TestFalse(TEXT("129 chars not valid"), FSvStoreApi::IsValidIdempotencyKey(FString::ChrN(129, TEXT('a'))));
	TestEqual(TEXT("steam hex"), FSvAuth::ToHex({0x14, 0x00, 0xab, 0xff}), FString(TEXT("1400ABFF")));
	return true;
}

// ─── errors ───────────────────────────────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSvErrorMappingTest, "SplatterVault.Unit.ErrorMapping", SV_TEST_FLAGS)
bool FSvErrorMappingTest::RunTest(const FString& Parameters)
{
	const FDateTime Now(2026, 11, 15, 12, 0, 0);
	{
		TMap<FString, FString> H;
		H.Add(TEXT("retry-after"), TEXT("7"));
		const FSvError E = FSvPipeline::ErrorFromResponse(JsonReply(429, TEXT("{\"error\":\"rate_limited\",\"message\":\"Too many requests.\"}"), H), Now);
		TestEqual(TEXT("status"), E.Status, 429);
		TestEqual(TEXT("code"), E.Code, FString(TEXT("rate_limited")));
		TestEqual(TEXT("message"), E.Message, FString(TEXT("Too many requests.")));
		TestTrue(TEXT("retry-after (header names ignore case)"), E.HasRetryAfter && FMath::IsNearlyEqual(E.RetryAfterSeconds, 7.f));
		TestTrue(TEXT("transient"), E.IsTransient());
		TestFalse(TEXT("not network"), E.IsNetworkError());
	}
	{
		const FSvError E = FSvPipeline::ErrorFromResponse(
			JsonReply(409, TEXT("{\"error\":\"version_conflict\",\"message\":\"x\",\"details\":[{\"store\":\"player\",\"key\":\"score\",\"version\":3},{\"store\":\"player\",\"key\":\"gone\",\"version\":null}]}")), Now);
		TestTrue(TEXT("version_conflict"), E.Is(TEXT("version_conflict")));
		const TArray<FSvVersionInfo> V = USplatterVaultBlueprintLibrary::GetVersionConflicts(E);
		TestEqual(TEXT("two conflicts"), V.Num(), 2);
		if (V.Num() == 2)
		{
			TestTrue(TEXT("score v3"), V[0].Key == TEXT("score") && V[0].HasVersion && V[0].Version == 3);
			TestFalse(TEXT("gone has no version"), V[1].HasVersion);
		}
		TestFalse(TEXT("409 not transient"), E.IsTransient());
	}
	{
		const FSvError E = FSvPipeline::ErrorFromResponse(JsonReply(404, TEXT("{\"name\":\"NOT_FOUND\",\"message\":\"Resource not found\",\"status\":404}")), Now);
		TestTrue(TEXT("framework shape: no code"), E.Code.IsEmpty());
		TestEqual(TEXT("framework name"), E.ErrorName, FString(TEXT("NOT_FOUND")));
	}
	{
		TMap<FString, FString> H;
		H.Add(TEXT("Retry-After"), TEXT("Sun, 15 Nov 2026 12:00:30 GMT"));
		const FSvError E = FSvPipeline::ErrorFromResponse(FSvHttpResponse::Make(502, TEXT("<html>Bad gateway token=abc</html>"), H), Now);
		TestEqual(TEXT("html: status kept"), E.Status, 502);
		TestTrue(TEXT("html: no code"), E.Code.IsEmpty());
		TestFalse(TEXT("html body never quoted"), E.Message.Contains(TEXT("html")) || E.Message.Contains(TEXT("abc")));
		TestTrue(TEXT("http-date retry-after"), E.HasRetryAfter && FMath::IsNearlyEqual(E.RetryAfterSeconds, 30.f, 0.01f));
	}
	{
		const FSvError Net = FSvPipeline::ErrorFromResponse(FSvHttpResponse::Failed(ESvTransportFailure::Network), Now);
		const FSvError Tmo = FSvPipeline::ErrorFromResponse(FSvHttpResponse::Failed(ESvTransportFailure::Timeout), Now);
		TestTrue(TEXT("network_error"), Net.Is(SvErrorCodes::NetworkError) && Net.Status == 0 && Net.IsNetworkError());
		TestTrue(TEXT("timeout"), Tmo.Is(SvErrorCodes::Timeout) && Tmo.IsNetworkError() && Tmo.IsTransient());
	}
	double S = 0;
	TestFalse(TEXT("garbage retry-after"), FSvPipeline::ParseRetryAfter(TEXT("soon"), Now, S));
	TestTrue(TEXT("huge retry-after capped"), FSvPipeline::ParseRetryAfter(TEXT("99999999999"), Now, S) && S == 86400.0);
	TestTrue(TEXT("past date is 0"), FSvPipeline::ParseRetryAfter(TEXT("Sun, 15 Nov 2026 11:00:00 GMT"), Now, S) && S == 0.0);
	TestTrue(TEXT("500 transient"), FSvError::IsTransientStatus(500));
	TestFalse(TEXT("400 not transient"), FSvError::IsTransientStatus(400));
	return true;
}

// ─── retry policy ─────────────────────────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSvRetryPolicyTest, "SplatterVault.Unit.RetryPolicy", SV_TEST_FLAGS)
bool FSvRetryPolicyTest::RunTest(const FString& Parameters)
{
	FSvRetryPolicy P;
	P.Jitter = 0;
	TestEqual(TEXT("backoff 1"), P.Backoff(1), 0.5);
	TestEqual(TEXT("backoff 2"), P.Backoff(2), 1.0);
	TestEqual(TEXT("backoff 3"), P.Backoff(3), 2.0);
	TestEqual(TEXT("backoff capped"), P.Backoff(10), 4.0);
	P.Jitter = 1;
	P.Random = []() { return 0.0; };
	TestEqual(TEXT("full jitter low end is half"), P.Backoff(2), 0.5);
	P.Random = []() { return 1.0; };
	TestEqual(TEXT("full jitter high end is all"), P.Backoff(2), 1.0);

	double Wait = 0;
	const FSvError Net(0, SvErrorCodes::NetworkError, FString());
	TestTrue(TEXT("network retried"), P.ShouldRetry(Net, 1, Wait));
	TestFalse(TEXT("attempts capped"), P.ShouldRetry(Net, 3, Wait));
	for (const int32 Status : {408, 429, 500, 502, 503, 504})
	{
		TestTrue(FString::Printf(TEXT("%d retried"), Status), P.ShouldRetry(FSvError(Status, TEXT("x"), TEXT("x")), 1, Wait));
	}
	for (const int32 Status : {400, 401, 403, 404, 409, 413, 422, 501})
	{
		TestFalse(FString::Printf(TEXT("%d never retried"), Status), P.ShouldRetry(FSvError(Status, TEXT("x"), TEXT("x")), 1, Wait));
	}
	FSvError Later(503, TEXT("player_sign_in_disabled"), TEXT("x"));
	Later.HasRetryAfter = true;
	Later.RetryAfterSeconds = 3;
	TestTrue(TEXT("retry-after honoured"), P.ShouldRetry(Later, 1, Wait) && Wait == 3.0);
	Later.RetryAfterSeconds = 30;
	TestFalse(TEXT("retry-after over the cap is not waited out"), P.ShouldRetry(Later, 1, Wait));
	TestFalse(TEXT("None: no retries"), FSvRetryPolicy::None().ShouldRetry(Net, 1, Wait));
	return true;
}

// ─── pipeline: request building and retries (injectable clock) ───────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSvPipelineTest, "SplatterVault.Unit.Pipeline", SV_TEST_FLAGS)
bool FSvPipelineTest::RunTest(const FString& Parameters)
{
	FHarness H;
	FSvPipeline& P = H.Client->GetPipeline();
	{
		FSvCall C = FSvCall::Post(TEXT("/store/me/purchases"), TEXT("{\"sku\":\"a\"}"), ESvRetry::Safe);
		C.Headers.Add(TEXT("Idempotency-Key"), TEXT("k1"));
		C.Headers.Add(TEXT("x-api-key"), TEXT("must-not-be-sent"));
		const FSvHttpRequest R = P.BuildRequest(C, TEXT("tok"));
		TestEqual(TEXT("url"), R.Url, FString(TEXT("https://api.test/rest/store/me/purchases")));
		TestEqual(TEXT("method"), R.Method, FString(TEXT("POST")));
		TestEqual(TEXT("accept"), R.Headers.FindRef(TEXT("Accept")), FString(TEXT("application/json")));
		TestEqual(TEXT("content-type"), R.Headers.FindRef(TEXT("Content-Type")), FString(TEXT("application/json")));
		TestEqual(TEXT("bearer"), R.Headers.FindRef(TEXT("Authorization")), FString(TEXT("Bearer tok")));
		TestEqual(TEXT("idempotency"), R.Headers.FindRef(TEXT("Idempotency-Key")), FString(TEXT("k1")));
		TestFalse(TEXT("never an API key"), R.Headers.Contains(TEXT("X-API-Key")));
		TestEqual(TEXT("default timeout"), R.TimeoutSeconds, 30.0);
	}
	{
		const FSvHttpRequest R = P.BuildRequest(FSvCall::Get(TEXT("/store/me")), FString());
		TestFalse(TEXT("no body, no content-type"), R.Headers.Contains(TEXT("Content-Type")));
		TestFalse(TEXT("no credential"), R.Headers.Contains(TEXT("Authorization")));
	}

	// 503 + Retry-After 2, then 503 (backoff), then 200: three sends, waits 2 s and the backoff.
	int32 N = 0;
	H.Transport->Handler = [&N](const FSvHttpRequest&)
	{
		++N;
		if (N == 1) return ErrorReply(503, TEXT("player_sign_in_disabled"), TEXT("2"));
		if (N == 2) return ErrorReply(503, TEXT("player_sign_in_disabled"));
		return JsonReply(200, TEXT("{}"));
	};
	bool bOk = false;
	P.Send(FSvCall::Get(TEXT("/store/catalog")), FString(), [&bOk](const TSvResult<FSvHttpResponse>& R) { bOk = R.IsOk(); });
	TestTrue(TEXT("succeeds after two retries"), bOk);
	TestEqual(TEXT("three sends"), N, 3);
	TestEqual(TEXT("two waits"), H.Clock->Delays.Num(), 2);
	if (H.Clock->Delays.Num() == 2)
	{
		TestEqual(TEXT("first wait is Retry-After"), H.Clock->Delays[0], 2.0);
		// Attempt 2 backoff: 1.0 s with jitter 1 and Random 0.5 -> 0.75 s.
		TestEqual(TEXT("second wait is jittered backoff"), H.Clock->Delays[1], 0.75);
	}

	// A write without an Idempotency-Key (Retry Never) is sent once.
	N = 0;
	H.Clock->Delays.Reset();
	H.Transport->Handler = [&N](const FSvHttpRequest&)
	{
		++N;
		return ErrorReply(503, TEXT("player_sign_in_disabled"));
	};
	FSvError Seen;
	P.Send(FSvCall::Post(TEXT("/player-auth/device/new"), TEXT("{}"), ESvRetry::Never), FString(), [&Seen](const TSvResult<FSvHttpResponse>& R) { Seen = R.GetError(); });
	TestEqual(TEXT("never-retry sent once"), N, 1);
	TestEqual(TEXT("its error"), Seen.Status, 503);

	// 4xx is never retried even when safe.
	N = 0;
	H.Transport->Handler = [&N](const FSvHttpRequest&)
	{
		++N;
		return ErrorReply(400, TEXT("bad_request"));
	};
	P.Send(FSvCall::Get(TEXT("/store/me")), FString(), [](const TSvResult<FSvHttpResponse>&) {});
	TestEqual(TEXT("400 sent once"), N, 1);

	// A resend window (refresh): no new send once the window would be passed.
	N = 0;
	H.Clock->Delays.Reset();
	H.Transport->Handler = [&N, &H](const FSvHttpRequest&)
	{
		++N;
		H.Clock->Advance(25); // each send takes 25 s: a second one would start past the 25 s window
		return FSvHttpResponse::Failed(ESvTransportFailure::Timeout);
	};
	FSvCall Refresh = FSvCall::Post(TEXT("/player-auth/refresh"), TEXT("{}"), ESvRetry::Safe);
	Refresh.RetryWindowSeconds = 25.0;
	P.Send(Refresh, FString(), [&Seen](const TSvResult<FSvHttpResponse>& R) { Seen = R.GetError(); });
	TestEqual(TEXT("window stops the second send"), N, 1);
	TestTrue(TEXT("timeout error"), Seen.Is(SvErrorCodes::Timeout));
	return true;
}

// ─── models: JSON round trips of every model against example payloads ────────────────────────────────────────────

namespace
{
	template <typename T>
	bool ReadText(const FString& Text, T& Out)
	{
		return SvModels::ReadText(Text, Out);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSvModelJsonTest, "SplatterVault.Unit.ModelJson", SV_TEST_FLAGS)
bool FSvModelJsonTest::RunTest(const FString& Parameters)
{
	// PlayerProfile (nullable fields as null).
	{
		FSvPlayerProfile P;
		TestTrue(TEXT("profile parses"), ReadText(ProfileBody, P));
		TestEqual(TEXT("profile id"), P.Player.Id, FString(PlayerA));
		TestEqual(TEXT("profile name"), P.Player.DisplayName, FString(TEXT("Ann")));
		TestTrue(TEXT("null lastSignedInAt"), P.Player.LastSignedInAt.IsEmpty());
		TestTrue(TEXT("null gameKey"), P.Game.GameKey.IsEmpty());
		TestTrue(TEXT("identity"), P.HasIdentity(TEXT("device")) && !P.HasIdentity(TEXT("email")));
		FSvPlayerProfile Back;
		TestTrue(TEXT("profile round trip"), SvModels::Read(SvModels::Write(P), Back) && Back.Player.Id == P.Player.Id && Back.Identities.Num() == 1 &&
			Back.Identities[0].LinkedAt == P.Identities[0].LinkedAt);
	}
	// CodeSent.
	{
		FSvCodeSent C;
		TestTrue(TEXT("code sent"), ReadText(TEXT("{\"expiresInSeconds\":600,\"resendAfterSeconds\":60}"), C) && C.ExpiresInSeconds == 600 && C.ResendAfterSeconds == 60);
	}
	// PlayerDataAnswer: values of every JSON kind, case-sensitive keys, keysOnly entries.
	{
		const FString Text = TEXT("{\"data\":{\"player\":{\"score\":{\"value\":42,\"version\":7,\"updatedAt\":\"2026-11-15T12:00:00.000Z\"},\"Score\":{\"value\":\"upper\",\"version\":8,\"updatedAt\":null},")
			TEXT("\"settings\":{\"value\":{\"fov\":90,\"name\":\"x\"},\"version\":9,\"updatedAt\":\"2026-11-15T12:00:00.000Z\"},\"flags\":{\"value\":[true,false],\"version\":10,\"updatedAt\":\"2026-11-15T12:00:00.000Z\"},")
			TEXT("\"bare\":{\"version\":11,\"updatedAt\":\"2026-11-15T12:00:00.000Z\"}},\"readonly\":{\"profile.tier\":{\"value\":\"bronze\",\"version\":1,\"updatedAt\":\"2026-11-15T12:00:00.000Z\"}}}}");
		FSvPlayerData D;
		TestTrue(TEXT("player data parses"), SvModels::ReadPlayerDataText(Text, D));
		TestEqual(TEXT("five player keys (case-sensitive)"), D.Player.Entries.Num(), 5);
		const FSvDataEntry* Score = D.Player.Find(TEXT("score"));
		const FSvDataEntry* Upper = D.Player.Find(TEXT("Score"));
		TestTrue(TEXT("score"), Score && USplatterVaultBlueprintLibrary::GetDataInt(*Score, 0) == 42 && Score->Version == 7);
		TestTrue(TEXT("Score"), Upper && USplatterVaultBlueprintLibrary::GetDataString(*Upper, TEXT("")) == TEXT("upper") && Upper->UpdatedAt.IsEmpty());
		const FSvDataEntry* Settings = D.Player.Find(TEXT("settings"));
		TestTrue(TEXT("object value"), Settings && USplatterVaultBlueprintLibrary::GetDataObjectInt(*Settings, TEXT("fov"), 0) == 90);
		const FSvDataEntry* Bare = D.Player.Find(TEXT("bare"));
		TestTrue(TEXT("keysOnly entry has no value"), Bare && Bare->ValueJson.IsEmpty() && Bare->Version == 11);
		TestEqual(TEXT("versionOf missing is 0"), D.Player.VersionOf(TEXT("nope")), (int64)0);
		const FSvDataEntry* Tier = D.Readonly.Find(TEXT("profile.tier"));
		TestTrue(TEXT("readonly"), Tier && USplatterVaultBlueprintLibrary::GetDataString(*Tier, TEXT("")) == TEXT("bronze"));
		FSvPlayerData Back;
		TestTrue(TEXT("player data round trip"), SvModels::ReadPlayerDataText(SvModels::WritePlayerDataText(D), Back));
		TestEqual(TEXT("round trip keys"), Back.Player.Entries.Num(), 5);
		const FSvDataEntry* Flags = Back.Player.Find(TEXT("flags"));
		TestTrue(TEXT("round trip array value"), Flags && Flags->ValueJson == TEXT("[true,false]"));
	}
	// GameDataAnswer.
	{
		FSvDataStore S;
		TestTrue(TEXT("game data"), SvModels::ReadGameDataText(TEXT("{\"data\":{\"motd\":{\"value\":\"hi\",\"version\":3,\"updatedAt\":\"2026-11-15T12:00:00.000Z\"}}}"), S));
		TestTrue(TEXT("motd"), S.Find(TEXT("motd")) && S.Find(TEXT("motd"))->ValueJson == TEXT("\"hi\""));
	}
	// DataOpResults.
	{
		FSvDataWriteResult W;
		TestTrue(TEXT("write result"), ReadText(TEXT("{\"results\":[{\"store\":\"player\",\"key\":\"score\",\"version\":12},{\"store\":\"player\",\"key\":\"old\",\"version\":null},{\"store\":\"player\",\"key\":\"n\",\"version\":13,\"value\":5}]}"), W));
		TestEqual(TEXT("three results"), W.Results.Num(), 3);
		if (W.Results.Num() == 3)
		{
			TestTrue(TEXT("set version"), W.Results[0].HasVersion && W.Results[0].Version == 12 && W.Results[0].ValueJson.IsEmpty());
			TestFalse(TEXT("delete: no version"), W.Results[1].HasVersion);
			TestTrue(TEXT("inc value"), W.Results[2].ValueJson == TEXT("5"));
		}
		FSvDataWriteResult Back;
		TestTrue(TEXT("write round trip"), SvModels::Read(SvModels::Write(W), Back) && Back.Results.Num() == 3 && !Back.Results[1].HasVersion && Back.Results[2].ValueJson == TEXT("5"));
	}
	// StoreCatalog (nullable cap / maxOwned, custom object, bundle).
	{
		const FString Text = TEXT("{\"settings\":{\"playerPurchases\":true,\"playerConsume\":false},\"currencies\":[{\"code\":\"COINS\",\"name\":\"Coins\",\"cap\":1000000},{\"code\":\"GEMS\",\"name\":\"Gems\",\"cap\":null}],")
			TEXT("\"items\":[{\"sku\":\"sword_basic\",\"name\":\"Sword\",\"kind\":\"item\",\"stackable\":false,\"maxOwned\":1,\"prices\":[{\"currency\":\"COINS\",\"amount\":250}],\"contents\":[],\"tags\":[\"weapon\"],\"custom\":{\"rarity\":\"common\"},\"purchasable\":true,\"consumable\":false},")
			TEXT("{\"sku\":\"starter_bundle\",\"name\":\"Starter\",\"kind\":\"bundle\",\"stackable\":false,\"maxOwned\":null,\"prices\":[{\"currency\":\"COINS\",\"amount\":500}],\"contents\":[{\"sku\":\"potion_small\",\"quantity\":5},{\"currency\":\"GEMS\",\"quantity\":50}],\"tags\":[],\"custom\":null,\"purchasable\":true,\"consumable\":false}]}");
		FSvStoreCatalog C;
		TestTrue(TEXT("catalog parses"), ReadText(Text, C));
		TestTrue(TEXT("settings"), C.Settings.PlayerPurchases && !C.Settings.PlayerConsume);
		TestTrue(TEXT("cap"), C.Currencies.Num() == 2 && C.Currencies[0].HasCap && C.Currencies[0].Cap == 1000000 && !C.Currencies[1].HasCap);
		const FSvCatalogItem* Sword = C.Find(TEXT("sword_basic"));
		int64 Price = 0;
		TestTrue(TEXT("sword"), Sword && Sword->HasMaxOwned && Sword->MaxOwned == 1 && Sword->PriceIn(TEXT("COINS"), Price) && Price == 250 && Sword->Purchasable);
		TestTrue(TEXT("custom json"), Sword && Sword->CustomJson == TEXT("{\"rarity\":\"common\"}") && Sword->Tags.Num() == 1);
		const FSvCatalogItem* Bundle = C.Find(TEXT("starter_bundle"));
		TestTrue(TEXT("bundle"), Bundle && !Bundle->HasMaxOwned && Bundle->CustomJson.IsEmpty() && Bundle->Contents.Num() == 2 && Bundle->Contents[1].Currency == TEXT("GEMS"));
		FSvStoreCatalog Back;
		TestTrue(TEXT("catalog round trip"), SvModels::Read(SvModels::Write(C), Back));
		TestTrue(TEXT("catalog round trip fields"), Back.Items.Num() == 2 && Back.Items[0].CustomJson == C.Items[0].CustomJson && !Back.Currencies[1].HasCap &&
			Back.Items[1].Contents.Num() == 2 && Back.Items[0].Prices[0].Amount == 250);
	}
	// StoreHoldings.
	{
		const FString Text = TEXT("{\"player\":{\"id\":\"0b6f2a8e-1c1d-4a59-9b3c-7f1f3e2a9c01\"},\"balances\":[{\"currency\":\"COINS\",\"amount\":960}],")
			TEXT("\"items\":[{\"instanceId\":\"7d5c1b6e-0000-4000-8000-000000000001\",\"sku\":\"potion_small\",\"quantity\":2,\"custom\":null,\"acquiredAt\":\"2026-11-15T12:00:00.000Z\"},")
			TEXT("{\"instanceId\":\"7d5c1b6e-0000-4000-8000-000000000002\",\"sku\":\"raffle_ticket\",\"quantity\":1,\"custom\":{\"n\":1},\"acquiredAt\":\"2026-11-15T12:00:00.000Z\"}]}");
		FSvStoreHoldings Hd;
		TestTrue(TEXT("holdings"), ReadText(Text, Hd));
		TestEqual(TEXT("holdings player"), Hd.PlayerId, FString(PlayerA));
		TestEqual(TEXT("balance"), Hd.BalanceOf(TEXT("COINS")), (int64)960);
		TestEqual(TEXT("count"), Hd.CountOf(TEXT("potion_small")), (int64)2);
		TestTrue(TEXT("item custom"), Hd.Items.Num() == 2 && Hd.Items[1].CustomJson == TEXT("{\"n\":1}") && Hd.Items[0].CustomJson.IsEmpty());
		FSvStoreHoldings Back;
		TestTrue(TEXT("holdings round trip"), SvModels::Read(SvModels::Write(Hd), Back) && Back.PlayerId == Hd.PlayerId && Back.Items[1].CustomJson == Hd.Items[1].CustomJson);
	}
	// StoreTxnResult.
	{
		const FString Text = TEXT("{\"txn\":{\"id\":\"11111111-2222-4333-8444-555555555555\",\"kind\":\"purchase\",\"createdAt\":\"2026-11-15T12:00:00.000Z\",\"reason\":null,\"reverses\":null},")
			TEXT("\"changes\":[{\"currency\":\"COINS\",\"delta\":-40,\"after\":960},{\"sku\":\"potion_small\",\"instanceId\":\"7d5c1b6e-0000-4000-8000-000000000001\",\"delta\":2,\"after\":2}],")
			TEXT("\"balances\":[{\"currency\":\"COINS\",\"amount\":960}],\"items\":[{\"instanceId\":\"7d5c1b6e-0000-4000-8000-000000000001\",\"sku\":\"potion_small\",\"quantity\":2,\"custom\":null,\"acquiredAt\":\"2026-11-15T12:00:00.000Z\"}],")
			TEXT("\"shortfall\":[],\"replayed\":false}");
		FSvStoreTxnResult T;
		TestTrue(TEXT("txn parses"), ReadText(Text, T));
		TestTrue(TEXT("txn fields"), T.Txn.Kind == TEXT("purchase") && T.Txn.Reason.IsEmpty() && T.Changes.Num() == 2 && T.Changes[0].Delta == -40 && T.Changes[1].Sku == TEXT("potion_small"));
		TestFalse(TEXT("not replayed"), T.Replayed);
		FSvStoreTxnResult Back;
		TestTrue(TEXT("txn round trip"), SvModels::Read(SvModels::Write(T), Back) && Back.Txn.Id == T.Txn.Id && Back.Changes.Num() == 2 && Back.Items.Num() == 1 && Back.Balances[0].Amount == 960);
	}
	// StoreHistoryPage.
	{
		const FString Text = TEXT("{\"txns\":[{\"id\":\"11111111-2222-4333-8444-555555555555\",\"kind\":\"consume\",\"createdAt\":\"2026-11-15T12:00:00.000Z\",\"reverses\":null,\"changes\":[{\"sku\":\"potion_small\",\"delta\":-1,\"after\":1}],\"shortfall\":[]},")
			TEXT("{\"id\":\"11111111-2222-4333-8444-555555555556\",\"kind\":\"reversal\",\"createdAt\":\"2026-11-15T12:00:00.000Z\",\"reverses\":\"11111111-2222-4333-8444-555555555555\",\"changes\":[],\"shortfall\":[{\"currency\":\"COINS\",\"missing\":5}]}],\"next\":null}");
		FSvStoreHistoryPage Pg;
		TestTrue(TEXT("history parses"), ReadText(Text, Pg));
		TestTrue(TEXT("history fields"), Pg.Txns.Num() == 2 && Pg.Next.IsEmpty() && Pg.Txns[1].Reverses == Pg.Txns[0].Id && Pg.Txns[1].Shortfall[0].Missing == 5);
		FSvStoreHistoryPage Back;
		TestTrue(TEXT("history round trip"), SvModels::Read(SvModels::Write(Pg), Back) && Back.Txns.Num() == 2 && Back.Txns[0].Changes[0].After == 1 && Back.Next.IsEmpty());
	}
	// MetaPurchaseSyncAnswer.
	{
		FSvMetaSyncResult M;
		TestTrue(TEXT("meta sync"), ReadText(TEXT("{\"granted\":1,\"alreadyGranted\":0,\"unmapped\":1,\"consumePending\":0}"), M) && M.Granted == 1 && M.Unmapped == 1);
		FSvMetaSyncResult Back;
		TestTrue(TEXT("meta sync round trip"), SvModels::Read(SvModels::Write(M), Back) && Back.Granted == 1 && Back.Unmapped == 1);
	}
	// Pending store change (the SDK's own stored shape).
	{
		FSvPendingStoreChange C;
		C.IdempotencyKey = TEXT("k");
		C.Kind = TEXT("purchase");
		C.Path = TEXT("/store/me/purchases");
		C.Body = TEXT("{\"sku\":\"a\",\"quantity\":1}");
		C.PlayerId = PlayerA;
		C.CreatedAt = TEXT("2026-11-15T12:00:00.000Z");
		FSvPendingStoreChange Back;
		TestTrue(TEXT("pending round trip"), SvModels::Read(SvModels::Write(C), Back) && Back.Body == C.Body && Back.IdempotencyKey == C.IdempotencyKey && Back.PlayerId == C.PlayerId);
	}
	// Unknown fields are ignored (forward compatible), and a wrong type is refused rather than guessed.
	{
		FSvCodeSent C;
		TestTrue(TEXT("extra fields ignored"), ReadText(TEXT("{\"expiresInSeconds\":600,\"resendAfterSeconds\":60,\"newField\":{\"a\":1}}"), C) && C.ExpiresInSeconds == 600);
	}
	return true;
}

// ─── raw JSON: case-sensitive keys, exact values ─────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSvRawJsonTest, "SplatterVault.Unit.RawJson", SV_TEST_FLAGS)
bool FSvRawJsonTest::RunTest(const FString& Parameters)
{
	FSvRawJson R;
	TestTrue(TEXT("parses"), R.Parse(TEXT(" {\"a\":1,\"A\":[true,null,-1.5e3],\"s\":\"x\\\"\\u00e9\\ud83d\\ude00\\n\",\"o\":{\"k\":{}}} ")));
	const int32 Root = R.Root();
	TestEqual(TEXT("four members, case kept"), R.Members(Root).Num(), 4);
	int64 A = 0;
	TestTrue(TEXT("a"), R.Int64(R.Member(Root, TEXT("a")), A) && A == 1);
	TestEqual(TEXT("A is the array"), R.Raw(R.Member(Root, TEXT("A"))), FString(TEXT("[true,null,-1.5e3]")));
	TestEqual(TEXT("array elements"), R.Elements(R.Member(Root, TEXT("A"))).Num(), 3);
	TestEqual(TEXT("string decoded"), R.String(R.Member(Root, TEXT("s"))), FString(TEXT("x\"\u00e9")) + FString(TEXT("\U0001F600")) + TEXT("\n"));
	TestEqual(TEXT("missing member"), R.Member(Root, TEXT("S")), FSvRawJson::None);
	TestEqual(TEXT("nested raw"), R.Raw(R.Member(Root, TEXT("o"))), FString(TEXT("{\"k\":{}}")));
	for (const TCHAR* Bad : {TEXT(""), TEXT("{"), TEXT("{\"a\" 1}"), TEXT("[1,]"), TEXT("01x"), TEXT("tru"), TEXT("\"\\q\""), TEXT("{} {}")})
	{
		FSvRawJson B;
		TestFalse(FString::Printf(TEXT("refuses %s"), Bad), B.Parse(Bad));
	}

	// Player data keys that differ only in case stay apart, and values keep nested keys that differ in case.
	FSvPlayerData D;
	TestTrue(TEXT("player data"), SvModels::ReadPlayerDataText(TEXT("{\"data\":{\"player\":{\"score\":{\"value\":1,\"version\":2,\"updatedAt\":null},\"Score\":{\"value\":{\"x\":1,\"X\":2},\"version\":3,\"updatedAt\":null}},\"readonly\":{}}}"), D));
	TestEqual(TEXT("both keys"), D.Player.Entries.Num(), 2);
	const FSvDataEntry* Upper = D.Player.Find(TEXT("Score"));
	TestTrue(TEXT("value verbatim"), Upper && Upper->ValueJson == TEXT("{\"x\":1,\"X\":2}") && Upper->Version == 3);
	FSvDataOps Ops;
	Ops.SetJson(TEXT("Score"), TEXT(" {\"x\":1,\"X\":2} "));
	TestEqual(TEXT("write keeps the value verbatim"), Ops.ToJson(), FString(TEXT("{\"ops\":[{\"op\":\"set\",\"store\":\"player\",\"key\":\"Score\",\"value\":{\"x\":1,\"X\":2}}]}")));
	return true;
}

// ─── data ops ─────────────────────────────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSvDataOpsTest, "SplatterVault.Unit.DataOps", SV_TEST_FLAGS)
bool FSvDataOpsTest::RunTest(const FString& Parameters)
{
	FSvDataOps Ops;
	Ops.SetNumber(TEXT("score"), 10, 0).SetJson(TEXT("settings"), TEXT("{\"fov\":90}"), 4).Delete(TEXT("tutorial"), 2).Inc(TEXT("matches"), 1, FString(), 0, 1000, 5);
	TestTrue(TEXT("no build error"), Ops.GetBuildError().IsEmpty());
	TestTrue(TEXT("all have ifVersion"), Ops.AllHaveIfVersion());
	const TSharedPtr<FJsonObject> Body = SvJson::ParseObject(Ops.ToJson());
	const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
	TestTrue(TEXT("ops array"), Body.IsValid() && Body->TryGetArrayField(TEXT("ops"), List) && List && List->Num() == 4);
	if (List && List->Num() == 4)
	{
		const TSharedPtr<FJsonObject> Set = (*List)[0]->AsObject();
		TestEqual(TEXT("op"), SvJson::GetString(Set, TEXT("op")), FString(TEXT("set")));
		TestEqual(TEXT("store"), SvJson::GetString(Set, TEXT("store")), FString(TEXT("player")));
		int64 V = -1;
		TestTrue(TEXT("ifVersion 0 is sent"), SvJson::GetInt64(Set, TEXT("ifVersion"), V) && V == 0);
		const TSharedPtr<FJsonObject> Inc = (*List)[3]->AsObject();
		int64 Min = -1, Max = -1, By = 0;
		TestTrue(TEXT("inc fields"), SvJson::GetInt64(Inc, TEXT("by"), By) && By == 1 && SvJson::GetInt64(Inc, TEXT("min"), Min) && Min == 0 && SvJson::GetInt64(Inc, TEXT("max"), Max) && Max == 1000);
		TestFalse(TEXT("no empty field"), Inc->HasField(TEXT("field")));
	}
	FSvDataOps NoVersion;
	NoVersion.SetString(TEXT("name"), TEXT("x"));
	TestFalse(TEXT("without ifVersion"), NoVersion.AllHaveIfVersion());

	TestTrue(TEXT("valid key"), FSvDataOps::IsValidKey(TEXT("profile.tier:2-a_b")));
	TestFalse(TEXT("reserved sv."), FSvDataOps::IsValidKey(TEXT("SV.thing")));
	TestFalse(TEXT("__proto__"), FSvDataOps::IsValidKey(TEXT("__proto__")));
	TestFalse(TEXT("leading dot"), FSvDataOps::IsValidKey(TEXT(".a")));
	TestFalse(TEXT("space"), FSvDataOps::IsValidKey(TEXT("a b")));
	TestFalse(TEXT("too long"), FSvDataOps::IsValidKey(FString::ChrN(129, TEXT('a'))));

	FSvDataOps Bad;
	Bad.SetJson(TEXT("x"), TEXT("not json"));
	TestFalse(TEXT("bad json refused"), Bad.GetBuildError().IsEmpty());
	FSvDataOps Null;
	Null.SetJson(TEXT("x"), TEXT("null"));
	TestFalse(TEXT("null refused"), Null.GetBuildError().IsEmpty());
	FSvDataOps BadKey;
	BadKey.Delete(TEXT("sv.reserved"));
	TestFalse(TEXT("bad key refused"), BadKey.GetBuildError().IsEmpty());
	FSvDataOps Many;
	for (int32 i = 0; i < 26; ++i) Many.Inc(FString::Printf(TEXT("k%d"), i), 1);
	TestTrue(TEXT("26 ops refused"), Many.Num() == 25 && !Many.GetBuildError().IsEmpty());

	// Blueprint-made ops make the same JSON.
	TArray<FSvDataOp> Bp;
	Bp.Add(USplatterVaultBlueprintLibrary::MakeSetIntDataOp(TEXT("score"), 10, 0));
	Bp.Add(USplatterVaultBlueprintLibrary::MakeSetStringDataOp(TEXT("name"), TEXT("Ann \"A\""), -1));
	Bp.Add(USplatterVaultBlueprintLibrary::MakeDeleteDataOp(TEXT("tutorial"), -1));
	Bp.Add(USplatterVaultBlueprintLibrary::MakeIncDataOp(TEXT("stats"), -2, TEXT("lives"), true, 0, false, 0, -1));
	const FSvDataOps FromBp(Bp);
	TestTrue(TEXT("bp ops valid"), FromBp.GetBuildError().IsEmpty() && FromBp.Num() == 4);
	const TSharedPtr<FJsonObject> BpBody = SvJson::ParseObject(FromBp.ToJson());
	const TArray<TSharedPtr<FJsonValue>>* BpList = nullptr;
	if (BpBody.IsValid() && BpBody->TryGetArrayField(TEXT("ops"), BpList) && BpList && BpList->Num() == 4)
	{
		TestEqual(TEXT("bp string value"), (*BpList)[1]->AsObject()->GetStringField(TEXT("value")), FString(TEXT("Ann \"A\"")));
		TestFalse(TEXT("bp no ifVersion"), (*BpList)[1]->AsObject()->HasField(TEXT("ifVersion")));
		TestEqual(TEXT("bp inc field"), (*BpList)[3]->AsObject()->GetStringField(TEXT("field")), FString(TEXT("lives")));
		TestFalse(TEXT("bp inc no max"), (*BpList)[3]->AsObject()->HasField(TEXT("max")));
	}
	else
	{
		AddError(TEXT("bp ops JSON"));
	}
	return true;
}

// ─── storage ──────────────────────────────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSvStorageTest, "SplatterVault.Unit.Storage", SV_TEST_FLAGS)
bool FSvStorageTest::RunTest(const FString& Parameters)
{
	// Memory: case-sensitive keys.
	{
		FSvMemorySecureStore M;
		FString V, E;
		M.Set(TEXT("Key"), TEXT("a"), E);
		M.Set(TEXT("key"), TEXT("b"), E);
		TestTrue(TEXT("memory Key"), M.Get(TEXT("Key"), V, E) == ESvStoreRead::Found && V == TEXT("a"));
		TestTrue(TEXT("memory key"), M.Get(TEXT("key"), V, E) == ESvStoreRead::Found && V == TEXT("b"));
		M.Delete(TEXT("Key"), E);
		TestTrue(TEXT("memory delete"), M.Get(TEXT("Key"), V, E) == ESvStoreRead::NotFound && M.Get(TEXT("key"), V, E) == ESvStoreRead::Found);
	}

	const FString Root = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("SplatterVault"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString Secret = TEXT("{\"v\":1,\"secret\":\"svpd1_TESTSECRETVALUE\",\"player\":\"p\"}");

	// Obfuscated file store: round trip, nothing readable on disk, tamper and a changed key read as Error (never missing).
	{
		TArray<uint8> Salt;
		const FString Dir = FPaths::Combine(Root, TEXT("obf"));
		TestTrue(TEXT("salt"), FSvObfuscatingProtector::LoadOrCreateSalt(FPaths::Combine(Dir, TEXT("store.salt")), Salt) == ESvSaltResult::Created && Salt.Num() == 32);
		TArray<uint8> Again;
		TestTrue(TEXT("salt kept"), FSvObfuscatingProtector::LoadOrCreateSalt(FPaths::Combine(Dir, TEXT("store.salt")), Again) == ESvSaltResult::Loaded && Again == Salt);
		FSvProtectedFileStore Store(Dir, MakeShared<FSvObfuscatingProtector>(TEXT("material-1"), Salt));
		FString V, E;
		TestTrue(TEXT("obf missing"), Store.Get(TEXT("guest"), V, E) == ESvStoreRead::NotFound);
		TestTrue(TEXT("obf set"), Store.Set(TEXT("guest"), Secret, E));
		TestTrue(TEXT("obf get"), Store.Get(TEXT("guest"), V, E) == ESvStoreRead::Found && V == Secret);
		TArray<uint8> Raw;
		FFileHelper::LoadFileToArray(Raw, *Store.PathFor(TEXT("guest")));
		const FString RawText = SvCrypto::FromUtf8(Raw);
		TestFalse(TEXT("not plain text on disk"), RawText.Contains(TEXT("TESTSECRET")));
		FSvProtectedFileStore Other(Dir, MakeShared<FSvObfuscatingProtector>(TEXT("material-2"), Salt));
		TestTrue(TEXT("other key material: Error, not missing"), Other.Get(TEXT("guest"), V, E) == ESvStoreRead::Error);
		TestFalse(TEXT("error text never has the value"), E.Contains(TEXT("TESTSECRET")));
		Raw[Raw.Num() / 2] ^= 0x5a;
		FFileHelper::SaveArrayToFile(Raw, *Store.PathFor(TEXT("guest")));
		TestTrue(TEXT("tampered: Error"), Store.Get(TEXT("guest"), V, E) == ESvStoreRead::Error);
		TestTrue(TEXT("obf delete"), Store.Delete(TEXT("guest"), E) && Store.Get(TEXT("guest"), V, E) == ESvStoreRead::NotFound);
		TestTrue(TEXT("delete missing is fine"), Store.Delete(TEXT("guest"), E));
	}
#if PLATFORM_WINDOWS
	// DPAPI file store.
	{
		FSvProtectedFileStore Store(FPaths::Combine(Root, TEXT("dpapi")), MakeShared<FSvDpapiProtector>());
		FString V, E;
		TestTrue(TEXT("dpapi set"), Store.Set(TEXT("refresh"), Secret, E));
		TestTrue(TEXT("dpapi get"), Store.Get(TEXT("refresh"), V, E) == ESvStoreRead::Found && V == Secret);
		TestTrue(TEXT("dpapi is OS-backed"), Store.IsHardwareOrOsBacked() && Store.GetProtection() == TEXT("dpapi"));
		TArray<uint8> Raw;
		FFileHelper::LoadFileToArray(Raw, *Store.PathFor(TEXT("refresh")));
		TestFalse(TEXT("dpapi: not plain text"), SvCrypto::FromUtf8(Raw).Contains(TEXT("TESTSECRET")));
		// A value sealed for another key name does not open under this one (the entropy binds it).
		IFileManager::Get().Copy(*Store.PathFor(TEXT("other")), *Store.PathFor(TEXT("refresh")));
		TestTrue(TEXT("dpapi: moved file is Error"), Store.Get(TEXT("other"), V, E) == ESvStoreRead::Error);
	}
#endif
	// The platform store answers and says how it protects values.
	{
		FString Warning;
		const FString Name = TEXT("automation-") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const TSharedRef<ISvSecureStore> Platform = SvCreatePlatformSecureStore(Name, &Warning);
		FString V, E;
		TestTrue(TEXT("platform set/get"), Platform->Set(TEXT("k"), TEXT("v"), E) && Platform->Get(TEXT("k"), V, E) == ESvStoreRead::Found && V == TEXT("v"));
		TestTrue(TEXT("platform delete"), Platform->Delete(TEXT("k"), E));
#if PLATFORM_WINDOWS
		TestEqual(TEXT("windows uses dpapi"), Platform->GetProtection(), FString(TEXT("dpapi")));
		TestTrue(TEXT("no warning on windows"), Warning.IsEmpty());
		IFileManager::Get().DeleteDirectory(*FPaths::Combine(FPlatformProcess::UserSettingsDir(), TEXT("SplatterVault"), FApp::GetProjectName(), Name), false, true);
#endif
	}
	IFileManager::Get().DeleteDirectory(*Root, false, true);
	return true;
}

// ─── auth flows (fake transport, test clock) ──────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSvGuestFlowTest, "SplatterVault.Unit.GuestFlow", SV_TEST_FLAGS)
bool FSvGuestFlowTest::RunTest(const FString& Parameters)
{
	TSharedPtr<ISvSecureStore> Shared = MakeShared<FSvMemorySecureStore>();
	FHarness H(Shared);
	const int64 Iat = Unix(H.Clock->Wall);
	const FString Token = MakeToken(PlayerA, Iat, Iat + 3600, TEXT("device"), TEXT("1"));
	const FString Secret = TEXT("svpd1_SECRETSECRETSECRET");
	H.Transport->bDeferred = true;
	H.Transport->Handler = [&](const FSvHttpRequest& R)
	{
		if (R.Url.EndsWith(TEXT("/player-auth/device/new"))) return JsonReply(200, SignInBody(Token, PlayerA, true, FString::Printf(TEXT(",\"deviceSecret\":\"%s\""), *Secret)));
		if (R.Url.EndsWith(TEXT("/player-auth/device"))) return JsonReply(200, SignInBody(Token, PlayerA, false));
		return ErrorReply(404, TEXT("not_found"));
	};
	int32 SignedIn = 0;
	H.Client->Auth().OnSignedIn.AddLambda([&SignedIn](const FSvSignInResult&) { ++SignedIn; });
	// Two callers at once on a first launch share ONE new guest.
	TSvFuture<FSvSignInResult> F1 = H.Client->Auth().SignInAsGuest(TEXT("Ann"));
	TSvFuture<FSvSignInResult> F2 = H.Client->Auth().SignInAsGuest(TEXT("Ann"));
	H.Transport->Pump();
	const TSvResult<FSvSignInResult> R1 = Now(MoveTemp(F1));
	const TSvResult<FSvSignInResult> R2 = Now(MoveTemp(F2));
	TestTrue(TEXT("guest signed in"), R1.IsOk() && R2.IsOk());
	TestEqual(TEXT("one device/new"), H.Transport->CountPath(TEXT("/player-auth/device/new")), 1);
	TestTrue(TEXT("new player, saved, renewable"), R1.GetValue().Player.NewPlayer && R1.GetValue().SavedToDevice && R1.GetValue().Renewable);
	TestEqual(TEXT("method"), R1.GetValue().Method, FString(SvProviders::Device));
	TestEqual(TEXT("player token in memory"), H.Client->Auth().GetPlayerToken(), Token);
	TestEqual(TEXT("signed-in event once"), SignedIn, 1);
	TestEqual(TEXT("display name sent"), SvJson::GetString(BodyOf(H.Transport->Requests[0]), TEXT("displayName")), FString(TEXT("Ann")));
	int64 GameId = 0;
	TestTrue(TEXT("gameId sent"), SvJson::GetInt64(BodyOf(H.Transport->Requests[0]), TEXT("gameId"), GameId) && GameId == 4242);
	TestTrue(TEXT("has guest"), H.Client->Auth().HasGuestOnDevice().GetValue());

	// A second launch (a new client on the same store) restores the guest with the device secret, never a new guest.
	FHarness H2(Shared);
	H2.Transport->Handler = H.Transport->Handler;
	const TSvResult<bool> Restored = Now(H2.Client->Auth().Restore());
	TestTrue(TEXT("restored"), Restored.IsOk() && Restored.GetValue());
	TestEqual(TEXT("restore used the device secret"), H2.Transport->CountPath(TEXT("/player-auth/device")), 1);
	TestEqual(TEXT("no new guest on restore"), H2.Transport->CountPath(TEXT("/player-auth/device/new")), 0);
	TestEqual(TEXT("secret sent"), SvJson::GetString(BodyOf(H2.Transport->Requests[0]), TEXT("deviceSecret")), Secret);
	TestEqual(TEXT("same player"), H2.Client->Auth().GetPlayerId(), FString(PlayerA));
	TestTrue(TEXT("CreateGuest refuses over an existing guest"), Now(H2.Client->Auth().CreateGuest()).GetError().Is(SvErrorCodes::InvalidState));

	// Signed out: restore answers false, and the guest secret stays.
	Now(H2.Client->Auth().SignOut());
	FHarness H3(Shared);
	H3.Transport->Handler = H.Transport->Handler;
	const TSvResult<bool> After = Now(H3.Client->Auth().Restore());
	TestTrue(TEXT("signed out: restore false"), After.IsOk() && !After.GetValue());
	TestTrue(TEXT("guest secret kept after sign-out"), H3.Client->Auth().HasGuestOnDevice().GetValue());
	TestEqual(TEXT("nothing sent"), H3.Transport->Requests.Num(), 0);

	TestTrue(TEXT("logs never carry the token"), H.LogsFreeOf(Token) && H2.LogsFreeOf(Token));
	TestTrue(TEXT("logs never carry the secret"), H.LogsFreeOf(Secret) && H2.LogsFreeOf(Secret));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSvStorageUnreadableTest, "SplatterVault.Unit.StorageUnreadable", SV_TEST_FLAGS)
bool FSvStorageUnreadableTest::RunTest(const FString& Parameters)
{
	TSharedRef<FFlakyStore> Flaky = MakeShared<FFlakyStore>();
	Flaky->UnreadableSuffixes.Add(TEXT("device_secret"));
	FHarness H(Flaky);
	H.Transport->Handler = [](const FSvHttpRequest&) { return ErrorReply(500, TEXT("internal_error")); };
	int32 Failures = 0;
	H.Client->Auth().OnStorageFailed.AddLambda([&Failures](const FSvStorageFailure& F) { ++Failures; });
	const TSvResult<FSvSignInResult> R = Now(H.Client->Auth().SignInAsGuest());
	TestTrue(TEXT("storage_unreadable"), R.IsError() && R.GetError().Is(SvErrorCodes::StorageUnreadable));
	TestEqual(TEXT("no new guest made over the unreadable one"), H.Transport->Requests.Num(), 0);
	TestTrue(TEXT("storage failure raised"), Failures > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSvRefreshSingleFlightTest, "SplatterVault.Unit.RefreshSingleFlight", SV_TEST_FLAGS)
bool FSvRefreshSingleFlightTest::RunTest(const FString& Parameters)
{
	FHarness H;
	const int64 Iat = Unix(H.Clock->Wall);
	const FString Token1 = MakeToken(PlayerA, Iat, Iat + 3600, TEXT("email"), TEXT("1"));
	const FString Token2 = MakeToken(PlayerA, Iat + 4000, Iat + 7600, TEXT("email"), TEXT("2"));
	const FString Refresh1 = TEXT("svpr1_REFRESHONE");
	const FString Refresh2 = TEXT("svpr1_REFRESHTWO");
	H.Transport->bDeferred = true;
	H.Transport->Handler = [&](const FSvHttpRequest& R)
	{
		if (R.Url.EndsWith(TEXT("/player-auth/email/password")))
		{
			return JsonReply(200, SignInBody(Token1, PlayerA, false, FString::Printf(TEXT(",\"refreshToken\":\"%s\",\"refreshExpiresAt\":\"2026-12-15T12:00:00.000Z\""), *Refresh1)));
		}
		if (R.Url.EndsWith(TEXT("/player-auth/refresh")))
		{
			return JsonReply(200, SignInBody(Token2, PlayerA, false, FString::Printf(TEXT(",\"refreshToken\":\"%s\",\"refreshExpiresAt\":\"2026-12-15T12:00:00.000Z\""), *Refresh2)));
		}
		if (R.Url.EndsWith(TEXT("/player-auth/me")))
		{
			const FString Auth = R.Headers.FindRef(TEXT("Authorization"));
			if (Auth == TEXT("Bearer ") + Token1) return JsonReply(401, TEXT("{\"error\":\"token_expired\",\"message\":\"Token expired\"}"));
			return JsonReply(200, ProfileBody);
		}
		return ErrorReply(404, TEXT("not_found"));
	};
	TSvFuture<FSvSignInResult> SignIn = H.Client->Auth().SignInWithPassword(TEXT("ann@example.com"), TEXT("correct horse"));
	H.Transport->Pump();
	TestTrue(TEXT("signed in, renewable"), SignIn.IsReady() && SignIn.Get().IsOk() && SignIn.Get().GetValue().Renewable);

	// The token runs out on the device clock: both requests wait for ONE refresh, then go with the new token.
	H.Clock->Advance(4000);
	TSvFuture<FSvPlayerProfile> A = H.Client->Auth().GetMe();
	TSvFuture<FSvPlayerProfile> B = H.Client->Auth().GetMe();
	H.Transport->Pump();
	TestTrue(TEXT("both answered"), A.IsReady() && B.IsReady() && A.Get().IsOk() && B.Get().IsOk());
	TestEqual(TEXT("exactly one refresh"), H.Transport->CountPath(TEXT("/player-auth/refresh")), 1);
	TestEqual(TEXT("refresh sent the stored token"), SvJson::GetString(BodyOf(H.Transport->Requests[1]), TEXT("refreshToken")), Refresh1);
	TestEqual(TEXT("new token in use"), H.Client->Auth().GetPlayerToken(), Token2);
	for (const FSvHttpRequest& R : H.Transport->Requests)
	{
		if (R.Url.EndsWith(TEXT("/player-auth/me"))) TestEqual(TEXT("me sent with the new token"), R.Headers.FindRef(TEXT("Authorization")), TEXT("Bearer ") + Token2);
	}

	// The API answers 401 token_expired early (the device clock says it is fine): renewed once and sent again.
	FHarness H2;
	const int64 Iat2 = Unix(H2.Clock->Wall);
	const FString T1 = MakeToken(PlayerA, Iat2, Iat2 + 3600, TEXT("email"), TEXT("a"));
	const FString T2 = MakeToken(PlayerA, Iat2, Iat2 + 3600, TEXT("email"), TEXT("b"));
	H2.Transport->Handler = [&](const FSvHttpRequest& R)
	{
		if (R.Url.EndsWith(TEXT("/player-auth/email/password")))
		{
			return JsonReply(200, SignInBody(T1, PlayerA, false, TEXT(",\"refreshToken\":\"svpr1_A\",\"refreshExpiresAt\":\"2026-12-15T12:00:00.000Z\"")));
		}
		if (R.Url.EndsWith(TEXT("/player-auth/refresh")))
		{
			return JsonReply(200, SignInBody(T2, PlayerA, false, TEXT(",\"refreshToken\":\"svpr1_B\",\"refreshExpiresAt\":\"2026-12-15T12:00:00.000Z\"")));
		}
		if (R.Headers.FindRef(TEXT("Authorization")) == TEXT("Bearer ") + T1) return JsonReply(401, TEXT("{\"error\":\"token_expired\",\"message\":\"Token expired\"}"));
		return JsonReply(200, TEXT("{\"player\":{\"id\":\"0b6f2a8e-1c1d-4a59-9b3c-7f1f3e2a9c01\"},\"balances\":[],\"items\":[]}"));
	};
	Now(H2.Client->Auth().SignInWithPassword(TEXT("ann@example.com"), TEXT("correct horse")));
	const TSvResult<FSvStoreHoldings> Holdings = Now(H2.Client->Store().GetHoldings());
	TestTrue(TEXT("401 token_expired: renewed and sent again"), Holdings.IsOk());
	TestEqual(TEXT("one refresh after 401"), H2.Transport->CountPath(TEXT("/player-auth/refresh")), 1);
	TestEqual(TEXT("holdings sent twice"), H2.Transport->CountPath(TEXT("/store/me")), 2);

	// refresh_invalid ends the sign-in: SignInRequired once, the request fails with token_expired, nothing more is sent.
	FHarness H3;
	const int64 Iat3 = Unix(H3.Clock->Wall);
	const FString T3 = MakeToken(PlayerA, Iat3, Iat3 + 3600, TEXT("email"), TEXT("c"));
	H3.Transport->Handler = [&](const FSvHttpRequest& R)
	{
		if (R.Url.EndsWith(TEXT("/player-auth/email/password")))
		{
			return JsonReply(200, SignInBody(T3, PlayerA, false, TEXT(",\"refreshToken\":\"svpr1_C\",\"refreshExpiresAt\":\"2026-12-15T12:00:00.000Z\"")));
		}
		if (R.Url.EndsWith(TEXT("/player-auth/refresh"))) return JsonReply(401, TEXT("{\"error\":\"refresh_invalid\",\"message\":\"x\"}"));
		return JsonReply(401, TEXT("{\"error\":\"token_expired\",\"message\":\"Token expired\"}"));
	};
	int32 Required = 0;
	ESvSignInRequiredReason Reason = ESvSignInRequiredReason::TokenExpired;
	H3.Client->Auth().OnSignInRequired.AddLambda([&](const FSvSignInRequired& Info)
	{
		++Required;
		Reason = Info.Reason;
	});
	Now(H3.Client->Auth().SignInWithPassword(TEXT("ann@example.com"), TEXT("correct horse")));
	const TSvResult<FSvPlayerProfile> Me = Now(H3.Client->Auth().GetMe());
	TestTrue(TEXT("fails token_expired"), Me.IsError() && Me.GetError().Is(TEXT("token_expired")));
	TestEqual(TEXT("sign-in required once"), Required, 1);
	TestTrue(TEXT("reason refresh_invalid"), Reason == ESvSignInRequiredReason::RefreshInvalid);
	const int32 Sent = H3.Transport->Requests.Num();
	const TSvResult<FSvPlayerProfile> Again = Now(H3.Client->Auth().GetMe());
	TestTrue(TEXT("dead token: refused locally"), Again.IsError() && Again.GetError().Status == 0 && Again.GetError().Is(TEXT("token_expired")));
	TestEqual(TEXT("nothing sent for a dead sign-in"), H3.Transport->Requests.Num(), Sent);
	TestEqual(TEXT("still one sign-in required"), Required, 1);

	TestTrue(TEXT("no token or refresh token in logs"), H.LogsFreeOf(Token1) && H.LogsFreeOf(Token2) && H.LogsFreeOf(Refresh1) && H.LogsFreeOf(Refresh2));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSvReauthenticateTest, "SplatterVault.Unit.Reauthenticate", SV_TEST_FLAGS)
bool FSvReauthenticateTest::RunTest(const FString& Parameters)
{
	FHarness H;
	const int64 Iat = Unix(H.Clock->Wall);
	const FString Old = MakeToken(PlayerA, Iat, Iat + 3600, TEXT("steam"), TEXT("old"));
	const FString Fresh = MakeToken(PlayerA, Iat, Iat + 3600, TEXT("steam"), TEXT("new"));
	int32 Steam = 0;
	H.Transport->Handler = [&](const FSvHttpRequest& R)
	{
		if (R.Url.EndsWith(TEXT("/player-auth/steam")))
		{
			++Steam;
			return JsonReply(200, SignInBody(Steam == 1 ? Old : Fresh, PlayerA, false));
		}
		if (R.Headers.FindRef(TEXT("Authorization")) == TEXT("Bearer ") + Old) return JsonReply(401, TEXT("{\"error\":\"token_expired\",\"message\":\"Token expired\"}"));
		return JsonReply(200, ProfileBody);
	};
	TestTrue(TEXT("steam sign-in"), Now(H.Client->Auth().SignInWithSteam(TEXT("deadbeef"))).IsOk());
	TestTrue(TEXT("steam ticket validated"), Now(H.Client->Auth().SignInWithSteam(TEXT("not-hex"))).GetError().Is(SvErrorCodes::InvalidArgument));
	TestTrue(TEXT("meta user id validated"), Now(H.Client->Auth().SignInWithMeta(TEXT("12ab"), TEXT("n"))).GetError().Is(SvErrorCodes::InvalidArgument));
	FSvAuth& Auth = H.Client->Auth();
	FSvAuth* AuthPtr = &Auth;
	Auth.SetReauthenticate([AuthPtr](const FSvSignInRequired& Info)
	{
		// A new Steam ticket, then the same sign-in; the waiting request is sent again with the new token.
		return AuthPtr->SignInWithSteam(TEXT("deadbeef")).Next([](const TSvResult<FSvSignInResult>& R) { return R.IsOk(); });
	});
	const TSvResult<FSvPlayerProfile> Me = Now(Auth.GetMe());
	TestTrue(TEXT("reauthenticated and sent again"), Me.IsOk());
	TestEqual(TEXT("two steam sign-ins"), Steam, 2);
	TestEqual(TEXT("new token"), Auth.GetPlayerToken(), Fresh);
	return true;
}

// ─── store: idempotency, pending queue, catalog ETag ──────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSvStoreIdempotencyTest, "SplatterVault.Unit.StoreIdempotency", SV_TEST_FLAGS)
bool FSvStoreIdempotencyTest::RunTest(const FString& Parameters)
{
	TSharedPtr<ISvSecureStore> Shared = MakeShared<FSvMemorySecureStore>();
	FHarness H(Shared);
	const int64 Iat = Unix(H.Clock->Wall);
	const FString Token = MakeToken(PlayerA, Iat, Iat + 3600, TEXT("device"), TEXT("1"));
	const FString TxnBody = TEXT("{\"txn\":{\"id\":\"11111111-2222-4333-8444-555555555555\",\"kind\":\"purchase\",\"createdAt\":\"2026-11-15T12:00:00.000Z\",\"reason\":null,\"reverses\":null},\"changes\":[],\"balances\":[{\"currency\":\"COINS\",\"amount\":980}],\"items\":[],\"shortfall\":[],\"replayed\":@REPLAYED@}");
	int32 Purchases = 0;
	TArray<FString> Keys;
	int32 PendingDuringSend = -1;
	FHarness* HP = &H;
	H.Transport->Handler = [&](const FSvHttpRequest& R)
	{
		if (R.Url.EndsWith(TEXT("/player-auth/device/new"))) return JsonReply(200, SignInBody(Token, PlayerA, true, TEXT(",\"deviceSecret\":\"svpd1_X\"")));
		if (R.Url.EndsWith(TEXT("/store/me/purchases")))
		{
			++Purchases;
			Keys.Add(R.Headers.FindRef(TEXT("Idempotency-Key")));
			PendingDuringSend = HP->Client->Store().GetPending().GetValue().Num();
			if (Purchases == 1) return FSvHttpResponse::Failed(ESvTransportFailure::Network);
			return JsonReply(Purchases == 2 ? 201 : 200, TxnBody.Replace(TEXT("@REPLAYED@"), Purchases == 2 ? TEXT("false") : TEXT("true"), ESearchCase::CaseSensitive));
		}
		return ErrorReply(404, TEXT("not_found"));
	};
	Now(H.Client->Auth().SignInAsGuest());
	TestTrue(TEXT("purchase refuses a bad quantity"), Now(H.Client->Store().Purchase(TEXT("potion_small"), TEXT("COINS"), 20, 101)).GetError().Is(SvErrorCodes::InvalidArgument));
	const TSvResult<FSvStoreTxnResult> R = Now(H.Client->Store().Purchase(TEXT("potion_small"), TEXT("COINS"), 20, 1));
	TestTrue(TEXT("purchase succeeds after a lost answer"), R.IsOk());
	TestEqual(TEXT("two sends"), Purchases, 2);
	TestTrue(TEXT("same Idempotency-Key on the retry"), Keys.Num() == 2 && Keys[0] == Keys[1] && Keys[0].Len() == 32);
	TestEqual(TEXT("result carries the key"), R.GetValue().IdempotencyKey, Keys[0]);
	TestEqual(TEXT("kept in storage before the send"), PendingDuringSend, 1);
	TestEqual(TEXT("forgotten once answered"), H.Client->Store().GetPending().GetValue().Num(), 0);
	const TSharedPtr<FJsonObject> Body = BodyOf(H.Transport->Requests.Last());
	int64 Expected = 0, Quantity = 0;
	TestTrue(TEXT("purchase body"), SvJson::GetString(Body, TEXT("sku")) == TEXT("potion_small") && SvJson::GetString(Body, TEXT("currency")) == TEXT("COINS") &&
		SvJson::GetInt64(Body, TEXT("expectedPrice"), Expected) && Expected == 20 && SvJson::GetInt64(Body, TEXT("quantity"), Quantity) && Quantity == 1);

	// Every send fails: the change stays pending (with its key and exact body), and a later run resumes it.
	H.Transport->Handler = [&](const FSvHttpRequest& Req)
	{
		if (Req.Url.EndsWith(TEXT("/store/me/consume"))) Keys.Add(Req.Headers.FindRef(TEXT("Idempotency-Key")));
		return FSvHttpResponse::Failed(ESvTransportFailure::Timeout);
	};
	Keys.Reset();
	const TSvResult<FSvStoreTxnResult> Lost = Now(H.Client->Store().ConsumeSku(TEXT("potion_small"), 1, TEXT("my-key-1")));
	TestTrue(TEXT("no answer"), Lost.IsError() && Lost.GetError().IsNetworkError());
	TestEqual(TEXT("three tries"), Keys.Num(), 3);
	const TArray<FSvPendingStoreChange> Pending = H.Client->Store().GetPending().GetValue();
	TestTrue(TEXT("still pending with its key"), Pending.Num() == 1 && Pending[0].IdempotencyKey == TEXT("my-key-1") && Pending[0].Kind == TEXT("consume"));

	FHarness Later(Shared);
	TArray<FString> LaterKeys;
	FString LaterBody;
	Later.Transport->Handler = [&](const FSvHttpRequest& Req)
	{
		if (Req.Url.EndsWith(TEXT("/player-auth/device"))) return JsonReply(200, SignInBody(Token, PlayerA, false));
		if (Req.Url.EndsWith(TEXT("/store/me/consume")))
		{
			LaterKeys.Add(Req.Headers.FindRef(TEXT("Idempotency-Key")));
			LaterBody = Req.Body;
			return JsonReply(200, TxnBody.Replace(TEXT("@REPLAYED@"), TEXT("true"), ESearchCase::CaseSensitive));
		}
		return ErrorReply(404, TEXT("not_found"));
	};
	TestTrue(TEXT("restore"), Now(Later.Client->Auth().Restore()).GetValue());
	const TSvResult<FSvResumeResult> Resumed = Now(Later.Client->Store().ResumePending());
	TestTrue(TEXT("resumed"), Resumed.IsOk() && Resumed.GetValue().Completed.Num() == 1 && Resumed.GetValue().StillPending == 0);
	TestTrue(TEXT("resumed with the same key"), LaterKeys.Num() == 1 && LaterKeys[0] == TEXT("my-key-1"));
	TestEqual(TEXT("resumed with the exact body"), LaterBody, Pending[0].Body);
	TestTrue(TEXT("replayed is a success"), Resumed.GetValue().Completed[0].Replayed);
	TestEqual(TEXT("nothing pending"), Later.Client->Store().GetPending().GetValue().Num(), 0);

	// A refusal (409 insufficient_funds) is dropped from the queue: nothing changed.
	Later.Transport->Handler = [](const FSvHttpRequest&) { return ErrorReply(409, TEXT("insufficient_funds")); };
	const TSvResult<FSvStoreTxnResult> Refused = Now(Later.Client->Store().Purchase(TEXT("sword_basic"), TEXT("COINS"), 250));
	TestTrue(TEXT("refused"), Refused.IsError() && Refused.GetError().Is(TEXT("insufficient_funds")));
	TestEqual(TEXT("refused is sent once"), Later.Transport->CountPath(TEXT("/store/me/purchases")), 1);
	TestEqual(TEXT("refused is not pending"), Later.Client->Store().GetPending().GetValue().Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSvCatalogETagTest, "SplatterVault.Unit.CatalogETag", SV_TEST_FLAGS)
bool FSvCatalogETagTest::RunTest(const FString& Parameters)
{
	FHarness H;
	const int64 Iat = Unix(H.Clock->Wall);
	const FString Token = MakeToken(PlayerA, Iat, Iat + 3600, TEXT("device"), TEXT("1"));
	const FString Catalog = TEXT("{\"settings\":{\"playerPurchases\":true,\"playerConsume\":true},\"currencies\":[],\"items\":[{\"sku\":\"a\",\"name\":\"A\",\"kind\":\"item\",\"stackable\":true,\"maxOwned\":null,\"prices\":[],\"contents\":[],\"tags\":[],\"custom\":null,\"purchasable\":true,\"consumable\":true}]}");
	TArray<FString> IfNoneMatch;
	H.Transport->Handler = [&](const FSvHttpRequest& R)
	{
		if (R.Url.EndsWith(TEXT("/player-auth/device/new"))) return JsonReply(200, SignInBody(Token, PlayerA, true, TEXT(",\"deviceSecret\":\"svpd1_X\"")));
		if (R.Url.EndsWith(TEXT("/store/catalog")) || R.Url.Contains(TEXT("/player-data/game")))
		{
			const FString Inm = R.Headers.FindRef(TEXT("If-None-Match"));
			IfNoneMatch.Add(Inm);
			TMap<FString, FString> Hd;
			Hd.Add(TEXT("ETag"), TEXT("W/\"1a-abc\""));
			if (Inm == TEXT("W/\"1a-abc\"")) return FSvHttpResponse::Make(304, FString(), Hd);
			return JsonReply(200, R.Url.EndsWith(TEXT("/store/catalog")) ? Catalog : FString(TEXT("{\"data\":{\"motd\":{\"value\":\"hi\",\"version\":1,\"updatedAt\":null}}}")), Hd);
		}
		return ErrorReply(404, TEXT("not_found"));
	};
	Now(H.Client->Auth().SignInAsGuest());
	const TSvResult<FSvStoreCatalog> First = Now(H.Client->Store().GetCatalog());
	const TSvResult<FSvStoreCatalog> Second = Now(H.Client->Store().GetCatalog());
	TestTrue(TEXT("first 200"), First.IsOk() && !First.GetValue().NotModified && First.GetValue().ETag == TEXT("W/\"1a-abc\""));
	TestTrue(TEXT("second 304 returns the kept copy"), Second.IsOk() && Second.GetValue().NotModified && Second.GetValue().Items.Num() == 1);
	TestTrue(TEXT("If-None-Match sent the second time"), IfNoneMatch.Num() == 2 && IfNoneMatch[0].IsEmpty() && IfNoneMatch[1] == TEXT("W/\"1a-abc\""));
	const TSvResult<FSvGameData> G1 = Now(H.Client->Data().GetGameData());
	const TSvResult<FSvGameData> G2 = Now(H.Client->Data().GetGameData());
	TestTrue(TEXT("game data 304"), G1.IsOk() && G2.IsOk() && !G1.GetValue().NotModified && G2.GetValue().NotModified && G2.GetValue().Data.Find(TEXT("motd")) != nullptr);
	H.Client->Store().ClearCatalogCache();
	IfNoneMatch.Reset();
	Now(H.Client->Store().GetCatalog());
	TestTrue(TEXT("cleared cache: no If-None-Match"), IfNoneMatch.Num() == 1 && IfNoneMatch[0].IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSvClientOptionsTest, "SplatterVault.Unit.ClientOptions", SV_TEST_FLAGS)
bool FSvClientOptionsTest::RunTest(const FString& Parameters)
{
	FString Error;
	FSvClientOptions O;
	O.SecureStore = MakeShared<FSvMemorySecureStore>();
	O.Transport = MakeShared<FFakeTransport>();
	TestFalse(TEXT("needs a game"), FSplatterVaultClient::Create(O, Error).IsValid());
	O.GameId = 1;
	O.GameKey = TEXT("k");
	TestFalse(TEXT("not both"), FSplatterVaultClient::Create(O, Error).IsValid());
	O.GameKey.Reset();
	O.BaseUrl = TEXT("http://api.example.com/rest");
	TestFalse(TEXT("http refused off localhost"), FSplatterVaultClient::Create(O, Error).IsValid());
	O.BaseUrl = TEXT("http://127.0.0.1:8787/rest/");
	const TSharedPtr<FSplatterVaultClient> Local = FSplatterVaultClient::Create(O, Error);
	TestTrue(TEXT("http localhost allowed"), Local.IsValid() && Local->GetBaseUrl() == TEXT("http://127.0.0.1:8787/rest"));
	O.BaseUrl.Reset();
	const TSharedPtr<FSplatterVaultClient> Prod = FSplatterVaultClient::Create(O, Error);
	TestTrue(TEXT("default base url"), Prod.IsValid() && Prod->GetBaseUrl() == TEXT("https://splattervault.com/rest"));
	TestEqual(TEXT("storage namespace"), FSplatterVaultClient::DefaultStorageNamespace(TEXT("https://SplatterVault.com/rest"), 42, FString()),
		FString(TEXT("https://splattervault.com|id:42")));
	TestEqual(TEXT("storage namespace port"), FSplatterVaultClient::DefaultStorageNamespace(TEXT("http://127.0.0.1:8787/rest"), 0, TEXT("gk")),
		FString(TEXT("http://127.0.0.1:8787|key:gk")));
	TestTrue(TEXT("not signed in"), Now(Prod->Store().GetHoldings()).GetError().Is(SvErrorCodes::NotSignedIn));
	TestTrue(TEXT("data keys validated"), Now(Prod->Data().GetMine({TEXT("bad key")})).GetError().Is(SvErrorCodes::InvalidArgument));
	TestTrue(TEXT("token lifetime from iat/exp"), FMath::IsNearlyEqual(FSvAuth::TokenLifetimeSeconds(MakeToken(TEXT("p"), 1000, 1300), FString()), 300.0));
	TestTrue(TEXT("token lifetime floor"), FMath::IsNearlyEqual(FSvAuth::TokenLifetimeSeconds(MakeToken(TEXT("p"), 1000, 1010), FString()), 60.0));
	TestTrue(TEXT("token lifetime default"), FMath::IsNearlyEqual(FSvAuth::TokenLifetimeSeconds(TEXT("not.a.jwt"), FString()), 3600.0));
	return true;
}

#endif
