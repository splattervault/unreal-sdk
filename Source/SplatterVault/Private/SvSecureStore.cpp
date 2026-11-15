// Copyright (c) SplatterVault. Licensed under the MIT License.

#include "SvSecureStore.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "SplatterVaultModule.h"
#include "SvCrypto.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#include <wincrypt.h>
#include <dpapi.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

#if PLATFORM_ANDROID
#include "Android/AndroidJavaEnv.h"
#include <jni.h>
#endif

#if PLATFORM_UNIX || PLATFORM_MAC
#include <sys/stat.h>
#endif

#if PLATFORM_ANDROID || PLATFORM_UNIX || PLATFORM_APPLE
#include <stdio.h>
#endif

namespace
{
	/** Owner-only permissions where the platform has them (the Android internal directory is private already). */
	void RestrictPath(const FString& Path, bool bDirectory)
	{
#if PLATFORM_UNIX || PLATFORM_MAC
		const FTCHARToUTF8 P(*FPaths::ConvertRelativePathToFull(Path));
		chmod(P.Get(), bDirectory ? 0700 : 0600);
#else
		(void)Path;
		(void)bDirectory;
#endif
	}

	bool EnsureDirectory(const FString& Directory)
	{
		IFileManager& FM = IFileManager::Get();
		if (!FM.DirectoryExists(*Directory))
		{
			if (!FM.MakeDirectory(*Directory, true)) return false;
		}
		RestrictPath(Directory, true);
		return true;
	}

	/** A new temporary file for one write of Path: unique per write, so two writers never share (and tear) one. */
	FString NewTmpOf(const FString& Path)
	{
		return Path + TEXT(".") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".tmp");
	}

	/** Every leftover temporary file of Path: "<file>.<id>.tmp", and "<file>.tmp" of earlier versions. */
	TArray<FString> TmpsOf(const FString& Path)
	{
		TArray<FString> Names;
		IFileManager::Get().FindFiles(Names, *(Path + TEXT("*.tmp")), true, false);
		const FString Dir = FPaths::GetPath(Path);
		const FString Base = FPaths::GetCleanFilename(Path);
		TArray<FString> Out;
		for (const FString& Name : Names)
		{
			if (Name.StartsWith(Base, ESearchCase::CaseSensitive)) Out.Add(FPaths::Combine(Dir, Name));
		}
		return Out;
	}

	/** A temporary file that does not open, younger than this, may be another process's write in progress: left alone. */
	const FTimespan TmpGrace = FTimespan::FromSeconds(30);

	/**
	 * Puts Tmp in place of Path in one step (the old file stays until the new one replaces it): MoveFileEx with
	 * REPLACE_EXISTING and WRITE_THROUGH on Windows, rename() elsewhere. Never delete-then-move.
	 */
	bool AtomicReplace(const FString& Tmp, const FString& Path)
	{
		const FString FullTmp = FPaths::ConvertRelativePathToFull(Tmp);
		const FString FullPath = FPaths::ConvertRelativePathToFull(Path);
#if PLATFORM_WINDOWS
		return MoveFileExW(*FullTmp, *FullPath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#elif PLATFORM_ANDROID || PLATFORM_UNIX || PLATFORM_APPLE
		const FTCHARToUTF8 From(*FullTmp);
		const FTCHARToUTF8 To(*FullPath);
		return rename(From.Get(), To.Get()) == 0;
#else
		return IFileManager::Get().Move(*FullPath, *FullTmp, true, true, false, true);
#endif
	}

	/** Writes a temporary file, then puts it in place of Path atomically. */
	bool WriteAtomic(const FString& Path, const TArray<uint8>& Bytes)
	{
		const FString Tmp = NewTmpOf(Path);
		if (!FFileHelper::SaveArrayToFile(Bytes, *Tmp))
		{
			IFileManager::Get().Delete(*Tmp, false, true, true);
			return false;
		}
		RestrictPath(Tmp, false);
		if (!AtomicReplace(Tmp, Path))
		{
			// The old value (if any) is untouched; the temporary file holds the new one and is picked up by the next read.
			return false;
		}
		RestrictPath(Path, false);
		return true;
	}

	TArray<uint8> Derive(const TArray<uint8>& Ikm, const TArray<uint8>& Salt, const char* Label)
	{
		const TArray<uint8> Prk = SvCrypto::HmacSha256(Salt, Ikm);
		TArray<uint8> Info;
		Info.Append(reinterpret_cast<const uint8*>(Label), FCStringAnsi::Strlen(Label));
		Info.Add(1);
		return SvCrypto::HmacSha256(Prk, Info);
	}
}

