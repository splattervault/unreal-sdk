// Copyright (c) SplatterVault. Licensed under the MIT License.

#include "SvJson.h"

#include "JsonObjectConverter.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace SvJson
{
	TSharedPtr<FJsonObject> ParseObject(const FString& Text)
	{
		if (Text.IsEmpty()) return nullptr;
		TSharedPtr<FJsonObject> Out;
		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Text);
		if (!FJsonSerializer::Deserialize(Reader, Out) || !Out.IsValid()) return nullptr;
		return Out;
	}

	TSharedPtr<FJsonValue> ParseValue(const FString& Text)
	{
		const FString Trimmed = Text.TrimStartAndEnd();
		if (Trimmed.IsEmpty()) return nullptr;
		// FJsonSerializer reads an object or an array at the top level; wrap anything else in an array.
		const FString Wrapped = TEXT("[") + Trimmed + TEXT("]");
		TArray<TSharedPtr<FJsonValue>> Values;
		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Wrapped);
		if (!FJsonSerializer::Deserialize(Reader, Values) || Values.Num() != 1 || !Values[0].IsValid()) return nullptr;
		return Values[0];
	}

	FString Write(const TSharedRef<FJsonObject>& Object)
	{
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Object, Writer);
		Writer->Close();
		return Out;
	}

	FString WriteValue(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid() || Value->IsNull()) return TEXT("null");
		// Serialize the value as the only element of an array, then strip the brackets.
		TArray<TSharedPtr<FJsonValue>> One;
		One.Add(Value);
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(One, Writer);
		Writer->Close();
		if (Out.Len() >= 2 && Out[0] == TEXT('[') && Out[Out.Len() - 1] == TEXT(']'))
		{
			return Out.Mid(1, Out.Len() - 2);
		}
		return Out;
	}

	FString GetString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		if (!Object.IsValid()) return FString();
		const TSharedPtr<FJsonValue> V = Object->TryGetField(Field);
		if (!V.IsValid() || V->Type != EJson::String) return FString();
		return V->AsString();
	}

	bool GetNumber(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, double& Out)
	{
		if (!Object.IsValid()) return false;
		const TSharedPtr<FJsonValue> V = Object->TryGetField(Field);
		if (!V.IsValid() || V->Type != EJson::Number) return false;
		Out = V->AsNumber();
		return FMath::IsFinite(Out);
	}

	bool GetInt64(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, int64& Out)
	{
		double D = 0;
		if (!GetNumber(Object, Field, D)) return false;
		if (D > 9007199254740991.0 || D < -9007199254740991.0) return false;
		Out = (int64)D;
		return true;
	}

	bool GetBool(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, bool Default)
	{
		if (!Object.IsValid()) return Default;
		const TSharedPtr<FJsonValue> V = Object->TryGetField(Field);
		if (!V.IsValid() || V->Type != EJson::Boolean) return Default;
		return V->AsBool();
	}

	TSharedPtr<FJsonObject> GetObj(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		if (!Object.IsValid()) return nullptr;
		const TSharedPtr<FJsonValue> V = Object->TryGetField(Field);
		if (!V.IsValid() || V->Type != EJson::Object) return nullptr;
		return V->AsObject();
	}

	FString GetRawJson(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		if (!Object.IsValid()) return FString();
		const TSharedPtr<FJsonValue> V = Object->TryGetField(Field);
		if (!V.IsValid() || V->IsNull()) return FString();
		return WriteValue(V);
	}

	TSharedRef<FJsonValue> Int(int64 Value)
	{
		return MakeShared<FJsonValueNumber>((double)Value);
	}
}

// ─── FSvRawJson ───────────────────────────────────────────────────────────────────────────────────────────────────

namespace
{
	const TArray<TPair<FString, int32>> NoMembers;
	const TArray<int32> NoElements;

	int32 HexDigit(TCHAR C)
	{
		if (C >= TEXT('0') && C <= TEXT('9')) return C - TEXT('0');
		if (C >= TEXT('a') && C <= TEXT('f')) return C - TEXT('a') + 10;
		if (C >= TEXT('A') && C <= TEXT('F')) return C - TEXT('A') + 10;
		return -1;
	}

	void AppendCodePoint(FString& Out, uint32 Cp)
	{
		// TCHAR is UTF-16 on Windows and Android (UE): write surrogate pairs; elsewhere one code unit.
		if (sizeof(TCHAR) == 2 && Cp > 0xFFFF)
		{
			Cp -= 0x10000;
			Out.AppendChar((TCHAR)(0xD800 + (Cp >> 10)));
			Out.AppendChar((TCHAR)(0xDC00 + (Cp & 0x3FF)));
		}
		else
		{
			Out.AppendChar((TCHAR)Cp);
		}
	}
}

