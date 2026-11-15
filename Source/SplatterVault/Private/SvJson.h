// Copyright (c) SplatterVault. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "SvTypes.h"

/** Small JSON helpers (the SDK never logs what it parses). */
namespace SvJson
{
	/** Parses JSON text as an object; null when it is not one. */
	TSharedPtr<FJsonObject> ParseObject(const FString& Text);

	/** Parses any JSON value (object, array, string, number, bool, null); null when unreadable. */
	TSharedPtr<FJsonValue> ParseValue(const FString& Text);

	/** Compact JSON text of an object. */
	FString Write(const TSharedRef<FJsonObject>& Object);

	/** Compact JSON text of any value (null writes "null"). */
	FString WriteValue(const TSharedPtr<FJsonValue>& Value);

	/** The field as a string; empty when missing, null or not a string. */
	FString GetString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field);

	/** The field as a whole number; false when missing, null or not a number. */
	bool GetInt64(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, int64& Out);

	/** The field as a number; false when missing, null or not a number. */
	bool GetNumber(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, double& Out);

	bool GetBool(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, bool Default = false);

	/** The field as an object; null when missing or not one. */
	TSharedPtr<FJsonObject> GetObj(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field);

	/** The field's raw value as JSON text; empty when missing or null. */
	FString GetRawJson(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field);

	/** A JSON number for an int64 (exact up to 2^53). */
	TSharedRef<FJsonValue> Int(int64 Value);
}

/**
 * A JSON document read without Unreal's JSON DOM, whose objects compare keys without case (FJsonObject keeps one of
 * "score" and "Score"). Here object keys stay case-sensitive and every value keeps its exact text: player data keys
 * and values come through as the API sent them.
 */
class FSvRawJson
{
public:
	static constexpr int32 None = -1;

	/** Reads Text (one JSON value, at most 64 levels deep). False when it is not JSON. */
	bool Parse(const FString& InText);

	/** The top value, or None. */
	int32 Root() const { return Nodes.Num() > 0 ? 0 : None; }

	/** The node's type (EJson::None for None). */
	EJson Type(int32 Node) const;

	/** The node's exact text. */
	FString Raw(int32 Node) const;

	/** The member Key of an object (case-sensitive), or None. */
	int32 Member(int32 Object, const FString& Key) const;

	/** An object's members in order (empty for anything else). */
	const TArray<TPair<FString, int32>>& Members(int32 Object) const;

	/** An array's elements in order (empty for anything else). */
	const TArray<int32>& Elements(int32 Array) const;

	/** A string node's decoded value (empty for anything else). */
	FString String(int32 Node) const;

	/** A number node's value. */
	bool Number(int32 Node, double& Out) const;

	/** A whole number within +-2^53. */
	bool Int64(int32 Node, int64& Out) const;

private:
	struct FNode
	{
		EJson Type = EJson::None;
		int32 Start = 0;
		int32 End = 0;
		FString Str;
		TArray<TPair<FString, int32>> Members;
		TArray<int32> Elements;
	};

	bool ParseValue(int32& Pos, int32 Depth, int32& OutNode);
	bool ParseString(int32& Pos, FString& Out) const;
	void SkipSpace(int32& Pos) const;

	FString Text;
	TArray<FNode> Nodes;
};

/** Models from and to the API's JSON (FJsonObjectConverter plus the fields it cannot map: nulls, raw JSON). */
namespace SvModels
{
	bool Read(const TSharedPtr<FJsonObject>& J, FSvIdentity& Out);
	bool Read(const TSharedPtr<FJsonObject>& J, FSvSignedInPlayer& Out);
	bool Read(const TSharedPtr<FJsonObject>& J, FSvPlayerProfile& Out);
	bool Read(const TSharedPtr<FJsonObject>& J, FSvCodeSent& Out);
	bool Read(const TSharedPtr<FJsonObject>& J, FSvDataWriteResult& Out);
	bool Read(const TSharedPtr<FJsonObject>& J, FSvVersionInfo& Out);
	bool Read(const TSharedPtr<FJsonObject>& J, FSvStoreCatalog& Out);
	bool Read(const TSharedPtr<FJsonObject>& J, FSvStoreHoldings& Out);
	bool Read(const TSharedPtr<FJsonObject>& J, FSvStoreTxnResult& Out);
	bool Read(const TSharedPtr<FJsonObject>& J, FSvStoreHistoryPage& Out);
	bool Read(const TSharedPtr<FJsonObject>& J, FSvMetaSyncResult& Out);
	bool Read(const TSharedPtr<FJsonObject>& J, FSvPendingStoreChange& Out);

	/** An answer's text into a model: the API's JSON as-is for player data (case-sensitive keys, exact values). */
	template <typename T>
	bool ReadText(const FString& Text, T& Out)
	{
		return Read(SvJson::ParseObject(Text), Out);
	}
	/** { data: { player: {...}, readonly: {...} } } */
	bool ReadPlayerDataText(const FString& Text, FSvPlayerData& Out);
	/** { data: { key: entry } } */
	bool ReadGameDataText(const FString& Text, FSvDataStore& Out);
	bool ReadText(const FString& Text, FSvDataWriteResult& Out);
	bool ReadText(const FString& Text, FSvStoreCatalog& Out);
	bool ReadText(const FString& Text, FSvStoreHoldings& Out);
	bool ReadText(const FString& Text, FSvStoreTxnResult& Out);

	TSharedRef<FJsonObject> Write(const FSvPlayerProfile& V);
	/** { key: entry } as JSON text (keys kept case-sensitive, values verbatim). */
	FString WriteDataStoreText(const FSvDataStore& V);
	FString WritePlayerDataText(const FSvPlayerData& V);
	TSharedRef<FJsonObject> Write(const FSvDataWriteResult& V);
	TSharedRef<FJsonObject> Write(const FSvStoreCatalog& V);
	TSharedRef<FJsonObject> Write(const FSvStoreHoldings& V);
	TSharedRef<FJsonObject> Write(const FSvStoreTxnResult& V);
	TSharedRef<FJsonObject> Write(const FSvStoreHistoryPage& V);
	TSharedRef<FJsonObject> Write(const FSvMetaSyncResult& V);
	TSharedRef<FJsonObject> Write(const FSvPendingStoreChange& V);
}
