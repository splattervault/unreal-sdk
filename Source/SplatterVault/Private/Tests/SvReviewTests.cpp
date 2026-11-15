// Copyright (c) SplatterVault. Licensed under the MIT License.

// Tests for the independent review's findings: late renewals (auth epoch), shared stores, unreadable storage, atomic
// writes, one send per store change, client lifetime, shutdown, the pending cache, Blueprint helpers.

#include "Tests/SvTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SplatterVaultBlueprintLibrary.h"
#include "SplatterVaultSubsystem.h"
#include "SvCrypto.h"
#include "SvPipeline.h"

using namespace SvTest;

namespace
{
	const TCHAR* Ann = TEXT("2a5f0c11-0000-4000-8000-0000000000a1");
	const TCHAR* Bob = TEXT("2a5f0c11-0000-4000-8000-0000000000b2");

	FString Answer(const FString& Token, const FString& PlayerId, const FString& Extra = FString())
	{
		return FString::Printf(TEXT("{\"token\":\"%s\",\"expiresAt\":\"2026-11-15T13:00:00.000Z\",\"player\":{\"id\":\"%s\",\"displayName\":null,\"createdAt\":\"2026-11-15T12:00:00.000Z\",\"newPlayer\":false},\"identities\":[]%s}"),
			*Token, *PlayerId, *Extra);
	}

	FString WithRefresh(const FString& Refresh)
	{
		return FString::Printf(TEXT(",\"refreshToken\":\"%s\",\"refreshExpiresAt\":\"2026-12-15T12:00:00.000Z\""), *Refresh);
	}

	FString Jwt(FHarness& H, const FString& Player, const FString& Unique, const FString& Amr)
	{
		const int64 Iat = Unix(H.Clock->Wall);
		return MakeToken(Player, Iat, Iat + 3600, Amr, Unique);
	}

	const TCHAR* Profile = TEXT("{\"player\":{\"id\":\"2a5f0c11-0000-4000-8000-0000000000a1\",\"displayName\":null,\"createdAt\":\"2026-11-15T12:00:00.000Z\",\"lastSignedInAt\":null},\"game\":{\"gameKey\":null},\"identities\":[]}");

	FString Txn(bool bReplayed)
	{
		return FString::Printf(TEXT("{\"txn\":{\"id\":\"11111111-2222-4333-8444-555555555555\",\"kind\":\"purchase\",\"createdAt\":\"2026-11-15T12:00:00.000Z\",\"reason\":null,\"reverses\":null},\"changes\":[],\"balances\":[],\"items\":[],\"shortfall\":[],\"replayed\":%s}"),
			bReplayed ? TEXT("true") : TEXT("false"));
	}

	/** The stored refresh token of a harness's client (empty when none). */
	FString StoredRefresh(FHarness& H)
	{
		FString Json, Why;
		if (H.Store->Get(H.Client->GetStoragePrefix() + TEXT("refresh_token"), Json, Why) != ESvStoreRead::Found) return FString();
		return SvJson::GetString(SvJson::ParseObject(Json), TEXT("token"));
	}

	FString TempDir(const TCHAR* Name)
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("SplatterVault"), FString(Name) + FGuid::NewGuid().ToString(EGuidFormats::Digits));
	}
}

