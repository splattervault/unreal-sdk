// Copyright (c) SplatterVault. Licensed under the MIT License.

#include "SvDataOps.h"

#include "SvJson.h"

namespace
{
	FString Quote(const FString& S)
	{
		return SvJson::WriteValue(MakeShared<FJsonValueString>(S));
	}
}

FSvDataOps::FSvDataOps(const TArray<FSvDataOp>& InOps)
{
	for (const FSvDataOp& Op : InOps) Add(Op);
}

bool FSvDataOps::IsValidKey(const FString& Key)
{
	if (Key.IsEmpty() || Key.Len() > 128) return false;
	if (Key.Equals(TEXT("__proto__"), ESearchCase::CaseSensitive) || Key.StartsWith(TEXT("sv."), ESearchCase::IgnoreCase)) return false;
	for (int32 i = 0; i < Key.Len(); ++i)
	{
		const TCHAR C = Key[i];
		const bool bOk = (C >= TEXT('a') && C <= TEXT('z')) || (C >= TEXT('A') && C <= TEXT('Z')) || (C >= TEXT('0') && C <= TEXT('9')) ||
			C == TEXT('_') || (i > 0 && (C == TEXT('.') || C == TEXT(':') || C == TEXT('-')));
		if (!bOk) return false;
	}
	return true;
}

void FSvDataOps::Fail(const FString& Why)
{
	if (BuildError.IsEmpty()) BuildError = Why;
}

FSvDataOps& FSvDataOps::Push(const TCHAR* Name, const FString& Key, int64 IfVersion, const FString& Fields)
{
	if (!IsValidKey(Key))
	{
		Fail(TEXT("Keys are 1 to 128 letters, digits, _ . : - (not starting with . : -); sv. and __proto__ are reserved."));
	}
	if (Ops.Num() >= MaxOps)
	{
		Fail(FString::Printf(TEXT("A player write holds at most %d ops."), MaxOps));
		return *this;
	}
	FOp Op;
	Op.Json = FString(TEXT("{\"op\":\"")) + Name + TEXT("\",\"store\":\"player\",\"key\":") + Quote(Key) + Fields;
	if (IfVersion >= 0)
	{
		Op.Json += FString::Printf(TEXT(",\"ifVersion\":%lld"), (long long)IfVersion);
		Op.bHasIfVersion = true;
	}
	Op.Json += TEXT("}");
	Ops.Add(MoveTemp(Op));
	return *this;
}

FSvDataOps& FSvDataOps::Set(const FString& Key, const TSharedPtr<FJsonValue>& Value, int64 IfVersion)
{
	if (!Value.IsValid() || Value->IsNull())
	{
		Fail(TEXT("null is not a value: delete the key instead."));
		return *this;
	}
	return Push(TEXT("set"), Key, IfVersion, TEXT(",\"value\":") + SvJson::WriteValue(Value));
}

FSvDataOps& FSvDataOps::SetJson(const FString& Key, const FString& ValueJson, int64 IfVersion)
{
	FSvRawJson Raw;
	if (!Raw.Parse(ValueJson))
	{
		Fail(TEXT("The value is not JSON text (a string must be quoted: \"text\")."));
		return *this;
	}
	if (Raw.Type(Raw.Root()) == EJson::Null)
	{
		Fail(TEXT("null is not a value: delete the key instead."));
		return *this;
	}
	return Push(TEXT("set"), Key, IfVersion, TEXT(",\"value\":") + ValueJson.TrimStartAndEnd());
}

FSvDataOps& FSvDataOps::SetString(const FString& Key, const FString& Value, int64 IfVersion)
{
	return Push(TEXT("set"), Key, IfVersion, TEXT(",\"value\":") + Quote(Value));
}

FSvDataOps& FSvDataOps::SetNumber(const FString& Key, double Value, int64 IfVersion)
{
	if (!FMath::IsFinite(Value))
	{
		Fail(TEXT("A number value must be finite."));
		return *this;
	}
	return Set(Key, MakeShared<FJsonValueNumber>(Value), IfVersion);
}

FSvDataOps& FSvDataOps::SetBool(const FString& Key, bool Value, int64 IfVersion)
{
	return Push(TEXT("set"), Key, IfVersion, Value ? TEXT(",\"value\":true") : TEXT(",\"value\":false"));
}

FSvDataOps& FSvDataOps::Delete(const FString& Key, int64 IfVersion)
{
	return Push(TEXT("delete"), Key, IfVersion, FString());
}

FSvDataOps& FSvDataOps::Inc(const FString& Key, int64 By, const FString& Field, TOptional<int64> Min, TOptional<int64> Max, int64 IfVersion)
{
	if (Min.IsSet() && Max.IsSet() && Min.GetValue() > Max.GetValue())
	{
		Fail(TEXT("min must not be above max."));
		return *this;
	}
	FString Fields = FString::Printf(TEXT(",\"by\":%lld"), (long long)By);
	if (!Field.IsEmpty())
	{
		if (!IsValidKey(Field)) Fail(TEXT("field follows the rules of keys."));
		Fields += TEXT(",\"field\":") + Quote(Field);
	}
	if (Min.IsSet()) Fields += FString::Printf(TEXT(",\"min\":%lld"), (long long)Min.GetValue());
	if (Max.IsSet()) Fields += FString::Printf(TEXT(",\"max\":%lld"), (long long)Max.GetValue());
	return Push(TEXT("inc"), Key, IfVersion, Fields);
}

FSvDataOps& FSvDataOps::Inc(const FString& Key, int64 By, const FSvIncOptions& Options)
{
	return Inc(Key, By, Options.Field, Options.Min, Options.Max, Options.IfVersion);
}

FSvDataOps& FSvDataOps::Add(const FSvDataOp& Op)
{
	const int64 IfVersion = Op.HasIfVersion ? FMath::Max<int64>(0, Op.IfVersion) : -1;
	if (Op.HasIfVersion && Op.IfVersion < 0) Fail(TEXT("ifVersion is 0 or more."));
	switch (Op.Op)
	{
	case ESvDataOpType::Set:
		return SetJson(Op.Key, Op.ValueJson, IfVersion);
	case ESvDataOpType::Delete:
		return Delete(Op.Key, IfVersion);
	case ESvDataOpType::Inc:
	default:
		return Inc(Op.Key, Op.By, Op.Field, Op.HasMin ? TOptional<int64>(Op.Min) : TOptional<int64>(), Op.HasMax ? TOptional<int64>(Op.Max) : TOptional<int64>(),
			IfVersion);
	}
}

bool FSvDataOps::AllHaveIfVersion() const
{
	if (Ops.Num() == 0) return false;
	for (const FOp& Op : Ops)
	{
		if (!Op.bHasIfVersion) return false;
	}
	return true;
}

FString FSvDataOps::ToJson() const
{
	FString Out = TEXT("{\"ops\":[");
	for (int32 i = 0; i < Ops.Num(); ++i)
	{
		if (i > 0) Out += TEXT(",");
		Out += Ops[i].Json;
	}
	return Out + TEXT("]}");
}
