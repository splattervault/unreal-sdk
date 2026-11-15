// Copyright (c) SplatterVault. Licensed under the MIT License.

#include "Tests/SvTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/GameInstance.h"
#include "SplatterVaultAsyncActions.h"
#include "SplatterVaultSettings.h"
#include "SplatterVaultSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

// The subsystem in a real (standalone) game instance: configuration, state getters, and a Blueprint async node
// started without a signed-in player.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSvSubsystemTest, "SplatterVault.Unit.Subsystem", SV_TEST_FLAGS)
bool FSvSubsystemTest::RunTest(const FString& Parameters)
{
	if (!GEngine)
	{
		AddWarning(TEXT("No engine: the subsystem test did not run."));
		return true;
	}
	// A game instance's outer is the engine (UGameInstance::GetEngine casts it).
	UGameInstance* GameInstance = NewObject<UGameInstance>(GEngine);
	GameInstance->InitializeStandalone();
	USplatterVaultSubsystem* Sv = GameInstance->GetSubsystem<USplatterVaultSubsystem>();
	UWorld* World = GameInstance->GetWorld();
	auto TearDown = [GameInstance, World]()
	{
		GameInstance->Shutdown();
		if (World)
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
		}
	};
	if (!TestNotNull(TEXT("subsystem created with the game instance"), Sv))
	{
		TearDown();
		return false;
	}
	const USplatterVaultSettings* Settings = GetDefault<USplatterVaultSettings>();
	if (Settings->GameId == 0 && Settings->GameKey.IsEmpty())
	{
		TestFalse(TEXT("not configured without a game"), Sv->IsConfigured());
		TestFalse(TEXT("says why"), Sv->GetConfigurationError().IsEmpty());
	}

	FString Error;
	TestFalse(TEXT("refuses http off localhost"), Sv->Configure(4242, FString(), TEXT("http://api.example.com/rest"), Error));
	TestFalse(TEXT("explains"), Error.IsEmpty());
	TestTrue(TEXT("configures from code"), Sv->Configure(4242, FString(), TEXT("http://127.0.0.1:9/rest"), Error));
	TestTrue(TEXT("configured"), Sv->IsConfigured() && Sv->GetClient().IsValid());
	// The same game and API again keeps the client (a refresh in flight is not lost); another API replaces it.
	const TSharedPtr<FSplatterVaultClient> First = Sv->GetClient(); // held, so a new client cannot reuse its address
	TestTrue(TEXT("same configuration: no-op"), Sv->Configure(4242, FString(), TEXT("http://127.0.0.1:9/rest/"), Error) && Sv->GetClient() == First);
	TestTrue(TEXT("another API: new client"), Sv->Configure(4242, FString(), TEXT("http://127.0.0.1:10/rest"), Error) && Sv->GetClient() != First);
	TestTrue(TEXT("back to the first API"), Sv->Configure(4242, FString(), TEXT("http://127.0.0.1:9/rest"), Error));
	// The short way to the client, from any object of the game instance; null without one.
	TestTrue(TEXT("GetClientFor: the game instance's client"), USplatterVaultSubsystem::GetClientFor(GameInstance) == Sv->GetClient() &&
		Sv->GetClient().IsValid());
	TestFalse(TEXT("GetClientFor(nullptr): null"), USplatterVaultSubsystem::GetClientFor(nullptr).IsValid());
	// A client captured by a continuation stays safe after it was replaced (shut down): every call answers
	// client_shutdown at once, nothing is sent, and no store change is kept.
	TestTrue(TEXT("replaced client: is shut down"), First->IsShutdown());
	TestTrue(TEXT("replaced client: a read answers client_shutdown"), SvTest::Now(First->Data().GetPublic()).GetError().Is(SvErrorCodes::Shutdown));
	TestTrue(TEXT("replaced client: a store change is refused"), SvTest::Now(First->Store().Purchase(TEXT("potion_small"), TEXT("COINS"), 20)).GetError().Is(SvErrorCodes::Shutdown));
	// Auto-resume of pending store changes after sign-in is off unless the game turns it on (the test host's
	// DefaultGame.ini does not set it, so this is the code's default).
	TestFalse(TEXT("resume after sign-in is off by default"), GetDefault<USplatterVaultSettings>()->bResumePendingAfterSignIn);
	TestTrue(TEXT("secrets excluded from Android backup by default"), GetDefault<USplatterVaultSettings>()->bExcludeSecretsFromAndroidBackup);
	TestEqual(TEXT("game id"), Sv->GetClient()->GetGameId(), 4242);
	TestFalse(TEXT("nobody signed in"), Sv->IsSignedIn());
	TestTrue(TEXT("no player id"), Sv->GetPlayerId().IsEmpty() && Sv->GetPlayerToken().IsEmpty());
	TestFalse(TEXT("storage protection named"), Sv->GetStorageProtection().IsEmpty());

	// A node with no signed-in player fails at once (On Failure, not_signed_in) and nothing is sent.
	USvHoldingsAsyncAction* Node = USvHoldingsAsyncAction::GetHoldings(GameInstance);
	TestNotNull(TEXT("node made"), Node);
	if (Node) Node->Activate();

	TearDown();
	TestFalse(TEXT("client dropped with the game instance"), Sv->IsConfigured());
	return true;
}

#endif
