// Copyright (c) SplatterVault. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"
#include "JsonObjectConverter.h"
#include "SvTypes.h"

/**
 * The optional parts of an Inc op: set only what you need, for example
 * FSvIncOptions().WithMax(9999).WithIfVersion(Entry.Version), or in a C++20 project
 * FSvIncOptions{.Max = 9999, .IfVersion = Entry.Version}.
 */
struct FSvIncOptions
{
	/** A whole-number field of an object value instead of the whole value (empty: the whole value). */
	FString Field;

	/** The result must stay at or above Min (out_of_range otherwise). */
	TOptional<int64> Min;

	/** The result must stay at or below Max (out_of_range otherwise). */
	TOptional<int64> Max;

	/** Change only while the key is still at this version (0: must not exist yet; -1: any version). */
	int64 IfVersion = -1;

	FSvIncOptions& WithField(const FString& InField) { Field = InField; return *this; }
	FSvIncOptions& WithMin(int64 InMin) { Min = InMin; return *this; }
	FSvIncOptions& WithMax(int64 InMax) { Max = InMax; return *this; }
	FSvIncOptions& WithIfVersion(int64 InIfVersion) { IfVersion = InIfVersion; return *this; }
};

/**
 * Ops for one player-data write (FSvPlayerDataApi::Write), applied all or nothing, in order. Players write only their
 * player store. At most 25 ops.
 *
 * @code
 * FSvDataOps Ops;
 * Ops.SetNumber(TEXT("fov"), 90, Data.Player.VersionOf(TEXT("fov")))
 *    .Delete(TEXT("tutorial"))
 *    .Inc(TEXT("matches"), 1);
 * Client->Data().Write(Ops);
 * @endcode
 *
 * IfVersion: the key must still be at that version (0: must not exist yet), else nothing is written and the answer
 * is version_conflict with the current versions. The API has no Idempotency-Key for data writes, so the SDK repeats a
 * failed write by itself only when EVERY op has IfVersion (a repeat of a write that did land then answers
 * version_conflict instead of writing twice: read again to see it).
 *
 * Builder calls with an invalid key or value are recorded and make Write fail with invalid_argument (nothing sent).
 */
class SPLATTERVAULT_API FSvDataOps
{
public:
	/** The most ops one player write may hold. */
	static constexpr int32 MaxOps = 25;

	FSvDataOps() = default;

	/** From Blueprint-made ops. */
	explicit FSvDataOps(const TArray<FSvDataOp>& InOps);

	/** Stores Value (any JSON but null: delete the key instead) under Key. IfVersion < 0: no condition. */
	FSvDataOps& Set(const FString& Key, const TSharedPtr<FJsonValue>& Value, int64 IfVersion = -1);
	/** Stores JSON text (for example {"fov":90} or 42), sent exactly as given. */
	FSvDataOps& SetJson(const FString& Key, const FString& ValueJson, int64 IfVersion = -1);
	FSvDataOps& SetString(const FString& Key, const FString& Value, int64 IfVersion = -1);
	FSvDataOps& SetNumber(const FString& Key, double Value, int64 IfVersion = -1);
	FSvDataOps& SetBool(const FString& Key, bool Value, int64 IfVersion = -1);

	/** Stores a USTRUCT as a JSON object (FJsonObjectConverter). */
	template <typename TStruct>
	FSvDataOps& SetStruct(const FString& Key, const TStruct& Value, int64 IfVersion = -1)
	{
		TSharedPtr<FJsonObject> Obj = FJsonObjectConverter::UStructToJsonObject(Value);
		return Set(Key, Obj.IsValid() ? MakeShared<FJsonValueObject>(Obj) : TSharedPtr<FJsonValue>(), IfVersion);
	}

	/** Deletes Key (its version still goes up). */
	FSvDataOps& Delete(const FString& Key, int64 IfVersion = -1);

	/**
	 * Adds By (negative to subtract) to a whole-number value (a missing one counts as 0), or to its whole-number
	 * Field. With Min / Max the result must stay within them (out_of_range otherwise). The answer holds the new value.
	 */
	FSvDataOps& Inc(const FString& Key, int64 By, const FString& Field = FString(), TOptional<int64> Min = TOptional<int64>(),
		TOptional<int64> Max = TOptional<int64>(), int64 IfVersion = -1);

	/** Inc with only the options you need: Ops.Inc(TEXT("coins"), 5, FSvIncOptions().WithMax(9999).WithIfVersion(Entry.Version)). */
	FSvDataOps& Inc(const FString& Key, int64 By, const FSvIncOptions& Options);

	/** Adds a Blueprint-made op. */
	FSvDataOps& Add(const FSvDataOp& Op);

	int32 Num() const { return Ops.Num(); }

	/** Every op carries ifVersion (and so may be repeated safely). */
	bool AllHaveIfVersion() const;

	/** The first builder mistake, or empty. */
	const FString& GetBuildError() const { return BuildError; }

	/** The request body: { "ops": [ ... ] } as JSON text. */
	FString ToJson() const;

	/** 1 to 128 letters, digits and _ . : - (first a letter, digit or _); sv. and __proto__ are reserved. */
	static bool IsValidKey(const FString& Key);

private:
	/** One op as JSON text: values are kept verbatim (Unreal's JSON objects would merge keys that differ only in case). */
	struct FOp
	{
		FString Json;
		bool bHasIfVersion = false;
	};

	/** Adds {"op","store","key"[,Fields],"ifVersion"?}; Fields is JSON text of more members, starting with a comma. */
	FSvDataOps& Push(const TCHAR* Name, const FString& Key, int64 IfVersion, const FString& Fields);
	void Fail(const FString& Why);

	TArray<FOp> Ops;
	FString BuildError;
};