// ─── unreadable / unavailable ─────────────────────────────────────────────────────────────────────────────────────

ESvStoreRead FSvUnreadableStore::Get(const FString& Key, FString& OutValue, FString& OutError)
{
	OutError = Why;
	return ESvStoreRead::Error;
}

bool FSvUnreadableStore::Set(const FString& Key, const FString& Value, FString& OutError)
{
	OutError = Why;
	return false;
}

bool FSvUnreadableStore::Delete(const FString& Key, FString& OutError)
{
	OutError = Why;
	return false;
}

ESvStoreRead FSvUnavailableStore::Get(const FString& Key, FString& OutValue, FString& OutError)
{
	return ESvStoreRead::NotFound;
}

bool FSvUnavailableStore::Set(const FString& Key, const FString& Value, FString& OutError)
{
	OutError = Why;
	return false;
}

bool FSvUnavailableStore::Delete(const FString& Key, FString& OutError)
{
	return true;
}

// ─── memory ───────────────────────────────────────────────────────────────────────────────────────────────────────

ESvStoreRead FSvMemorySecureStore::Get(const FString& Key, FString& OutValue, FString& OutError)
{
	for (const TPair<FString, FString>& Pair : Values)
	{
		if (Pair.Key.Equals(Key, ESearchCase::CaseSensitive))
		{
			OutValue = Pair.Value;
			return ESvStoreRead::Found;
		}
	}
	return ESvStoreRead::NotFound;
}

bool FSvMemorySecureStore::Set(const FString& Key, const FString& Value, FString& OutError)
{
	for (TPair<FString, FString>& Pair : Values)
	{
		if (Pair.Key.Equals(Key, ESearchCase::CaseSensitive))
		{
			Pair.Value = Value;
			return true;
		}
	}
	Values.Add(TPair<FString, FString>(Key, Value));
	return true;
}

bool FSvMemorySecureStore::Delete(const FString& Key, FString& OutError)
{
	Values.RemoveAll([&Key](const TPair<FString, FString>& Pair) { return Pair.Key.Equals(Key, ESearchCase::CaseSensitive); });
	return true;
}

// ─── protected files ──────────────────────────────────────────────────────────────────────────────────────────────

FSvProtectedFileStore::FSvProtectedFileStore(const FString& InDirectory, TSharedRef<ISvBlobProtector> InProtector)
	: Directory(InDirectory)
	, Protector(InProtector)
{
}

FString FSvProtectedFileStore::PathFor(const FString& Key) const
{
	const FString Hash = SvCrypto::Hex(SvCrypto::Sha256(SvCrypto::Utf8(Key))).Left(32);
	return FPaths::Combine(Directory, Hash + TEXT(".svs"));
}

