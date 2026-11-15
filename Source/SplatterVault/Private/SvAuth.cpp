// Copyright (c) SplatterVault. Licensed under the MIT License.

#include "GenericPlatform/GenericPlatformHttp.h"
#include "Misc/Base64.h"
#include "SplatterVaultClient.h"
#include "SvJson.h"
#include "SvPipeline.h"

namespace
{
	const TCHAR* TokenExpiredCode = TEXT("token_expired");
	const TCHAR* RefreshItem = TEXT("refresh_token");
	const TCHAR* GuestItem = TEXT("guest_secret");
	const TCHAR* LastItem = TEXT("last_sign_in");
	constexpr double DefaultLifetime = 3600.0;
	constexpr double MinLifetime = 60.0;
	constexpr double MaxLifetime = 86400.0;
	constexpr double ClockMargin = 5.0;

	struct FStamp
	{
		FDateTime Wall;
		double Mono = 0;
	};

	struct FRefreshRecord
	{
		FString Token;
		/** Empty for one stored without its player: never used. */
		FString PlayerId;
	};

	struct FDeviceRecord
	{
		FString Secret;
		FString PlayerId;
	};

	struct FLastSignIn
	{
		/** Empty: signed out on this device. */
		FString Method;
		FString PlayerId;
	};

	/** The full sign-in answer, secrets included (never handed to the game, never in a UPROPERTY). */
	struct FSignInAnswer
	{
		FString Token;
		FString ExpiresAt;
		bool bHasPlayer = false;
		FSvSignedInPlayer Player;
		TArray<FSvIdentity> Identities;
		FString RefreshToken;
		FString DeviceSecret;
	};

	struct FProfileAnswer
	{
		FSvPlayerProfile Profile;
		bool bHasPlayer = false;
		FString RefreshToken;
	};

	enum class ERenewMode : uint8
	{
		/** Refresh token, else (a guest session) the device secret. */
		Auto,
		/** The refresh token only. */
		RefreshOnly,
		/** The refresh token only, even when the current token is still fine. */
		ForceRefresh
	};

	FString Esc(const FString& S)
	{
		return FGenericPlatformHttp::UrlEncode(S);
	}

	TSharedPtr<FJsonObject> ReadClaims(const FString& Jwt)
	{
		TArray<FString> Parts;
		Jwt.ParseIntoArray(Parts, TEXT("."), false);
		if (Parts.Num() != 3) return nullptr;
		FString B64 = Parts[1].Replace(TEXT("-"), TEXT("+")).Replace(TEXT("_"), TEXT("/"));
		switch (B64.Len() % 4)
		{
		case 2:
			B64 += TEXT("==");
			break;
		case 3:
			B64 += TEXT("=");
			break;
		default:
			break;
		}
		TArray<uint8> Bytes;
		if (!FBase64::Decode(B64, Bytes)) return nullptr;
		Bytes.Add(0);
		const FString Json = UTF8_TO_TCHAR(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()));
		return SvJson::ParseObject(Json);
	}

	/** Unix seconds as a time; unset when missing, not finite, or outside 1970-9999. */
	TOptional<FDateTime> FromUnix(const TSharedPtr<FJsonObject>& Claims, const TCHAR* Field)
	{
		double S = 0;
		if (!SvJson::GetNumber(Claims, Field, S)) return TOptional<FDateTime>();
		if (S < 0 || S > 253402300799.0) return TOptional<FDateTime>();
		return FDateTime(1970, 1, 1) + FTimespan::FromSeconds(S);
	}

	TOptional<FDateTime> ParseIso(const FString& Iso)
	{
		if (Iso.IsEmpty()) return TOptional<FDateTime>();
		FDateTime At;
		if (!FDateTime::ParseIso8601(*Iso, At)) return TOptional<FDateTime>();
		return At;
	}

	bool ParseSignIn(const FSvHttpResponse& R, FSignInAnswer& Out, FSvError& OutError)
	{
		const TSharedPtr<FJsonObject> J = SvJson::ParseObject(R.Body);
		if (!J.IsValid())
		{
			OutError = FSvError(R.Status, SvErrorCodes::InvalidResponse, TEXT("The sign-in answer could not be read."));
			return false;
		}
		Out.Token = SvJson::GetString(J, TEXT("token"));
		Out.ExpiresAt = SvJson::GetString(J, TEXT("expiresAt"));
		Out.RefreshToken = SvJson::GetString(J, TEXT("refreshToken"));
		Out.DeviceSecret = SvJson::GetString(J, TEXT("deviceSecret"));
		const TSharedPtr<FJsonObject> P = SvJson::GetObj(J, TEXT("player"));
		Out.bHasPlayer = P.IsValid() && SvModels::Read(P, Out.Player) && !Out.Player.Id.IsEmpty();
		const TArray<TSharedPtr<FJsonValue>>* Ids = nullptr;
		if (J->TryGetArrayField(TEXT("identities"), Ids) && Ids)
		{
			for (const TSharedPtr<FJsonValue>& V : *Ids)
			{
				FSvIdentity I;
				if (V.IsValid() && V->Type == EJson::Object && SvModels::Read(V->AsObject(), I)) Out.Identities.Add(I);
			}
		}
		if (Out.Token.IsEmpty() || !Out.bHasPlayer)
		{
			OutError = FSvError(R.Status, SvErrorCodes::InvalidResponse, TEXT("The sign-in answer had no token."));
			return false;
		}
		return true;
	}

	bool ParseProfile(const FSvHttpResponse& R, FProfileAnswer& Out, FSvError& OutError)
	{
		const TSharedPtr<FJsonObject> J = SvJson::ParseObject(R.Body);
		if (!J.IsValid() || !SvModels::Read(J, Out.Profile))
		{
			OutError = FSvError(R.Status, SvErrorCodes::InvalidResponse, TEXT("The player answer could not be read."));
			return false;
		}
		Out.bHasPlayer = !Out.Profile.Player.Id.IsEmpty();
		Out.RefreshToken = SvJson::GetString(J, TEXT("refreshToken"));
		return true;
	}

	template <typename T>
	bool ParseModel(const FSvHttpResponse& R, T& Out, FSvError& OutError)
	{
		const TSharedPtr<FJsonObject> J = SvJson::ParseObject(R.Body);
		if (!J.IsValid() || !SvModels::Read(J, Out))
		{
			OutError = FSvError(R.Status, SvErrorCodes::InvalidResponse, TEXT("The answer could not be read."));
			return false;
		}
		return true;
	}

	FSvError Invalid(const FString& Why)
	{
		return FSvError(0, SvErrorCodes::InvalidArgument, Why);
	}

	FSvError SignInChangedError()
	{
		return FSvError(0, SvErrorCodes::SignInChanged,
			TEXT("The sign-in changed (another sign-in, a sign-out or an erase) while this request was in flight; its answer was not used."));
	}

	/**
	 * What a sign-in answer must still match to be used: the auth epoch (a renewal or a restore: any change of the
	 * sign-in, a renewal of it included, makes it stale) or the sign-in generation (a game-initiated sign-in: a later
	 * sign-in, a sign-out or an erase makes it stale; a background renewal does not).
	 */
	struct FSentUnder
	{
		TOptional<uint64> Epoch;
		TOptional<uint64> SignIn;

		static FSentUnder Renewal(uint64 InEpoch)
		{
			FSentUnder U;
			U.Epoch = InEpoch;
			return U;
		}

		static FSentUnder GameSignIn(uint64 InGeneration)
		{
			FSentUnder U;
			U.SignIn = InGeneration;
			return U;
		}
	};

	bool IsDigits(const FString& S, int32 MaxLen)
	{
		if (S.IsEmpty() || S.Len() > MaxLen) return false;
		for (const TCHAR C : S)
		{
			if (C < TEXT('0') || C > TEXT('9')) return false;
		}
		return true;
	}

	bool IsHex(const FString& S)
	{
		if (S.IsEmpty()) return false;
		for (const TCHAR C : S)
		{
			if (!((C >= TEXT('0') && C <= TEXT('9')) || (C >= TEXT('a') && C <= TEXT('f')) || (C >= TEXT('A') && C <= TEXT('F')))) return false;
		}
		return true;
	}

	/** A 404 for a route the API does not have (not_found, a framework answer or a bare 404). Never a 404 with another code. */
	bool IsRouteMissing(const FSvError& E)
	{
		return E.Status == 404 && (E.Code.IsEmpty() || E.Is(TEXT("not_found")));
	}
}

struct FSvSession
{
	FString Token;
	FString ExpiresAt;
	/** On this device's wall clock (UTC). */
	TOptional<FDateTime> Expiry;
	TOptional<FDateTime> RenewAt;
	/** The same two moments on the monotonic clock (a time sync cannot move them). */
	TOptional<double> MonoExpiry;
	TOptional<double> MonoRenewAt;
	FString Method;
	FSvSignedInPlayer Player;
	TArray<FSvIdentity> Identities;
	/** Set when the API ended this sign-in's renewal (refresh_invalid, a refused guest secret). */
	TOptional<ESvSignInRequiredReason> EndReason;
	FString EndCode;
};

struct FSvAuthState
{
	double RenewLead = 120.0;
	double RefreshTimeout = 8.0;

	TSharedPtr<FSvSession> Session;
	/**
	 * Bumped by every set-session, clear-session, sign-out and erase. A renewal or a restore records it when sent; an
	 * answer that arrives under another epoch is never accepted (it would resurrect a signed-out player or replace a
	 * newer one): its refresh token is signed out on the server instead. Shared, so a refresh answer that arrives
	 * after the client was shut down can still tell.
	 */
	TSharedRef<uint64> Epoch = MakeShared<uint64>(0);
	/**
	 * Bumped when a game-initiated sign-in starts and by every clear-session (sign-out, erase, a refused token). A game
	 * sign-in sets the session only while it holds: a later sign-in, a sign-out or an erase wins over it.
	 */
	uint64 SignInGeneration = 0;
	FString LastMethod;
	FString DeadToken;
	bool bDeviceRejected = false;
	ESvSignInRequiredReason LastRenewReason = ESvSignInRequiredReason::TokenExpired;
	FString LastRenewCode;
	TOptional<FSvError> LastRenewError;

	bool bRefreshLoaded = false;
	TOptional<FRefreshRecord> Refresh;
	bool bRefreshUnsaved = false;
	bool bDeviceLoaded = false;
	TOptional<FDeviceRecord> Device;
	bool bDeviceUnsaved = false;

	TSvSingleFlight<FString> QuietRenew;
	TSvSingleFlight<FString> AskGame;
	/** First-launch guest sign-in: concurrent callers share one, so one device never makes two guests. */
	TSvSingleFlight<FSvSignInResult> GuestFlight;
	/** Serializes everything that may make or replace this device's guest. */
	FSvAsyncLock GuestLock;

	TFunction<TFuture<bool>(const FSvSignInRequired&)> Reauthenticate;
};

/** The auth logic, as static functions with access to FSvAuth's internals. */
struct FSvAuthOps
{
	using FSessionPtr = TSharedPtr<FSvSession>;

	static FSvAuthState& S(FSvAuth& A) { return *A.State; }
	static FSvPipeline& P(FSvAuth& A) { return A.Owner.GetPipeline(); }
	static ISvSecureStore& Store(FSvAuth& A) { return A.Owner.GetSecureStore(); }
	static FString RefreshKey(FSvAuth& A) { return A.Owner.GetStoragePrefix() + TEXT("refresh_token"); }
	static FString DeviceKey(FSvAuth& A) { return A.Owner.GetStoragePrefix() + TEXT("device_secret"); }
	static FString LastKey(FSvAuth& A) { return A.Owner.GetStoragePrefix() + TEXT("last_sign_in"); }

	static TSharedRef<FJsonObject> GameBody(FSvAuth& A)
	{
		TSharedRef<FJsonObject> B = MakeShared<FJsonObject>();
		A.Owner.AddGame(B);
		return B;
	}

	static FStamp Now(FSvAuth& A)
	{
		FStamp St;
		St.Wall = P(A).Now();
		St.Mono = P(A).Mono();
		return St;
	}

	// ─── session ─────────────────────────────────────────────────────────────────────────────────────────────────

	/** Due on EITHER clock: the monotonic one (a time sync backwards cannot delay it) or the wall clock. */
	static bool IsDue(FSvAuth& A, const TOptional<FDateTime>& Wall, const TOptional<double>& Mono)
	{
		return (Wall.IsSet() && P(A).Now() >= Wall.GetValue()) || (Mono.IsSet() && P(A).Mono() >= Mono.GetValue());
	}

	static bool IsExpired(FSvAuth& A, const FSessionPtr& Sn)
	{
		return Sn.IsValid() && IsDue(A, Sn->Expiry, Sn->MonoExpiry);
	}

	static void SetSession(FSvAuth& A, const FSessionPtr& Sn)
	{
		++*S(A).Epoch;
		S(A).Session = Sn;
		S(A).LastMethod = Sn->Method;
		S(A).DeadToken.Reset();
	}