// ─── finding 1: a late renewal or restore never resurrects or replaces a sign-in (auth epoch) ────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSvAuthEpochTest, "SplatterVault.Unit.AuthEpoch", SV_TEST_FLAGS)
bool FSvAuthEpochTest::RunTest(const FString& Parameters)
{
	// a) Sign-out while a refresh is in flight: the refresh answer is dropped and its new refresh token signed out.
	{
		FHarness H;
		const FString T1 = Jwt(H, Ann, TEXT("1"), TEXT("email"));
		const FString T2 = Jwt(H, Ann, TEXT("2"), TEXT("email"));
		TArray<FString> SignedOutTokens;
		H.Transport->bDeferred = true;
		H.Transport->Handler = [&](const FSvHttpRequest& R)
		{
			if (R.Url.EndsWith(TEXT("/player-auth/email/password"))) return JsonReply(200, Answer(T1, Ann, WithRefresh(TEXT("svpr1_ONE"))));
			if (R.Url.EndsWith(TEXT("/player-auth/refresh"))) return JsonReply(200, Answer(T2, Ann, WithRefresh(TEXT("svpr1_TWO"))));
			if (R.Url.EndsWith(TEXT("/player-auth/sign-out")))
			{
				SignedOutTokens.Add(SvJson::GetString(BodyOf(R), TEXT("refreshToken")));
				return FSvHttpResponse::Make(204, FString());
			}
			return ErrorReply(404, TEXT("not_found"));
		};
		int32 SignedIn = 0, SignedOut = 0;
		H.Client->Auth().OnSignedIn.AddLambda([&SignedIn](const FSvSignInResult&) { ++SignedIn; });
		H.Client->Auth().OnSignedOut.AddLambda([&SignedOut]() { ++SignedOut; });
		H.Client->Auth().SignInWithPassword(TEXT("ann@example.com"), TEXT("password1"));
		H.Transport->Pump();
		TSvFuture<FSvSignInResult> Renew = H.Client->Auth().Refresh();
		H.Client->Auth().SignOut();
		H.Transport->Pump();
		TestFalse(TEXT("sign-out: still signed out"), H.Client->Auth().IsSignedIn());
		TestTrue(TEXT("sign-out: the late refresh is refused"), Renew.IsReady() && Renew.Get().GetError().Is(SvErrorCodes::SignInChanged));
		TestTrue(TEXT("sign-out: no sign-in after the sign-out"), SignedIn == 1 && SignedOut == 1);
		TestTrue(TEXT("sign-out: the stale refresh token was signed out on the server"), SignedOutTokens.Contains(TEXT("svpr1_TWO")));
		TestTrue(TEXT("sign-out: nothing stored"), StoredRefresh(H).IsEmpty());
	}
	// b) The startup restore's refresh lands after the game signed in with Steam: the Steam session stays.
	{
		TSharedPtr<ISvSecureStore> Shared = MakeShared<FSvMemorySecureStore>();
		{
			FHarness First(Shared);
			const FString T = Jwt(First, Ann, TEXT("e"), TEXT("email"));
			First.Transport->Handler = [&](const FSvHttpRequest&) { return JsonReply(200, Answer(T, Ann, WithRefresh(TEXT("svpr1_E1")))); };
			Now(First.Client->Auth().SignInWithPassword(TEXT("ann@example.com"), TEXT("password1")));
		}
		FHarness H(Shared);
		const FString Email = Jwt(H, Ann, TEXT("e2"), TEXT("email"));
		const FString Steam = Jwt(H, Bob, TEXT("s"), TEXT("steam"));
		TArray<FString> SignedOutTokens;
		H.Transport->bDeferred = true;
		H.Transport->Handler = [&](const FSvHttpRequest& R)
		{
			if (R.Url.EndsWith(TEXT("/player-auth/refresh"))) return JsonReply(200, Answer(Email, Ann, WithRefresh(TEXT("svpr1_E2"))));
			if (R.Url.EndsWith(TEXT("/player-auth/steam"))) return JsonReply(200, Answer(Steam, Bob));
			SignedOutTokens.Add(SvJson::GetString(BodyOf(R), TEXT("refreshToken")));
			return FSvHttpResponse::Make(204, FString());
		};
		TSvFuture<bool> Restore = H.Client->Auth().Restore();
		H.Client->Auth().SignInWithSteam(TEXT("abcd"));
		TestTrue(TEXT("restore: Steam answered first"), H.Transport->PumpOne(TEXT("/player-auth/steam")));
		TestTrue(TEXT("restore: then the refresh"), H.Transport->PumpOne(TEXT("/player-auth/refresh")));
		H.Transport->Pump();
		TestTrue(TEXT("restore: answers false (it signed nobody in)"), Restore.IsReady() && Restore.Get().IsOk() && !Restore.Get().GetValue());
		TestEqual(TEXT("restore: the Steam player stays"), H.Client->Auth().GetPlayerId(), FString(Bob));
		TestEqual(TEXT("restore: the Steam token stays"), H.Client->Auth().GetPlayerToken(), Steam);
		TestTrue(TEXT("restore: the late refresh token was signed out"), SignedOutTokens.Contains(TEXT("svpr1_E2")));
	}
	// c) A guest renewal answered after a sign-out does not sign the guest back in.
	{
		FHarness H;
		const FString G1 = Jwt(H, Ann, TEXT("g1"), TEXT("device"));
		const FString G2 = Jwt(H, Ann, TEXT("g2"), TEXT("device"));
		H.Transport->bDeferred = true;
		H.Transport->Handler = [&](const FSvHttpRequest& R)
		{
			if (R.Url.EndsWith(TEXT("/player-auth/device/new"))) return JsonReply(200, Answer(G1, Ann, TEXT(",\"deviceSecret\":\"svpd1_G\"")));
			if (R.Url.EndsWith(TEXT("/player-auth/device"))) return JsonReply(200, Answer(G2, Ann));
			return JsonReply(200, Profile);
		};
		H.Client->Auth().SignInAsGuest();
		H.Transport->Pump();
		H.Clock->Advance(4000);
		TSvFuture<FSvPlayerProfile> Me = H.Client->Auth().GetMe(); // due: renews with the device secret first
		TestTrue(TEXT("guest: renewal sent"), H.Transport->Queued() == 1 && H.Transport->CountPath(TEXT("/player-auth/device")) == 2);
		H.Client->Auth().SignOut();
		H.Transport->Pump();
		TestFalse(TEXT("guest: still signed out"), H.Client->Auth().IsSignedIn());
		TestTrue(TEXT("guest: the request fails, not sent under a resurrected sign-in"), Me.IsReady() && Me.Get().IsError());
		TestEqual(TEXT("guest: /player-auth/me never sent"), H.Transport->CountPath(TEXT("/player-auth/me")), 0);
	}
	// d) A new guest made while the restore's device sign-in is in flight waits for it (the guest lock).
	{
		TSharedPtr<ISvSecureStore> Shared = MakeShared<FSvMemorySecureStore>();
		{
			FHarness First(Shared);
			const FString T = Jwt(First, Ann, TEXT("d"), TEXT("device"));
			First.Transport->Handler = [&](const FSvHttpRequest&) { return JsonReply(200, Answer(T, Ann, TEXT(",\"deviceSecret\":\"svpd1_D\""))); };
			Now(First.Client->Auth().SignInAsGuest());
		}
		FHarness H(Shared);
		const FString Old = Jwt(H, Ann, TEXT("old"), TEXT("device"));
		const FString Fresh = Jwt(H, Bob, TEXT("new"), TEXT("device"));
		H.Transport->bDeferred = true;
		H.Transport->Handler = [&](const FSvHttpRequest& R)
		{
			if (R.Url.EndsWith(TEXT("/player-auth/device/new"))) return JsonReply(200, Answer(Fresh, Bob, TEXT(",\"deviceSecret\":\"svpd1_NEW\"")));
			return JsonReply(200, Answer(Old, Ann));
		};
		TSvFuture<bool> Restore = H.Client->Auth().Restore();
		TSvFuture<FSvSignInResult> Replace = H.Client->Auth().CreateGuest(FString(), true);
		TestEqual(TEXT("lock: only the restore is sent"), H.Transport->Queued(), 1);
		H.Transport->Pump();
		// The game's new guest started after the restore: the restore's answer is not used (rule 14), then the new guest
		// is made under the lock.
		TestTrue(TEXT("lock: the restore signed nobody in"), Restore.IsReady() && Restore.Get().IsOk() && !Restore.Get().GetValue());
		TestTrue(TEXT("lock: then the new guest"), Replace.IsReady() && Replace.Get().IsOk() && H.Client->Auth().GetPlayerId() == Bob);
		TestEqual(TEXT("lock: one OnSignedIn-worthy session, the new guest's"), H.Client->Auth().GetPlayerToken(), Fresh);
	}
	return true;
}