ESvStoreRead FSvProtectedFileStore::Get(const FString& Key, FString& OutValue, FString& OutError)
{
	const FString Path = PathFor(Key);
	IFileManager& FM = IFileManager::Get();
	TArray<FString> Leftovers = TmpsOf(Path);
	if (Leftovers.Num() > 0)
	{
		// Writes that stopped before their temporary file replaced the value. The newest one that opens and is not older
		// than the value is a complete, newer value: finish its replace and use it. Older ones (a write a later one
		// overtook) and broken ones are deleted, except a young broken one: another process may still be writing it.
		const FDateTime ValueTime = FM.FileExists(*Path) ? FM.GetTimeStamp(*Path) : FDateTime::MinValue();
		const FDateTime Now = FDateTime::UtcNow();
		TArray<TPair<FDateTime, FString>> ByAge;
		for (const FString& Tmp : Leftovers) ByAge.Add(TPair<FDateTime, FString>(FM.GetTimeStamp(*Tmp), Tmp));
		ByAge.Sort([](const TPair<FDateTime, FString>& A, const TPair<FDateTime, FString>& B) { return A.Key > B.Key; });
		bool bRecovered = false;
		for (const TPair<FDateTime, FString>& Tmp : ByAge)
		{
			if (!bRecovered && Tmp.Key >= ValueTime)
			{
				TArray<uint8> Blob;
				TArray<uint8> Data;
				if (FFileHelper::LoadFileToArray(Blob, *Tmp.Value, FILEREAD_Silent) && Protector->Unprotect(Blob, Key, Data))
				{
					AtomicReplace(Tmp.Value, Path);
					OutValue = SvCrypto::FromUtf8(Data);
					FMemory::Memzero(Data.GetData(), Data.Num());
					bRecovered = true;
					continue;
				}
			}
			if (Now - Tmp.Key >= TmpGrace) FM.Delete(*Tmp.Value, false, true, true);
		}
		if (bRecovered) return ESvStoreRead::Found;
	}
	if (!FM.FileExists(*Path)) return ESvStoreRead::NotFound;
	TArray<uint8> Blob;
	if (!FFileHelper::LoadFileToArray(Blob, *Path, FILEREAD_Silent))
	{
		OutError = TEXT("A stored value could not be read (file ") + FPaths::GetCleanFilename(Path) + TEXT(").");
		return ESvStoreRead::Error;
	}
	TArray<uint8> Data;
	if (!Protector->Unprotect(Blob, Key, Data))
	{
		OutError = TEXT("A stored value exists but could not be opened (another device or account, a changed key or salt, or a damaged file: ") +
			FPaths::GetCleanFilename(Path) + TEXT(").");
		return ESvStoreRead::Error;
	}
	OutValue = SvCrypto::FromUtf8(Data);
	FMemory::Memzero(Data.GetData(), Data.Num());
	return ESvStoreRead::Found;
}

bool FSvProtectedFileStore::Set(const FString& Key, const FString& Value, FString& OutError)
{
	if (!EnsureDirectory(Directory))
	{
		OutError = TEXT("The secure store's directory could not be made.");
		return false;
	}
	TArray<uint8> Data = SvCrypto::Utf8(Value);
	TArray<uint8> Blob;
	const bool bSealed = Protector->Protect(Data, Key, Blob);
	FMemory::Memzero(Data.GetData(), Data.Num());
	if (!bSealed)
	{
		OutError = TEXT("The value could not be sealed (") + Protector->GetName() + TEXT(").");
		return false;
	}
	if (!WriteAtomic(PathFor(Key), Blob))
	{
		OutError = TEXT("The sealed value could not be written.");
		return false;
	}
	return true;
}

bool FSvProtectedFileStore::Delete(const FString& Key, FString& OutError)
{
	const FString Path = PathFor(Key);
	IFileManager& FM = IFileManager::Get();
	// Leftover temporary files hold a secret too (sign-out, erase): they go first, or a later read would bring one back.
	for (const FString& Tmp : TmpsOf(Path)) FM.Delete(*Tmp, false, true, true);
	if (TmpsOf(Path).Num() > 0)
	{
		OutError = TEXT("A stored value could not be deleted.");
		return false;
	}
	if (!FM.FileExists(*Path)) return true;
	FM.Delete(*Path, false, true, true);
	if (FM.FileExists(*Path))
	{
		OutError = TEXT("A stored value could not be deleted.");
		return false;
	}
	return true;
}