void FSvRawJson::SkipSpace(int32& Pos) const
{
	while (Pos < Text.Len())
	{
		const TCHAR C = Text[Pos];
		if (C != TEXT(' ') && C != TEXT('\t') && C != TEXT('\n') && C != TEXT('\r')) break;
		++Pos;
	}
}

bool FSvRawJson::ParseString(int32& Pos, FString& Out) const
{
	if (Pos >= Text.Len() || Text[Pos] != TEXT('"')) return false;
	++Pos;
	Out.Reset();
	while (Pos < Text.Len())
	{
		const TCHAR C = Text[Pos++];
		if (C == TEXT('"')) return true;
		if (C < 0x20) return false;
		if (C != TEXT('\\'))
		{
			Out.AppendChar(C);
			continue;
		}
		if (Pos >= Text.Len()) return false;
		const TCHAR E = Text[Pos++];
		switch (E)
		{
		case TEXT('"'): Out.AppendChar(TEXT('"')); break;
		case TEXT('\\'): Out.AppendChar(TEXT('\\')); break;
		case TEXT('/'): Out.AppendChar(TEXT('/')); break;
		case TEXT('b'): Out.AppendChar(TEXT('\b')); break;
		case TEXT('f'): Out.AppendChar(TEXT('\f')); break;
		case TEXT('n'): Out.AppendChar(TEXT('\n')); break;
		case TEXT('r'): Out.AppendChar(TEXT('\r')); break;
		case TEXT('t'): Out.AppendChar(TEXT('\t')); break;
		case TEXT('u'):
		{
			if (Pos + 4 > Text.Len()) return false;
			uint32 Cp = 0;
			for (int32 i = 0; i < 4; ++i)
			{
				const int32 D = HexDigit(Text[Pos + i]);
				if (D < 0) return false;
				Cp = (Cp << 4) | (uint32)D;
			}
			Pos += 4;
			if (Cp >= 0xD800 && Cp <= 0xDBFF && Pos + 6 <= Text.Len() && Text[Pos] == TEXT('\\') && Text[Pos + 1] == TEXT('u'))
			{
				uint32 Lo = 0;
				bool bHex = true;
				for (int32 i = 0; i < 4; ++i)
				{
					const int32 D = HexDigit(Text[Pos + 2 + i]);
					if (D < 0) bHex = false;
					Lo = (Lo << 4) | (uint32)FMath::Max(D, 0);
				}
				if (bHex && Lo >= 0xDC00 && Lo <= 0xDFFF)
				{
					Pos += 6;
					Cp = 0x10000 + ((Cp - 0xD800) << 10) + (Lo - 0xDC00);
				}
			}
			AppendCodePoint(Out, Cp);
			break;
		}
		default:
			return false;
		}
	}
	return false;
}