// ─── finding 2: two clients on one store; storage namespaces per instance ─────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSvSharedStoreTest, "SplatterVault.Unit.SharedStore", SV_TEST_FLAGS)
bool FSvSharedStoreTest::RunTest(const FString& Parameters)
{
	TSharedPtr<ISvSecureStore> Shared = MakeShared<FSvMemorySecureStore>();
	TArray<FString> Sent;
	int32 Rotation = 0;
	auto Handler = [&](FHarness& H)
	{
		return [&Sent, &Rotation, &H](const FSvHttpRequest& R)
		{
			const FString Token = Jwt(H, Ann, FString::FromInt(Rotation), TEXT("email"));
			if (R.Url.EndsWith(TEXT("/player-auth/email/password"))) return JsonReply(200, Answer(Token, Ann, WithRefresh(TEXT("svpr1_R0"))));
			if (R.Url.EndsWith(TEXT("/player-auth/refresh")))
			{
				Sent.Add(SvJson::GetString(BodyOf(R), TEXT("refreshToken")));
				++Rotation;
				return JsonReply(200, Answer(Token, Ann, WithRefresh(FString::Printf(TEXT("svpr1_R%d"), Rotation))));
			}
			return JsonReply(200, Profile);
		};
	};
	FHarness A(Shared);
	A.Transport->Handler = Handler(A);
	TestTrue(TEXT("client A signs in"), Now(A.Client->Auth().SignInWithPassword(TEXT("ann@example.com"), TEXT("password1"))).IsOk());
	FHarness B(Shared);
	B.Transport->Handler = Handler(B);
	TestTrue(TEXT("client B restores (rotates R0 -> R1)"), Now(B.Client->Auth().Restore()).GetValue());
	TestTrue(TEXT("client A refreshes"), Now(A.Client->Auth().Refresh()).IsOk());
	TestTrue(TEXT("A read the store again and sent R1, not the spent R0"), Sent.Num() == 2 && Sent[0] == TEXT("svpr1_R0") && Sent[1] == TEXT("svpr1_R1"));

	const FString Base = TEXT("https://splattervault.com/rest");
	TestEqual(TEXT("namespace: plain"), USplatterVaultSubsystem::StorageNamespaceFor(Base, 7, FString(), INDEX_NONE, TEXT("")),
		FString(TEXT("https://splattervault.com|id:7")));
	TestEqual(TEXT("namespace: first PIE window keeps it"), USplatterVaultSubsystem::StorageNamespaceFor(Base, 7, FString(), 0, TEXT("")),
		FString(TEXT("https://splattervault.com|id:7")));
	TestEqual(TEXT("namespace: second PIE window"), USplatterVaultSubsystem::StorageNamespaceFor(Base, 7, FString(), 1, TEXT("")),
		FString(TEXT("https://splattervault.com|id:7|pie:1")));
	TestEqual(TEXT("namespace: command line"), USplatterVaultSubsystem::StorageNamespaceFor(Base, 7, FString(), INDEX_NONE, TEXT("-game -SvStorageNamespace=second")),
		FString(TEXT("https://splattervault.com|id:7|second")));
	return true;
}