	static void ClearSession(FSvAuth& A, bool bRaise)
	{
		++*S(A).Epoch;
		++S(A).SignInGeneration;
		const bool bHad = S(A).Session.IsValid();
		S(A).Session.Reset();
		if (bRaise && bHad) A.OnSignedOut.Broadcast();
	}

	static void RaiseSignInRequired(FSvAuth& A, const FSvSignInRequired& Info)
	{
		A.OnSignInRequired.Broadcast(Info);
	}

	static FSvSignInRequired Required(ESvSignInRequiredReason Reason, const FString& Method, const FString& Code)
	{
		FSvSignInRequired R;
		R.Reason = Reason;
		R.Method = Method;
		R.ErrorCode = Code;
		return R;
	}

	static FSvSignInResult ToResult(const FSessionPtr& Sn, bool bRenewable, bool bSaved)
	{
		FSvSignInResult R;
		if (Sn.IsValid())
		{
			R.Method = Sn->Method;
			R.ExpiresAt = Sn->ExpiresAt;
			R.Player = Sn->Player;
			R.Identities = Sn->Identities;
		}
		R.Renewable = bRenewable;
		R.SavedToDevice = bSaved;
		return R;
	}

	/** Renewable after a restart too: a saved refresh token, or (a guest) its saved device secret. */
	static bool IsRenewableNow(FSvAuth& A, const FSessionPtr& Sn)
	{
		if (!Sn.IsValid()) return false;
		FSvAuthState& St = S(A);
		if (St.Refresh.IsSet() && !St.bRefreshUnsaved && !St.Refresh->PlayerId.IsEmpty() && SvSame(St.Refresh->PlayerId, Sn->Player.Id)) return true;
		return SvSame(Sn->Method, SvProviders::Device) && St.Device.IsSet() && !St.bDeviceUnsaved &&
			(St.Device->PlayerId.IsEmpty() || Sn->Player.Id.IsEmpty() || SvSame(St.Device->PlayerId, Sn->Player.Id));
	}

	/** The SDK can renew this session without the player. Storage errors: no. */
	static bool IsRenewableQuiet(FSvAuth& A, const FSessionPtr& Sn)
	{
		FString Token;
		FSvError Err;
		if (UsableRefreshToken(A, Sn, nullptr, Token, Err) && !Token.IsEmpty()) return true;
		if (!SvSame(Sn->Method, SvProviders::Device) || S(A).bDeviceRejected) return false;
		TOptional<FDeviceRecord> Device;
		return GetDevice(A, Device, Err) && Device.IsSet();
	}

	// ─── storage ─────────────────────────────────────────────────────────────────────────────────────────────────

	/** A stored value. False (and OutError storage_unreadable) when it cannot be read. */
	static bool ReadKey(FSvAuth& A, const FString& Key, const FString& Item, TOptional<FString>& Out, FSvError& OutError)
	{
		FString Value;
		FString Why;
		const ESvStoreRead R = Store(A).Get(Key, Value, Why);
		if (R == ESvStoreRead::Error)
		{
			A.RaiseStorageFailed(Item, ESvStorageOperation::Read, Why);
			OutError = FSvAuth::Unreadable(Item);
			return false;
		}
		Out.Reset();
		if (R == ESvStoreRead::Found && !Value.IsEmpty()) Out = Value;
		return true;
	}

	static bool WriteKey(FSvAuth& A, const FString& Key, const FString& Item, const FString& Value)
	{
		FString Why;
		if (Store(A).Set(Key, Value, Why)) return true;
		A.RaiseStorageFailed(Item, ESvStorageOperation::Write, Why);
		return false;
	}

	static bool DeleteKey(FSvAuth& A, const FString& Key, const FString& Item)
	{
		FString Why;
		if (Store(A).Delete(Key, Why)) return true;
		A.RaiseStorageFailed(Item, ESvStorageOperation::Delete, Why);
		return false;
	}

	static bool ParseRefresh(const TOptional<FString>& Stored, TOptional<FRefreshRecord>& Out)
	{
		Out.Reset();
		if (!Stored.IsSet()) return true;
		// A bare token (no player): kept readable, never used (it cannot be tied to the signed-in player).
		if (Stored->StartsWith(TEXT("svpr1_"), ESearchCase::CaseSensitive))
		{
			FRefreshRecord R;
			R.Token = Stored.GetValue();
			Out = R;
			return true;
		}
		const TSharedPtr<FJsonObject> O = SvJson::ParseObject(Stored.GetValue());
		const FString Token = SvJson::GetString(O, TEXT("token"));
		if (!O.IsValid() || Token.IsEmpty()) return false;
		FRefreshRecord R;
		R.Token = Token;
		R.PlayerId = SvJson::GetString(O, TEXT("player"));
		Out = R;
		return true;
	}

	static bool GetRefreshRecord(FSvAuth& A, TOptional<FRefreshRecord>& Out, FSvError& OutError)
	{
		FSvAuthState& St = S(A);
		if (St.bRefreshLoaded)
		{
			Out = St.Refresh;
			return true;
		}
		TOptional<FString> Stored;
		if (!ReadKey(A, RefreshKey(A), RefreshItem, Stored, OutError)) return false;
		TOptional<FRefreshRecord> Record;
		if (!ParseRefresh(Stored, Record))
		{
			A.RaiseStorageFailed(RefreshItem, ESvStorageOperation::Read, TEXT("The stored refresh token is not in a known format."));
			OutError = FSvAuth::Unreadable(RefreshItem);
			return false;
		}
		St.Refresh = Record;
		St.bRefreshLoaded = true;
		Out = St.Refresh;
		return true;
	}

	/**
	 * The stored refresh token when it is THIS player's: the signed-in player's, or with no session the player of the
	 * last sign-in when that was an email sign-in. Anything else is a leftover: never used, and deleted again.
	 */
	static bool UsableRefreshToken(FSvAuth& A, const FSessionPtr& Sn, const FLastSignIn* Last, FString& OutToken, FSvError& OutError)
	{
		OutToken.Reset();
		TOptional<FRefreshRecord> Record;
		if (!GetRefreshRecord(A, Record, OutError)) return false;
		if (!Record.IsSet()) return true;
		FString Expected;
		if (Sn.IsValid())
		{
			Expected = Sn->Player.Id;
		}
		else
		{
			TOptional<FLastSignIn> Read;
			const FLastSignIn* L = Last;
			if (!L)
			{
				if (!ReadLastSignIn(A, true, Read, OutError)) return false;
				if (Read.IsSet()) L = &Read.GetValue();
			}
			Expected = L && SvSame(L->Method, SvProviders::Email) ? L->PlayerId : FString();
		}
		if (!Expected.IsEmpty() && SvSame(Record->PlayerId, Expected))
		{
			OutToken = Record->Token;
			return true;
		}
		A.Owner.Log(ELogVerbosity::Log, TEXT("A stored refresh token of another player (or of none) was not used, and is deleted."));
		DeleteRefreshTokenIf(A, Record->Token);
		return true;
	}

