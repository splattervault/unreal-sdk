// Copyright (c) SplatterVault. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"

/** The outcome of ISvSecureStore::Get. */
enum class ESvStoreRead : uint8
{
	/** The value was read. */
	Found,
	/** There is certainly no value. */
	NotFound,
	/** The store could not be read, or a value exists but cannot be opened. Never "missing". */
	Error
};

/**
 * Where the SDK keeps a player's long-lived secrets on the device: the refresh token of an email sign-in, a guest's
 * device secret (its only copy), the last way the player signed in, and the store changes still waiting for an
 * answer. The player token itself is never stored: it lives in memory only.
 *
 * Contract. Get returns NotFound ONLY when there is certainly no value. When the store cannot be read, or a value
 * exists but cannot be opened (another device, a changed key, a damaged file), it returns Error: the SDK then never
 * makes a new guest over the old one and reads again next time. Set must keep the value once it returned true.
 * Delete returns true when nothing is left (also when there was nothing). Error texts never contain a value.
 *
 * Calls are made on the game thread and must be quick (a small file, a Keystore call).
 */
class SPLATTERVAULT_API ISvSecureStore
{
public:
	virtual ~ISvSecureStore() = default;

	virtual ESvStoreRead Get(const FString& Key, FString& OutValue, FString& OutError) = 0;
	virtual bool Set(const FString& Key, const FString& Value, FString& OutError) = 0;
	virtual bool Delete(const FString& Key, FString& OutError) = 0;

	/** How values are protected: "dpapi", "android-keystore", "obfuscated" or "memory". */
	virtual FString GetProtection() const = 0;

	/** Values are sealed with a key the operating system keeps (DPAPI, Android Keystore), not just obfuscated. */
	virtual bool IsHardwareOrOsBacked() const = 0;
};

/**
 * A store that exists but cannot be read (its salt or key is gone or damaged): every read is Error and every write
 * fails, so the SDK answers storage_unreadable and never makes a new guest over the old one.
 */
class SPLATTERVAULT_API FSvUnreadableStore : public ISvSecureStore
{
public:
	explicit FSvUnreadableStore(const FString& InWhy) : Why(InWhy) {}
	virtual ESvStoreRead Get(const FString& Key, FString& OutValue, FString& OutError) override;
	virtual bool Set(const FString& Key, const FString& Value, FString& OutError) override;
	virtual bool Delete(const FString& Key, FString& OutError) override;
	virtual FString GetProtection() const override { return TEXT("unreadable"); }
	virtual bool IsHardwareOrOsBacked() const override { return false; }

private:
	FString Why;
};

/**
 * No storage on this device (it could not be set up): nothing was ever saved (reads answer NotFound) and nothing is
 * saved (writes fail, so a new guest reports SavedToDevice false and OnStorageFailed is raised).
 */
class SPLATTERVAULT_API FSvUnavailableStore : public ISvSecureStore
{
public:
	explicit FSvUnavailableStore(const FString& InWhy) : Why(InWhy) {}
	virtual ESvStoreRead Get(const FString& Key, FString& OutValue, FString& OutError) override;
	virtual bool Set(const FString& Key, const FString& Value, FString& OutError) override;
	virtual bool Delete(const FString& Key, FString& OutError) override;
	virtual FString GetProtection() const override { return TEXT("unavailable"); }
	virtual bool IsHardwareOrOsBacked() const override { return false; }

private:
	FString Why;
};

/** Keeps values in memory only: nothing survives a restart (tests, or a game that wants no storage). */
class SPLATTERVAULT_API FSvMemorySecureStore : public ISvSecureStore
{
public:
	virtual ESvStoreRead Get(const FString& Key, FString& OutValue, FString& OutError) override;
	virtual bool Set(const FString& Key, const FString& Value, FString& OutError) override;
	virtual bool Delete(const FString& Key, FString& OutError) override;
	virtual FString GetProtection() const override { return TEXT("memory"); }
	virtual bool IsHardwareOrOsBacked() const override { return false; }

private:
	/** Keys are compared case-sensitively (a TMap of FString would not). */
	TArray<TPair<FString, FString>> Values;
};

/** What FSvObfuscatingProtector::LoadOrCreateSalt found. */
enum class ESvSaltResult : uint8
{
	Loaded,
	Created,
	/** A salt (or values sealed with one) exists but cannot be read: the store is unreadable, never "empty". */
	Unreadable,
	/** No salt could be made (no writable directory): nothing can be saved. */
	Unavailable
};

/** Seals and opens a stored value (a platform's data protection, or FSvObfuscatingProtector). */
class SPLATTERVAULT_API ISvBlobProtector
{
public:
	virtual ~ISvBlobProtector() = default;

	/** The sealed form of Data, bound to Key. False when sealing failed. */
	virtual bool Protect(const TArray<uint8>& Data, const FString& Key, TArray<uint8>& OutBlob) = 0;

