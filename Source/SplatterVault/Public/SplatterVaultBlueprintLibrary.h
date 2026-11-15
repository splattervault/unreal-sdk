// Copyright (c) SplatterVault. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "SvTypes.h"
#include "SplatterVaultBlueprintLibrary.generated.h"

/** Blueprint helpers: data ops, reading values, errors, the catalog and holdings. */
UCLASS()
class SPLATTERVAULT_API USplatterVaultBlueprintLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	// ─── data ops ────────────────────────────────────────────────────────────────────────────────────────────────

	/** Set Key to a JSON value (for example 42, "text" with its quotes, {"fov":90}). If Version < 0: no condition; 0: only while the key does not exist. */
	UFUNCTION(BlueprintPure, Category = "SplatterVault|Data", meta = (AdvancedDisplay = "IfVersion"))
	static FSvDataOp MakeSetJsonDataOp(const FString& Key, const FString& ValueJson, int64 IfVersion = -1);

	UFUNCTION(BlueprintPure, Category = "SplatterVault|Data", meta = (AdvancedDisplay = "IfVersion"))
	static FSvDataOp MakeSetStringDataOp(const FString& Key, const FString& Value, int64 IfVersion = -1);

	UFUNCTION(BlueprintPure, Category = "SplatterVault|Data", meta = (AdvancedDisplay = "IfVersion"))
	static FSvDataOp MakeSetIntDataOp(const FString& Key, int64 Value, int64 IfVersion = -1);

	UFUNCTION(BlueprintPure, Category = "SplatterVault|Data", meta = (AdvancedDisplay = "IfVersion"))
	static FSvDataOp MakeSetFloatDataOp(const FString& Key, double Value, int64 IfVersion = -1);

	UFUNCTION(BlueprintPure, Category = "SplatterVault|Data", meta = (AdvancedDisplay = "IfVersion"))
	static FSvDataOp MakeSetBoolDataOp(const FString& Key, bool Value, int64 IfVersion = -1);

	/** Delete Key (its version still goes up). */
	UFUNCTION(BlueprintPure, Category = "SplatterVault|Data", meta = (AdvancedDisplay = "IfVersion"))
	static FSvDataOp MakeDeleteDataOp(const FString& Key, int64 IfVersion = -1);

	/** Add By to a whole-number value (or its Field); with Min / Max the result must stay within them (out_of_range). */
	UFUNCTION(BlueprintPure, Category = "SplatterVault|Data", meta = (AdvancedDisplay = "Field,bHasMin,Min,bHasMax,Max,IfVersion"))
	static FSvDataOp MakeIncDataOp(const FString& Key, int64 By, const FString& Field, bool bHasMin, int64 Min, bool bHasMax, int64 Max, int64 IfVersion = -1);

	// ─── reading data ────────────────────────────────────────────────────────────────────────────────────────────

	/** The entry of Key (case-sensitive). */
	UFUNCTION(BlueprintPure, Category = "SplatterVault|Data")
	static bool FindDataEntry(const FSvDataStore& Store, const FString& Key, FSvDataEntry& Entry);

	/** The version of Key, or 0 when it does not exist (what If Version takes for "must not exist"). */
	UFUNCTION(BlueprintPure, Category = "SplatterVault|Data")
	static int64 GetDataVersion(const FSvDataStore& Store, const FString& Key);

	/** The entry's value as text (a JSON string), or Default. */
	UFUNCTION(BlueprintPure, Category = "SplatterVault|Data")
	static FString GetDataString(const FSvDataEntry& Entry, const FString& Default);

	/** The entry's value as a whole number, or Default. */
	UFUNCTION(BlueprintPure, Category = "SplatterVault|Data")
	static int64 GetDataInt(const FSvDataEntry& Entry, int64 Default);

	/** The entry's value as a number, or Default. */
	UFUNCTION(BlueprintPure, Category = "SplatterVault|Data")
	static double GetDataFloat(const FSvDataEntry& Entry, double Default);

	/** The entry's value as a bool, or Default. */
	UFUNCTION(BlueprintPure, Category = "SplatterVault|Data")
	static bool GetDataBool(const FSvDataEntry& Entry, bool Default);

	/** A whole-number field of the entry's object value (for example "level" of {"level":3}), or Default. */
	UFUNCTION(BlueprintPure, Category = "SplatterVault|Data")
	static int64 GetDataObjectInt(const FSvDataEntry& Entry, const FString& Field, int64 Default);

	/** A text field of the entry's object value, or Default. */
	UFUNCTION(BlueprintPure, Category = "SplatterVault|Data")
	static FString GetDataObjectString(const FSvDataEntry& Entry, const FString& Field, const FString& Default);

	/** Text as a JSON string value (quoted and escaped), for Make Set Json Data Op. */
	UFUNCTION(BlueprintPure, Category = "SplatterVault|Data")
	static FString ToJsonString(const FString& Text);

	// ─── errors ──────────────────────────────────────────────────────────────────────────────────────────────────

	/** The error's code is Code (for example insufficient_funds, version_conflict, token_expired). */
	UFUNCTION(BlueprintPure, Category = "SplatterVault|Error")
	static bool IsErrorCode(const FSvError& Error, const FString& Code);

	/** No answer arrived (offline, or no answer in time). */
	UFUNCTION(BlueprintPure, Category = "SplatterVault|Error")
	static bool IsNetworkError(const FSvError& Error);

	/**
	 * The change may or may not have been applied (no answer, 500/502/504, or a conflict after the SDK sent a data
	 * write again). Read again before redoing it; for a store change call Resume Pending Store Changes or send it with
	 * Error.IdempotencyKey, never as a new purchase.
	 */
	UFUNCTION(BlueprintPure, Category = "SplatterVault|Error")
	static bool IsOutcomeUnknown(const FSvError& Error);

	/**
	 * A store change that is still kept on this device: show "finish" (Resume Pending, or the same node with
	 * Error.IdempotencyKey), never "buy again".
	 */
	UFUNCTION(BlueprintPure, Category = "SplatterVault|Error")
	static bool IsChangePending(const FSvError& Error);

	/** The same request may succeed later (no answer, 408, 429, 500, 502, 503, 504). */
	UFUNCTION(BlueprintPure, Category = "SplatterVault|Error")
	static bool IsTransientError(const FSvError& Error);

	/** "code (status): message", for logs. */
	UFUNCTION(BlueprintPure, Category = "SplatterVault|Error", meta = (DisplayName = "To String (SvError)", CompactNodeTitle = "->", BlueprintAutocast))
	static FString ErrorToString(const FSvError& Error);

	/** The current versions a version_conflict answered. */
	UFUNCTION(BlueprintPure, Category = "SplatterVault|Error")
	static TArray<FSvVersionInfo> GetVersionConflicts(const FSvError& Error);

	// ─── store ───────────────────────────────────────────────────────────────────────────────────────────────────

	UFUNCTION(BlueprintPure, Category = "SplatterVault|Store")
	static bool FindCatalogItem(const FSvStoreCatalog& Catalog, const FString& Sku, FSvCatalogItem& Item);

	/** The item's unit price in Currency. */
	UFUNCTION(BlueprintPure, Category = "SplatterVault|Store")
	static bool GetItemPrice(const FSvCatalogItem& Item, const FString& Currency, int64& Amount);

	/** The balance of Currency (0 when the player holds none). */
	UFUNCTION(BlueprintPure, Category = "SplatterVault|Store")
	static int64 GetBalance(const FSvStoreHoldings& Holdings, const FString& Currency);

	/** How many of Sku the player holds, all stacks and copies together. */
	UFUNCTION(BlueprintPure, Category = "SplatterVault|Store")
	static int64 CountItems(const FSvStoreHoldings& Holdings, const FString& Sku);

	/** A new Idempotency-Key (to name a purchase yourself and send it again with the same key). */
	UFUNCTION(BlueprintCallable, Category = "SplatterVault|Store")
	static FString NewIdempotencyKey();

	// ─── misc ────────────────────────────────────────────────────────────────────────────────────────────────────

	/** A Steam ticket's bytes as hex (for Sign In With Steam). */
	UFUNCTION(BlueprintPure, Category = "SplatterVault|Auth")
	static FString SteamTicketToHex(const TArray<uint8>& Ticket);

	/** The SDK's version. */
	UFUNCTION(BlueprintPure, Category = "SplatterVault")
	static FString GetSdkVersion();
};