	static FString RefreshJson(const FRefreshRecord& R)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("v"), 1);
		O->SetStringField(TEXT("token"), R.Token);
		if (!R.PlayerId.IsEmpty()) O->SetStringField(TEXT("player"), R.PlayerId);
		return SvJson::Write(O);
	}

	static bool SaveRefreshToken(FSvAuth& A, const FString& Token, const FString& PlayerId)
	{
		FSvAuthState& St = S(A);
		FRefreshRecord Record;
		Record.Token = Token;
		Record.PlayerId = PlayerId;
		St.Refresh = Record;
		St.bRefreshLoaded = true;
		const bool bOk = WriteKey(A, RefreshKey(A), RefreshItem, RefreshJson(Record));
		St.bRefreshUnsaved = !bOk;
		if (!bOk)
		{
			A.Owner.Log(ELogVerbosity::Warning, TEXT("The refresh token could not be saved: it is kept in memory, and the player signs in again after a restart."));
		}
		return bOk;
	}

	static void DeleteRefreshToken(FSvAuth& A)
	{
		FSvAuthState& St = S(A);
		St.Refresh.Reset();
		St.bRefreshLoaded = true;
		St.bRefreshUnsaved = false;
		// Still stored when the delete fails: read (and checked) again next time.
		if (!DeleteKey(A, RefreshKey(A), RefreshItem)) St.bRefreshLoaded = false;
	}

	/** Deletes the refresh token only when it is still the one that was sent (a newer one may have been saved meanwhile). */
	static void DeleteRefreshTokenIf(FSvAuth& A, const FString& Sent)
	{
		FSvAuthState& St = S(A);
		if (St.bRefreshLoaded && St.Refresh.IsSet() && !SvSame(St.Refresh->Token, Sent)) return;
		FString Value;
		FString Why;
		const ESvStoreRead R = Store(A).Get(RefreshKey(A), Value, Why);
		if (R == ESvStoreRead::Found)
		{
			TOptional<FRefreshRecord> Stored;
			if (ParseRefresh(TOptional<FString>(Value), Stored) && Stored.IsSet() && !SvSame(Stored->Token, Sent))
			{
				St.Refresh = Stored;
				St.bRefreshLoaded = true;
				return;
			}
		}
		DeleteRefreshToken(A);
	}

	/** Forgets the stored email sign-in, and ends it on the server in the background. */
	static void DropRefreshToken(FSvAuth& A, bool bSignOutOnServer)
	{
		TOptional<FRefreshRecord> Old;
		FSvError Err;
		if (!GetRefreshRecord(A, Old, Err))
		{
			DeleteRefreshToken(A);
			return;
		}
		if (!Old.IsSet()) return;
		DeleteRefreshToken(A);
		if (bSignOutOnServer) SignOutQuietly(A, Old->Token);
	}

	static void SignOutQuietly(FSvAuth& A, const FString& RefreshToken)
	{
		TSharedRef<FJsonObject> B = MakeShared<FJsonObject>();
		B->SetStringField(TEXT("refreshToken"), RefreshToken);
		// Best effort: nobody holds that refresh token any more.
		P(A).Send(FSvCall::Post(TEXT("/player-auth/sign-out"), SvJson::Write(B), ESvRetry::Never), FString(), [](const TSvResult<FSvHttpResponse>&) {});
	}

	static bool ParseDevice(const TOptional<FString>& Stored, TOptional<FDeviceRecord>& Out)
	{
		Out.Reset();
		if (!Stored.IsSet()) return true;
		if (Stored->StartsWith(TEXT("svpd1_"), ESearchCase::CaseSensitive))
		{
			FDeviceRecord R;
			R.Secret = Stored.GetValue();
			Out = R;
			return true;
		}
		const TSharedPtr<FJsonObject> O = SvJson::ParseObject(Stored.GetValue());
		const FString Secret = SvJson::GetString(O, TEXT("secret"));
		if (!O.IsValid() || Secret.IsEmpty()) return false;
		FDeviceRecord R;
		R.Secret = Secret;
		R.PlayerId = SvJson::GetString(O, TEXT("player"));
		Out = R;
		return true;
	}

	static bool GetDevice(FSvAuth& A, TOptional<FDeviceRecord>& Out, FSvError& OutError)
	{
		FSvAuthState& St = S(A);
		if (St.bDeviceLoaded)
		{
			Out = St.Device;
			return true;
		}
		TOptional<FString> Stored;
		if (!ReadKey(A, DeviceKey(A), GuestItem, Stored, OutError)) return false;
		TOptional<FDeviceRecord> Record;
		if (!ParseDevice(Stored, Record))
		{
			A.RaiseStorageFailed(GuestItem, ESvStorageOperation::Read, TEXT("The stored guest secret is not in a known format."));
			OutError = FSvAuth::Unreadable(GuestItem);
			return false;
		}
		St.Device = Record;
		St.bDeviceLoaded = true;
		Out = St.Device;
		return true;
	}

	static FString DeviceJson(const FDeviceRecord& R)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("v"), 1);
		O->SetStringField(TEXT("secret"), R.Secret);
		if (!R.PlayerId.IsEmpty()) O->SetStringField(TEXT("player"), R.PlayerId);
		return SvJson::Write(O);
	}

	/** Saves a guest secret with the player it belongs to, and reads it back. False when it is not safely stored. */
	static bool SaveDevice(FSvAuth& A, const FString& Secret, const FString& PlayerId)
	{
		FSvAuthState& St = S(A);
		FDeviceRecord Record;
		Record.Secret = Secret;
		Record.PlayerId = PlayerId;
		St.Device = Record;
		St.bDeviceLoaded = true;
		const FString Json = DeviceJson(Record);
		bool bOk = WriteKey(A, DeviceKey(A), GuestItem, Json);
		if (bOk)
		{
			FString Back;
			FString Why;
			const ESvStoreRead R = Store(A).Get(DeviceKey(A), Back, Why);
			if (R == ESvStoreRead::Error)
			{
				bOk = false;
				A.RaiseStorageFailed(GuestItem, ESvStorageOperation::Read, Why);
			}
			else if (R != ESvStoreRead::Found || !Back.Equals(Json, ESearchCase::CaseSensitive))
			{
				bOk = false;
				A.RaiseStorageFailed(GuestItem, ESvStorageOperation::Write, TEXT("The saved guest secret does not read back."));
			}
		}
		St.bDeviceUnsaved = !bOk;
		if (!bOk)
		{
			A.Owner.Log(ELogVerbosity::Warning,
				TEXT("The guest secret could not be saved: it is kept in memory, and the guest is lost when the game quits unless the player adds an email."));
		}
		return bOk;
	}

	/** A guest secret stored without its player gets the player (best effort). */
	static void RecordDeviceOwner(FSvAuth& A, const FString& PlayerId)
	{
		FSvAuthState& St = S(A);
		if (!St.bDeviceLoaded || !St.Device.IsSet() || !St.Device->PlayerId.IsEmpty() || PlayerId.IsEmpty() || St.bDeviceUnsaved) return;
		St.Device->PlayerId = PlayerId;
		// A failed write leaves the stored record without its player: retried by RetrySavingSecrets.
		if (!WriteKey(A, DeviceKey(A), GuestItem, DeviceJson(St.Device.GetValue()))) St.bDeviceUnsaved = true;
	}

	static bool ForgetGuest(FSvAuth& A)
	{
		FSvAuthState& St = S(A);
		St.Device.Reset();
		St.bDeviceLoaded = true;
		St.bDeviceRejected = false;
		St.bDeviceUnsaved = false;
		if (!DeleteKey(A, DeviceKey(A), GuestItem))
		{
			St.bDeviceLoaded = false; // still stored: read it again next time
			return false;
		}
		return true;
	}

	/** Forgets this device's guest secret only when it belongs to PlayerId. */
	static void ForgetDeviceIfOwnedBy(FSvAuth& A, const FString& PlayerId)
	{
		if (PlayerId.IsEmpty()) return;
		TOptional<FDeviceRecord> Record;
		FSvError Err;
		if (!GetDevice(A, Record, Err)) return; // unreadable: it may be another player's; keep it
		if (!Record.IsSet() || Record->PlayerId.IsEmpty() || !SvSame(Record->PlayerId, PlayerId)) return;
		ForgetGuest(A);
	}

	/** The last sign-in record. When it cannot be read: with bErrorIfUnreadable false + storage_unreadable, else unset. */
	static bool ReadLastSignIn(FSvAuth& A, bool bErrorIfUnreadable, TOptional<FLastSignIn>& Out, FSvError& OutError)
	{
		Out.Reset();
		FString Value;
		FString Why;
		const ESvStoreRead R = Store(A).Get(LastKey(A), Value, Why);
		if (R == ESvStoreRead::Error)
		{
			A.RaiseStorageFailed(LastItem, ESvStorageOperation::Read, Why);
			if (bErrorIfUnreadable)
			{
				OutError = FSvAuth::Unreadable(LastItem);
				return false;
			}
			return true;
		}
		if (R == ESvStoreRead::NotFound || Value.IsEmpty()) return true;
		const TSharedPtr<FJsonObject> O = SvJson::ParseObject(Value);
		if (!O.IsValid())
		{
			A.RaiseStorageFailed(LastItem, ESvStorageOperation::Read, TEXT("The last sign-in is not in a known format."));
			if (bErrorIfUnreadable)
			{
				OutError = FSvAuth::Unreadable(LastItem);
				return false;
			}
			return true;
		}
		FLastSignIn L;
		L.Method = SvJson::GetString(O, TEXT("method"));
		L.PlayerId = SvJson::GetString(O, TEXT("player"));
		Out = L;
		return true;
	}

	/** Records how the player signed in (empty: signed out), for Restore. */
	static bool WriteLastSignIn(FSvAuth& A, const FString& Method, const FString& PlayerId)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		if (Method.IsEmpty()) O->SetField(TEXT("method"), MakeShared<FJsonValueNull>());
		else O->SetStringField(TEXT("method"), Method);
		if (PlayerId.IsEmpty()) O->SetField(TEXT("player"), MakeShared<FJsonValueNull>());
		else O->SetStringField(TEXT("player"), PlayerId);
		return WriteKey(A, LastKey(A), LastItem, SvJson::Write(O));
	}

	/**
	 * The player no longer exists: forget what this device kept for them, and only for them. bStillSignedIn: that
	 * player is still the one signed in, so the sign-in records go too; otherwise (someone else signed in meanwhile)
	 * only records that name the erased player are touched.
	 */
	static void ForgetErasedPlayer(FSvAuth& A, const FString& PlayerId, bool bStillSignedIn)
	{
		TOptional<FRefreshRecord> Refresh;
		FSvError Err;
		if (bStillSignedIn) DeleteRefreshToken(A);
		else if (GetRefreshRecord(A, Refresh, Err) && Refresh.IsSet() && !PlayerId.IsEmpty() && SvSame(Refresh->PlayerId, PlayerId)) DeleteRefreshToken(A);
		ForgetDeviceIfOwnedBy(A, PlayerId);
		if (!PlayerId.IsEmpty()) A.Owner.Store().ForgetPendingForPlayer(PlayerId);
		TOptional<FLastSignIn> Last;
		if (bStillSignedIn || (ReadLastSignIn(A, false, Last, Err) && Last.IsSet() && !PlayerId.IsEmpty() && SvSame(Last->PlayerId, PlayerId)))
		{
			WriteLastSignIn(A, FString(), FString());
		}
	}

	/**
	 * A game-initiated sign-in starts a new epoch BEFORE it sends anything (renewals and restores still in flight are
	 * not used), and a new sign-in generation (an earlier game sign-in still in flight is not used either).
	 */
	static uint64 BeginSignIn(FSvAuth& A)
	{
		++*S(A).Epoch;
		return ++S(A).SignInGeneration;
	}

	static bool StillHolds(FSvAuth& A, const FSentUnder& Under)
	{
		return (!Under.Epoch.IsSet() || *S(A).Epoch == Under.Epoch.GetValue()) && (!Under.SignIn.IsSet() || S(A).SignInGeneration == Under.SignIn.GetValue());
	}

	/**
	 * A refresh token answered for PlayerId by an account change (password, email): saved only while that player is
	 * still the one signed in; otherwise nobody holds it, and it is signed out on the server.
	 */
	static void KeepAccountChangeToken(FSvAuth& A, const FString& RefreshToken, const FString& PlayerId)
	{
		if (RefreshToken.IsEmpty()) return;
		const FSessionPtr Sn = S(A).Session;
		if (Sn.IsValid() && !PlayerId.IsEmpty() && SvSame(Sn->Player.Id, PlayerId)) SaveRefreshToken(A, RefreshToken, PlayerId);
		else SignOutQuietly(A, RefreshToken);
	}

	/** The current session's token when it is a usable one other than UsedToken, else empty. */
	static FString CurrentFreshToken(FSvAuth& A, const FString& UsedToken)
	{
		const FSessionPtr Cur = S(A).Session;
		if (Cur.IsValid() && !SvSame(Cur->Token, UsedToken) && !SvSame(Cur->Token, S(A).DeadToken) && !IsExpired(A, Cur)) return Cur->Token;
		return FString();
	}

	// ─── sign-in plumbing ────────────────────────────────────────────────────────────────────────────────────────

	/**
	 * Takes a sign-in answer: the refresh token and the device secret are saved BEFORE the new player token is used;
	 * a sign-in without a refresh token ends the stored email sign-in (another way in replaced it). The token's expiry
	 * is counted on this device's clock from Started.
	 */
	static TSvResult<FSvSignInResult> Accept(FSvAuth& A, const FSignInAnswer& Answer, const FString& Method, bool bFromRefresh, const FStamp& Started)
	{
		FSvAuthState& St = S(A);
		bool bSaved = true;
		if (!Answer.RefreshToken.IsEmpty()) bSaved &= SaveRefreshToken(A, Answer.RefreshToken, Answer.Player.Id);
		else if (!bFromRefresh) DropRefreshToken(A, true);
		if (!Answer.DeviceSecret.IsEmpty()) bSaved &= SaveDevice(A, Answer.DeviceSecret, Answer.Player.Id);
		else if (SvSame(Method, SvProviders::Device)) RecordDeviceOwner(A, Answer.Player.Id);
		if (SvSame(Method, SvProviders::Device)) St.bDeviceRejected = false;

		const double Lifetime = FSvAuth::TokenLifetimeSeconds(Answer.Token, Answer.ExpiresAt);
		const double Valid = Lifetime - ClockMargin;
		// Renew ahead of expiry, but never more often than every half lifetime.
		const double Lead = FMath::Min(St.RenewLead, Lifetime / 2);
		FSessionPtr Sn = MakeShared<FSvSession>();
		Sn->Token = Answer.Token;
		Sn->ExpiresAt = Answer.ExpiresAt;
		Sn->Expiry = Started.Wall + FTimespan::FromSeconds(Valid);
		Sn->RenewAt = Started.Wall + FTimespan::FromSeconds(Valid - Lead);
		Sn->MonoExpiry = Started.Mono + Valid;
		Sn->MonoRenewAt = Started.Mono + Valid - Lead;
		Sn->Method = Method;
		Sn->Player = Answer.Player;
		Sn->Identities = Answer.Identities;
		SetSession(A, Sn);
		WriteLastSignIn(A, Method, Answer.Player.Id);
		const FSvSignInResult Result = ToResult(Sn, IsRenewableNow(A, Sn), bSaved);
		A.OnSignedIn.Broadcast(Result);
		return TSvResult<FSvSignInResult>::Ok(Result);
	}

	static FSvPlayerProfile Profile(FSvAuth& A, const FProfileAnswer& Answer)
	{
		FSessionPtr Sn = S(A).Session;
		if (Sn.IsValid() && Answer.bHasPlayer && (Sn->Player.Id.IsEmpty() || SvSame(Sn->Player.Id, Answer.Profile.Player.Id)))
		{
			Sn->Identities = Answer.Profile.Identities;
			Sn->Player.Id = Answer.Profile.Player.Id;
			Sn->Player.DisplayName = Answer.Profile.Player.DisplayName;
			if (!Answer.Profile.Player.CreatedAt.IsEmpty()) Sn->Player.CreatedAt = Answer.Profile.Player.CreatedAt;
		}
		return Answer.Profile;
	}

	/** Sends a sign-in (no credential, never repeated) and accepts its answer. */
	/**
	 * Sends a sign-in (no credential, never repeated) and accepts its answer, unless the sign-in changed meanwhile in a
	 * way that makes it stale (Under): then its refresh token is signed out on the server and the answer is dropped.
	 */
	static void SignInCall(FSvAuth& A, const FString& Path, const TSharedRef<FJsonObject>& Body, const FString& Method, TSvCallback<FSvSignInResult> Done,
		const FSentUnder& Under = FSentUnder())
	{
		const FStamp Started = Now(A);
		FSvAuth* Self = &A;
		P(A).Send(FSvCall::Post(Path, SvJson::Write(Body), ESvRetry::Never), FString(), [Self, Method, Started, Under, Done](const TSvResult<FSvHttpResponse>& R)
		{
			if (R.IsError())
			{
				if (SvSame(Method, SvProviders::Device) && R.GetError().Is(TEXT("proof_invalid")) && StillHolds(*Self, Under))
				{
					// No guest has this secret any more. Never sent again by the SDK; the secret stays stored, and the game
					// decides (ForgetGuestOnDevice / CreateGuest(bReplaceExistingGuest)).
					S(*Self).bDeviceRejected = true;
				}
				Done(TSvResult<FSvSignInResult>::Fail(R.GetError()));
				return;
			}
			FSignInAnswer Answer;
			FSvError Err;
			if (!ParseSignIn(R.GetValue(), Answer, Err))
			{
				Done(TSvResult<FSvSignInResult>::Fail(Err));
				return;
			}
			if (!StillHolds(*Self, Under))
			{
				if (!Answer.RefreshToken.IsEmpty()) SignOutQuietly(*Self, Answer.RefreshToken);
				Done(TSvResult<FSvSignInResult>::Fail(SignInChangedError()));
				return;
			}
			Done(Accept(*Self, Answer, Method, false, Started));
		});
	}

	static TSvFuture<FSvSignInResult> Exchange(FSvAuth& A, const FString& Provider, const TSharedRef<FJsonObject>& Proof, const FString& AppId)
	{
		TSharedRef<FJsonObject> Body = GameBody(A);
		if (!AppId.IsEmpty()) Body->SetStringField(TEXT("appId"), AppId);
		for (const auto& F : Proof->Values) Body->SetField(F.Key, F.Value);
		FSvAuth* Self = &A;
		const uint64 Started = BeginSignIn(A);
		return SvAsync<FSvSignInResult>([Self, Provider, Body, Started](TSvCallback<FSvSignInResult> Done)
		{
			SignInCall(*Self, TEXT("/player-auth/") + Provider, Body, Provider, Done, FSentUnder::GameSignIn(Started));
		});
	}

	static TSvFuture<FSvPlayerProfile> Link(FSvAuth& A, const FString& Provider, const TSharedRef<FJsonObject>& Proof, const FString& AppId)
	{
		if (!AppId.IsEmpty()) Proof->SetStringField(TEXT("appId"), AppId);
		FSvAuth* Self = &A;
		const FString Body = SvJson::Write(Proof);
		return SvAsync<FSvPlayerProfile>([Self, Provider, Body](TSvCallback<FSvPlayerProfile> Done)
		{
			Self->SendAsPlayer(FSvCall::Post(TEXT("/player-auth/link/") + Esc(Provider), Body, ESvRetry::Never), [Self, Done](const TSvResult<FSvHttpResponse>& R)
			{
				if (R.IsError()) return Done(TSvResult<FSvPlayerProfile>::Fail(R.GetError()));
				FProfileAnswer Answer;
				FSvError Err;
				if (!ParseProfile(R.GetValue(), Answer, Err)) return Done(TSvResult<FSvPlayerProfile>::Fail(Err));
				Done(TSvResult<FSvPlayerProfile>::Ok(Profile(*Self, Answer)));
			});
		});
	}

	static TSvFuture<FSvPlayerProfile> LinkEmail(FSvAuth& A, const TSharedRef<FJsonObject>& Body)
	{
		FSvAuth* Self = &A;
		const FString Json = SvJson::Write(Body);
		const FString PlayerId = A.GetPlayerId();
		return SvAsync<FSvPlayerProfile>([Self, Json, PlayerId](TSvCallback<FSvPlayerProfile> Done)
		{
			Self->SendAsPlayer(FSvCall::Post(TEXT("/player-auth/link/email"), Json, ESvRetry::Never), [Self, PlayerId, Done](const TSvResult<FSvHttpResponse>& R)
			{
				if (R.IsError()) return Done(TSvResult<FSvPlayerProfile>::Fail(R.GetError()));
				FProfileAnswer Answer;
				FSvError Err;
				if (!ParseProfile(R.GetValue(), Answer, Err)) return Done(TSvResult<FSvPlayerProfile>::Fail(Err));
				// A changed email signs out every other device and answers this device's new refresh token.
				KeepAccountChangeToken(*Self, Answer.RefreshToken, PlayerId);
				Done(TSvResult<FSvPlayerProfile>::Ok(Profile(*Self, Answer)));
			});
		});
	}

	static void CreateGuestCore(FSvAuth& A, const FString& DisplayName, TSvCallback<FSvSignInResult> Done, const FSentUnder& Under = FSentUnder())
	{
		TSharedRef<FJsonObject> Body = GameBody(A);
		if (!DisplayName.IsEmpty()) Body->SetStringField(TEXT("displayName"), DisplayName);
		SignInCall(A, TEXT("/player-auth/device/new"), Body, SvProviders::Device, Done, Under);
	}

	static void DeviceSignInCore(FSvAuth& A, const FDeviceRecord& Device, TSvCallback<FSvSignInResult> Done, const FSentUnder& Under = FSentUnder())
	{
		TSharedRef<FJsonObject> Body = GameBody(A);
		Body->SetStringField(TEXT("deviceSecret"), Device.Secret);
		SignInCall(A, TEXT("/player-auth/device"), Body, SvProviders::Device, Done, Under);
	}

	static void RefreshCore(FSvAuth& A, const FString& RefreshToken, const FString& OwnerId, uint64 SentEpoch, TSvCallback<FSvVoid> Done)
	{
		TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("refreshToken"), RefreshToken);
		FSvCall Call = FSvCall::Post(TEXT("/player-auth/refresh"), SvJson::Write(Body), ESvRetry::Safe);
		// The token is spent when the API reads it, and a lost answer may be resent with the same token only within
		// 30 seconds: short sends and no new send after 25 s keep every resend inside that grace.
		Call.RetryWindowSeconds = 25.0;
		Call.TimeoutSeconds = S(A).RefreshTimeout;
		{
			// The answer may arrive after the client was shut down (the game instance ended): the rotated refresh token
			// is the only one that still works, so it is kept, unless the sign-in changed before the shutdown.
			TSharedRef<ISvSecureStore> Store = A.Owner.GetSecureStoreRef();
			TSharedRef<uint64> Epoch = S(A).Epoch;
			const FString Key = RefreshKey(A);
			Call.OnAnswerAfterShutdown = [Store, Epoch, SentEpoch, Key, OwnerId](const FSvHttpResponse& Response)
			{
				const TSharedPtr<FJsonObject> J = SvJson::ParseObject(Response.Body);
				const FString Rotated = SvJson::GetString(J, TEXT("refreshToken"));
				if (Rotated.IsEmpty() || *Epoch != SentEpoch || OwnerId.IsEmpty()) return;
				FRefreshRecord Record;
				Record.Token = Rotated;
				Record.PlayerId = OwnerId;
				FString Why;
				Store->Set(Key, RefreshJson(Record), Why);
			};
		}
		const FStamp Started = Now(A);
		FSvAuth* Self = &A;
		P(A).Send(Call, FString(), [Self, Started, SentEpoch, Done](const TSvResult<FSvHttpResponse>& R)
		{
			if (R.IsError()) return Done(TSvResult<FSvVoid>::Fail(R.GetError()));
			FSignInAnswer Answer;
			FSvError Err;
			if (!ParseSignIn(R.GetValue(), Answer, Err)) return Done(TSvResult<FSvVoid>::Fail(Err));
			if (*S(*Self).Epoch != SentEpoch)
			{
				// Signed out, erased, or another sign-in meanwhile: never resurrect or replace it.
				if (!Answer.RefreshToken.IsEmpty()) SignOutQuietly(*Self, Answer.RefreshToken);
				return Done(TSvResult<FSvVoid>::Fail(SignInChangedError()));
			}
			const TSvResult<FSvSignInResult> Accepted = Accept(*Self, Answer, SvProviders::Email, true, Started);
			if (Accepted.IsError()) return Done(TSvResult<FSvVoid>::Fail(Accepted.GetError()));
			Done(TSvResult<FSvVoid>::Ok(FSvVoid()));
		});
	}

	static void MarkEnded(FSvAuth& A, const FSessionPtr& Sn, ESvSignInRequiredReason Reason, const FSvError& E)
	{
		FSvAuthState& St = S(A);
		St.LastRenewReason = Reason;
		St.LastRenewCode = E.Code;
		St.LastRenewError = E;
		if (!Sn.IsValid()) return;
		Sn->EndReason = Reason;
		Sn->EndCode = E.Code;
	}

	/**
	 * Renews without the player: the refresh token, else (a guest session, Auto) the device secret of that same guest.
	 * The new token, or empty when neither works. Network failures, other refusals and unreadable storage are errors
	 * (nothing stored is dropped).
	 */
	static void RenewQuietCore(FSvAuth& A, const FString& UsedToken, ERenewMode Mode, TSvCallback<FString> Done)
	{
		FSvAuthState& St = S(A);
		const FSessionPtr Cur = St.Session;
		if (Mode != ERenewMode::ForceRefresh && Cur.IsValid() && !SvSame(Cur->Token, UsedToken) && !SvSame(Cur->Token, St.DeadToken) && !IsExpired(A, Cur))
		{
			return Done(TSvResult<FString>::Ok(Cur->Token));
		}
		St.LastRenewReason = Cur.IsValid() && Cur->EndReason.IsSet() ? Cur->EndReason.GetValue() : ESvSignInRequiredReason::TokenExpired;
		St.LastRenewCode = Cur.IsValid() && !Cur->EndCode.IsEmpty() ? Cur->EndCode : FString(TokenExpiredCode);
		St.LastRenewError.Reset();
		TryRefresh(A, Cur, Mode, 0, FString(), *St.Epoch, Done);
	}

	/**
	 * Only this player's own refresh token. A refresh_invalid may race a newer token saved meanwhile (a changed
	 * password, a changed email): that one is tried once.
	 */
	static void TryRefresh(FSvAuth& A, const FSessionPtr& Cur, ERenewMode Mode, int32 Attempt, const FString& Sent, uint64 Started, TSvCallback<FString> Done)
	{
		// Every attempt belongs to the sign-in the renewal started under: after a sign-out or another sign-in, nothing
		// stored is read, spent or deleted on its behalf.
		if (*S(A).Epoch != Started) return Done(TSvResult<FString>::Fail(SignInChangedError()));
		// Read the stored token again right before refreshing: another client on the same store (a second game process,
		// another Play In Editor window) may have rotated it; sending the spent one would end the whole sign-in.
		if (!S(A).bRefreshUnsaved) S(A).bRefreshLoaded = false;
		FString Refresh;
		FSvError Err;
		if (!UsableRefreshToken(A, Cur, nullptr, Refresh, Err)) return Done(TSvResult<FString>::Fail(Err));
		if (Attempt >= 2 || Refresh.IsEmpty() || SvSame(Refresh, Sent)) return DeviceRenew(A, Cur, Mode, Started, Done);
		const FString OwnerId = S(A).Refresh.IsSet() ? S(A).Refresh->PlayerId : FString();
		FSvAuth* Self = &A;
		RefreshCore(A, Refresh, OwnerId, Started, [Self, Cur, Mode, Attempt, Refresh, Started, Done](const TSvResult<FSvVoid>& R)
		{
			if (R.IsOk())
			{
				const FSessionPtr Latest = S(*Self).Session;
				return Done(TSvResult<FString>::Ok(Latest.IsValid() ? Latest->Token : FString()));
			}
			// Answered after a sign-out or another sign-in: never clean up (or retry) for the sign-in that ended.
			if (*S(*Self).Epoch != Started) return Done(TSvResult<FString>::Fail(SignInChangedError()));
			if (R.GetError().Is(TEXT("refresh_invalid")))
			{
				DeleteRefreshTokenIf(*Self, Refresh);
				MarkEnded(*Self, Cur, ESvSignInRequiredReason::RefreshInvalid, R.GetError());
				return TryRefresh(*Self, Cur, Mode, Attempt + 1, Refresh, Started, Done);
			}
			Done(TSvResult<FString>::Fail(R.GetError()));
		});
	}

	static void DeviceRenew(FSvAuth& A, const FSessionPtr& Cur, ERenewMode Mode, uint64 Started, TSvCallback<FString> Done)
	{
		FSvAuthState& St = S(A);
		if (*St.Epoch != Started) return Done(TSvResult<FString>::Fail(SignInChangedError()));
		if (Mode != ERenewMode::Auto) return Done(TSvResult<FString>::Ok(FString()));
		// Only a signed-in guest session is renewed with the device secret: never a sign-in that already ended.
		if (!Cur.IsValid() || !SvSame(Cur->Method, SvProviders::Device) || St.bDeviceRejected) return Done(TSvResult<FString>::Ok(FString()));
		TOptional<FDeviceRecord> Device;
		FSvError Err;
		if (!GetDevice(A, Device, Err)) return Done(TSvResult<FString>::Fail(Err));
		if (!Device.IsSet()) return Done(TSvResult<FString>::Ok(FString()));
		// Only that guest's own secret renews a guest session: never another player's.
		const FString PlayerId = Cur.IsValid() ? Cur->Player.Id : FString();
		if (!PlayerId.IsEmpty() && !Device->PlayerId.IsEmpty() && !SvSame(Device->PlayerId, PlayerId)) return Done(TSvResult<FString>::Ok(FString()));
		FSvAuth* Self = &A;
		DeviceSignInCore(A, Device.GetValue(), [Self, Cur, Started, Done](const TSvResult<FSvSignInResult>& R)
		{
			if (R.IsOk())
			{
				const FSessionPtr Latest = S(*Self).Session;
				return Done(TSvResult<FString>::Ok(Latest.IsValid() ? Latest->Token : FString()));
			}
			if (*S(*Self).Epoch != Started) return Done(TSvResult<FString>::Fail(SignInChangedError()));
			if (R.GetError().Is(TEXT("proof_invalid")))
			{
				MarkEnded(*Self, Cur, ESvSignInRequiredReason::DeviceSecretRejected, R.GetError());
				return Done(TSvResult<FString>::Ok(FString()));
			}
			Done(TSvResult<FString>::Fail(R.GetError()));
		}, FSentUnder::Renewal(Started));
	}

	/** Tells the game (once per ended sign-in) and awaits Reauthenticate. */
	static void AskGameCore(FSvAuth& A, const FString& UsedToken, TSvCallback<FString> Done)
	{
		FSvAuthState& St = S(A);
		const FSessionPtr Cur = St.Session;
		if (Cur.IsValid() && !SvSame(Cur->Token, UsedToken) && !SvSame(Cur->Token, St.DeadToken) && !IsExpired(A, Cur)) return Done(TSvResult<FString>::Ok(Cur->Token));
		if (!UsedToken.IsEmpty() && SvSame(UsedToken, St.DeadToken)) return Done(TSvResult<FString>::Ok(FString()));
		St.DeadToken = UsedToken;
		const ESvSignInRequiredReason Reason = Cur.IsValid() && Cur->EndReason.IsSet() ? Cur->EndReason.GetValue() : St.LastRenewReason;
		FString Code = Cur.IsValid() && !Cur->EndCode.IsEmpty() ? Cur->EndCode : St.LastRenewCode;
		if (Code.IsEmpty()) Code = TokenExpiredCode;
		const FSvSignInRequired Info = Required(Reason, Cur.IsValid() ? Cur->Method : St.LastMethod, Code);
		RaiseSignInRequired(A, Info);
		if (!St.Reauthenticate) return Done(TSvResult<FString>::Ok(FString()));
		FSvAuth* Self = &A;
		TSharedRef<bool> Alive = P(A).GetAliveFlag();
		St.Reauthenticate(Info).Next([Self, Alive, UsedToken, Done](bool bOk)
		{
			if (!*Alive) return;
			const FSessionPtr After = S(*Self).Session;
			Done(TSvResult<FString>::Ok(bOk && After.IsValid() && !SvSame(After->Token, UsedToken) ? After->Token : FString()));
		});
	}

	/** A 401 on a player route: the sign-in codes end the session; other codes are the route's own. */
	static void OnTokenRefused(FSvAuth& A, const FString& Token, const FSvError& E)
	{
		const FSessionPtr Cur = S(A).Session;
		if (!Cur.IsValid() || !SvSame(Cur->Token, Token)) return;
		if (E.Is(TEXT("token_invalid")))
		{
			ClearSession(A, true);
			RaiseSignInRequired(A, Required(ESvSignInRequiredReason::TokenInvalid, Cur->Method, E.Code));
		}
		else if (E.Is(TEXT("player_deleted")))
		{
			ClearSession(A, true);
			ForgetErasedPlayer(A, Cur->Player.Id, true);
			RaiseSignInRequired(A, Required(ESvSignInRequiredReason::PlayerDeleted, Cur->Method, E.Code));
		}
	}

	static void TokenForRequest(FSvAuth& A, TSvCallback<FString> Done)
	{
		FSvAuthState& St = S(A);
		const FSessionPtr Sn = St.Session;
		if (!Sn.IsValid()) return Done(TSvResult<FString>::Fail(FSvError(0, SvErrorCodes::NotSignedIn, FString())));
		// Already refused by the API and not renewable: nothing is sent (status 0), so a dead sign-in never spends the
		// player's requests.
		if (SvSame(Sn->Token, St.DeadToken)) return Done(TSvResult<FString>::Fail(FSvError(0, TokenExpiredCode, TEXT("Token expired. Sign in again."))));
		if (IsDue(A, Sn->RenewAt, Sn->MonoRenewAt) && !Sn->EndReason.IsSet() && IsRenewableQuiet(A, Sn))
		{
			FSvAuth* Self = &A;
			const FString Used = Sn->Token;
			St.QuietRenew.Run([Self, Used](TSvCallback<FString> Cb) { RenewQuietCore(*Self, Used, ERenewMode::Auto, Cb); },
				[Self, Sn, Done](const TSvResult<FString>& R)
				{
					if (R.IsOk() && !R.GetValue().IsEmpty()) return Done(R);
					if (R.IsError() && R.GetError().Is(SvErrorCodes::SignInChanged))
					{
						// Signed out or signed in again meanwhile: the request uses the session there is now (sent only for
						// its own player: SendAsPlayer checks), never a renewal of the old one.
						const FSessionPtr Current = S(*Self).Session;
						if (!Current.IsValid()) return Done(TSvResult<FString>::Fail(FSvError(0, SvErrorCodes::NotSignedIn, FString())));
						return Done(TSvResult<FString>::Ok(Current->Token));
					}
					if (R.IsError() && !((R.GetError().IsTransient() || R.GetError().Is(SvErrorCodes::StorageUnreadable)) && !IsExpired(*Self, Sn)))
					{
						return Done(R);
					}
					// Not renewed this time; the current token still works for a little while.
					const FSessionPtr Latest = S(*Self).Session;
					if (!Latest.IsValid()) return Done(TSvResult<FString>::Fail(FSvError(0, SvErrorCodes::NotSignedIn, FString())));
					Done(TSvResult<FString>::Ok(Latest->Token));
				});
			return;
		}
		Done(TSvResult<FString>::Ok(Sn->Token));
	}

	/**
	 * A 401 token_expired for a request sent under SentEpoch. If the sign-in changed since (a renewal, a sign-out,
	 * another sign-in), the current session's token is used when there is one, and nothing is renewed: a late 401 from
	 * before a sign-out never signs anyone in again, and never asks the game to.
	 */
	static void RenewAfterExpiry(FSvAuth& A, const FString& UsedToken, uint64 SentEpoch, TSvCallback<FString> Done)
	{
		if (*S(A).Epoch != SentEpoch) return Done(TSvResult<FString>::Ok(CurrentFreshToken(A, UsedToken)));
		FSvAuth* Self = &A;
		S(A).QuietRenew.Run([Self, UsedToken](TSvCallback<FString> Cb) { RenewQuietCore(*Self, UsedToken, ERenewMode::Auto, Cb); },
			[Self, UsedToken, SentEpoch, Done](const TSvResult<FString>& R)
			{
				if (R.IsOk() && !R.GetValue().IsEmpty()) return Done(R);
				if (R.IsError() && !R.GetError().Is(SvErrorCodes::SignInChanged)) return Done(R);
				if (*S(*Self).Epoch != SentEpoch) return Done(TSvResult<FString>::Ok(CurrentFreshToken(*Self, UsedToken)));
				S(*Self).AskGame.Run([Self, UsedToken](TSvCallback<FString> Cb) { AskGameCore(*Self, UsedToken, Cb); }, Done);
			});
	}

	static TSharedRef<FJsonObject> MetaProof(const FString& UserId, const FString& Nonce, FSvError& OutError)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		if (!IsDigits(UserId, 20)) OutError = Invalid(TEXT("UserId must be the Meta user id: 1 to 20 digits."));
		else if (Nonce.IsEmpty()) OutError = Invalid(TEXT("Nonce is required (ovr_User_GetUserProof)."));
		O->SetStringField(TEXT("userId"), UserId);
		O->SetStringField(TEXT("nonce"), Nonce);
		return O;
	}

	static TSharedRef<FJsonObject> SteamProof(const FString& TicketHex, FSvError& OutError)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		if (!IsHex(TicketHex)) OutError = Invalid(TEXT("TicketHex must be the Steam ticket as hex."));
		O->SetStringField(TEXT("ticket"), TicketHex);
		return O;
	}

	static TSharedRef<FJsonObject> One(const TCHAR* Field, const FString& Value)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(Field, Value);
		return O;
	}
};