// ─── findings 3 and 4: unreadable storage is never "empty"; atomic writes and temporary files ────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSvStorageSafetyTest, "SplatterVault.Unit.StorageSafety", SV_TEST_FLAGS)
bool FSvStorageSafetyTest::RunTest(const FString& Parameters)
{
	// Salt states.
	{
		const FString Damaged = TempDir(TEXT("salt-damaged-"));
		IFileManager::Get().MakeDirectory(*Damaged, true);
		FFileHelper::SaveStringToFile(TEXT("short"), *FPaths::Combine(Damaged, TEXT("store.salt")));
		TArray<uint8> Salt;
		TestTrue(TEXT("damaged salt: unreadable"), FSvObfuscatingProtector::LoadOrCreateSalt(FPaths::Combine(Damaged, TEXT("store.salt")), Salt) == ESvSaltResult::Unreadable);
		const FString Orphans = TempDir(TEXT("salt-missing-"));
		IFileManager::Get().MakeDirectory(*Orphans, true);
		FFileHelper::SaveStringToFile(TEXT("sealed"), *FPaths::Combine(Orphans, TEXT("0123456789abcdef0123456789abcdef.svs")));
		TestTrue(TEXT("values without their salt: unreadable"), FSvObfuscatingProtector::LoadOrCreateSalt(FPaths::Combine(Orphans, TEXT("store.salt")), Salt) == ESvSaltResult::Unreadable);
		const FString Blocked = TempDir(TEXT("salt-blocked-"));
		FFileHelper::SaveStringToFile(TEXT("a file, not a directory"), *Blocked);
		TestTrue(TEXT("no place for a salt: unavailable"), FSvObfuscatingProtector::LoadOrCreateSalt(FPaths::Combine(Blocked, TEXT("store.salt")), Salt) == ESvSaltResult::Unavailable);
		IFileManager::Get().DeleteDirectory(*Damaged, false, true);
		IFileManager::Get().DeleteDirectory(*Orphans, false, true);
		IFileManager::Get().Delete(*Blocked);
	}
	// An unreadable store never lets a new guest replace the old one.
	{
		FHarness H(MakeShared<FSvUnreadableStore>(TEXT("test")));
		H.Transport->Handler = [](const FSvHttpRequest&) { return JsonReply(200, TEXT("{}")); };
		const TSvResult<FSvSignInResult> R = Now(H.Client->Auth().SignInAsGuest());
		TestTrue(TEXT("unreadable store: storage_unreadable, nothing sent"), R.IsError() && R.GetError().Is(SvErrorCodes::StorageUnreadable) && H.Transport->Requests.Num() == 0);
	}
	// An unavailable store makes a guest that says it is not saved.
	{
		FHarness H(MakeShared<FSvUnavailableStore>(TEXT("test")));
		const FString T = Jwt(H, Ann, TEXT("u"), TEXT("device"));
		H.Transport->Handler = [&](const FSvHttpRequest&) { return JsonReply(200, Answer(T, Ann, TEXT(",\"deviceSecret\":\"svpd1_U\""))); };
		const TSvResult<FSvSignInResult> R = Now(H.Client->Auth().SignInAsGuest());
		TestTrue(TEXT("unavailable store: guest made, SavedToDevice false"), R.IsOk() && !R.GetValue().SavedToDevice && !R.GetValue().Renewable);
	}
	// Atomic replace, temporary-file recovery, and deletes that take the temporary file too.
	{
		const FString Dir = TempDir(TEXT("atomic-"));
		TArray<uint8> Salt;
		FSvObfuscatingProtector::LoadOrCreateSalt(FPaths::Combine(Dir, TEXT("store.salt")), Salt);
		TSharedRef<FSvObfuscatingProtector> Protector = MakeShared<FSvObfuscatingProtector>(TEXT("material"), Salt);
		FSvProtectedFileStore Store(Dir, Protector);
		FString V, E;
		TestTrue(TEXT("first value"), Store.Set(TEXT("k"), TEXT("v1"), E));
		TestTrue(TEXT("replaced in place"), Store.Set(TEXT("k"), TEXT("v2"), E) && Store.Get(TEXT("k"), V, E) == ESvStoreRead::Found && V == TEXT("v2"));
		// A write that stopped after its temporary file was complete: the next read finishes it.
		// (Earlier versions named it "<file>.tmp"; this version "<file>.<id>.tmp": both are found.)
		IFileManager::Get().SetTimeStamp(*Store.PathFor(TEXT("k")), FDateTime::UtcNow() - FTimespan::FromMinutes(1));
		TArray<uint8> Blob;
		Protector->Protect(SvCrypto::Utf8(TEXT("v3")), TEXT("k"), Blob);
		FFileHelper::SaveArrayToFile(Blob, *(Store.PathFor(TEXT("k")) + TEXT(".tmp")));
		TestTrue(TEXT("complete temporary file is the newer value"), Store.Get(TEXT("k"), V, E) == ESvStoreRead::Found && V == TEXT("v3"));
		TestFalse(TEXT("and it took its place"), IFileManager::Get().FileExists(*(Store.PathFor(TEXT("k")) + TEXT(".tmp"))));
		TestTrue(TEXT("still v3 after"), Store.Get(TEXT("k"), V, E) == ESvStoreRead::Found && V == TEXT("v3"));
		// A write cut short: the old value stays; the broken temporary file goes once it is old (a young one may be
		// another process's write in progress).
		const FString Broken = Store.PathFor(TEXT("k")) + TEXT(".0123456789abcdef0123456789abcdef.tmp");
		FFileHelper::SaveStringToFile(TEXT("cut short"), *Broken);
		TestTrue(TEXT("broken temporary file ignored"), Store.Get(TEXT("k"), V, E) == ESvStoreRead::Found && V == TEXT("v3"));
		TestTrue(TEXT("a young one is left alone"), IFileManager::Get().FileExists(*Broken));
		IFileManager::Get().SetTimeStamp(*Broken, FDateTime::UtcNow() - FTimespan::FromMinutes(5));
		TestTrue(TEXT("still v3"), Store.Get(TEXT("k"), V, E) == ESvStoreRead::Found && V == TEXT("v3"));
		TestFalse(TEXT("an old one is removed"), IFileManager::Get().FileExists(*Broken));
		// A complete temporary file OLDER than the value (a write a later one overtook) is never brought back.
		TArray<uint8> Stale;
		Protector->Protect(SvCrypto::Utf8(TEXT("stale")), TEXT("k"), Stale);
		const FString Overtaken = Store.PathFor(TEXT("k")) + TEXT(".fedcba9876543210fedcba9876543210.tmp");
		FFileHelper::SaveArrayToFile(Stale, *Overtaken);
		IFileManager::Get().SetTimeStamp(*Overtaken, FDateTime::UtcNow() - FTimespan::FromMinutes(5));
		TestTrue(TEXT("an overtaken write is not brought back"), Store.Get(TEXT("k"), V, E) == ESvStoreRead::Found && V == TEXT("v3"));
		TestFalse(TEXT("and it is removed"), IFileManager::Get().FileExists(*Overtaken));
		// Several leftovers newer than the value: the newest complete one wins.
		IFileManager::Get().SetTimeStamp(*Store.PathFor(TEXT("k")), FDateTime::UtcNow() - FTimespan::FromMinutes(10));
		TArray<uint8> Older, Newer;
		Protector->Protect(SvCrypto::Utf8(TEXT("v4")), TEXT("k"), Older);
		Protector->Protect(SvCrypto::Utf8(TEXT("v5")), TEXT("k"), Newer);
		const FString TmpOlder = Store.PathFor(TEXT("k")) + TEXT(".11111111111111111111111111111111.tmp");
		const FString TmpNewer = Store.PathFor(TEXT("k")) + TEXT(".22222222222222222222222222222222.tmp");
		FFileHelper::SaveArrayToFile(Older, *TmpOlder);
		FFileHelper::SaveArrayToFile(Newer, *TmpNewer);
		IFileManager::Get().SetTimeStamp(*TmpOlder, FDateTime::UtcNow() - FTimespan::FromMinutes(2));
		IFileManager::Get().SetTimeStamp(*TmpNewer, FDateTime::UtcNow() - FTimespan::FromMinutes(1));
		TestTrue(TEXT("the newest complete leftover wins"), Store.Get(TEXT("k"), V, E) == ESvStoreRead::Found && V == TEXT("v5"));
		TestFalse(TEXT("the older leftover is removed"), IFileManager::Get().FileExists(*TmpOlder));
		// Writes use their own temporary files and leave none behind.
		TestTrue(TEXT("write"), Store.Set(TEXT("k"), TEXT("v6"), E));
		TArray<FString> Left;
		IFileManager::Get().FindFiles(Left, *(Store.PathFor(TEXT("k")) + TEXT("*.tmp")), true, false);
		TestEqual(TEXT("no temporary file left by a write"), Left.Num(), 0);
		// A delete (sign-out, erase) takes every leftover temporary file too (old and new names): nothing comes back.
		FFileHelper::SaveArrayToFile(Blob, *(Store.PathFor(TEXT("k")) + TEXT(".tmp")));
		FFileHelper::SaveArrayToFile(Blob, *(Store.PathFor(TEXT("k")) + TEXT(".33333333333333333333333333333333.tmp")));
		TestTrue(TEXT("delete"), Store.Delete(TEXT("k"), E));
		TestTrue(TEXT("nothing comes back"), Store.Get(TEXT("k"), V, E) == ESvStoreRead::NotFound);
		Left.Reset();
		IFileManager::Get().FindFiles(Left, *(Store.PathFor(TEXT("k")) + TEXT("*.tmp")), true, false);
		TestEqual(TEXT("no temporary file left by a delete"), Left.Num(), 0);
		IFileManager::Get().DeleteDirectory(*Dir, false, true);
	}
	// A guest record whose player could not be written stays "unsaved" (retried by RetrySavingSecrets).
	{
		TSharedRef<FFlakyStore> Flaky = MakeShared<FFlakyStore>();
		FHarness H(Flaky);
		FString Why;
		Flaky->Set(H.Client->GetStoragePrefix() + TEXT("device_secret"), TEXT("svpd1_BARE"), Why); // an old record without its player
		Flaky->bFailWrites = true;
		const FString T = Jwt(H, Ann, TEXT("b"), TEXT("device"));
		H.Transport->Handler = [&](const FSvHttpRequest&) { return JsonReply(200, Answer(T, Ann)); };
		const TSvResult<FSvSignInResult> R = Now(H.Client->Auth().SignInWithDevice());
		TestTrue(TEXT("owner write failed: not renewable after a restart"), R.IsOk() && !R.GetValue().Renewable);
		Flaky->bFailWrites = false;
		TestTrue(TEXT("saved again"), H.Client->Auth().RetrySavingSecrets());
		FString Stored, E;
		Flaky->Get(H.Client->GetStoragePrefix() + TEXT("device_secret"), Stored, E);
		TestTrue(TEXT("the record now names its player"), Stored.Contains(Ann) && Stored.Contains(TEXT("svpd1_BARE")));
	}
	return true;
}