bool FSvRawJson::ParseValue(int32& Pos, int32 Depth, int32& OutNode)
{
	if (Depth > 64) return false;
	SkipSpace(Pos);
	if (Pos >= Text.Len()) return false;
	const int32 Index = Nodes.AddDefaulted();
	Nodes[Index].Start = Pos;
	OutNode = Index;
	const TCHAR C = Text[Pos];
	if (C == TEXT('{'))
	{
		Nodes[Index].Type = EJson::Object;
		++Pos;
		SkipSpace(Pos);
		if (Pos < Text.Len() && Text[Pos] == TEXT('}'))
		{
			++Pos;
			Nodes[Index].End = Pos;
			return true;
		}
		while (true)
		{
			SkipSpace(Pos);
			FString Key;
			if (!ParseString(Pos, Key)) return false;
			SkipSpace(Pos);
			if (Pos >= Text.Len() || Text[Pos] != TEXT(':')) return false;
			++Pos;
			int32 Child = None;
			if (!ParseValue(Pos, Depth + 1, Child)) return false;
			Nodes[Index].Members.Add(TPair<FString, int32>(MoveTemp(Key), Child));
			SkipSpace(Pos);
			if (Pos >= Text.Len()) return false;
			if (Text[Pos] == TEXT(','))
			{
				++Pos;
				continue;
			}
			if (Text[Pos] != TEXT('}')) return false;
			++Pos;
			Nodes[Index].End = Pos;
			return true;
		}
	}
	if (C == TEXT('['))
	{
		Nodes[Index].Type = EJson::Array;
		++Pos;
		SkipSpace(Pos);
		if (Pos < Text.Len() && Text[Pos] == TEXT(']'))
		{
			++Pos;
			Nodes[Index].End = Pos;
			return true;
		}
		while (true)
		{
			int32 Child = None;
			if (!ParseValue(Pos, Depth + 1, Child)) return false;
			Nodes[Index].Elements.Add(Child);
			SkipSpace(Pos);
			if (Pos >= Text.Len()) return false;
			if (Text[Pos] == TEXT(','))
			{
				++Pos;
				continue;
			}
			if (Text[Pos] != TEXT(']')) return false;
			++Pos;
			Nodes[Index].End = Pos;
			return true;
		}
	}
	if (C == TEXT('"'))
	{
		Nodes[Index].Type = EJson::String;
		FString Str;
		if (!ParseString(Pos, Str)) return false;
		Nodes[Index].Str = MoveTemp(Str);
		Nodes[Index].End = Pos;
		return true;
	}
	auto Literal = [this, &Pos](const TCHAR* Word) -> bool
	{
		const int32 N = FCString::Strlen(Word);
		if (Pos + N > Text.Len() || FCString::Strncmp(*Text + Pos, Word, N) != 0) return false;
		Pos += N;
		return true;
	};
	if (C == TEXT('t') || C == TEXT('f'))
	{
		Nodes[Index].Type = EJson::Boolean;
		if (!Literal(C == TEXT('t') ? TEXT("true") : TEXT("false"))) return false;
		Nodes[Index].End = Pos;
		return true;
	}
	if (C == TEXT('n'))
	{
		Nodes[Index].Type = EJson::Null;
		if (!Literal(TEXT("null"))) return false;
		Nodes[Index].End = Pos;
		return true;
	}
	if (C == TEXT('-') || (C >= TEXT('0') && C <= TEXT('9')))
	{
		Nodes[Index].Type = EJson::Number;
		int32 P = Pos;
		if (Text[P] == TEXT('-')) ++P;
		const int32 DigitsStart = P;
		while (P < Text.Len() && FChar::IsDigit(Text[P])) ++P;
		if (P == DigitsStart) return false;
		if (P < Text.Len() && Text[P] == TEXT('.'))
		{
			++P;
			const int32 FracStart = P;
			while (P < Text.Len() && FChar::IsDigit(Text[P])) ++P;
			if (P == FracStart) return false;
		}
		if (P < Text.Len() && (Text[P] == TEXT('e') || Text[P] == TEXT('E')))
		{
			++P;
			if (P < Text.Len() && (Text[P] == TEXT('+') || Text[P] == TEXT('-'))) ++P;
			const int32 ExpStart = P;
			while (P < Text.Len() && FChar::IsDigit(Text[P])) ++P;
			if (P == ExpStart) return false;
		}
		Pos = P;
		Nodes[Index].End = Pos;
		return true;
	}
	return false;
}

bool FSvRawJson::Parse(const FString& InText)
{
	Text = InText;
	Nodes.Reset();
	int32 Pos = 0;
	int32 RootNode = None;
	if (!ParseValue(Pos, 0, RootNode))
	{
		Nodes.Reset();
		return false;
	}
	SkipSpace(Pos);
	if (Pos != Text.Len())
	{
		Nodes.Reset();
		return false;
	}
	return true;
}

EJson FSvRawJson::Type(int32 Node) const
{
	return Nodes.IsValidIndex(Node) ? Nodes[Node].Type : EJson::None;
}

FString FSvRawJson::Raw(int32 Node) const
{
	return Nodes.IsValidIndex(Node) ? Text.Mid(Nodes[Node].Start, Nodes[Node].End - Nodes[Node].Start) : FString();
}

int32 FSvRawJson::Member(int32 Object, const FString& Key) const
{
	for (const TPair<FString, int32>& M : Members(Object))
	{
		if (M.Key.Equals(Key, ESearchCase::CaseSensitive)) return M.Value;
	}
	return None;
}

const TArray<TPair<FString, int32>>& FSvRawJson::Members(int32 Object) const
{
	return Nodes.IsValidIndex(Object) && Nodes[Object].Type == EJson::Object ? Nodes[Object].Members : NoMembers;
}

const TArray<int32>& FSvRawJson::Elements(int32 Array) const
{
	return Nodes.IsValidIndex(Array) && Nodes[Array].Type == EJson::Array ? Nodes[Array].Elements : NoElements;
}

FString FSvRawJson::String(int32 Node) const
{
	return Nodes.IsValidIndex(Node) && Nodes[Node].Type == EJson::String ? Nodes[Node].Str : FString();
}