// ─── FSvAuth ──────────────────────────────────────────────────────────────────────────────────────────────────────

using Ops = FSvAuthOps;

FSvAuth::FSvAuth(FSplatterVaultClient& InOwner)
	: Owner(InOwner)
	, State(MakeUnique<FSvAuthState>())
{
}

FSvAuth::~FSvAuth() = default;

void FSvAuth::Abandon()
{
	const FSvError Gone(0, SvErrorCodes::Shutdown, TEXT("The SplatterVault client was shut down."));
	State->GuestLock.Clear();
	State->GuestFlight.Abandon(Gone);
	State->AskGame.Abandon(Gone);
	State->QuietRenew.Abandon(Gone);
}

void FSvAuth::Configure(double RenewLeadSeconds, double RefreshTimeoutSeconds)
{
	State->RenewLead = FMath::Max(0.0, RenewLeadSeconds);
	State->RefreshTimeout = RefreshTimeoutSeconds;
}

bool FSvAuth::IsSignedIn() const { return State->Session.IsValid(); }
FString FSvAuth::GetPlayerToken() const { return State->Session.IsValid() ? State->Session->Token : FString(); }
FString FSvAuth::GetPlayerId() const { return State->Session.IsValid() ? State->Session->Player.Id : FString(); }
FSvSignedInPlayer FSvAuth::GetPlayer() const { return State->Session.IsValid() ? State->Session->Player : FSvSignedInPlayer(); }
FString FSvAuth::GetMethod() const { return State->Session.IsValid() ? State->Session->Method : FString(); }
TArray<FSvIdentity> FSvAuth::GetIdentities() const { return State->Session.IsValid() ? State->Session->Identities : TArray<FSvIdentity>(); }
FString FSvAuth::GetTokenExpiresAt() const { return State->Session.IsValid() ? State->Session->ExpiresAt : FString(); }
TOptional<FDateTime> FSvAuth::GetTokenExpiresAtUtc() const { return State->Session.IsValid() ? State->Session->Expiry : TOptional<FDateTime>(); }

