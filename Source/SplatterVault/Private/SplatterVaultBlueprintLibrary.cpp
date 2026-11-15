// Copyright (c) SplatterVault. Licensed under the MIT License.

#include "SplatterVaultBlueprintLibrary.h"

#include "SplatterVaultClient.h"
#include "SvJson.h"

namespace
{
	FSvDataOp SetOp(const FString& Key, const FString& Json, int64 IfVersion)
	{
		FSvDataOp Op;
		Op.Op = ESvDataOpType::Set;
		Op.Key = Key;
		Op.ValueJson = Json;
		Op.HasIfVersion = IfVersion >= 0;
		Op.IfVersion = FMath::Max<int64>(0, IfVersion);
		return Op;
	}

	TSharedPtr<FJsonValue> ValueOf(const FSvDataEntry& Entry)
	{
		return Entry.ValueJson.IsEmpty() ? nullptr : SvJson::ParseValue(Entry.ValueJson);
	}
}

FSvDataOp USplatterVaultBlueprintLibrary::MakeSetJsonDataOp(const FString& Key, const FString& ValueJson, int64 IfVersion)
{
	return SetOp(Key, ValueJson, IfVersion);
}

FSvDataOp USplatterVaultBlueprintLibrary::MakeSetStringDataOp(const FString& Key, const FString& Value, int64 IfVersion)
{
	return SetOp(Key, ToJsonString(Value), IfVersion);
}

FSvDataOp USplatterVaultBlueprintLibrary::MakeSetIntDataOp(const FString& Key, int64 Value, int64 IfVersion)
{
	return SetOp(Key, FString::Printf(TEXT("%lld"), (long long)Value), IfVersion);
}

FSvDataOp USplatterVaultBlueprintLibrary::MakeSetFloatDataOp(const FString& Key, double Value, int64 IfVersion)
{
	return SetOp(Key, SvJson::WriteValue(MakeShared<FJsonValueNumber>(Value)), IfVersion);
}

FSvDataOp USplatterVaultBlueprintLibrary::MakeSetBoolDataOp(const FString& Key, bool Value, int64 IfVersion)
{
	return SetOp(Key, Value ? TEXT("true") : TEXT("false"), IfVersion);
}

FSvDataOp USplatterVaultBlueprintLibrary::MakeDeleteDataOp(const FString& Key, int64 IfVersion)
{
	FSvDataOp Op;
	Op.Op = ESvDataOpType::Delete;
	Op.Key = Key;
	Op.HasIfVersion = IfVersion >= 0;
	Op.IfVersion = FMath::Max<int64>(0, IfVersion);
	return Op;
}

FSvDataOp USplatterVaultBlueprintLibrary::MakeIncDataOp(const FString& Key, int64 By, const FString& Field, bool bHasMin, int64 Min, bool bHasMax, int64 Max,
	int64 IfVersion)
{
	FSvDataOp Op;
	Op.Op = ESvDataOpType::Inc;
	Op.Key = Key;
	Op.By = By;
	Op.Field = Field;
	Op.HasMin = bHasMin;
	Op.Min = Min;
	Op.HasMax = bHasMax;
	Op.Max = Max;
	Op.HasIfVersion = IfVersion >= 0;
	Op.IfVersion = FMath::Max<int64>(0, IfVersion);
	return Op;
}

bool USplatterVaultBlueprintLibrary::FindDataEntry(const FSvDataStore& Store, const FString& Key, FSvDataEntry& Entry)
{
	const FSvDataEntry* Found = Store.Find(Key);
	if (!Found) return false;
	Entry = *Found;
	return true;
}

int64 USplatterVaultBlueprintLibrary::GetDataVersion(const FSvDataStore& Store, const FString& Key)
{
	return Store.VersionOf(Key);
}

FString USplatterVaultBlueprintLibrary::GetDataString(const FSvDataEntry& Entry, const FString& Default)
{
	const TSharedPtr<FJsonValue> V = ValueOf(Entry);
	return V.IsValid() && V->Type == EJson::String ? V->AsString() : Default;
}

int64 USplatterVaultBlueprintLibrary::GetDataInt(const FSvDataEntry& Entry, int64 Default)
{
	const TSharedPtr<FJsonValue> V = ValueOf(Entry);
	return V.IsValid() && V->Type == EJson::Number ? (int64)V->AsNumber() : Default;
}

double USplatterVaultBlueprintLibrary::GetDataFloat(const FSvDataEntry& Entry, double Default)
{
	const TSharedPtr<FJsonValue> V = ValueOf(Entry);
	return V.IsValid() && V->Type == EJson::Number ? V->AsNumber() : Default;
}