// ─── obfuscated (AES-256-CTR + HMAC-SHA256) ───────────────────────────────────────────────────────────────────────

namespace
{
	constexpr uint8 ObfVersion = 1;
	constexpr int32 ObfIv = 16;
	constexpr int32 ObfMac = 32;
}

FSvObfuscatingProtector::FSvObfuscatingProtector(const FString& KeyMaterial, const TArray<uint8>& Salt)
{
	check(Salt.Num() >= 16);
	const TArray<uint8> Ikm = SvCrypto::Utf8(KeyMaterial);
	EncKey = Derive(Ikm, Salt, "splattervault-store-enc");
	MacKey = Derive(Ikm, Salt, "splattervault-store-mac");
}

bool FSvObfuscatingProtector::Protect(const TArray<uint8>& Data, const FString& Key, TArray<uint8>& OutBlob)
{
	TArray<uint8> Iv;
	if (!SvCrypto::RandomBytes(ObfIv, Iv)) return false;
	OutBlob.SetNumZeroed(1 + ObfIv + Data.Num() + ObfMac);
	OutBlob[0] = ObfVersion;
	FMemory::Memcpy(OutBlob.GetData() + 1, Iv.GetData(), ObfIv);
	SvCrypto::Aes256Ctr(EncKey, Iv.GetData(), Data.GetData(), OutBlob.GetData() + 1 + ObfIv, Data.Num());
	const int32 Signed = 1 + ObfIv + Data.Num();
	TArray<uint8> MacInput;
	MacInput.Append(OutBlob.GetData(), Signed);
	MacInput.Append(SvCrypto::Utf8(Key));
	const TArray<uint8> Mac = SvCrypto::HmacSha256(MacKey, MacInput);
	FMemory::Memcpy(OutBlob.GetData() + Signed, Mac.GetData(), ObfMac);
	return true;
}

bool FSvObfuscatingProtector::Unprotect(const TArray<uint8>& Blob, const FString& Key, TArray<uint8>& OutData)
{
	if (Blob.Num() < 1 + ObfIv + ObfMac || Blob[0] != ObfVersion) return false;
	const int32 Signed = Blob.Num() - ObfMac;
	TArray<uint8> MacInput;
	MacInput.Append(Blob.GetData(), Signed);
	MacInput.Append(SvCrypto::Utf8(Key));
	const TArray<uint8> Mac = SvCrypto::HmacSha256(MacKey, MacInput);
	if (!SvCrypto::ConstantTimeEquals(Mac.GetData(), Blob.GetData() + Signed, ObfMac)) return false;
	const int32 Length = Signed - 1 - ObfIv;
	OutData.SetNumZeroed(Length);
	SvCrypto::Aes256Ctr(EncKey, Blob.GetData() + 1, Blob.GetData() + 1 + ObfIv, OutData.GetData(), Length);
	return true;
}

ESvSaltResult FSvObfuscatingProtector::LoadOrCreateSalt(const FString& Path, TArray<uint8>& OutSalt)
{
	IFileManager& FM = IFileManager::Get();
	if (FM.FileExists(*Path))
	{
		if (FFileHelper::LoadFileToArray(OutSalt, *Path, FILEREAD_Silent) && OutSalt.Num() >= 16) return ESvSaltResult::Loaded;
		// A damaged or unreadable salt: the values sealed with it cannot be opened. Never replaced silently.
		return ESvSaltResult::Unreadable;
	}
	// No salt, but sealed values: they were sealed with a salt that is gone (they cannot be opened either).
	TArray<FString> Sealed;
	FM.FindFiles(Sealed, *FPaths::Combine(FPaths::GetPath(Path), TEXT("*.svs")), true, false);
	if (Sealed.Num() > 0) return ESvSaltResult::Unreadable;
	if (!EnsureDirectory(FPaths::GetPath(Path)) || !SvCrypto::RandomBytes(32, OutSalt) || !WriteAtomic(Path, OutSalt)) return ESvSaltResult::Unavailable;
	return ESvSaltResult::Created;
}