void FSvAuth::SetReauthenticate(TFunction<TFuture<bool>(const FSvSignInRequired&)> InReauthenticate)
{
	State->Reauthenticate = MoveTemp(InReauthenticate);
}

FSvError FSvAuth::Unreadable(const FString& Item)
{
	const FString What = Item == GuestItem ? FString(TEXT("guest secret")) : Item == RefreshItem ? FString(TEXT("sign-in")) : Item.Replace(TEXT("_"), TEXT(" "));
	return FSvError(0, SvErrorCodes::StorageUnreadable, TEXT("The ") + What + TEXT(" stored on this device could not be read. Nothing was changed; try again."));
}

void FSvAuth::RaiseStorageFailed(const FString& Item, ESvStorageOperation Operation, const FString& Why)
{
	const TSharedPtr<FSplatterVaultClient> Pinned = SvPin(Owner);
	const TCHAR* Op = Operation == ESvStorageOperation::Read ? TEXT("read") : Operation == ESvStorageOperation::Write ? TEXT("write") : TEXT("delete");
	Owner.Log(ELogVerbosity::Warning, FString::Printf(TEXT("Secure storage: %s of %s failed (%s)."), Op, *Item, *Why));
	FSvStorageFailure F;
	F.Item = Item;
	F.Operation = Operation;
	F.Message = Why;
	OnStorageFailed.Broadcast(F);
}

double FSvAuth::TokenLifetimeSeconds(const FString& Token, const FString& ExpiresAt)
{
	const TSharedPtr<FJsonObject> Claims = ReadClaims(Token);
	const TOptional<FDateTime> Iat = FromUnix(Claims, TEXT("iat"));
	TOptional<FDateTime> End = FromUnix(Claims, TEXT("exp"));
	if (!End.IsSet()) End = ParseIso(ExpiresAt);
	if (Iat.IsSet() && End.IsSet())
	{
		const double Life = (End.GetValue() - Iat.GetValue()).GetTotalSeconds();
		if (Life > 0) return FMath::Clamp(Life, MinLifetime, MaxLifetime);
	}
	return DefaultLifetime;
}

FString FSvAuth::ToHex(const TArray<uint8>& Bytes)
{
	FString Out;
	Out.Reserve(Bytes.Num() * 2);
	for (const uint8 B : Bytes) Out += FString::Printf(TEXT("%02X"), B);
	return Out;
}

namespace
{
	/** A call bound to a player (store changes) is never sent under another player's token. */
	bool WrongPlayer(const FSvCall& Call, const FString& SignedInPlayerId, FSvError& OutError)
	{
		if (Call.ExpectedPlayerId.IsEmpty() || SvSame(Call.ExpectedPlayerId, SignedInPlayerId)) return false;
		OutError = FSvError(0, SvErrorCodes::PlayerChanged,
			TEXT("This change belongs to another player than the signed-in one: nothing was sent. It stays pending for its player."));
		OutError.Attempts = Call.PriorAttempts;
		return true;
	}
}

