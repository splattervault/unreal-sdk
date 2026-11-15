# SplatterVault for Unreal Engine

SplatterVault's studio services for Unreal Engine game clients, in C++ and Blueprint:

- **Player sign-in and accounts**: guest (device), email code and password, Meta Quest, Steam, Xsolla, PlayFab,
  Supabase, and tokens your own backend issues. Automatic token renewal, restore on the next launch, link / unlink,
  email add / change / remove, password change, account erase.
- **Player data**: the player's own key/value store with `ifVersion` conflict checks, the read-only store your backend
  writes, and the game's public data (ETag-cached).
- **Store**: catalog (ETag-cached), balances and items, purchases with virtual currency, consuming items, history,
  Meta Quest purchase sync, and a pending-change queue that finishes interrupted purchases after a crash or a lost
  connection.

Version **0.1.0 (preview)**. Unreal Engine **5.3 or newer**, on Win64 and Android (Meta Quest). The code also has
paths for Mac, iOS and Linux (with an obfuscated token store, see [Security](#security-and-storage)), but they are not
built yet and the plugin does not list them.

> **No API keys in a game.** This plugin never takes an organization API key. Players sign in with a platform proof,
> an email account or a guest on the device; every request then carries that player's short-lived token. Your
> organization key belongs on your servers only (use the SplatterVault server SDK there).

## Contents

- [Install](#install)
- [Configure](#configure)
- [How calls work](#how-calls-work)
- [Quick start: sign-in](#sign-in)
- [Quick start: player data](#player-data)
- [Quick start: store](#store)
- [Meta Quest](#meta-quest)
- [Steam](#steam)
- [Errors and retries](#errors-and-retries)
- [Security and storage](#security-and-storage)
- [Testing](#testing)
- [Blueprint node list](#blueprint-nodes)

## Install

**From Fab**: add the plugin to your engine from the Fab library (Epic Games Launcher > Unreal Engine > Library > Fab
Library > Install to Engine), then enable **SplatterVault** in Edit > Plugins and restart the editor.

**From source** (this repository): copy the folder that holds `SplatterVault.uplugin` to `<YourProject>/Plugins/SplatterVault/`,
then open the project and let the editor build it (Visual Studio 2022 with the "Game development with C++" workload).
A Blueprint-only project cannot build plugin source: use the prebuilt Fab version, or add one C++ class to the project
first (Tools > New C++ Class). In a C++ project, add the module where you call it:

```csharp
// YourGame.Build.cs
PublicDependencyModuleNames.Add("SplatterVault");
```

## Configure

Edit > Project Settings > Plugins > **SplatterVault**:

| Setting | |
|---|---|
| **Game Id** | Your game's id (the number on the game console's Sign-in tab). Preferred: it never changes. |
| **Game Key** | The game's public Game Key, only when Game Id is 0. It is not a secret and not an API key. |
| Base Url Override | Another API (a staging server or a local mock), `/rest` included. Empty: `https://splattervault.com/rest`. |
| Allow Insecure Http For Development | Allows `http://` to a host that is not loopback (`127.0.0.1`, `::1`, `localhost` are always allowed). Ignored in Shipping builds. |
| Request Timeout Seconds / Max Attempts | Per-request timeout (30 s) and sends per retryable request (3). |
| Restore On Startup | Signs the player back in when the game starts (email and guest sign-ins). Raises **On Restore Finished**. |
| Resume Pending After Sign In | Off by default. When on, sends again the signed-in player's store changes that never got an answer, after every sign-in (see [Store](#store)). |
| Exclude Secrets From Android Backup | On by default: keeps the SDK's Keystore-sealed files out of Android Auto Backup and device transfer (see [Security](#security-and-storage)). |
| Enable In Editor | Starts the SDK in Play In Editor sessions. |

Numbers out of range (a NaN or negative timeout, Max Attempts outside 1-10...) are refused when the SDK starts:
**Get Configuration Error** says which. The settings land in `Config/DefaultGame.ini`:

```ini
[/Script/SplatterVault.SplatterVaultSettings]
GameId=42
```

Or configure from code: `USplatterVaultSubsystem::Configure(GameId, GameKey, BaseUrl, OutError)`.

## How calls work

`USplatterVaultSubsystem` (a game-instance subsystem) owns one `FSplatterVaultClient`.

**C++**: every call returns `TFuture<TSvResult<T>>` (`TSvFuture<T>`), completed on the game thread. Attach a
continuation with `Next`; `TSvResult` holds either the value or an `FSvError`:

```cpp
#include "SplatterVaultSubsystem.h"
#include "SplatterVaultClient.h"

// From any actor, component, widget or the game instance (null when the SDK is not configured).
TSharedPtr<FSplatterVaultClient> Sv = USplatterVaultSubsystem::GetClientFor(this);
if (!Sv) return;   // GetConfigurationError() says why
Sv->Auth().SignInAsGuest().Next([Sv](const TSvResult<FSvSignInResult>& R)
{
    if (R.IsError()) { UE_LOG(LogTemp, Warning, TEXT("%s"), *R.GetError().ToString()); return; }
    Sv->Data().GetMine();   // capture Sv and keep going: no re-fetching
});
```

**Continuations capture `Sv`** (the `TSharedPtr`), not a raw pointer: it is always safe to call. When the game
instance ends, the client is shut down and every call on it answers `client_shutdown` at once (nothing is sent, no
store change is kept). A client you make yourself with `FSplatterVaultClient::Create`: call `Shutdown()` when you are
done with it. Make all calls on the game thread. Capture `TWeakObjectPtr`s of your UObjects in continuations: an answer can arrive after a level change.
**Never block the game thread on a future** (`Get()`, `Wait()`): the answer is delivered on the game thread, so it
would wait forever. Use `Next`. Do not drop the last reference to a client from inside its own events.

**Blueprint**: every call is an async node (search the palette for "SplatterVault") with **On Success** and **On
Failure** pins; both carry the result and an `Error`. The subsystem (**Get SplatterVaultSubsystem**) has the state
getters (Is Signed In, Get Player Id, ...) and the events (On Signed In, On Signed Out, On Sign In Required, On
Storage Failed, On Restore Finished, On Pending Store Changes Resumed).

**Sample**: [`Docs/Samples/SvQuickStartSample.inl`](Docs/Samples/SvQuickStartSample.inl) is a complete C++ flow
(guest sign-in, resume pending store changes, read + conditional write of player data, catalog, purchase, consume).
The plugin's integration tests compile it and run it against the API mock.

## Sign-in

### Guest (device)

The first launch makes a guest; later launches sign the same guest in with a secret kept in the device's secure
storage. A guest can add an email later and keep their player.

```cpp
Sv->Auth().SignInAsGuest(TEXT("Player One")).Next([](const TSvResult<FSvSignInResult>& R)
{
    if (R.IsOk() && !R.GetValue().SavedToDevice)
    {
        // Secure storage failed: this guest exists only until the game quits. Suggest adding an email.
    }
});
```

Blueprint: **Sign In As Guest (SplatterVault)**.

### Restore on the next launch

With **Restore On Startup** on, the subsystem does this for you and raises **On Restore Finished (bSignedIn, Error)**.
By hand: `Sv->Auth().Restore()` / **Restore Sign In (SplatterVault)**. It answers false after a platform sign-in
(Meta, Steam...) or a sign-out: use **Get Last Sign In Method** to sign in with that platform again. Offline is an
error (`Error.IsNetworkError()`), never "signed out".

### Email

```cpp
Sv->Auth().RequestEmailCode(Email);                      // emails an 8-digit code (one a minute per address)
Sv->Auth().VerifyEmailCode(Email, Code, NewPassword);    // signs in or up; NewPassword optional (sets / resets it)
Sv->Auth().SignInWithPassword(Email, Password);          // invalid_credentials for any wrong pair
```

Email sign-ins are renewable: the refresh token is kept in secure storage and the SDK renews the player token by
itself (one refresh at a time, also when many requests run out together). Account changes: `RequestLinkEmailCode` +
`AddEmail` / `ChangeEmail`, `RequestEmailRemovalCode` + `RemoveEmail`, `ChangePassword`. Blueprint: **Request Email
Code**, **Verify Email Code**, **Sign In With Password**, **Add Email**, **Change Email**, **Remove Email**, **Change
Password**.

### Xsolla, PlayFab, Supabase

```cpp
Sv->Auth().SignInWithXsolla(XsollaLoginToken);
Sv->Auth().SignInWithPlayFab(PlayFabSessionTicket);
Sv->Auth().SignInWithSupabase(SupabaseAccessToken);
```

The optional `AppId` picks the app when your game has several of that kind (Login project id, title id, project ref).

### Your own backend (studio token)

Your server calls `POST /rest/player-auth/studio` with its organization key and hands the player token to the game:

```cpp
Sv->Auth().SignInWithStudioToken(PlayerTokenFromYourServer);   // checked with GET /player-auth/me first
```

### Platform sign-ins cannot renew themselves

Meta, Steam, Xsolla, PlayFab, Supabase and studio tokens need a fresh proof when the player token runs out (about an
hour). The SDK raises **On Sign In Required**; in C++ you can also let waiting requests continue after you sign in
again:

```cpp
Sv->Auth().SetReauthenticate([Weak = TWeakObjectPtr<UMyGameInstance>(this)](const FSvSignInRequired& Info)
{
    TPromise<bool> Done;   // get a new Steam ticket / Meta nonce, sign in again, then Done.SetValue(true)
    ...
    return Done.GetFuture();
});
```

### Account

`GetMe`, `Link{Meta,Steam,Xsolla,PlayFab,Supabase}` (Steam: `LinkSteamTicket(Bytes)` or `LinkSteam(Hex)`, as for
sign-in), `Unlink(Provider)` (`last_identity` refuses the only way in),
`SignOut(bForgetGuest)`, `EraseMe` (erases the player everywhere; cannot be undone). For your game server:
`GetPlayerToken()` (verify it offline with `GET /rest/player-auth/keys`; never log or store it).

## Player data

```cpp
// Read (the player and readonly stores). Keys are case-sensitive.
Sv->Data().GetMine().Next([Sv](const TSvResult<FSvPlayerData>& R)
{
    const int64 Version = R.GetValue().Player.VersionOf(TEXT("settings"));   // 0 when it does not exist

    // Write: all or nothing, at most 25 ops. IfVersion makes a write conditional (0: must not exist yet).
    FSvDataOps Ops;
    Ops.SetJson(TEXT("settings"), TEXT("{\"fov\":90}"), Version)
       .Inc(TEXT("matches"), 1)
       .Inc(TEXT("coins"), 50, FSvIncOptions().WithMax(9999))   // also WithField, WithMin, WithIfVersion
       .Delete(TEXT("tutorial"));
    Sv->Data().Write(Ops).Next([](const TSvResult<FSvDataWriteResult>& W)
    {
        if (W.IsError() && W.GetError().OutcomeUnknown)
        {
            // No answer (or 500/502/504, or a conflict after the SDK sent it again): it may have applied. Read again;
            // if the key's version moved and the value is yours (an inc: it went up by your amount), it applied.
            // Never apply it again blindly.
        }
        else if (W.IsError() && W.GetError().Is(TEXT("version_conflict")))
        {
            // Someone else changed it first: read again (GetVersionConflicts has the versions), merge, write again.
        }
    });
});

Sv->Data().GetGameData();          // the game's public data as this player (If-None-Match: NotModified on 304)
Sv->Data().GetPublic();            // the same with no sign-in (a launcher, a title screen)
```

**Retries of data writes.** Data writes have no Idempotency-Key. A write whose every op has `IfVersion` is sent again
by the SDK only after an answer that proves nothing was applied (408, 429, 503, or an expired token it renewed);
after no answer, 500, 502 or 504 it is not, and the error has `OutcomeUnknown` set (Blueprint: **Is Outcome
Unknown**). A `version_conflict` after the write was sent more than once is flagged the same way: the conflict may be
with your own earlier send. A write without `IfVersion` is never sent again.

Values are JSON; `FSvDataEntry::ValueJson` holds the exact text the API sent and `GetValue()` parses it. Keys are
case-sensitive (`score` and `Score` are two keys): the SDK reads player data and sends `SetJson` values without
Unreal's JSON DOM, whose objects merge keys that differ only in case. `GetValue()` and the Blueprint getters do use
Unreal's JSON, so inside one value avoid field names that differ only in case. `SetStruct(Key, MyUStruct)` stores a
USTRUCT. The player store is written by the player and is untrusted: keep currency, XP and unlocks in the
readonly store (your backend) or the store service.

Blueprint: **Get My Data**, **Write My Data** with **Make Set Int / Float / Bool / String / Json Data Op**, **Make
Delete Data Op**, **Make Inc Data Op**; read with **Find Data Entry**, **Get Data Int / Float / Bool / String**, **Get
Data Object Int / String**, **Get Data Version**; **Get Game Data**, **Get Public Game Data**.

## Store

```cpp
Sv->Store().GetCatalog().Next([Sv](const TSvResult<FSvStoreCatalog>& R)
{
    const FSvCatalogItem* Potion = R.GetValue().Find(TEXT("potion_small"));
    int64 Price = 0;
    if (Potion && Potion->Purchasable && Potion->PriceIn(TEXT("COINS"), Price))
    {
        // ExpectedPrice is the price the player saw: a changed price is refused (price_changed).
        Sv->Store().Purchase(TEXT("potion_small"), TEXT("COINS"), Price, 2).Next([](const TSvResult<FSvStoreTxnResult>& P)
        {
            if (P.IsOk()) { /* P.GetValue().Balances / Items: trust these, not your own arithmetic */ }
            else if (P.GetError().Is(TEXT("insufficient_funds"))) { /* ... */ }
        });
    }
});

Sv->Store().GetHoldings();                              // balances and items
Sv->Store().ConsumeSku(TEXT("potion_small"), 1);        // or ConsumeInstance(InstanceId, Quantity)
Sv->Store().GetHistory();                               // newest first, 50 a page: pass Page.Next as Before
Sv->Store().SyncMetaPurchases();                        // Quest IAP: grants each mapped purchase once
```

**Idempotency and the pending queue.** Every purchase and consume gets an Idempotency-Key (a new random one, or
yours) that is kept in secure storage with the exact request before the first send. The SDK retries it with the same
key and the byte-identical body, so the API applies it once; if the game quits or the network dies before an answer,
the change stays pending. A repeat of a change that already happened answers `Replayed = true`: a success. A refusal
(`insufficient_funds`...) drops it.

- **Every store error says whether the change is still kept**: `FSvError::Pending` (Blueprint: **Is Change
  Pending**), with its key in `FSvError::IdempotencyKey`. It is true after no answer, 408, 429, 500, 502, 503, 504, a
  sign-in problem, another player signing in, or a shutdown, not only when `OutcomeUnknown` is set. **While a change
  is pending, offer "Finish", never "Buy again"**: Finish calls **Resume Pending Store Changes**
  (`Store().ResumePending()`) or sends the same change with the same key:
  `Purchase(Sku, Currency, Price, Quantity, Error.IdempotencyKey)`. A new purchase without the key would be a second
  purchase, and the first would still be applied by the next resume. `Pending` false: nothing is kept (refused, or
  never sent): a new attempt is safe.
- **One change per key.** The same change under a key that is kept or being sent finishes it (a send in flight is
  joined: one send, both callers get its answer). A different request under that key is refused on the device
  (`change_in_progress`, nothing sent, `Pending` false) and the kept change is left as it is.
- **Pending changes belong to their player.** `GetPending` and `ResumePending` see only the signed-in player's
  changes. Right before each send, `ResumePending` checks that the change is still kept and that its player is still
  the one signed in; when another player signed in, it stops (`FSvResumeResult::PlayerChanged`) and the rest stays
  pending for its own player. A change is never sent under another player's token (`player_changed`, nothing sent).
  A change being sent right now is skipped (that send decides it), so resuming from inside a re-authentication
  callback never waits on itself.
- **When to resume.** The queue can also hold a change the API refused for a passing reason and never applied (an
  expired sign-in, a 503). Resuming applies it then, maybe much later. Resume at a moment the player expects the
  purchase to go through (for example on the store screen), or **Discard Pending Store Change**. Automatic resume after
  sign-in is off by default (Project Settings: **Resume Pending After Sign In**).

The catalog is ETag-cached per player: an unchanged catalog comes back from the SDK's copy (`NotModified`).

Blueprint: **Get Catalog**, **Get Holdings**, **Purchase**, **Consume Item**, **Consume Sku**, **Get Store History**,
**Sync Meta Purchases**, **Resume Pending Store Changes**; helpers **Find Catalog Item**, **Get Item Price**, **Get
Balance**, **Count Items**, **New Idempotency Key**.

## Meta Quest

Sign-in uses the Meta Platform SDK's user proof: the player's **user id** and a single-use **nonce**.

1. Enable Meta's platform integration: the **Meta XR Platform** plugin (or the engine's **OnlineSubsystemOculus**
   with your App ID in `DefaultEngine.ini`) and initialize the Platform SDK as Meta documents (entitlement check first).
2. Get the logged-in user id: `ovr_User_GetLoggedInUser()` → `ovr_User_GetID(User)` (an `ovrID`, a 64-bit number).
   The Meta XR Platform plugin exposes the same request (Users > GetLoggedInUser) to Blueprint.
3. Get a nonce: `ovr_User_GetUserProof()` → `ovr_UserProof_GetNonce(Proof)` (Users > GetUserProof in the plugin).
4. Sign in (a nonce works once; get a new one for every sign-in, also after `provider_unavailable`):

```cpp
Sv->Auth().SignInWithMeta(static_cast<uint64>(UserId), UTF8_TO_TCHAR(Nonce));
```

Blueprint: **Sign In With Meta (SplatterVault)** with the id as text (decimal digits).

Quest purchases: after a purchase, and at startup, call `SyncMetaPurchases()`: the API asks Meta and grants each
mapped purchase once (`meta_not_linked`: the player has no Meta sign-in). On Android the token store uses the Android
Keystore (see below). Build for Android with the Android SDK/NDK your engine version requires (UE 5.5+: NDK r27c).

## Steam

Sign-in uses a Web API ticket for the identity **"splattervault"** from the Steamworks SDK (the engine's
OnlineSubsystemSteam / Steamworks plugin, or the SDK directly):

```cpp
// With steam_api.h (Steamworks SDK 1.57+):
SteamUser()->GetAuthTicketForWebApi("splattervault");
// ... then in your GetTicketForWebApiResponse_t callback:
TArray<uint8> Ticket(Response->m_rgubTicket, Response->m_cubTicket);
Sv->Auth().SignInWithSteamTicket(Ticket);         // or SignInWithSteam(HexString)
// After the answer: SteamUser()->CancelAuthTicket(Response->m_hAuthTicket);
```

Blueprint: **Sign In With Steam Ticket Bytes (SplatterVault)** takes the ticket bytes (or convert them with **Steam
Ticket To Hex** for **Sign In With Steam**); linking has the same pair (**Link Steam Ticket Bytes**, **Link Steam**). A
ticket works once.

## Errors and retries

`FSvError` carries `Status` (HTTP; 0 when no answer arrived or the SDK refused the call), `Code` (the API's stable
`error` code, an open set: key your handling on it), `Message` (a sentence to log or show; it may change),
`HasRetryAfter` / `RetryAfterSeconds`, `DetailsJson` (for example the versions of a `version_conflict`),
`OutcomeUnknown` (the change may or may not have been applied: read again, or resume the store change),
`IdempotencyKey` (store changes: the key to send it again with) and `Attempts` (how many times it was sent). SDK codes
(status 0 unless noted): `network_error`, `timeout`, `not_signed_in`, `storage_unreadable`, `invalid_argument`,
`invalid_state`, `invalid_response` (keeps the HTTP status), `not_configured`, `client_shutdown`, `player_changed`
(a store change of another player: nothing sent), `sign_in_changed` (a renewal or restore answered after another
sign-in, a sign-out or an erase: its answer was not used), `change_in_progress` (another request under an
Idempotency-Key that is being sent right now: nothing sent), `redirect_refused` (keeps the 3xx status: see below). Codes you will meet often:
`token_expired`, `proof_invalid`, `proof_replayed`, `identity_in_use`, `invalid_credentials`, `code_invalid`,
`version_conflict`, `insufficient_funds`, `price_changed`, `already_owned`, `rate_limited`.

The SDK sends a request again by itself only when that is safe: reads and store changes (same Idempotency-Key) after
a transient failure (no answer, 408, 429, 500, 502, 503, 504; never another 4xx); data writes whose every op has
`IfVersion` only after 408, 429 or 503 (see [Player data](#player-data)); and a refresh within the API's 30-second
resend window. At most **Max Attempts** sends, with jittered exponential backoff (0.5 s doubling, at most 4 s). A `Retry-After` is always honoured; one longer than 10 s is not waited out: the error
carries it for you to schedule. Sign-ins, email codes, account changes and the Meta purchase check are never repeated.

## Security and storage

- The **player token** lives in memory only. Refresh tokens, the guest's device secret, the last sign-in method and
  pending store changes are kept in secure storage, sealed per value (file names are hashes; the value never appears
  in a log, an error or an event):

| Platform | Storage |
|---|---|
| Windows | DPAPI (`CryptProtectData`, current user) in `%LOCALAPPDATA%/SplatterVault/<Project>/players` |
| Android / Quest | Android Keystore (AES-256-GCM key that never leaves the keystore) in the app's private files dir |
| Mac, Linux | **Obfuscated only**: AES-256 + HMAC with a per-install salt and a key derived from the app and machine, owner-only file modes, in the user settings dir |
| iOS, others | **Obfuscated only** (Keychain support is planned), in the project's Saved dir |

  Obfuscated storage keeps secrets from being read as plain text, but anyone who can read the app's files can open
  them with enough effort. On those platforms, prefer platform sign-ins or email over long-lived guests.
- Storage that cannot be read (another device's backup, a changed key) is never taken as "nothing stored": the SDK
  refuses to make a new guest over the old one (`storage_unreadable`) and reads again next time. To give up on it,
  `CreateGuest(DisplayName, /*bReplaceExistingGuest*/ true)`.
- **Android backups.** The SDK's files (`files/splattervault/`) are sealed with a Keystore key that never leaves the
  device, so a backup restored elsewhere could not open them (`storage_unreadable`). The plugin's UPL step adds backup
  rules that leave them out of Auto Backup (`android:fullBackupContent`) and of cloud backup and device transfer
  (`android:dataExtractionRules`, Android 12+). If your manifest already names its own rules, they are kept and the
  build log says so: add `<exclude domain="file" path="splattervault/"/>` to them. **Exclude Secrets From Android
  Backup** turns this off.
- **No redirects, no plain http.** The base URL must be https; `http://` is accepted only for loopback hosts
  (`127.0.0.1`, `::1`, `localhost`), or for another host with **Allow Insecure Http For Development** (ignored in
  Shipping builds). A 3xx answer is never used and never sent again (`redirect_refused`). Unreal's HTTP module
  (libcurl) always follows redirects by itself and offers no switch to stop it: libcurl does not forward the
  `Authorization` header to another host, but a 307/308 re-sends the request body (which can hold a refresh token, a
  device secret or a password). On UE 5.5+ the SDK detects a followed redirect and refuses its answer; on 5.3-5.4 it
  cannot see one. Point the SDK only at the SplatterVault API over https, or plug in your own `ISvHttpTransport`
  (`FSvClientOptions::Transport`) if you need to forbid redirects at the transport.
- Tokens, Idempotency-Keys and player ids are compared case-sensitively.
- **Logs.** The SDK never logs a token or a secret, but Unreal's HTTP module at `LogHttp Verbose` / `VeryVerbose`
  prints request headers, `Authorization` included. Never ship (or share logs of) a build with those levels on.
- **Several clients on one machine.** Each Play In Editor window gets its own store; for two game processes on one PC
  pass `-SvStorageNamespace=<name>` to one of them, and give a second client you make yourself in the same process its
  own `FSvClientOptions::StorageNamespace` (a client reads its pending store changes once and then keeps them in
  memory). Before each refresh the SDK reads the stored refresh token again.
- Every value is written to a temporary file that then replaces the old one in one step; a write cut short keeps the
  old value. Storage that cannot be set up at all (no writable directory) saves nothing: a new guest reports
  `SavedToDevice` false and **On Storage Failed** fires.
- **Shutdown.** When the game instance ends (or you call `FSplatterVaultClient::Shutdown`), requests still waiting
  (on an answer, a renewal or a retry delay) complete with `client_shutdown`. A refresh answer that arrives after
  that still keeps its new refresh token, so the next launch stays signed in. An event handler may release the
  client: it stays alive until the answer has been handled.
- Plug in your own store with `FSvClientOptions::SecureStore` (implement `ISvSecureStore`).
- The SDK strips any `X-API-Key` header from its requests and never logs tokens, secrets, proofs or passwords.

## Testing

The plugin ships Unreal automation tests (Session Frontend > Automation, filter "SplatterVault"):

- `SplatterVault.Unit.*` (no network): crypto vectors, request building, JSON round trips of every model,
  case-sensitive raw JSON, data ops, error mapping, retry/backoff on an injected clock, single-flight refresh,
  re-authentication, guest flows, unreadable storage, idempotency and the pending queue, ETag caching, client
  options, storage (DPAPI and obfuscated) round trips, the subsystem in a standalone game instance.
- `SplatterVault.Integration.*`: full flows against the SplatterVault API mock (guest → data → store → forced token
  expiry → injected 503 + Retry-After → lost answers; email code → refresh → restore → password; every platform
  sign-in, studio token, Meta purchase sync, link / unlink, pending resume, erase; the quick start sample). Start the
  mock in [`tools/sdk-mock`](tools/sdk-mock/README.md) (Node 18+, fake test credentials, it never calls SplatterVault)
  and set `SV_MOCK_URL` (for example `http://127.0.0.1:8787/rest`); without it these tests pass with a warning and do
  nothing. See [CONTRIBUTING.md](CONTRIBUTING.md#running-the-automation-tests).

Headless:

```bat
start "sdk-mock" node tools\sdk-mock\server.mjs --port 8787
set SV_MOCK_URL=http://127.0.0.1:8787/rest
UnrealEditor-Cmd.exe C:\Path\Host.uproject -ExecCmds="Automation RunTests SplatterVault" -TestExit="Automation Test Queue Empty" -unattended -nullrhi -nosplash -log -ReportExportPath=C:\Path\Report
```

## Blueprint nodes

| Area | Nodes |
|---|---|
| Sign-in (async) | Sign In As Guest, Create Guest, Sign In With Device, Restore Sign In, Request Email Code, Verify Email Code, Sign In With Password, Sign In With Meta, Sign In With Steam, Sign In With Steam Ticket Bytes, Sign In With Xsolla, Sign In With PlayFab, Sign In With Supabase, Sign In With Studio Token, Refresh Sign In, Sign Out |
| Account (async) | Get Me, Link Meta / Steam / Steam Ticket Bytes / Xsolla / PlayFab / Supabase, Unlink, Request Link Email Code, Add Email, Change Email, Request Email Removal Code, Remove Email, Change Password, Erase Player |
| Player data (async) | Get My Data, Write My Data, Get Game Data, Get Public Game Data |
| Store (async) | Get Catalog, Get Holdings, Purchase, Consume Item, Consume Sku, Get Store History, Sync Meta Purchases, Resume Pending Store Changes |
| Subsystem | Configure, Is Configured, Get Configuration Error, Is Signed In, Get Player Id, Get Player, Get Sign In Method, Get Identities, Get Player Token, Get Last Sign In Method, Has Guest On Device, Forget Guest On Device, Retry Saving Secrets, Get Storage Protection, Get Pending Store Changes, Discard Pending Store Change, Has Restore Finished; events On Signed In, On Signed Out, On Sign In Required, On Storage Failed, On Restore Finished, On Pending Store Changes Resumed |
| Helpers | Make Set Json / String / Int / Float / Bool Data Op, Make Delete Data Op, Make Inc Data Op, Find Data Entry, Get Data Version, Get Data String / Int / Float / Bool, Get Data Object Int / String, To Json String, Is Error Code, Is Network Error, Is Outcome Unknown, Is Transient Error, To String (SvError), Get Version Conflicts, Find Catalog Item, Get Item Price, Get Balance, Count Items, New Idempotency Key, Steam Ticket To Hex, Get Sdk Version |

API reference: the public headers in `Source/SplatterVault/Public/` are documented; `doxygen Docs/Doxyfile` builds
HTML into `Docs/api/`.

## License

MIT (see [LICENSE](LICENSE)).