FString FSvObfuscatingProtector::DefaultKeyMaterial()
{
	return FString(TEXT("splattervault|")) + FApp::GetProjectName() + TEXT("|") + FPlatformMisc::GetLoginId();
}

// ─── Windows DPAPI ────────────────────────────────────────────────────────────────────────────────────────────────

#if PLATFORM_WINDOWS
namespace
{
	TArray<uint8> DpapiEntropy(const FString& Key)
	{
		return SvCrypto::Utf8(FString(TEXT("splattervault|")) + Key);
	}
}

bool FSvDpapiProtector::Protect(const TArray<uint8>& Data, const FString& Key, TArray<uint8>& OutBlob)
{
	TArray<uint8> Entropy = DpapiEntropy(Key);
	DATA_BLOB In;
	In.pbData = const_cast<BYTE*>(Data.GetData());
	In.cbData = (DWORD)Data.Num();
	DATA_BLOB Ent;
	Ent.pbData = Entropy.GetData();
	Ent.cbData = (DWORD)Entropy.Num();
	DATA_BLOB Out;
	Out.pbData = nullptr;
	Out.cbData = 0;
	if (!CryptProtectData(&In, L"SplatterVault", &Ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &Out)) return false;
	OutBlob.Reset();
	OutBlob.Append(Out.pbData, (int32)Out.cbData);
	LocalFree(Out.pbData);
	return true;
}

bool FSvDpapiProtector::Unprotect(const TArray<uint8>& Blob, const FString& Key, TArray<uint8>& OutData)
{
	TArray<uint8> Entropy = DpapiEntropy(Key);
	DATA_BLOB In;
	In.pbData = const_cast<BYTE*>(Blob.GetData());
	In.cbData = (DWORD)Blob.Num();
	DATA_BLOB Ent;
	Ent.pbData = Entropy.GetData();
	Ent.cbData = (DWORD)Entropy.Num();
	DATA_BLOB Out;
	Out.pbData = nullptr;
	Out.cbData = 0;
	if (!CryptUnprotectData(&In, nullptr, &Ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &Out)) return false;
	OutData.Reset();
	OutData.Append(Out.pbData, (int32)Out.cbData);
	SecureZeroMemory(Out.pbData, Out.cbData);
	LocalFree(Out.pbData);
	return true;
}
#endif

// ─── Android Keystore ─────────────────────────────────────────────────────────────────────────────────────────────

#if PLATFORM_ANDROID
namespace
{
	bool ClearJavaException(JNIEnv* Env)
	{
		if (!Env->ExceptionCheck()) return false;
		Env->ExceptionClear();
		return true;
	}