void FSvAuth::SendAsPlayer(const FSvCall& InCall, TSvCallback<FSvHttpResponse> Done)
{
	const TSharedPtr<FSplatterVaultClient> Pinned = SvPin(Owner);
	FSvAuth* Self = this;
	// Every player request belongs to the player signed in when it starts: if a renewal or a new sign-in puts another
	// player in place before it is sent, it is not sent under that player (player_changed).
	FSvCall Call = InCall;
	if (Call.ExpectedPlayerId.IsEmpty()) Call.ExpectedPlayerId = GetPlayerId();
	Ops::TokenForRequest(*this, [Self, Call, Done](const TSvResult<FString>& T)
	{
		if (T.IsError()) return Done(TSvResult<FSvHttpResponse>::Fail(T.GetError()));
		FSvError Wrong;
		if (WrongPlayer(Call, Self->GetPlayerId(), Wrong)) return Done(TSvResult<FSvHttpResponse>::Fail(Wrong));
		const FString Token = T.GetValue();
		const uint64 SentEpoch = *Self->State->Epoch;
		Ops::P(*Self).Send(Call, Token, [Self, Call, Token, SentEpoch, Done](const TSvResult<FSvHttpResponse>& R)
		{
			if (R.IsOk()) return Done(R);
			const FSvError& E = R.GetError();
			if (E.Status == 401 && E.Is(TokenExpiredCode))
			{
				Ops::RenewAfterExpiry(*Self, Token, SentEpoch, [Self, Call, E, Done](const TSvResult<FString>& Renewed)
				{
					if (Renewed.IsError()) return Done(TSvResult<FSvHttpResponse>::Fail(Renewed.GetError()));
					if (Renewed.GetValue().IsEmpty()) return Done(TSvResult<FSvHttpResponse>::Fail(E));
					FSvError WrongAfter;
					if (WrongPlayer(Call, Self->GetPlayerId(), WrongAfter)) return Done(TSvResult<FSvHttpResponse>::Fail(WrongAfter));
					const FString NewToken = Renewed.GetValue();
					// The 401 proves the first send was not applied; the resend still counts in Attempts.
					FSvCall Resend = Call;
					Resend.PriorAttempts = E.Attempts;
					Ops::P(*Self).Send(Resend, NewToken, [Self, NewToken, Done](const TSvResult<FSvHttpResponse>& Again)
					{
						if (Again.IsError() && Again.GetError().Status == 401) Ops::OnTokenRefused(*Self, NewToken, Again.GetError());
						Done(Again);
					});
				});
				return;
			}
			if (E.Status == 401) Ops::OnTokenRefused(*Self, Token, E);
			Done(R);
		});
	});
}

// ─── restore ──────────────────────────────────────────────────────────────────────────────────────────────────────

TSvFuture<bool> FSvAuth::Restore()
{
	const TSharedPtr<FSplatterVaultClient> Pinned = SvPin(Owner);
	FSvAuth* Self = this;
	return SvAsync<bool>([Self](TSvCallback<bool> Done)
	{
		FSvAuthState& St = *Self->State;
		const Ops::FSessionPtr Sn = St.Session;
		if (Sn.IsValid() && !Ops::IsExpired(*Self, Sn) && !SvSame(Sn->Token, St.DeadToken)) return Done(TSvResult<bool>::Ok(true));
		TOptional<FLastSignIn> Last;
		FSvError Err;
		if (!Ops::ReadLastSignIn(*Self, true, Last, Err)) return Done(TSvResult<bool>::Fail(Err));
		// Nobody signed in here, or they signed out.
		if (!Last.IsSet() || Last->Method.IsEmpty()) return Done(TSvResult<bool>::Ok(false));
		if (SvSame(Last->Method, SvProviders::Email))
		{
			FString Refresh;
			const FLastSignIn LastValue = Last.GetValue();
			if (!Ops::UsableRefreshToken(*Self, Sn, &LastValue, Refresh, Err)) return Done(TSvResult<bool>::Fail(Err));
			if (Refresh.IsEmpty()) return Done(TSvResult<bool>::Ok(false));
			const FString Used = Sn.IsValid() ? Sn->Token : FString();
			St.QuietRenew.Run([Self, Used](TSvCallback<FString> Cb) { Ops::RenewQuietCore(*Self, Used, ERenewMode::RefreshOnly, Cb); },
				[Done](const TSvResult<FString>& R)
				{
					// Another sign-in, a sign-out or an erase while restoring: this restore signed nobody in.
					if (R.IsError() && R.GetError().Is(SvErrorCodes::SignInChanged)) return Done(TSvResult<bool>::Ok(false));
					if (R.IsError()) return Done(TSvResult<bool>::Fail(R.GetError()));
					Done(TSvResult<bool>::Ok(!R.GetValue().IsEmpty()));
				});
			return;
		}
		if (!SvSame(Last->Method, SvProviders::Device)) return Done(TSvResult<bool>::Ok(false));
		const FString LastPlayer = Last->PlayerId;
		const uint64 Epoch = *St.Epoch;
		// Under the guest lock: a new guest made meanwhile (CreateGuest) never races this sign-in.
		St.GuestLock.Run([Self, LastPlayer, Epoch, Done](TFunction<void()> Release)
		{
			auto Finish = [Release, Done](const TSvResult<bool>& R)
			{
				Release();
				Done(R);
			};
			FSvAuthState& St2 = *Self->State;
			if (*St2.Epoch != Epoch) return Finish(TSvResult<bool>::Ok(false));
			TOptional<FDeviceRecord> Device;
			FSvError Err2;
			if (!Ops::GetDevice(*Self, Device, Err2)) return Finish(TSvResult<bool>::Fail(Err2));
			if (!Device.IsSet() || St2.bDeviceRejected) return Finish(TSvResult<bool>::Ok(false));
			if (!Device->PlayerId.IsEmpty() && !LastPlayer.IsEmpty() && !SvSame(Device->PlayerId, LastPlayer)) return Finish(TSvResult<bool>::Ok(false));
			Ops::DeviceSignInCore(*Self, Device.GetValue(), [Finish](const TSvResult<FSvSignInResult>& R)
			{
				if (R.IsOk()) return Finish(TSvResult<bool>::Ok(true));
				if (R.GetError().Is(TEXT("proof_invalid")) || R.GetError().Is(SvErrorCodes::SignInChanged)) return Finish(TSvResult<bool>::Ok(false));
				Finish(TSvResult<bool>::Fail(R.GetError()));
			}, FSentUnder::Renewal(Epoch));
		});
	});
}

TSvResult<FString> FSvAuth::GetLastSignInMethod()
{
	const TSharedPtr<FSplatterVaultClient> Pinned = SvPin(Owner);
	TOptional<FLastSignIn> Last;
	FSvError Err;
	if (!Ops::ReadLastSignIn(*this, true, Last, Err)) return TSvResult<FString>::Fail(Err);
	return TSvResult<FString>::Ok(Last.IsSet() ? Last->Method : FString());
}

bool FSvAuth::RetrySavingSecrets()
{
	const TSharedPtr<FSplatterVaultClient> Pinned = SvPin(Owner);
	bool bOk = true;
	if (State->bRefreshUnsaved && State->Refresh.IsSet())
	{
		const FRefreshRecord R = State->Refresh.GetValue();
		bOk &= Ops::SaveRefreshToken(*this, R.Token, R.PlayerId);
	}
	if (State->bDeviceUnsaved && State->Device.IsSet())
	{
		const FDeviceRecord D = State->Device.GetValue();
		bOk &= Ops::SaveDevice(*this, D.Secret, D.PlayerId);
	}
	return bOk;
}

// ─── platform proofs ──────────────────────────────────────────────────────────────────────────────────────────────

TSvFuture<FSvSignInResult> FSvAuth::SignInWithMeta(const FString& UserId, const FString& Nonce, const FString& AppId)
{
	FSvError Err;
	const TSharedRef<FJsonObject> Proof = Ops::MetaProof(UserId, Nonce, Err);
	if (!Err.Code.IsEmpty()) return SvFailed<FSvSignInResult>(Err);
	return Ops::Exchange(*this, SvProviders::Meta, Proof, AppId);
}

TSvFuture<FSvSignInResult> FSvAuth::SignInWithMeta(uint64 UserId, const FString& Nonce, const FString& AppId)
{
	return SignInWithMeta(FString::Printf(TEXT("%llu"), (unsigned long long)UserId), Nonce, AppId);
}

TSvFuture<FSvSignInResult> FSvAuth::SignInWithSteam(const FString& TicketHex, const FString& AppId)
{
	FSvError Err;
	const TSharedRef<FJsonObject> Proof = Ops::SteamProof(TicketHex, Err);
	if (!Err.Code.IsEmpty()) return SvFailed<FSvSignInResult>(Err);
	return Ops::Exchange(*this, SvProviders::Steam, Proof, AppId);
}

TSvFuture<FSvSignInResult> FSvAuth::SignInWithSteamTicket(const TArray<uint8>& Ticket, const FString& AppId)
{
	return SignInWithSteam(ToHex(Ticket), AppId);
}

TSvFuture<FSvSignInResult> FSvAuth::SignInWithXsolla(const FString& Token, const FString& AppId)
{
	if (Token.IsEmpty()) return SvFailed<FSvSignInResult>(Invalid(TEXT("Token is required.")));
	return Ops::Exchange(*this, SvProviders::Xsolla, Ops::One(TEXT("token"), Token), AppId);
}

TSvFuture<FSvSignInResult> FSvAuth::SignInWithPlayFab(const FString& SessionTicket, const FString& AppId)
{
	if (SessionTicket.IsEmpty()) return SvFailed<FSvSignInResult>(Invalid(TEXT("SessionTicket is required.")));
	return Ops::Exchange(*this, SvProviders::PlayFab, Ops::One(TEXT("sessionTicket"), SessionTicket), AppId);
}

TSvFuture<FSvSignInResult> FSvAuth::SignInWithSupabase(const FString& AccessToken, const FString& AppId)
{
	if (AccessToken.IsEmpty()) return SvFailed<FSvSignInResult>(Invalid(TEXT("AccessToken is required.")));
	return Ops::Exchange(*this, SvProviders::Supabase, Ops::One(TEXT("token"), AccessToken), AppId);
}

TSvFuture<FSvPlayerProfile> FSvAuth::SignInWithStudioToken(const FString& PlayerToken, const FString& ExpiresAt)
{
	if (PlayerToken.IsEmpty()) return SvFailed<FSvPlayerProfile>(Invalid(TEXT("PlayerToken is required.")));
	FSvAuth* Self = this;
	const uint64 Started = Ops::BeginSignIn(*this);
	return SvAsync<FSvPlayerProfile>([Self, PlayerToken, ExpiresAt, Started](TSvCallback<FSvPlayerProfile> Done)
	{
		const TSharedPtr<FJsonObject> Claims = ReadClaims(PlayerToken);
		FString Method = SvProviders::Studio;
		const TArray<TSharedPtr<FJsonValue>>* Amr = nullptr;
		if (Claims.IsValid() && Claims->TryGetArrayField(TEXT("amr"), Amr) && Amr && Amr->Num() > 0 && (*Amr)[0].IsValid() && (*Amr)[0]->Type == EJson::String)
		{
			Method = (*Amr)[0]->AsString();
		}
		const FString Sub = SvJson::GetString(Claims, TEXT("sub"));
		TOptional<FDateTime> ServerExpiry = ParseIso(ExpiresAt);
		if (!ServerExpiry.IsSet()) ServerExpiry = FromUnix(Claims, TEXT("exp"));

		// Checked BEFORE anything stored is touched: a refused token leaves the current sign-in as it was.
		Ops::P(*Self).Send(FSvCall::Get(TEXT("/player-auth/me")), PlayerToken,
			[Self, PlayerToken, ExpiresAt, Method, Sub, ServerExpiry, Started, Done](const TSvResult<FSvHttpResponse>& R)
			{
				if (R.IsError()) return Done(TSvResult<FSvPlayerProfile>::Fail(R.GetError()));
				// Another sign-in or a sign-out started meanwhile: this one is not used.
				if (Self->State->SignInGeneration != Started) return Done(TSvResult<FSvPlayerProfile>::Fail(SignInChangedError()));
				const FStamp Arrived = Ops::Now(*Self);
				FProfileAnswer Answer;
				FSvError Err;
				if (!ParseProfile(R.GetValue(), Answer, Err)) return Done(TSvResult<FSvPlayerProfile>::Fail(Err));

				// The token's age is unknown (your backend issued it), so its expiry is the API's: corrected for this
				// device's clock with the answer's Date when there is one.
				TOptional<FDateTime> LocalExpiry = ServerExpiry;
				FDateTime ServerNow;
				if (ServerExpiry.IsSet() && FDateTime::ParseHttpDate(R.GetValue().GetHeader(TEXT("Date")), ServerNow))
				{
					LocalExpiry = ServerExpiry.GetValue() - (ServerNow - Arrived.Wall);
				}
				TOptional<double> MonoExpiry;
				if (LocalExpiry.IsSet()) MonoExpiry = Arrived.Mono + (LocalExpiry.GetValue() - Arrived.Wall).GetTotalSeconds();

				Ops::DropRefreshToken(*Self, true);
				Ops::FSessionPtr Sn = MakeShared<FSvSession>();
				Sn->Token = PlayerToken;
				Sn->ExpiresAt = !ExpiresAt.IsEmpty() ? ExpiresAt : (ServerExpiry.IsSet() ? ServerExpiry->ToIso8601() : FString());
				Sn->Expiry = LocalExpiry;
				Sn->MonoExpiry = MonoExpiry;
				Sn->Method = Method;
				Sn->Player.Id = Answer.bHasPlayer ? Answer.Profile.Player.Id : Sub;
				Sn->Player.DisplayName = Answer.Profile.Player.DisplayName;
				Sn->Player.CreatedAt = Answer.Profile.Player.CreatedAt;
				Sn->Identities = Answer.Profile.Identities;
				Ops::SetSession(*Self, Sn);
				Ops::WriteLastSignIn(*Self, Method, Sn->Player.Id);
				Self->OnSignedIn.Broadcast(Ops::ToResult(Sn, false, true));
				Done(TSvResult<FSvPlayerProfile>::Ok(Answer.Profile));
			});
	});
}

