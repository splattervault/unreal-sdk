// Copyright (c) SplatterVault. Licensed under the MIT License.
//
// SplatterVault quick start (C++): sign in as a guest, save and read player data, buy and use an item, and finish
// any store change an earlier run left unanswered (at a moment the player expects it). Copy what you need into your game (a UGameInstance subclass is a
// good home). The plugin's automation tests compile this file and run it against the API mock, so it stays correct.
//
// In a game, get the client from any actor, component, widget or the game instance:
//
//     TSharedPtr<FSplatterVaultClient> Sv = USplatterVaultSubsystem::GetClientFor(this);   // null: not configured
//
// Every call returns TFuture<TSvResult<T>> completed on the game thread. Continuations capture Sv itself: after the
// game instance ends, every call on it answers client_shutdown (nothing is sent), so it is always safe to call.

#pragma once

#include "SplatterVaultClient.h"

namespace SvSample
{
	/** Runs the quick start; Done(true) when every step worked. Log lines go to OnLine (never tokens or secrets). */
	inline void RunQuickStart(TSharedRef<FSplatterVaultClient> Sv, TFunction<void(const FString&)> OnLine, TFunction<void(bool)> Done)
	{
		auto Fail = [OnLine, Done](const TCHAR* Step, const FSvError& Error)
		{
			// Key handling on Error.Code; show Error.Message.
			OnLine(FString::Printf(TEXT("%s failed: %s"), Step, *Error.ToString()));
			Done(false);
		};

		// 1. Sign in: the guest stored on this device, or a new one on the first launch.
		Sv->Auth().SignInAsGuest(TEXT("Quick Start")).Next([Sv, OnLine, Done, Fail](const TSvResult<FSvSignInResult>& SignIn)
		{
			if (SignIn.IsError()) return Fail(TEXT("Sign-in"), SignIn.GetError());
			OnLine(FString::Printf(TEXT("Signed in as %s (%s)"), *SignIn.GetValue().Player.Id, SignIn.GetValue().Player.NewPlayer ? TEXT("new") : TEXT("returning")));

			// 2. Finish store changes an earlier run left without an answer (same Idempotency-Key, so never twice).
			Sv->Store().ResumePending().Next([Sv, OnLine, Done, Fail](const TSvResult<FSvResumeResult>& Resumed)
			{
				if (Resumed.IsOk() && Resumed.GetValue().Completed.Num() > 0)
				{
					OnLine(FString::Printf(TEXT("Finished %d earlier store change(s)"), Resumed.GetValue().Completed.Num()));
				}

				// 3. Read the player's data, then write with the version we read (IfVersion: no lost updates).
				Sv->Data().GetMine({TEXT("launches")}).Next([Sv, OnLine, Done, Fail](const TSvResult<FSvPlayerData>& Data)
				{
					if (Data.IsError()) return Fail(TEXT("Read data"), Data.GetError());
					FSvDataOps Ops;
					Ops.Inc(TEXT("launches"), 1, FSvIncOptions().WithIfVersion(Data.GetValue().Player.VersionOf(TEXT("launches"))));
					Sv->Data().Write(Ops).Next([Sv, OnLine, Done, Fail](const TSvResult<FSvDataWriteResult>& Written)
					{
						if (Written.IsError())
						{
							// OutcomeUnknown (no answer, 500/502/504, or a conflict after the SDK sent it again): it may have
							// applied. Read again: if "launches" moved past the version we sent, it did; do not add 1 again.
							// A plain version_conflict: another device wrote first; read again and decide.
							return Fail(TEXT("Write data"), Written.GetError());
						}
						OnLine(FString::Printf(TEXT("Launch number %s"), *Written.GetValue().Results[0].ValueJson));

						// 4. Show the catalog and buy one item at the price the player saw.
						Sv->Store().GetCatalog().Next([Sv, OnLine, Done, Fail](const TSvResult<FSvStoreCatalog>& Catalog)
						{
							if (Catalog.IsError()) return Fail(TEXT("Catalog"), Catalog.GetError());
							const FSvCatalogItem* Potion = Catalog.GetValue().Find(TEXT("potion_small"));
							int64 Price = 0;
							if (!Potion || !Potion->Purchasable || !Potion->PriceIn(TEXT("COINS"), Price))
							{
								OnLine(TEXT("No potion_small for COINS in this game's catalog"));
								return Done(true);
							}
							Sv->Store().Purchase(Potion->Sku, TEXT("COINS"), Price).Next([Sv, OnLine, Done, Fail](const TSvResult<FSvStoreTxnResult>& Bought)
							{
								if (Bought.IsError())
								{
									// Still kept (no answer, 408 / 429 / 5xx, a sign-in problem): show "Finish", not "Buy". Finish
									// calls Sv->Store().ResumePending(), or Purchase again WITH Bought.GetError().IdempotencyKey;
									// a new Purchase without the key would be a second purchase.
									if (Bought.GetError().Pending)
									{
										OnLine(FString::Printf(TEXT("Purchase not finished yet (key %s): offer Finish."), *Bought.GetError().IdempotencyKey));
									}
									// Not kept: insufficient_funds, price_changed (show the new price), already_owned...
									return Fail(TEXT("Purchase"), Bought.GetError());
								}
								for (const FSvBalance& B : Bought.GetValue().Balances)
								{
									OnLine(FString::Printf(TEXT("%s balance: %lld"), *B.Currency, (long long)B.Amount));
								}

								// 5. Use one up.
								Sv->Store().ConsumeSku(TEXT("potion_small"), 1).Next([OnLine, Done, Fail](const TSvResult<FSvStoreTxnResult>& Used)
								{
									if (Used.IsError()) return Fail(TEXT("Consume"), Used.GetError());
									OnLine(TEXT("Used a potion"));
									Done(true);
								});
							});
						});
					});
				});
			});
		});
	}
}