	/** The value; false when the blob does not open (tampered, another device, another key or salt). */
	virtual bool Unprotect(const TArray<uint8>& Blob, const FString& Key, TArray<uint8>& OutData) = 0;

	virtual FString GetName() const = 0;
	virtual bool IsHardwareOrOsBacked() const = 0;
};

/**
 * One file per value in a directory, sealed by an ISvBlobProtector. File names are hashes of the keys; writes go
 * to a temporary file first and then replace the old one. A value that exists but does not open reads as Error.
 */
class SPLATTERVAULT_API FSvProtectedFileStore : public ISvSecureStore
{
public:
	FSvProtectedFileStore(const FString& InDirectory, TSharedRef<ISvBlobProtector> InProtector);

	virtual ESvStoreRead Get(const FString& Key, FString& OutValue, FString& OutError) override;
	virtual bool Set(const FString& Key, const FString& Value, FString& OutError) override;
	virtual bool Delete(const FString& Key, FString& OutError) override;
	virtual FString GetProtection() const override { return Protector->GetName(); }
	virtual bool IsHardwareOrOsBacked() const override { return Protector->IsHardwareOrOsBacked(); }

	const FString& GetDirectory() const { return Directory; }

	/** The file that holds Key. */
	FString PathFor(const FString& Key) const;

private:
	FString Directory;
	TSharedRef<ISvBlobProtector> Protector;
};

/**
 * AES-256 (CTR) with HMAC-SHA256 (encrypt, then MAC); keys derived from KeyMaterial and a random salt kept next to
 * the values. The fallback where a platform has no secure storage: values are not readable as plain text, but anyone
 * who can read the app's files and knows how the key material is made can open them. Treat it as obfuscation.
 */
class SPLATTERVAULT_API FSvObfuscatingProtector : public ISvBlobProtector
{
public:
	/** Salt: at least 16 random bytes kept with the store (see LoadOrCreateSalt). */
	FSvObfuscatingProtector(const FString& KeyMaterial, const TArray<uint8>& Salt);

	virtual bool Protect(const TArray<uint8>& Data, const FString& Key, TArray<uint8>& OutBlob) override;
	virtual bool Unprotect(const TArray<uint8>& Blob, const FString& Key, TArray<uint8>& OutData) override;
	virtual FString GetName() const override { return TEXT("obfuscated"); }
	virtual bool IsHardwareOrOsBacked() const override { return false; }

	/**
	 * The salt in Path, made (32 random bytes) when there is none and nothing was sealed yet. Unreadable when a salt
	 * exists but cannot be read, or sealed values exist without it; Unavailable when none can be made.
	 */
	static ESvSaltResult LoadOrCreateSalt(const FString& Path, TArray<uint8>& OutSalt);

	/** Something of this device and app: the project name and the platform's login/device id. */
	static FString DefaultKeyMaterial();

private:
	TArray<uint8> EncKey;
	TArray<uint8> MacKey;
};

#if PLATFORM_WINDOWS
/** Windows DPAPI (CryptProtectData, current user): only this Windows account on this machine opens the values. */
class SPLATTERVAULT_API FSvDpapiProtector : public ISvBlobProtector
{
public:
	virtual bool Protect(const TArray<uint8>& Data, const FString& Key, TArray<uint8>& OutBlob) override;
	virtual bool Unprotect(const TArray<uint8>& Blob, const FString& Key, TArray<uint8>& OutData) override;
	virtual FString GetName() const override { return TEXT("dpapi"); }
	virtual bool IsHardwareOrOsBacked() const override { return true; }
};
#endif

#if PLATFORM_ANDROID
/**
 * Android Keystore: an AES-256-GCM key that never leaves the device's keystore seals each value (the Java helper
 * com.splattervault.sdk.SvKeystore, added to the APK by the plugin's UPL file).
 */
class SPLATTERVAULT_API FSvAndroidKeystoreProtector : public ISvBlobProtector
{
public:
	/** The Keystore helper is in the APK and answers. */
	static bool IsAvailable();

	/** The app's private internal files directory (Context.getFilesDir()), or empty. */
	static FString InternalFilesDir();

	virtual bool Protect(const TArray<uint8>& Data, const FString& Key, TArray<uint8>& OutBlob) override;
	virtual bool Unprotect(const TArray<uint8>& Blob, const FString& Key, TArray<uint8>& OutData) override;
	virtual FString GetName() const override { return TEXT("android-keystore"); }
	virtual bool IsHardwareOrOsBacked() const override { return true; }
};
#endif

/**
 * The platform's best store for Name: DPAPI-sealed files on Windows, Android Keystore-sealed files in the app's
 * internal storage on Android (the helper the plugin adds to the APK is required: without it nothing is saved), else
 * obfuscated files (FSvObfuscatingProtector). OutWarning is set when the store is only obfuscated, unreadable or
 * unavailable.
 */
SPLATTERVAULT_API TSharedRef<ISvSecureStore> SvCreatePlatformSecureStore(const FString& Name, FString* OutWarning = nullptr);