bool FSvRawJson::Number(int32 Node, double& Out) const
{
	if (!Nodes.IsValidIndex(Node) || Nodes[Node].Type != EJson::Number) return false;
	Out = FCString::Atod(*Raw(Node));
	return FMath::IsFinite(Out);
}

bool FSvRawJson::Int64(int32 Node, int64& Out) const
{
	double D = 0;
	if (!Number(Node, D) || D > 9007199254740991.0 || D < -9007199254740991.0) return false;
	Out = (int64)D;
	return true;
}

namespace
{
	template <typename T>
	bool Convert(const TSharedPtr<FJsonObject>& J, T& Out)
	{
		if (!J.IsValid()) return false;
		Out = T();
		return FJsonObjectConverter::JsonObjectToUStruct(J.ToSharedRef(), &Out, 0, 0);
	}

	template <typename T>
	TSharedRef<FJsonObject> ToObject(const T& V)
	{
		const TSharedPtr<FJsonObject> O = FJsonObjectConverter::UStructToJsonObject(V);
		return O.IsValid() ? O.ToSharedRef() : MakeShared<FJsonObject>();
	}

	TArray<TSharedPtr<FJsonValue>> ArrayField(const TSharedPtr<FJsonObject>& J, const TCHAR* Field)
	{
		if (!J.IsValid()) return TArray<TSharedPtr<FJsonValue>>();
		const TSharedPtr<FJsonValue> V = J->TryGetField(Field);
		if (!V.IsValid() || V->Type != EJson::Array) return TArray<TSharedPtr<FJsonValue>>();
		return V->AsArray();
	}

	TSharedPtr<FJsonObject> ObjectAt(const TArray<TSharedPtr<FJsonValue>>& A, int32 Index)
	{
		if (!A.IsValidIndex(Index) || !A[Index].IsValid() || A[Index]->Type != EJson::Object) return nullptr;
		return A[Index]->AsObject();
	}

	/** Raw JSON text into a field (null when empty or unreadable). */
	void SetRaw(const TSharedRef<FJsonObject>& O, const TCHAR* Field, const FString& Json)
	{
		const TSharedPtr<FJsonValue> V = Json.IsEmpty() ? nullptr : SvJson::ParseValue(Json);
		O->SetField(Field, V.IsValid() ? V : MakeShared<FJsonValueNull>());
	}

	void SetNullableString(const TSharedRef<FJsonObject>& O, const TCHAR* Field, const FString& Value)
	{
		if (Value.IsEmpty()) O->SetField(Field, MakeShared<FJsonValueNull>());
		else O->SetStringField(Field, Value);
	}

	void ReadItemFixups(const TSharedPtr<FJsonObject>& J, FSvInventoryItem& Item)
	{
		Item.CustomJson = SvJson::GetRawJson(J, TEXT("custom"));
	}

	void ReadItemsFixups(const TArray<TSharedPtr<FJsonValue>>& A, TArray<FSvInventoryItem>& Items)
	{
		for (int32 i = 0; i < Items.Num(); ++i) ReadItemFixups(ObjectAt(A, i), Items[i]);
	}

	TSharedRef<FJsonObject> WriteItem(const FSvInventoryItem& Item)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("instanceId"), Item.InstanceId);
		O->SetStringField(TEXT("sku"), Item.Sku);
		O->SetField(TEXT("quantity"), SvJson::Int(Item.Quantity));
		SetRaw(O, TEXT("custom"), Item.CustomJson);
		O->SetStringField(TEXT("acquiredAt"), Item.AcquiredAt);
		return O;
	}

	TArray<TSharedPtr<FJsonValue>> WriteItems(const TArray<FSvInventoryItem>& Items)
	{
		TArray<TSharedPtr<FJsonValue>> A;
		for (const FSvInventoryItem& I : Items) A.Add(MakeShared<FJsonValueObject>(WriteItem(I)));
		return A;
	}

	TSharedRef<FJsonObject> WriteChange(const FSvLedgerChange& C)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		if (!C.Currency.IsEmpty()) O->SetStringField(TEXT("currency"), C.Currency);
		if (!C.Sku.IsEmpty()) O->SetStringField(TEXT("sku"), C.Sku);
		if (!C.InstanceId.IsEmpty()) O->SetStringField(TEXT("instanceId"), C.InstanceId);
		O->SetField(TEXT("delta"), SvJson::Int(C.Delta));
		O->SetField(TEXT("after"), SvJson::Int(C.After));
		return O;
	}

	TSharedRef<FJsonObject> WriteShortfall(const FSvShortfall& S)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		if (!S.Currency.IsEmpty()) O->SetStringField(TEXT("currency"), S.Currency);
		if (!S.Sku.IsEmpty()) O->SetStringField(TEXT("sku"), S.Sku);
		O->SetField(TEXT("missing"), SvJson::Int(S.Missing));
		return O;
	}

	template <typename T, typename F>
	TArray<TSharedPtr<FJsonValue>> WriteArray(const TArray<T>& Items, F&& Fn)
	{
		TArray<TSharedPtr<FJsonValue>> A;
		for (const T& I : Items) A.Add(MakeShared<FJsonValueObject>(Fn(I)));
		return A;
	}

	TSharedRef<FJsonObject> WriteBalance(const FSvBalance& B)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("currency"), B.Currency);
		O->SetField(TEXT("amount"), SvJson::Int(B.Amount));
		return O;
	}

	TSharedRef<FJsonObject> WriteIdentity(const FSvIdentity& I)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("provider"), I.Provider);
		O->SetStringField(TEXT("linkedAt"), I.LinkedAt);
		return O;
	}
}