// ─── finding 5: one send per (player, key) at a time ──────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSvOneSendPerChangeTest, "SplatterVault.Unit.OneSendPerChange", SV_TEST_FLAGS)
bool FSvOneSendPerChangeTest::RunTest(const FString& Parameters)
{
	FHarness H;
	const FString T = Jwt(H, Ann, TEXT("1"), TEXT("steam"));
	H.Transport->Handler = [&](const FSvHttpRequest& R)
	{
		if (R.Url.EndsWith(TEXT("/player-auth/steam"))) return JsonReply(200, Answer(T, Ann));
		return JsonReply(201, Txn(false));
	};
	Now(H.Client->Auth().SignInWithSteam(TEXT("abcd")));
	H.Transport->bDeferred = true;
	TSvFuture<FSvStoreTxnResult> First = H.Client->Store().Purchase(TEXT("potion_small"), TEXT("COINS"), 20, 1, TEXT("key-1"));
	TSvFuture<FSvStoreTxnResult> Retry = H.Client->Store().Purchase(TEXT("potion_small"), TEXT("COINS"), 20, 1, TEXT("key-1"));
	const TSvResult<FSvStoreTxnResult> Other = Now(H.Client->Store().Purchase(TEXT("sword_basic"), TEXT("COINS"), 250, 1, TEXT("key-1")));
	TestTrue(TEXT("another request under the same key: refused locally (never sent, nothing kept)"), Other.IsError() &&
		Other.GetError().Is(SvErrorCodes::ChangeInProgress) && !Other.GetError().OutcomeUnknown && !Other.GetError().Pending &&
		Other.GetError().IdempotencyKey == TEXT("key-1"));
	const TSvResult<FSvResumeResult> Resumed = Now(H.Client->Store().ResumePending());
	TestTrue(TEXT("resume leaves it to the running send"), Resumed.IsOk() && Resumed.GetValue().StillPending == 1 && Resumed.GetValue().Completed.Num() == 0);
	TestEqual(TEXT("one send only"), H.Transport->CountPath(TEXT("/store/me/purchases")), 1);
	H.Transport->Pump();
	TestTrue(TEXT("both callers get the answer"), First.IsReady() && Retry.IsReady() && First.Get().IsOk() && Retry.Get().IsOk() &&
		Retry.Get().GetValue().IdempotencyKey == TEXT("key-1"));
	TestEqual(TEXT("nothing pending"), H.Client->Store().GetPending().GetValue().Num(), 0);
	return true;
}