	/**
	 * com.splattervault.sdk.SvKeystore through the app's class loader, without AndroidJavaEnv::FindJavaClassGlobalRef
	 * (whose exception check asserts when a class is missing). Null when the helper is not in the APK.
	 */
	jclass KeystoreClass()
	{
		static jclass Cls = nullptr;
		static bool bLooked = false;
		if (bLooked) return Cls;
		bLooked = true;
		JNIEnv* Env = AndroidJavaEnv::GetJavaEnv();
		const jobject Loader = AndroidJavaEnv::GetClassLoader();
		if (!Env || !Loader) return nullptr;
		const jclass LoaderClass = Env->GetObjectClass(Loader);
		const jmethodID LoadClass = LoaderClass ? Env->GetMethodID(LoaderClass, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;") : nullptr;
		if (ClearJavaException(Env) || !LoadClass)
		{
			if (LoaderClass) Env->DeleteLocalRef(LoaderClass);
			return nullptr;
		}
		const jstring Name = Env->NewStringUTF("com.splattervault.sdk.SvKeystore");
		if (ClearJavaException(Env) || !Name)
		{
			Env->DeleteLocalRef(LoaderClass);
			return nullptr;
		}
		const jobject Found = Env->CallObjectMethod(Loader, LoadClass, Name);
		if (!ClearJavaException(Env) && Found)
		{
			Cls = (jclass)Env->NewGlobalRef(Found);
		}
		if (Found) Env->DeleteLocalRef(Found);
		Env->DeleteLocalRef(Name);
		Env->DeleteLocalRef(LoaderClass);
		return Cls;
	}

	/** A Java byte[] of Bytes, or null (with no exception pending) when it could not be made. */
	jbyteArray ToJava(JNIEnv* Env, const TArray<uint8>& Bytes)
	{
		jbyteArray A = Env->NewByteArray(Bytes.Num());
		if (ClearJavaException(Env) || !A) return nullptr;
		if (Bytes.Num() > 0)
		{
			Env->SetByteArrayRegion(A, 0, Bytes.Num(), reinterpret_cast<const jbyte*>(Bytes.GetData()));
			if (ClearJavaException(Env))
			{
				Env->DeleteLocalRef(A);
				return nullptr;
			}
		}
		return A;
	}

	bool CallBytes(const char* Method, const TArray<uint8>& Input, const FString& Key, TArray<uint8>& Out)
	{
		JNIEnv* Env = AndroidJavaEnv::GetJavaEnv();
		const jclass Cls = KeystoreClass();
		if (!Env || !Cls) return false;
		const jmethodID M = Env->GetStaticMethodID(Cls, Method, "([B[B)[B");
		if (ClearJavaException(Env) || !M) return false;
		jbyteArray JIn = ToJava(Env, Input);
		jbyteArray JAad = ToJava(Env, SvCrypto::Utf8(FString(TEXT("splattervault|")) + Key));
		bool bOk = false;
		if (JIn && JAad)
		{
			jbyteArray JOut = (jbyteArray)Env->CallStaticObjectMethod(Cls, M, JIn, JAad);
			if (!ClearJavaException(Env) && JOut)
			{
				const jsize N = Env->GetArrayLength(JOut);
				Out.SetNumUninitialized(N);
				if (N > 0) Env->GetByteArrayRegion(JOut, 0, N, reinterpret_cast<jbyte*>(Out.GetData()));
				bOk = !ClearJavaException(Env);
			}
			if (JOut) Env->DeleteLocalRef(JOut);
		}
		if (JIn) Env->DeleteLocalRef(JIn);
		if (JAad) Env->DeleteLocalRef(JAad);
		return bOk;
	}
}

bool FSvAndroidKeystoreProtector::IsAvailable()
{
	return KeystoreClass() != nullptr;
}

FString FSvAndroidKeystoreProtector::InternalFilesDir()
{
	JNIEnv* Env = AndroidJavaEnv::GetJavaEnv();
	const jclass Cls = KeystoreClass();
	if (!Env || !Cls) return FString();
	const jmethodID M = Env->GetStaticMethodID(Cls, "filesDir", "(Landroid/app/Activity;)Ljava/lang/String;");
	if (!M || Env->ExceptionCheck())
	{
		Env->ExceptionClear();
		return FString();
	}
	jstring JDir = (jstring)Env->CallStaticObjectMethod(Cls, M, AndroidJavaEnv::GetGameActivityThis());
	FString Dir;
	if (ClearJavaException(Env))
	{
	}
	else if (JDir)
	{
		const char* Utf = Env->GetStringUTFChars(JDir, nullptr);
		if (Utf)
		{
			Dir = UTF8_TO_TCHAR(Utf);
			Env->ReleaseStringUTFChars(JDir, Utf);
		}
	}
	if (JDir) Env->DeleteLocalRef(JDir);
	return Dir;
}

bool FSvAndroidKeystoreProtector::Protect(const TArray<uint8>& Data, const FString& Key, TArray<uint8>& OutBlob)
{
	return CallBytes("protect", Data, Key, OutBlob);
}

bool FSvAndroidKeystoreProtector::Unprotect(const TArray<uint8>& Blob, const FString& Key, TArray<uint8>& OutData)
{
	return CallBytes("unprotect", Blob, Key, OutData);
}
#endif

// ─── the platform's store ─────────────────────────────────────────────────────────────────────────────────────────

#if !PLATFORM_WINDOWS && !PLATFORM_ANDROID
namespace
{
	TSharedRef<ISvSecureStore> MakeObfuscated(const FString& Directory, FString* OutWarning)
	{
		TArray<uint8> Salt;
		const FString SaltPath = FPaths::Combine(Directory, TEXT("store.salt"));
		switch (FSvObfuscatingProtector::LoadOrCreateSalt(SaltPath, Salt))
		{
		case ESvSaltResult::Unreadable:
			// Values exist that cannot be opened: unreadable (storage_unreadable), never "nothing stored".
			if (OutWarning) *OutWarning = TEXT("The SplatterVault secure store's salt is missing or damaged: what it holds cannot be read.");
			return MakeShared<FSvUnreadableStore>(TEXT("The secure store's salt is missing or damaged: its values cannot be opened."));
		case ESvSaltResult::Unavailable:
			if (OutWarning) *OutWarning = TEXT("The SplatterVault secure store could not be set up: nothing is saved on this device (secrets live in memory until the game quits).");
			return MakeShared<FSvUnavailableStore>(TEXT("The secure store could not be set up on this device."));
		default:
			break;
		}
		if (OutWarning)
		{
			*OutWarning = TEXT("This platform has no OS-backed secure storage in the SplatterVault SDK yet: refresh tokens and guest secrets are only obfuscated on disk.");
		}
		return MakeShared<FSvProtectedFileStore>(Directory, MakeShared<FSvObfuscatingProtector>(FSvObfuscatingProtector::DefaultKeyMaterial(), Salt));
	}
}
#endif

TSharedRef<ISvSecureStore> SvCreatePlatformSecureStore(const FString& Name, FString* OutWarning)
{
	const FString Project = FApp::GetProjectName();
#if PLATFORM_WINDOWS
	const FString Directory = FPaths::Combine(FPlatformProcess::UserSettingsDir(), TEXT("SplatterVault"), Project, Name);
	return MakeShared<FSvProtectedFileStore>(Directory, MakeShared<FSvDpapiProtector>());
#elif PLATFORM_ANDROID
	// The Keystore helper is part of the plugin (its UPL file adds it to every APK). Without it, or without the app's
	// private files directory, nothing is saved: no fallback to obfuscated files in shared storage.
	const FString Internal = FSvAndroidKeystoreProtector::IsAvailable() ? FSvAndroidKeystoreProtector::InternalFilesDir() : FString();
	if (!Internal.IsEmpty())
	{
		return MakeShared<FSvProtectedFileStore>(FPaths::Combine(Internal, TEXT("splattervault"), Name), MakeShared<FSvAndroidKeystoreProtector>());
	}
	if (OutWarning)
	{
		*OutWarning = TEXT("The SplatterVault Android Keystore helper (com.splattervault.sdk.SvKeystore) is missing from the APK: nothing is saved on this device.");
	}
	return MakeShared<FSvUnavailableStore>(TEXT("The Android Keystore helper is missing from the APK."));
#elif PLATFORM_UNIX || PLATFORM_MAC
	return MakeObfuscated(FPaths::Combine(FPlatformProcess::UserSettingsDir(), TEXT("SplatterVault"), Project, Name), OutWarning);
#else
	// TODO: iOS Keychain and console stores.
	return MakeObfuscated(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SplatterVault"), Name), OutWarning);
#endif
}