namespace SvModels
{
	bool Read(const TSharedPtr<FJsonObject>& J, FSvIdentity& Out) { return Convert(J, Out); }

	bool Read(const TSharedPtr<FJsonObject>& J, FSvSignedInPlayer& Out) { return Convert(J, Out); }

	bool Read(const TSharedPtr<FJsonObject>& J, FSvPlayerProfile& Out) { return Convert(J, Out); }

	bool Read(const TSharedPtr<FJsonObject>& J, FSvCodeSent& Out) { return Convert(J, Out); }

	namespace
	{
		bool ReadStore(const FSvRawJson& R, int32 Object, FSvDataStore& Out)
		{
			Out = FSvDataStore();
			if (R.Type(Object) != EJson::Object) return false;
			for (const TPair<FString, int32>& M : R.Members(Object))
			{
				if (R.Type(M.Value) != EJson::Object) continue;
				FSvDataEntry E;
				E.Key = M.Key;
				const int32 Value = R.Member(M.Value, TEXT("value"));
				if (Value != FSvRawJson::None && R.Type(Value) != EJson::Null) E.ValueJson = R.Raw(Value);
				R.Int64(R.Member(M.Value, TEXT("version")), E.Version);
				E.UpdatedAt = R.String(R.Member(M.Value, TEXT("updatedAt")));
				Out.Entries.Add(E);
			}
			return true;
		}

		/** The raw "custom" of each element of the array member Field (empty when null or missing). */
		void RawCustoms(const FSvRawJson& R, const TCHAR* Field, TArray<FString>& Out)
		{
			Out.Reset();
			for (const int32 Item : R.Elements(R.Member(R.Root(), Field)))
			{
				const int32 Custom = R.Member(Item, TEXT("custom"));
				Out.Add(Custom != FSvRawJson::None && R.Type(Custom) != EJson::Null ? R.Raw(Custom) : FString());
			}
		}

		void AppendEscaped(FString& Out, const FString& S)
		{
			Out += SvJson::WriteValue(MakeShared<FJsonValueString>(S));
		}
	}

	bool ReadPlayerDataText(const FString& Text, FSvPlayerData& Out)
	{
		Out = FSvPlayerData();
		FSvRawJson R;
		if (!R.Parse(Text)) return false;
		const int32 Data = R.Member(R.Root(), TEXT("data"));
		if (R.Type(Data) != EJson::Object) return false;
		ReadStore(R, R.Member(Data, TEXT("player")), Out.Player);
		ReadStore(R, R.Member(Data, TEXT("readonly")), Out.Readonly);
		return true;
	}

	bool ReadGameDataText(const FString& Text, FSvDataStore& Out)
	{
		Out = FSvDataStore();
		FSvRawJson R;
		if (!R.Parse(Text)) return false;
		return ReadStore(R, R.Member(R.Root(), TEXT("data")), Out);
	}

	bool ReadText(const FString& Text, FSvDataWriteResult& Out)
	{
		if (!Read(SvJson::ParseObject(Text), Out)) return false;
		FSvRawJson R;
		if (!R.Parse(Text)) return false;
		const TArray<int32>& Results = R.Elements(R.Member(R.Root(), TEXT("results")));
		for (int32 i = 0; i < Out.Results.Num() && i < Results.Num(); ++i)
		{
			const int32 Value = R.Member(Results[i], TEXT("value"));
			Out.Results[i].ValueJson = Value != FSvRawJson::None && R.Type(Value) != EJson::Null ? R.Raw(Value) : FString();
		}
		return true;
	}