bool USplatterVaultBlueprintLibrary::GetDataBool(const FSvDataEntry& Entry, bool Default)
{
	const TSharedPtr<FJsonValue> V = ValueOf(Entry);
	return V.IsValid() && V->Type == EJson::Boolean ? V->AsBool() : Default;
}

int64 USplatterVaultBlueprintLibrary::GetDataObjectInt(const FSvDataEntry& Entry, const FString& Field, int64 Default)
{
	// Field names are matched case-sensitively, as stored (Unreal's JSON objects would merge "x" and "X").
	FSvRawJson Raw;
	if (Entry.ValueJson.IsEmpty() || !Raw.Parse(Entry.ValueJson)) return Default;
	int64 Out = Default;
	return Raw.Int64(Raw.Member(Raw.Root(), Field), Out) ? Out : Default;
}

FString USplatterVaultBlueprintLibrary::GetDataObjectString(const FSvDataEntry& Entry, const FString& Field, const FString& Default)
{
	FSvRawJson Raw;
	if (Entry.ValueJson.IsEmpty() || !Raw.Parse(Entry.ValueJson)) return Default;
	const int32 Node = Raw.Member(Raw.Root(), Field);
	return Raw.Type(Node) == EJson::String ? Raw.String(Node) : Default;
}

FString USplatterVaultBlueprintLibrary::ToJsonString(const FString& Text)
{
	return SvJson::WriteValue(MakeShared<FJsonValueString>(Text));
}

bool USplatterVaultBlueprintLibrary::IsErrorCode(const FSvError& Error, const FString& Code)
{
	return Error.Is(Code);
}

bool USplatterVaultBlueprintLibrary::IsNetworkError(const FSvError& Error)
{
	return Error.IsNetworkError();
}

bool USplatterVaultBlueprintLibrary::IsOutcomeUnknown(const FSvError& Error)
{
	return Error.OutcomeUnknown;
}

bool USplatterVaultBlueprintLibrary::IsChangePending(const FSvError& Error)
{
	return Error.Pending;
}

bool USplatterVaultBlueprintLibrary::IsTransientError(const FSvError& Error)
{
	return Error.IsTransient();
}

FString USplatterVaultBlueprintLibrary::ErrorToString(const FSvError& Error)
{
	return Error.ToString();
}

TArray<FSvVersionInfo> USplatterVaultBlueprintLibrary::GetVersionConflicts(const FSvError& Error)
{
	TArray<FSvVersionInfo> Out;
	const TSharedPtr<FJsonValue> D = Error.DetailsJson.IsEmpty() ? nullptr : SvJson::ParseValue(Error.DetailsJson);
	if (!D.IsValid()) return Out;
	TArray<TSharedPtr<FJsonValue>> Items;
	if (D->Type == EJson::Array)
	{
		Items = D->AsArray();
	}
	else if (D->Type == EJson::Object)
	{
		// Also accept { versions: [...] } or { conflicts: [...] }.
		const TArray<TSharedPtr<FJsonValue>>* Inner = nullptr;
		if (D->AsObject()->TryGetArrayField(TEXT("versions"), Inner) || D->AsObject()->TryGetArrayField(TEXT("conflicts"), Inner))
		{
			if (Inner) Items = *Inner;
		}
	}
	for (const TSharedPtr<FJsonValue>& I : Items)
	{
		FSvVersionInfo V;
		if (I.IsValid() && I->Type == EJson::Object && SvModels::Read(I->AsObject(), V)) Out.Add(V);
	}
	return Out;
}

bool USplatterVaultBlueprintLibrary::FindCatalogItem(const FSvStoreCatalog& Catalog, const FString& Sku, FSvCatalogItem& Item)
{
	const FSvCatalogItem* Found = Catalog.Find(Sku);
	if (!Found) return false;
	Item = *Found;
	return true;
}

bool USplatterVaultBlueprintLibrary::GetItemPrice(const FSvCatalogItem& Item, const FString& Currency, int64& Amount)
{
	Amount = 0;
	return Item.PriceIn(Currency, Amount);
}

int64 USplatterVaultBlueprintLibrary::GetBalance(const FSvStoreHoldings& Holdings, const FString& Currency)
{
	return Holdings.BalanceOf(Currency);
}

int64 USplatterVaultBlueprintLibrary::CountItems(const FSvStoreHoldings& Holdings, const FString& Sku)
{
	return Holdings.CountOf(Sku);
}

FString USplatterVaultBlueprintLibrary::NewIdempotencyKey()
{
	return FSvStoreApi::NewIdempotencyKey();
}

FString USplatterVaultBlueprintLibrary::SteamTicketToHex(const TArray<uint8>& Ticket)
{
	return FSvAuth::ToHex(Ticket);
}

FString USplatterVaultBlueprintLibrary::GetSdkVersion()
{
	return SPLATTERVAULT_SDK_VERSION;
}