// ─── finding 6: a handler may drop the last reference to the client ───────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSvClientLifetimeTest, "SplatterVault.Unit.ClientLifetime", SV_TEST_FLAGS)
bool FSvClientLifetimeTest::RunTest(const FString& Parameters)
{
	{
		FHarness H;
		const FString T = Jwt(H, Ann, TEXT("1"), TEXT("device"));
		H.Transport->Handler = [&](const FSvHttpRequest&) { return JsonReply(200, Answer(T, Ann, TEXT(",\"deviceSecret\":\"svpd1_L\""))); };
		TWeakPtr<FSplatterVaultClient> Weak = H.Client;
		bool bAliveInHandler = false;
		H.Client->Auth().OnSignedIn.AddLambda([&H, &Weak, &bAliveInHandler](const FSvSignInResult&)
		{
			H.Client.Reset(); // the game's only reference, dropped while the answer is handled
			bAliveInHandler = Weak.IsValid();
		});
		H.Transport->bDeferred = true; // the answer arrives later, the way a real HTTP stack answers
		TSvFuture<FSvSignInResult> F = H.Client->Auth().SignInAsGuest();
		H.Transport->Pump();
		TestTrue(TEXT("answer handled to the end"), F.IsReady() && F.Get().IsOk());
		TestTrue(TEXT("kept alive while the answer was handled"), bAliveInHandler);
		TestFalse(TEXT("released afterwards"), Weak.IsValid());
	}
	{
		FHarness H;
		const FString T = Jwt(H, Ann, TEXT("2"), TEXT("steam"));
		H.Transport->Handler = [&](const FSvHttpRequest&) { return JsonReply(200, Answer(T, Ann)); };
		Now(H.Client->Auth().SignInWithSteam(TEXT("abcd")));
		TWeakPtr<FSplatterVaultClient> Weak = H.Client;
		H.Client->Auth().OnSignedOut.AddLambda([&H]() { H.Client.Reset(); });
		TSharedPtr<FSplatterVaultClient> Caller = H.Client; // the reference the call is made through...
		FSvAuth& Auth = Caller->Auth();
		Caller.Reset();                                     // ...dropped, so the handler releases the last one
		TSvFuture<FSvVoid> Out = Auth.SignOut();
		TestTrue(TEXT("sign-out completed"), Out.IsReady() && Out.Get().IsOk());
		TestFalse(TEXT("released after the call returned"), Weak.IsValid());
	}
	return true;
}