	bool ReadText(const FString& Text, FSvStoreCatalog& Out)
	{
		if (!Read(SvJson::ParseObject(Text), Out)) return false;
		FSvRawJson R;
		if (!R.Parse(Text)) return false;
		TArray<FString> Customs;
		RawCustoms(R, TEXT("items"), Customs);
		for (int32 i = 0; i < Out.Items.Num() && i < Customs.Num(); ++i) Out.Items[i].CustomJson = Customs[i];
		return true;
	}

	bool ReadText(const FString& Text, FSvStoreHoldings& Out)
	{
		if (!Read(SvJson::ParseObject(Text), Out)) return false;
		FSvRawJson R;
		if (!R.Parse(Text)) return false;
		TArray<FString> Customs;
		RawCustoms(R, TEXT("items"), Customs);
		for (int32 i = 0; i < Out.Items.Num() && i < Customs.Num(); ++i) Out.Items[i].CustomJson = Customs[i];
		return true;
	}

	bool ReadText(const FString& Text, FSvStoreTxnResult& Out)
	{
		if (!Read(SvJson::ParseObject(Text), Out)) return false;
		FSvRawJson R;
		if (!R.Parse(Text)) return false;
		TArray<FString> Customs;
		RawCustoms(R, TEXT("items"), Customs);
		for (int32 i = 0; i < Out.Items.Num() && i < Customs.Num(); ++i) Out.Items[i].CustomJson = Customs[i];
		return true;
	}

	bool Read(const TSharedPtr<FJsonObject>& J, FSvDataWriteResult& Out)
	{
		if (!Convert(J, Out)) return false;
		const TArray<TSharedPtr<FJsonValue>> A = ArrayField(J, TEXT("results"));
		for (int32 i = 0; i < Out.Results.Num(); ++i)
		{
			const TSharedPtr<FJsonObject> R = ObjectAt(A, i);
			int64 V = 0;
			Out.Results[i].HasVersion = SvJson::GetInt64(R, TEXT("version"), V);
			Out.Results[i].Version = Out.Results[i].HasVersion ? V : 0;
			Out.Results[i].ValueJson = SvJson::GetRawJson(R, TEXT("value"));
		}
		return true;
	}

	bool Read(const TSharedPtr<FJsonObject>& J, FSvVersionInfo& Out)
	{
		if (!J.IsValid()) return false;
		Out = FSvVersionInfo();
		Out.Store = SvJson::GetString(J, TEXT("store"));
		Out.Key = SvJson::GetString(J, TEXT("key"));
		Out.HasVersion = SvJson::GetInt64(J, TEXT("version"), Out.Version);
		return true;
	}

	bool Read(const TSharedPtr<FJsonObject>& J, FSvStoreCatalog& Out)
	{
		if (!Convert(J, Out)) return false;
		const TArray<TSharedPtr<FJsonValue>> Currencies = ArrayField(J, TEXT("currencies"));
		for (int32 i = 0; i < Out.Currencies.Num(); ++i)
		{
			int64 Cap = 0;
			Out.Currencies[i].HasCap = SvJson::GetInt64(ObjectAt(Currencies, i), TEXT("cap"), Cap);
			Out.Currencies[i].Cap = Out.Currencies[i].HasCap ? Cap : 0;
		}
		const TArray<TSharedPtr<FJsonValue>> Items = ArrayField(J, TEXT("items"));
		for (int32 i = 0; i < Out.Items.Num(); ++i)
		{
			const TSharedPtr<FJsonObject> I = ObjectAt(Items, i);
			int64 Max = 0;
			Out.Items[i].HasMaxOwned = SvJson::GetInt64(I, TEXT("maxOwned"), Max);
			Out.Items[i].MaxOwned = Out.Items[i].HasMaxOwned ? Max : 0;
			Out.Items[i].CustomJson = SvJson::GetRawJson(I, TEXT("custom"));
		}
		return true;
	}

	bool Read(const TSharedPtr<FJsonObject>& J, FSvStoreHoldings& Out)
	{
		if (!Convert(J, Out)) return false;
		Out.PlayerId = SvJson::GetString(SvJson::GetObj(J, TEXT("player")), TEXT("id"));
		ReadItemsFixups(ArrayField(J, TEXT("items")), Out.Items);
		return true;
	}

	bool Read(const TSharedPtr<FJsonObject>& J, FSvStoreTxnResult& Out)
	{
		if (!Convert(J, Out)) return false;
		ReadItemsFixups(ArrayField(J, TEXT("items")), Out.Items);
		Out.IdempotencyKey.Reset();
		return true;
	}

	bool Read(const TSharedPtr<FJsonObject>& J, FSvStoreHistoryPage& Out) { return Convert(J, Out); }

