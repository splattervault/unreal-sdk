// Copyright (c) SplatterVault. Licensed under the MIT License.

#include "SvTypes.h"

#include "SvJson.h"

FSvError::FSvError(int32 InStatus, const FString& InCode, const FString& InMessage)
	: Status(InStatus)
	, Code(InCode)
	, Message(InMessage)
{
	if (Message.IsEmpty())
	{
		if (Code == SvErrorCodes::NetworkError) Message = TEXT("No answer from SplatterVault (network error).");
		else if (Code == SvErrorCodes::Timeout) Message = TEXT("No answer from SplatterVault in time.");
		else if (Code == SvErrorCodes::NotSignedIn) Message = TEXT("No player is signed in.");
		else if (Code == SvErrorCodes::StorageUnreadable) Message = TEXT("The device's secure storage could not be read. Nothing was changed.");
		else if (!Code.IsEmpty()) Message = Code;
		else Message = Status > 0 ? FString::Printf(TEXT("HTTP %d"), Status) : FString(TEXT("Request failed."));
	}
}

bool FSvError::IsNetworkError() const
{
	return Status == 0 && (Code == SvErrorCodes::NetworkError || Code == SvErrorCodes::Timeout);
}

bool FSvError::IsTransientStatus(int32 InStatus)
{
	return InStatus == 408 || InStatus == 429 || InStatus == 500 || InStatus == 502 || InStatus == 503 || InStatus == 504;
}

bool FSvError::IsTransient() const
{
	return IsNetworkError() || IsTransientStatus(Status);
}

FString FSvError::ToString() const
{
	return FString::Printf(TEXT("%s (%d): %s"), Code.IsEmpty() ? (ErrorName.IsEmpty() ? TEXT("error") : *ErrorName) : *Code, Status, *Message);
}

bool FSvPlayerProfile::HasIdentity(const FString& Provider) const
{
	for (const FSvIdentity& I : Identities)
	{
		if (I.Provider.Equals(Provider, ESearchCase::CaseSensitive)) return true;
	}
	return false;
}

TSharedPtr<FJsonValue> FSvDataEntry::GetValue() const
{
	return ValueJson.IsEmpty() ? nullptr : SvJson::ParseValue(ValueJson);
}

const FSvDataEntry* FSvDataStore::Find(const FString& Key) const
{
	for (const FSvDataEntry& E : Entries)
	{
		if (E.Key.Equals(Key, ESearchCase::CaseSensitive)) return &E;
	}
	return nullptr;
}

int64 FSvDataStore::VersionOf(const FString& Key) const
{
	const FSvDataEntry* E = Find(Key);
	return E ? E->Version : 0;
}

bool FSvCatalogItem::PriceIn(const FString& InCurrency, int64& OutAmount) const
{
	for (const FSvPrice& P : Prices)
	{
		if (P.Currency.Equals(InCurrency, ESearchCase::CaseSensitive))
		{
			OutAmount = P.Amount;
			return true;
		}
	}
	return false;
}

const FSvCatalogItem* FSvStoreCatalog::Find(const FString& Sku) const
{
	for (const FSvCatalogItem& I : Items)
	{
		if (I.Sku.Equals(Sku, ESearchCase::CaseSensitive)) return &I;
	}
	return nullptr;
}

int64 FSvStoreHoldings::BalanceOf(const FString& Currency) const
{
	for (const FSvBalance& B : Balances)
	{
		if (B.Currency.Equals(Currency, ESearchCase::CaseSensitive)) return B.Amount;
	}
	return 0;
}

int64 FSvStoreHoldings::CountOf(const FString& Sku) const
{
	int64 N = 0;
	for (const FSvInventoryItem& I : Items)
	{
		if (I.Sku.Equals(Sku, ESearchCase::CaseSensitive)) N += I.Quantity;
	}
	return N;
}
