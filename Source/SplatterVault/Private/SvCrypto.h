// Copyright (c) SplatterVault. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"

/** Hashing, MACs and random bytes for the secure store and idempotency keys. */
namespace SvCrypto
{
	/** SHA-256 of Data (32 bytes). */
	TArray<uint8> Sha256(const uint8* Data, int64 Length);
	TArray<uint8> Sha256(const TArray<uint8>& Data);

	/** HMAC-SHA256 (RFC 2104). */
	TArray<uint8> HmacSha256(const TArray<uint8>& Key, const uint8* Data, int64 Length);
	TArray<uint8> HmacSha256(const TArray<uint8>& Key, const TArray<uint8>& Data);

	/** Fills Out with Count bytes from the operating system's secure random source. False when none answered. */
	bool RandomBytes(int32 Count, TArray<uint8>& Out);

	/** Lower-case hex. */
	FString Hex(const TArray<uint8>& Bytes);

	/** UTF-8 bytes of Text. */
	TArray<uint8> Utf8(const FString& Text);

	/** Text from UTF-8 bytes. */
	FString FromUtf8(const TArray<uint8>& Bytes);

	/** AES-256 in counter mode (encrypts and decrypts alike). Key: 32 bytes; Iv: 16 bytes. */
	void Aes256Ctr(const TArray<uint8>& Key, const uint8* Iv, const uint8* In, uint8* Out, int64 Length);

	/** Equal without an early exit. */
	bool ConstantTimeEquals(const uint8* A, const uint8* B, int32 Length);
}