// ─── email accounts ───────────────────────────────────────────────────────────────────────────────────────────────

TSvFuture<FSvCodeSent> FSvAuth::RequestEmailCode(const FString& Email)
{
	if (Email.IsEmpty()) return SvFailed<FSvCodeSent>(Invalid(TEXT("Email is required.")));
	TSharedRef<FJsonObject> Body = Ops::GameBody(*this);
	Body->SetStringField(TEXT("email"), Email);
	FSvAuth* Self = this;
	const FString Json = SvJson::Write(Body);
	return SvAsync<FSvCodeSent>([Self, Json](TSvCallback<FSvCodeSent> Done)
	{
		Ops::P(*Self).Send(FSvCall::Post(TEXT("/player-auth/email/code"), Json, ESvRetry::Never), FString(), [Done](const TSvResult<FSvHttpResponse>& R)
		{
			if (R.IsError()) return Done(TSvResult<FSvCodeSent>::Fail(R.GetError()));
			FSvCodeSent Sent;
			FSvError Err;
			if (!ParseModel(R.GetValue(), Sent, Err)) return Done(TSvResult<FSvCodeSent>::Fail(Err));
			Done(TSvResult<FSvCodeSent>::Ok(Sent));
		});
	});
}

TSvFuture<FSvSignInResult> FSvAuth::VerifyEmailCode(const FString& Email, const FString& Code, const FString& NewPassword, const FString& DisplayName)
{
	if (Email.IsEmpty() || Code.IsEmpty()) return SvFailed<FSvSignInResult>(Invalid(TEXT("Email and Code are required.")));
	TSharedRef<FJsonObject> Body = Ops::GameBody(*this);
	Body->SetStringField(TEXT("email"), Email);
	Body->SetStringField(TEXT("code"), Code);
	if (!NewPassword.IsEmpty()) Body->SetStringField(TEXT("password"), NewPassword);
	if (!DisplayName.IsEmpty()) Body->SetStringField(TEXT("displayName"), DisplayName);
	FSvAuth* Self = this;
	const uint64 Started = Ops::BeginSignIn(*this);
	return SvAsync<FSvSignInResult>([Self, Body, Started](TSvCallback<FSvSignInResult> Done)
	{
		Ops::SignInCall(*Self, TEXT("/player-auth/email/code/verify"), Body, SvProviders::Email, Done, FSentUnder::GameSignIn(Started));
	});
}

TSvFuture<FSvSignInResult> FSvAuth::SignInWithPassword(const FString& Email, const FString& Password)
{
	if (Email.IsEmpty() || Password.IsEmpty()) return SvFailed<FSvSignInResult>(Invalid(TEXT("Email and Password are required.")));
	TSharedRef<FJsonObject> Body = Ops::GameBody(*this);
	Body->SetStringField(TEXT("email"), Email);
	Body->SetStringField(TEXT("password"), Password);
	FSvAuth* Self = this;
	const uint64 Started = Ops::BeginSignIn(*this);
	return SvAsync<FSvSignInResult>([Self, Body, Started](TSvCallback<FSvSignInResult> Done)
	{
		Ops::SignInCall(*Self, TEXT("/player-auth/email/password"), Body, SvProviders::Email, Done, FSentUnder::GameSignIn(Started));
	});
}

TSvFuture<FSvSignInResult> FSvAuth::Refresh()
{
	const TSharedPtr<FSplatterVaultClient> Pinned = SvPin(Owner);
	FString Token;
	FSvError Err;
	if (!Ops::UsableRefreshToken(*this, State->Session, nullptr, Token, Err)) return SvFailed<FSvSignInResult>(Err);
	if (Token.IsEmpty())
	{
		return SvFailed<FSvSignInResult>(FSvError(0, SvErrorCodes::InvalidState, TEXT("There is no email sign-in of this player to refresh on this device.")));
	}
	FSvAuth* Self = this;
	const FString Used = State->Session.IsValid() ? State->Session->Token : FString();
	return SvAsync<FSvSignInResult>([Self, Used](TSvCallback<FSvSignInResult> Done)
	{
		Self->State->QuietRenew.Run([Self, Used](TSvCallback<FString> Cb) { Ops::RenewQuietCore(*Self, Used, ERenewMode::ForceRefresh, Cb); },
			[Self, Done](const TSvResult<FString>& R)
			{
				if (R.IsError()) return Done(TSvResult<FSvSignInResult>::Fail(R.GetError()));
				if (R.GetValue().IsEmpty())
				{
					const FSvAuthState& St = *Self->State;
					return Done(TSvResult<FSvSignInResult>::Fail(St.LastRenewError.IsSet() ? St.LastRenewError.GetValue()
						: FSvError(401, TEXT("refresh_invalid"), TEXT("This sign-in has ended. Sign in again."))));
				}
				const Ops::FSessionPtr Sn = Self->State->Session;
				Done(TSvResult<FSvSignInResult>::Ok(Ops::ToResult(Sn, Ops::IsRenewableNow(*Self, Sn), !Self->State->bRefreshUnsaved)));
			});
	});
}

TSvFuture<FSvVoid> FSvAuth::SignOut(bool bForgetGuest)
{
	const TSharedPtr<FSplatterVaultClient> Pinned = SvPin(Owner);
	TOptional<FRefreshRecord> Refresh;
	FSvError Err;
	// Unreadable: it is deleted below; there is nothing to end on the server we could name.
	Ops::GetRefreshRecord(*this, Refresh, Err);
	Ops::DeleteRefreshToken(*this);
	if (bForgetGuest) ForgetGuestOnDevice();
	Ops::WriteLastSignIn(*this, FString(), FString());
	Ops::ClearSession(*this, true);
	if (!Refresh.IsSet()) return SvAsync<FSvVoid>([](TSvCallback<FSvVoid> Done) { Done(TSvResult<FSvVoid>::Ok(FSvVoid())); });
	TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("refreshToken"), Refresh->Token);
	FSvAuth* Self = this;
	const FString Json = SvJson::Write(Body);
	return SvAsync<FSvVoid>([Self, Json](TSvCallback<FSvVoid> Done)
	{
		Ops::P(*Self).Send(FSvCall::Post(TEXT("/player-auth/sign-out"), Json, ESvRetry::Never), FString(), [Self, Done](const TSvResult<FSvHttpResponse>& R)
		{
			if (R.IsError())
			{
				Self->Owner.Log(ELogVerbosity::Warning, TEXT("Signed out on this device; the server sign-out failed (") +
					(R.GetError().Code.IsEmpty() ? FString::FromInt(R.GetError().Status) : R.GetError().Code) + TEXT(")."));
			}
			Done(TSvResult<FSvVoid>::Ok(FSvVoid()));
		});
	});
}

// ─── guests ───────────────────────────────────────────────────────────────────────────────────────────────────────

TSvFuture<FSvSignInResult> FSvAuth::SignInAsGuest(const FString& DisplayName)
{
	const TSharedPtr<FSplatterVaultClient> Pinned = SvPin(Owner);
	FSvAuth* Self = this;
	return SvAsync<FSvSignInResult>([Self, DisplayName](TSvCallback<FSvSignInResult> Done)
	{
		// Two callers at once (a first launch with two scripts) share ONE sign-in: never two guests.
		Self->State->GuestFlight.Run([Self, DisplayName](TSvCallback<FSvSignInResult> Cb)
		{
			// The sign-in starts now (callers that join it do not start another).
			const uint64 Started = Ops::BeginSignIn(*Self);
			Self->State->GuestLock.Run([Self, DisplayName, Started, Cb](TFunction<void()> Release)
			{
				auto Finish = [Release, Cb](const TSvResult<FSvSignInResult>& R)
				{
					Release();
					Cb(R);
				};
				if (Self->State->SignInGeneration != Started) return Finish(TSvResult<FSvSignInResult>::Fail(SignInChangedError()));
				TOptional<FDeviceRecord> Device;
				FSvError Err;
				if (!Ops::GetDevice(*Self, Device, Err)) return Finish(TSvResult<FSvSignInResult>::Fail(Err));
				if (Device.IsSet()) return Ops::DeviceSignInCore(*Self, Device.GetValue(), Finish, FSentUnder::GameSignIn(Started));
				Ops::CreateGuestCore(*Self, DisplayName, Finish, FSentUnder::GameSignIn(Started));
			});
		}, Done);
	});
}

TSvFuture<FSvSignInResult> FSvAuth::CreateGuest(const FString& DisplayName, bool bReplaceExistingGuest)
{
	const TSharedPtr<FSplatterVaultClient> Pinned = SvPin(Owner);
	FSvAuth* Self = this;
	const uint64 Started = Ops::BeginSignIn(*this);
	return SvAsync<FSvSignInResult>([Self, DisplayName, bReplaceExistingGuest, Started](TSvCallback<FSvSignInResult> Done)
	{
		Self->State->GuestLock.Run([Self, DisplayName, bReplaceExistingGuest, Started, Done](TFunction<void()> Release)
		{
			auto Finish = [Release, Done](const TSvResult<FSvSignInResult>& R)
			{
				Release();
				Done(R);
			};
			if (Self->State->SignInGeneration != Started) return Finish(TSvResult<FSvSignInResult>::Fail(SignInChangedError()));
			if (!bReplaceExistingGuest)
			{
				TOptional<FDeviceRecord> Device;
				FSvError Err;
				if (!Ops::GetDevice(*Self, Device, Err)) return Finish(TSvResult<FSvSignInResult>::Fail(Err));
				if (Device.IsSet())
				{
					return Finish(TSvResult<FSvSignInResult>::Fail(FSvError(0, SvErrorCodes::InvalidState,
						TEXT("This device already has a guest. Sign in with SignInAsGuest, or pass bReplaceExistingGuest to make a new one."))));
				}
			}
			Ops::CreateGuestCore(*Self, DisplayName, Finish, FSentUnder::GameSignIn(Started));
		});
	});
}

TSvFuture<FSvSignInResult> FSvAuth::SignInWithDevice()
{
	const TSharedPtr<FSplatterVaultClient> Pinned = SvPin(Owner);
	FSvAuth* Self = this;
	const uint64 Started = Ops::BeginSignIn(*this);
	return SvAsync<FSvSignInResult>([Self, Started](TSvCallback<FSvSignInResult> Done)
	{
		Self->State->GuestLock.Run([Self, Started, Done](TFunction<void()> Release)
		{
			auto Finish = [Release, Done](const TSvResult<FSvSignInResult>& R)
			{
				Release();
				Done(R);
			};
			if (Self->State->SignInGeneration != Started) return Finish(TSvResult<FSvSignInResult>::Fail(SignInChangedError()));
			TOptional<FDeviceRecord> Device;
			FSvError Err;
			if (!Ops::GetDevice(*Self, Device, Err)) return Finish(TSvResult<FSvSignInResult>::Fail(Err));
			if (!Device.IsSet()) return Finish(TSvResult<FSvSignInResult>::Fail(FSvError(0, SvErrorCodes::InvalidState, TEXT("There is no guest on this device."))));
			Ops::DeviceSignInCore(*Self, Device.GetValue(), Finish, FSentUnder::GameSignIn(Started));
		});
	});
}

TSvResult<bool> FSvAuth::HasGuestOnDevice()
{
	const TSharedPtr<FSplatterVaultClient> Pinned = SvPin(Owner);
	TOptional<FDeviceRecord> Device;
	FSvError Err;
	if (!Ops::GetDevice(*this, Device, Err)) return TSvResult<bool>::Fail(Err);
	return TSvResult<bool>::Ok(Device.IsSet());
}

bool FSvAuth::ForgetGuestOnDevice()
{
	const TSharedPtr<FSplatterVaultClient> Pinned = SvPin(Owner);
	return Ops::ForgetGuest(*this);
}

// ─── the signed-in player ─────────────────────────────────────────────────────────────────────────────────────────

TSvFuture<FSvPlayerProfile> FSvAuth::GetMe()
{
	FSvAuth* Self = this;
	return SvAsync<FSvPlayerProfile>([Self](TSvCallback<FSvPlayerProfile> Done)
	{
		Self->SendAsPlayer(FSvCall::Get(TEXT("/player-auth/me")), [Self, Done](const TSvResult<FSvHttpResponse>& R)
		{
			if (R.IsError()) return Done(TSvResult<FSvPlayerProfile>::Fail(R.GetError()));
			FProfileAnswer Answer;
			FSvError Err;
			if (!ParseProfile(R.GetValue(), Answer, Err)) return Done(TSvResult<FSvPlayerProfile>::Fail(Err));
			Done(TSvResult<FSvPlayerProfile>::Ok(Ops::Profile(*Self, Answer)));
		});
	});
}