// ─── finding 7: shutdown fails the waiters, cancels waits, and keeps a late refresh token ────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSvShutdownTest, "SplatterVault.Unit.Shutdown", SV_TEST_FLAGS)
bool FSvShutdownTest::RunTest(const FString& Parameters)
{
	{
		TSharedPtr<ISvSecureStore> Shared = MakeShared<FSvMemorySecureStore>();
		FHarness H(Shared);
		const FString T1 = Jwt(H, Ann, TEXT("1"), TEXT("email"));
		const FString T2 = Jwt(H, Ann, TEXT("2"), TEXT("email"));
		H.Transport->bDeferred = true;
		H.Transport->Handler = [&](const FSvHttpRequest& R)
		{
			if (R.Url.EndsWith(TEXT("/player-auth/email/password"))) return JsonReply(200, Answer(T1, Ann, WithRefresh(TEXT("svpr1_A"))));
			if (R.Url.EndsWith(TEXT("/player-auth/refresh"))) return JsonReply(200, Answer(T2, Ann, WithRefresh(TEXT("svpr1_B"))));
			return JsonReply(200, Profile);
		};
		H.Client->Auth().SignInWithPassword(TEXT("ann@example.com"), TEXT("password1"));
		H.Transport->Pump();
		H.Clock->Advance(4000);
		TSvFuture<FSvPlayerProfile> A = H.Client->Auth().GetMe();
		TSvFuture<FSvPlayerProfile> B = H.Client->Auth().GetMe();
		TestEqual(TEXT("one refresh in flight"), H.Transport->CountPath(TEXT("/player-auth/refresh")), 1);
		H.Client->Shutdown();
		TestTrue(TEXT("waiters fail at once with client_shutdown"), A.IsReady() && B.IsReady() && A.Get().GetError().Is(SvErrorCodes::Shutdown) &&
			B.Get().GetError().Is(SvErrorCodes::Shutdown));
		H.Transport->Pump(); // the refresh answer arrives after the shutdown
		TestEqual(TEXT("the rotated refresh token is kept"), StoredRefresh(H), FString(TEXT("svpr1_B")));
	}
	{
		// The default delay (FTSTicker) is removed by a shutdown: the waiting request completes with client_shutdown.
		TSharedRef<FFakeTransport> Transport = MakeShared<FFakeTransport>();
		Transport->Handler = [](const FSvHttpRequest&) { return ErrorReply(503, TEXT("player_sign_in_disabled"), TEXT("5")); };
		FSvClientOptions O;
		O.GameId = 1;
		O.SecureStore = MakeShared<FSvMemorySecureStore>();
		O.Transport = Transport;
		FString Error;
		TSharedPtr<FSplatterVaultClient> Client = FSplatterVaultClient::Create(O, Error);
		TSvFuture<FSvGameData> Waiting = Client->Data().GetPublic();
		TestFalse(TEXT("waiting out Retry-After"), Waiting.IsReady());
		Client->Shutdown();
		TestTrue(TEXT("shutdown ends the wait"), Waiting.IsReady() && Waiting.Get().GetError().Is(SvErrorCodes::Shutdown));
		TestEqual(TEXT("never sent again"), Transport->Requests.Num(), 1);
	}
	{
		// Work queued behind the guest lock fails with client_shutdown.
		FHarness H;
		H.Transport->bDeferred = true;
		H.Transport->Handler = [](const FSvHttpRequest&) { return JsonReply(500, TEXT("{}")); };
		H.Client->Auth().SignInAsGuest();
		TSvFuture<FSvSignInResult> Queued = H.Client->Auth().CreateGuest(FString(), true);
		TestFalse(TEXT("queued behind the lock"), Queued.IsReady());
		H.Client->Shutdown();
		TestTrue(TEXT("queued work fails"), Queued.IsReady() && Queued.Get().GetError().Is(SvErrorCodes::Shutdown));
	}
	return true;
}

