// Copyright (c) SplatterVault. Licensed under the MIT License.

#include "SvCrypto.h"

#include "Misc/AES.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#include <bcrypt.h>
#include "Windows/HideWindowsPlatformTypes.h"
#elif PLATFORM_ANDROID || PLATFORM_UNIX || PLATFORM_APPLE
#include <stdio.h>
#endif

namespace
{
	const uint32 K256[64] = {
		0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
		0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
		0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
		0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
		0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
		0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
		0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
		0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

	FORCEINLINE uint32 Rotr(uint32 X, uint32 N) { return (X >> N) | (X << (32 - N)); }

	struct FSha256
	{
		uint32 H[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
		uint8 Block[64] = {};
		int32 BlockLen = 0;
		uint64 TotalBits = 0;

		void Compress(const uint8* P)
		{
			uint32 W[64];
			for (int32 i = 0; i < 16; ++i)
			{
				W[i] = (uint32(P[i * 4]) << 24) | (uint32(P[i * 4 + 1]) << 16) | (uint32(P[i * 4 + 2]) << 8) | uint32(P[i * 4 + 3]);
			}
			for (int32 i = 16; i < 64; ++i)
			{
				const uint32 S0 = Rotr(W[i - 15], 7) ^ Rotr(W[i - 15], 18) ^ (W[i - 15] >> 3);
				const uint32 S1 = Rotr(W[i - 2], 17) ^ Rotr(W[i - 2], 19) ^ (W[i - 2] >> 10);
				W[i] = W[i - 16] + S0 + W[i - 7] + S1;
			}
			uint32 A = H[0], B = H[1], C = H[2], D = H[3], E = H[4], F = H[5], G = H[6], Hh = H[7];
			for (int32 i = 0; i < 64; ++i)
			{
				const uint32 S1 = Rotr(E, 6) ^ Rotr(E, 11) ^ Rotr(E, 25);
				const uint32 Ch = (E & F) ^ (~E & G);
				const uint32 T1 = Hh + S1 + Ch + K256[i] + W[i];
				const uint32 S0 = Rotr(A, 2) ^ Rotr(A, 13) ^ Rotr(A, 22);
				const uint32 Maj = (A & B) ^ (A & C) ^ (B & C);
				const uint32 T2 = S0 + Maj;
				Hh = G;
				G = F;
				F = E;
				E = D + T1;
				D = C;
				C = B;
				B = A;
				A = T1 + T2;
			}
			H[0] += A;
			H[1] += B;
			H[2] += C;
			H[3] += D;
			H[4] += E;
			H[5] += F;
			H[6] += G;
			H[7] += Hh;
		}

		void Update(const uint8* Data, int64 Length)
		{
			for (int64 i = 0; i < Length; ++i)
			{
				Block[BlockLen++] = Data[i];
				if (BlockLen == 64)
				{
					Compress(Block);
					BlockLen = 0;
				}
			}
			TotalBits += uint64(Length) * 8;
		}

		TArray<uint8> Final()
		{
			const uint64 Bits = TotalBits;
			const uint8 One = 0x80;
			Update(&One, 1);
			const uint8 Zero = 0;
			while (BlockLen != 56)
			{
				Update(&Zero, 1);
			}
			uint8 Len[8];
			for (int32 i = 0; i < 8; ++i) Len[i] = uint8(Bits >> (56 - 8 * i));
			Update(Len, 8);
			TArray<uint8> Out;
			Out.SetNumUninitialized(32);
			for (int32 i = 0; i < 8; ++i)
			{
				Out[i * 4] = uint8(H[i] >> 24);
				Out[i * 4 + 1] = uint8(H[i] >> 16);
				Out[i * 4 + 2] = uint8(H[i] >> 8);
				Out[i * 4 + 3] = uint8(H[i]);
			}
			return Out;
		}
	};
}

namespace SvCrypto
{
	TArray<uint8> Sha256(const uint8* Data, int64 Length)
	{
		FSha256 S;
		S.Update(Data, Length);
		return S.Final();
	}

	TArray<uint8> Sha256(const TArray<uint8>& Data)
	{
		return Sha256(Data.GetData(), Data.Num());
	}

	TArray<uint8> HmacSha256(const TArray<uint8>& Key, const uint8* Data, int64 Length)
	{
		uint8 K[64] = {};
		if (Key.Num() > 64)
		{
			const TArray<uint8> Hashed = Sha256(Key);
			FMemory::Memcpy(K, Hashed.GetData(), 32);
		}
		else if (Key.Num() > 0)
		{
			FMemory::Memcpy(K, Key.GetData(), Key.Num());
		}
		uint8 IPad[64];
		uint8 OPad[64];
		for (int32 i = 0; i < 64; ++i)
		{
			IPad[i] = K[i] ^ 0x36;
			OPad[i] = K[i] ^ 0x5c;
		}
		FSha256 Inner;
		Inner.Update(IPad, 64);
		Inner.Update(Data, Length);
		const TArray<uint8> InnerHash = Inner.Final();
		FSha256 Outer;
		Outer.Update(OPad, 64);
		Outer.Update(InnerHash.GetData(), InnerHash.Num());
		return Outer.Final();
	}

	TArray<uint8> HmacSha256(const TArray<uint8>& Key, const TArray<uint8>& Data)
	{
		return HmacSha256(Key, Data.GetData(), Data.Num());
	}

	bool RandomBytes(int32 Count, TArray<uint8>& Out)
	{
		Out.SetNumZeroed(Count);
		if (Count <= 0) return true;
#if PLATFORM_WINDOWS
		const NTSTATUS Status = BCryptGenRandom(nullptr, Out.GetData(), (ULONG)Count, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
		return Status == 0;
#elif PLATFORM_ANDROID || PLATFORM_UNIX || PLATFORM_APPLE
		FILE* F = fopen("/dev/urandom", "rb");
		if (!F) return false;
		const size_t Read = fread(Out.GetData(), 1, (size_t)Count, F);
		fclose(F);
		return Read == (size_t)Count;
#else
		return false;
#endif
	}

	FString Hex(const TArray<uint8>& Bytes)
	{
		static const TCHAR* Digits = TEXT("0123456789abcdef");
		FString Out;
		Out.Reserve(Bytes.Num() * 2);
		for (const uint8 B : Bytes)
		{
			Out.AppendChar(Digits[B >> 4]);
			Out.AppendChar(Digits[B & 15]);
		}
		return Out;
	}

	TArray<uint8> Utf8(const FString& Text)
	{
		const FTCHARToUTF8 Converted(*Text);
		TArray<uint8> Out;
		Out.Append(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
		return Out;
	}

	FString FromUtf8(const TArray<uint8>& Bytes)
	{
		if (Bytes.Num() == 0) return FString();
		const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
		return FString(Converted.Length(), Converted.Get());
	}

	void Aes256Ctr(const TArray<uint8>& Key, const uint8* Iv, const uint8* In, uint8* Out, int64 Length)
	{
		check(Key.Num() == 32);
		FAES::FAESKey AesKey;
		FMemory::Memcpy(AesKey.Key, Key.GetData(), 32);
		uint8 Counter[16];
		FMemory::Memcpy(Counter, Iv, 16);
		uint8 Stream[16];
		for (int64 Offset = 0; Offset < Length; Offset += 16)
		{
			FMemory::Memcpy(Stream, Counter, 16);
			FAES::EncryptData(Stream, 16, AesKey);
			const int64 N = FMath::Min<int64>(16, Length - Offset);
			for (int64 i = 0; i < N; ++i)
			{
				Out[Offset + i] = In[Offset + i] ^ Stream[i];
			}
			// Big-endian increment of the whole 128-bit counter.
			for (int32 i = 15; i >= 0; --i)
			{
				if (++Counter[i] != 0) break;
			}
		}
		FMemory::Memzero(AesKey.Key, 32);
	}

	bool ConstantTimeEquals(const uint8* A, const uint8* B, int32 Length)
	{
		uint8 Diff = 0;
		for (int32 i = 0; i < Length; ++i) Diff |= A[i] ^ B[i];
		return Diff == 0;
	}
}