TSvFuture<FSvVoid> FSvAuth::EraseMe()
{
	FSvAuth* Self = this;
	const FString PlayerId = GetPlayerId();
	const uint64 Started = *State->Epoch;
	return SvAsync<FSvVoid>([Self, PlayerId, Started](TSvCallback<FSvVoid> Done)
	{
		FSvCall Call;
		Call.Method = TEXT("DELETE");
		Call.Path = TEXT("/player-auth/me");
		Call.Retry = ESvRetry::Never;
		Self->SendAsPlayer(Call, [Self, PlayerId, Started, Done](const TSvResult<FSvHttpResponse>& R)
		{
			// player_deleted: already erased (an earlier answer was lost): the outcome asked for.
			if (R.IsError() && !R.GetError().Is(TEXT("player_deleted"))) return Done(TSvResult<FSvVoid>::Fail(R.GetError()));
			// Someone else may have signed in while the erase was in flight: then only the erased player's own records
			// go, and the new sign-in stays.
			const Ops::FSessionPtr Current = Self->State->Session;
			const bool bStillSignedIn = *Self->State->Epoch == Started || (Current.IsValid() && SvSame(Current->Player.Id, PlayerId));
			Ops::ForgetErasedPlayer(*Self, PlayerId, bStillSignedIn);
			if (bStillSignedIn) Ops::ClearSession(*Self, true);
			Done(TSvResult<FSvVoid>::Ok(FSvVoid()));
		});
	});
}

TSvFuture<FSvPlayerProfile> FSvAuth::LinkMeta(const FString& UserId, const FString& Nonce, const FString& AppId)
{
	FSvError Err;
	const TSharedRef<FJsonObject> Proof = Ops::MetaProof(UserId, Nonce, Err);
	if (!Err.Code.IsEmpty()) return SvFailed<FSvPlayerProfile>(Err);
	return Ops::Link(*this, SvProviders::Meta, Proof, AppId);
}

TSvFuture<FSvPlayerProfile> FSvAuth::LinkSteam(const FString& TicketHex, const FString& AppId)
{
	FSvError Err;
	const TSharedRef<FJsonObject> Proof = Ops::SteamProof(TicketHex, Err);
	if (!Err.Code.IsEmpty()) return SvFailed<FSvPlayerProfile>(Err);
	return Ops::Link(*this, SvProviders::Steam, Proof, AppId);
}

TSvFuture<FSvPlayerProfile> FSvAuth::LinkSteamTicket(const TArray<uint8>& Ticket, const FString& AppId)
{
	return LinkSteam(ToHex(Ticket), AppId);
}

TSvFuture<FSvPlayerProfile> FSvAuth::LinkXsolla(const FString& Token, const FString& AppId)
{
	if (Token.IsEmpty()) return SvFailed<FSvPlayerProfile>(Invalid(TEXT("Token is required.")));
	return Ops::Link(*this, SvProviders::Xsolla, Ops::One(TEXT("token"), Token), AppId);
}

TSvFuture<FSvPlayerProfile> FSvAuth::LinkPlayFab(const FString& SessionTicket, const FString& AppId)
{
	if (SessionTicket.IsEmpty()) return SvFailed<FSvPlayerProfile>(Invalid(TEXT("SessionTicket is required.")));
	return Ops::Link(*this, SvProviders::PlayFab, Ops::One(TEXT("sessionTicket"), SessionTicket), AppId);
}

TSvFuture<FSvPlayerProfile> FSvAuth::LinkSupabase(const FString& AccessToken, const FString& AppId)
{
	if (AccessToken.IsEmpty()) return SvFailed<FSvPlayerProfile>(Invalid(TEXT("AccessToken is required.")));
	return Ops::Link(*this, SvProviders::Supabase, Ops::One(TEXT("token"), AccessToken), AppId);
}

TSvFuture<FSvPlayerProfile> FSvAuth::Unlink(const FString& Provider)
{
	if (Provider.IsEmpty()) return SvFailed<FSvPlayerProfile>(Invalid(TEXT("Provider is required.")));
	if (SvSame(Provider, SvProviders::Email)) return SvFailed<FSvPlayerProfile>(Invalid(TEXT("Remove the email with RemoveEmail (it needs proof).")));
	if (SvSame(Provider, SvProviders::Studio)) return SvFailed<FSvPlayerProfile>(Invalid(TEXT("Only your backend can remove the studio link.")));
	const TCHAR* Unlinkable[] = {SvProviders::Xsolla, SvProviders::Meta, SvProviders::Steam, SvProviders::PlayFab, SvProviders::Supabase, SvProviders::Device};
	bool bKnown = false;
	for (const TCHAR* P : Unlinkable) bKnown |= SvSame(Provider, P);
	if (!bKnown) return SvFailed<FSvPlayerProfile>(Invalid(TEXT("Provider is one of xsolla, meta, steam, playfab, supabase or device.")));
	FSvAuth* Self = this;
	const FString PlayerId = GetPlayerId();
	return SvAsync<FSvPlayerProfile>([Self, Provider, PlayerId](TSvCallback<FSvPlayerProfile> Done)
	{
		FSvCall Call;
		Call.Method = TEXT("DELETE");
		Call.Path = TEXT("/player-auth/link/") + Esc(Provider);
		Call.Retry = ESvRetry::Never;
		Self->SendAsPlayer(Call, [Self, Provider, PlayerId, Done](const TSvResult<FSvHttpResponse>& R)
		{
			if (R.IsError()) return Done(TSvResult<FSvPlayerProfile>::Fail(R.GetError()));
			FProfileAnswer Answer;
			FSvError Err;
			if (!ParseProfile(R.GetValue(), Answer, Err)) return Done(TSvResult<FSvPlayerProfile>::Fail(Err));
			const FSvPlayerProfile Profile = Ops::Profile(*Self, Answer);
			if (SvSame(Provider, SvProviders::Device)) Ops::ForgetDeviceIfOwnedBy(*Self, PlayerId);
			Done(TSvResult<FSvPlayerProfile>::Ok(Profile));
		});
	});
}

TSvFuture<FSvCodeSent> FSvAuth::RequestLinkEmailCode(const FString& Email)
{
	if (Email.IsEmpty()) return SvFailed<FSvCodeSent>(Invalid(TEXT("Email is required.")));
	FSvAuth* Self = this;
	const FString Json = SvJson::Write(Ops::One(TEXT("email"), Email));
	return SvAsync<FSvCodeSent>([Self, Json](TSvCallback<FSvCodeSent> Done)
	{
		Self->SendAsPlayer(FSvCall::Post(TEXT("/player-auth/link/email/code"), Json, ESvRetry::Never), [Done](const TSvResult<FSvHttpResponse>& R)
		{
			if (R.IsError()) return Done(TSvResult<FSvCodeSent>::Fail(R.GetError()));
			FSvCodeSent Sent;
			FSvError Err;
			if (!ParseModel(R.GetValue(), Sent, Err)) return Done(TSvResult<FSvCodeSent>::Fail(Err));
			Done(TSvResult<FSvCodeSent>::Ok(Sent));
		});
	});
}

TSvFuture<FSvCodeSent> FSvAuth::RequestEmailRemovalCode()
{
	FSvAuth* Self = this;
	TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetBoolField(TEXT("unlink"), true);
	const FString Json = SvJson::Write(Body);
	return SvAsync<FSvCodeSent>([Self, Json](TSvCallback<FSvCodeSent> Done)
	{
		Self->SendAsPlayer(FSvCall::Post(TEXT("/player-auth/link/email/code"), Json, ESvRetry::Never), [Done](const TSvResult<FSvHttpResponse>& R)
		{
			if (R.IsError()) return Done(TSvResult<FSvCodeSent>::Fail(R.GetError()));
			FSvCodeSent Sent;
			FSvError Err;
			if (!ParseModel(R.GetValue(), Sent, Err)) return Done(TSvResult<FSvCodeSent>::Fail(Err));
			Done(TSvResult<FSvCodeSent>::Ok(Sent));
		});
	});
}

TSvFuture<FSvPlayerProfile> FSvAuth::AddEmail(const FString& Email, const FString& Code, const FString& Password)
{
	if (Email.IsEmpty() || Code.IsEmpty()) return SvFailed<FSvPlayerProfile>(Invalid(TEXT("Email and Code are required.")));
	TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("email"), Email);
	Body->SetStringField(TEXT("code"), Code);
	if (!Password.IsEmpty()) Body->SetStringField(TEXT("password"), Password);
	return Ops::LinkEmail(*this, Body);
}

TSvFuture<FSvPlayerProfile> FSvAuth::ChangeEmail(const FString& NewEmail, const FString& Code, const FString& CurrentPassword, const FString& OldCode)
{
	if (NewEmail.IsEmpty() || Code.IsEmpty()) return SvFailed<FSvPlayerProfile>(Invalid(TEXT("NewEmail and Code are required.")));
	TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("email"), NewEmail);
	Body->SetStringField(TEXT("code"), Code);
	if (!CurrentPassword.IsEmpty()) Body->SetStringField(TEXT("currentPassword"), CurrentPassword);
	if (!OldCode.IsEmpty()) Body->SetStringField(TEXT("oldCode"), OldCode);
	return Ops::LinkEmail(*this, Body);
}

TSvFuture<FSvPlayerProfile> FSvAuth::RemoveEmail(const FString& CurrentPassword, const FString& OldCode)
{
	TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	if (!CurrentPassword.IsEmpty()) Body->SetStringField(TEXT("currentPassword"), CurrentPassword);
	if (!OldCode.IsEmpty()) Body->SetStringField(TEXT("oldCode"), OldCode);
	const FString Json = SvJson::Write(Body);
	FSvAuth* Self = this;
	return SvAsync<FSvPlayerProfile>([Self, Json](TSvCallback<FSvPlayerProfile> Done)
	{
		auto Finish = [Self, Done](const TSvResult<FSvHttpResponse>& R)
		{
			if (R.IsError()) return Done(TSvResult<FSvPlayerProfile>::Fail(R.GetError()));
			FProfileAnswer Answer;
			FSvError Err;
			if (!ParseProfile(R.GetValue(), Answer, Err)) return Done(TSvResult<FSvPlayerProfile>::Fail(Err));
			const FSvPlayerProfile Profile = Ops::Profile(*Self, Answer);
			Ops::DeleteRefreshToken(*Self);
			Done(TSvResult<FSvPlayerProfile>::Ok(Profile));
		};
		Self->SendAsPlayer(FSvCall::Post(TEXT("/player-auth/link/email/remove"), Json, ESvRetry::Never), [Self, Json, Finish](const TSvResult<FSvHttpResponse>& R)
		{
			if (R.IsError() && IsRouteMissing(R.GetError()))
			{
				// Nothing was removed (the route does not exist there): the older API's form, once.
				FSvCall Old;
				Old.Method = TEXT("DELETE");
				Old.Path = TEXT("/player-auth/link/email");
				Old.Body = Json;
				Old.Retry = ESvRetry::Never;
				Self->SendAsPlayer(Old, Finish);
				return;
			}
			Finish(R);
		});
	});
}

TSvFuture<FSvVoid> FSvAuth::ChangePassword(const FString& CurrentPassword, const FString& NewPassword)
{
	if (CurrentPassword.IsEmpty() || NewPassword.IsEmpty()) return SvFailed<FSvVoid>(Invalid(TEXT("CurrentPassword and NewPassword are required.")));
	TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("currentPassword"), CurrentPassword);
	Body->SetStringField(TEXT("newPassword"), NewPassword);
	const FString Json = SvJson::Write(Body);
	FSvAuth* Self = this;
	const FString PlayerId = GetPlayerId();
	return SvAsync<FSvVoid>([Self, Json, PlayerId](TSvCallback<FSvVoid> Done)
	{
		FSvCall Call;
		Call.Method = TEXT("PUT");
		Call.Path = TEXT("/player-auth/me/password");
		Call.Body = Json;
		Call.Retry = ESvRetry::Never;
		Self->SendAsPlayer(Call, [Self, PlayerId, Done](const TSvResult<FSvHttpResponse>& R)
		{
			if (R.IsError()) return Done(TSvResult<FSvVoid>::Fail(R.GetError()));
			const TSharedPtr<FJsonObject> J = SvJson::ParseObject(R.GetValue().Body);
			// This device's new refresh token: kept only while the player who changed the password is still signed in.
			Ops::KeepAccountChangeToken(*Self, SvJson::GetString(J, TEXT("refreshToken")), PlayerId);
			Done(TSvResult<FSvVoid>::Ok(FSvVoid()));
		});
	});
}