	bool Read(const TSharedPtr<FJsonObject>& J, FSvMetaSyncResult& Out) { return Convert(J, Out); }

	bool Read(const TSharedPtr<FJsonObject>& J, FSvPendingStoreChange& Out)
	{
		if (!J.IsValid()) return false;
		Out = FSvPendingStoreChange();
		Out.IdempotencyKey = SvJson::GetString(J, TEXT("key"));
		Out.Kind = SvJson::GetString(J, TEXT("kind"));
		Out.Path = SvJson::GetString(J, TEXT("path"));
		Out.Body = SvJson::GetString(J, TEXT("body"));
		Out.PlayerId = SvJson::GetString(J, TEXT("playerId"));
		Out.CreatedAt = SvJson::GetString(J, TEXT("createdAt"));
		return !Out.IdempotencyKey.IsEmpty() && !Out.Path.IsEmpty();
	}

	TSharedRef<FJsonObject> Write(const FSvPlayerProfile& V)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("id"), V.Player.Id);
		SetNullableString(P, TEXT("displayName"), V.Player.DisplayName);
		P->SetStringField(TEXT("createdAt"), V.Player.CreatedAt);
		SetNullableString(P, TEXT("lastSignedInAt"), V.Player.LastSignedInAt);
		O->SetObjectField(TEXT("player"), P);
		TSharedRef<FJsonObject> G = MakeShared<FJsonObject>();
		SetNullableString(G, TEXT("gameKey"), V.Game.GameKey);
		O->SetObjectField(TEXT("game"), G);
		O->SetArrayField(TEXT("identities"), WriteArray(V.Identities, WriteIdentity));
		return O;
	}

	FString WriteDataStoreText(const FSvDataStore& V)
	{
		FString Out = TEXT("{");
		for (int32 i = 0; i < V.Entries.Num(); ++i)
		{
			const FSvDataEntry& E = V.Entries[i];
			if (i > 0) Out += TEXT(",");
			AppendEscaped(Out, E.Key);
			Out += TEXT(":{");
			if (!E.ValueJson.IsEmpty()) Out += TEXT("\"value\":") + E.ValueJson + TEXT(",");
			Out += FString::Printf(TEXT("\"version\":%lld,\"updatedAt\":"), (long long)E.Version);
			if (E.UpdatedAt.IsEmpty()) Out += TEXT("null");
			else AppendEscaped(Out, E.UpdatedAt);
			Out += TEXT("}");
		}
		return Out + TEXT("}");
	}

	FString WritePlayerDataText(const FSvPlayerData& V)
	{
		return TEXT("{\"data\":{\"player\":") + WriteDataStoreText(V.Player) + TEXT(",\"readonly\":") + WriteDataStoreText(V.Readonly) + TEXT("}}");
	}

	TSharedRef<FJsonObject> Write(const FSvDataWriteResult& V)
	{
		TArray<TSharedPtr<FJsonValue>> A;
		for (const FSvDataOpResult& R : V.Results)
		{
			TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
			E->SetStringField(TEXT("store"), R.Store);
			E->SetStringField(TEXT("key"), R.Key);
			E->SetField(TEXT("version"), R.HasVersion ? SvJson::Int(R.Version) : TSharedRef<FJsonValue>(MakeShared<FJsonValueNull>()));
			if (!R.ValueJson.IsEmpty()) SetRaw(E, TEXT("value"), R.ValueJson);
			A.Add(MakeShared<FJsonValueObject>(E));
		}
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetArrayField(TEXT("results"), A);
		return O;
	}

	TSharedRef<FJsonObject> Write(const FSvStoreCatalog& V)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		TSharedRef<FJsonObject> S = MakeShared<FJsonObject>();
		S->SetBoolField(TEXT("playerPurchases"), V.Settings.PlayerPurchases);
		S->SetBoolField(TEXT("playerConsume"), V.Settings.PlayerConsume);
		O->SetObjectField(TEXT("settings"), S);
		O->SetArrayField(TEXT("currencies"), WriteArray(V.Currencies, [](const FSvCurrency& C)
		{
			TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
			E->SetStringField(TEXT("code"), C.Code);
			E->SetStringField(TEXT("name"), C.Name);
			E->SetField(TEXT("cap"), C.HasCap ? SvJson::Int(C.Cap) : TSharedRef<FJsonValue>(MakeShared<FJsonValueNull>()));
			return E;
		}));
		O->SetArrayField(TEXT("items"), WriteArray(V.Items, [](const FSvCatalogItem& I)
		{
			TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
			E->SetStringField(TEXT("sku"), I.Sku);
			E->SetStringField(TEXT("name"), I.Name);
			E->SetStringField(TEXT("kind"), I.Kind);
			E->SetBoolField(TEXT("stackable"), I.Stackable);
			E->SetField(TEXT("maxOwned"), I.HasMaxOwned ? SvJson::Int(I.MaxOwned) : TSharedRef<FJsonValue>(MakeShared<FJsonValueNull>()));
			E->SetArrayField(TEXT("prices"), WriteArray(I.Prices, [](const FSvPrice& P)
			{
				TSharedRef<FJsonObject> PO = MakeShared<FJsonObject>();
				PO->SetStringField(TEXT("currency"), P.Currency);
				PO->SetField(TEXT("amount"), SvJson::Int(P.Amount));
				return PO;
			}));
			E->SetArrayField(TEXT("contents"), WriteArray(I.Contents, [](const FSvBundleLine& L)
			{
				TSharedRef<FJsonObject> LO = MakeShared<FJsonObject>();
				if (!L.Sku.IsEmpty()) LO->SetStringField(TEXT("sku"), L.Sku);
				if (!L.Currency.IsEmpty()) LO->SetStringField(TEXT("currency"), L.Currency);
				LO->SetField(TEXT("quantity"), SvJson::Int(L.Quantity));
				return LO;
			}));
			TArray<TSharedPtr<FJsonValue>> Tags;
			for (const FString& T : I.Tags) Tags.Add(MakeShared<FJsonValueString>(T));
			E->SetArrayField(TEXT("tags"), Tags);
			SetRaw(E, TEXT("custom"), I.CustomJson);
			E->SetBoolField(TEXT("purchasable"), I.Purchasable);
			E->SetBoolField(TEXT("consumable"), I.Consumable);
			return E;
		}));
		return O;
	}

	TSharedRef<FJsonObject> Write(const FSvStoreHoldings& V)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("id"), V.PlayerId);
		O->SetObjectField(TEXT("player"), P);
		O->SetArrayField(TEXT("balances"), WriteArray(V.Balances, WriteBalance));
		O->SetArrayField(TEXT("items"), WriteItems(V.Items));
		return O;
	}

	TSharedRef<FJsonObject> Write(const FSvStoreTxnResult& V)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		TSharedRef<FJsonObject> T = MakeShared<FJsonObject>();
		T->SetStringField(TEXT("id"), V.Txn.Id);
		T->SetStringField(TEXT("kind"), V.Txn.Kind);
		T->SetStringField(TEXT("createdAt"), V.Txn.CreatedAt);
		SetNullableString(T, TEXT("reason"), V.Txn.Reason);
		SetNullableString(T, TEXT("reverses"), V.Txn.Reverses);
		O->SetObjectField(TEXT("txn"), T);
		O->SetArrayField(TEXT("changes"), WriteArray(V.Changes, WriteChange));
		O->SetArrayField(TEXT("balances"), WriteArray(V.Balances, WriteBalance));
		O->SetArrayField(TEXT("items"), WriteItems(V.Items));
		O->SetArrayField(TEXT("shortfall"), WriteArray(V.Shortfall, WriteShortfall));
		O->SetBoolField(TEXT("replayed"), V.Replayed);
		return O;
	}

	TSharedRef<FJsonObject> Write(const FSvStoreHistoryPage& V)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetArrayField(TEXT("txns"), WriteArray(V.Txns, [](const FSvHistoryEntry& H)
		{
			TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
			E->SetStringField(TEXT("id"), H.Id);
			E->SetStringField(TEXT("kind"), H.Kind);
			E->SetStringField(TEXT("createdAt"), H.CreatedAt);
			SetNullableString(E, TEXT("reverses"), H.Reverses);
			E->SetArrayField(TEXT("changes"), WriteArray(H.Changes, WriteChange));
			E->SetArrayField(TEXT("shortfall"), WriteArray(H.Shortfall, WriteShortfall));
			return E;
		}));
		SetNullableString(O, TEXT("next"), V.Next);
		return O;
	}

	TSharedRef<FJsonObject> Write(const FSvMetaSyncResult& V) { return ToObject(V); }

	TSharedRef<FJsonObject> Write(const FSvPendingStoreChange& V)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("key"), V.IdempotencyKey);
		O->SetStringField(TEXT("kind"), V.Kind);
		O->SetStringField(TEXT("path"), V.Path);
		O->SetStringField(TEXT("body"), V.Body);
		O->SetStringField(TEXT("playerId"), V.PlayerId);
		O->SetStringField(TEXT("createdAt"), V.CreatedAt);
		return O;
	}
}