// ─── findings 9 and 10: the pending list is read once; Blueprint object fields are case-sensitive ────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSvPendingCacheTest, "SplatterVault.Unit.PendingCache", SV_TEST_FLAGS)
bool FSvPendingCacheTest::RunTest(const FString& Parameters)
{
	TSharedRef<FCountingStore> Counting = MakeShared<FCountingStore>();
	Counting->CountSuffix = TEXT("pending_store_changes");
	FHarness H(Counting);
	const FString T = Jwt(H, Ann, TEXT("1"), TEXT("steam"));
	H.Transport->Handler = [&](const FSvHttpRequest& R)
	{
		if (R.Url.EndsWith(TEXT("/player-auth/steam"))) return JsonReply(200, Answer(T, Ann));
		return JsonReply(201, Txn(false));
	};
	Now(H.Client->Auth().SignInWithSteam(TEXT("abcd")));
	TestTrue(TEXT("purchase"), Now(H.Client->Store().Purchase(TEXT("potion_small"), TEXT("COINS"), 20)).IsOk());
	TestTrue(TEXT("first purchase: one read of the kept list"), Counting->Reads == 1);
	Counting->Reads = 0;
	Now(H.Client->Store().Purchase(TEXT("potion_small"), TEXT("COINS"), 20));
	Now(H.Client->Store().ConsumeSku(TEXT("potion_small"), 1));
	Now(H.Client->Store().ResumePending());
	TestEqual(TEXT("later: no reads (write-through cache)"), Counting->Reads, 0);

	FSvDataEntry Entry;
	Entry.ValueJson = TEXT("{\"x\":1,\"X\":2,\"name\":\"lower\",\"Name\":\"upper\"}");
	TestEqual(TEXT("object int field: exact case"), USplatterVaultBlueprintLibrary::GetDataObjectInt(Entry, TEXT("X"), 0), (int64)2);
	TestEqual(TEXT("object int field: other case"), USplatterVaultBlueprintLibrary::GetDataObjectInt(Entry, TEXT("x"), 0), (int64)1);
	TestEqual(TEXT("object string field: exact case"), USplatterVaultBlueprintLibrary::GetDataObjectString(Entry, TEXT("Name"), TEXT("")), FString(TEXT("upper")));
	TestEqual(TEXT("missing field: default"), USplatterVaultBlueprintLibrary::GetDataObjectInt(Entry, TEXT("y"), 9), (int64)9);
	TestTrue(TEXT("a game client creates the subsystem"), GetDefault<USplatterVaultSubsystem>()->ShouldCreateSubsystem(nullptr) || IsRunningDedicatedServer());
	return true;
}

#endif
