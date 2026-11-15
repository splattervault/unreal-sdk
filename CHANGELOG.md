# Changelog

All notable changes to the SplatterVault Unreal Engine plugin. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow [Semantic Versioning](https://semver.org/).

## [0.1.0] - 2026-11-15 (preview)

First preview, for Unreal Engine 5.3 and newer.

### Added

- `USplatterVaultSubsystem` (game-instance subsystem) configured from Project Settings > Plugins > SplatterVault
  (Game Id or public Game Key, base URL override, timeouts, restore on startup, resume pending store changes) or from
  code, with Blueprint events for sign-in, sign-out, sign-in required, storage failures, restore and resumed changes.
- `FSplatterVaultClient` C++ API (`Auth()`, `Data()`, `Store()`) returning `TFuture<TSvResult<T>>`, and Blueprint
  async nodes for every call.
- Player sign-in: guest (device secret in secure storage, one guest per device even with concurrent callers), email
  code and password, Meta Quest (user id + nonce), Steam (Web API ticket), Xsolla, PlayFab, Supabase, studio-issued
  tokens; restore on the next launch; automatic single-flight renewal (refresh token, or the guest's device secret);
  re-authentication hook for platform sign-ins; link / unlink; email add / change / remove; password change; sign-out;
  account erase.
- Player data: read the player and readonly stores, all-or-nothing writes (set / delete / inc) with `ifVersion`
  conflicts, game data and public data with an ETag cache.
- Store: catalog (ETag cache per player), holdings, history paging, purchases with an expected price, consume by
  instance or SKU, Meta Quest purchase sync, Idempotency-Key on every change, a persisted pending-change queue that is
  resumed with the same key.
- Typed errors (`FSvError`: status, code, message, Retry-After, details) and a retry policy limited to safe requests
  (transient failures only: no answer, 408, 429, 500, 502, 503, 504; jittered exponential backoff; Retry-After
  honoured).
- Secure storage: Windows DPAPI, Android Keystore (Java helper added through UPL), obfuscated AES-256 + HMAC files
  elsewhere (documented); pluggable `ISvSecureStore`.
- Automation tests: unit tests (no network) and integration tests against the SplatterVault API mock.

### Changed (review round, before release)

- Player-data writes with `ifVersion` are sent again only after 408, 429 or 503 (or a renewed expired token); after
  no answer, 500, 502 or 504 the error has `OutcomeUnknown`, and so has a `version_conflict` after a resend.
- Store errors carry the change's `IdempotencyKey`; `idempotency_conflict` after a resend (the first send still
  applying) keeps the change pending.
- Pending store changes belong to their player: matched by key and player, checked right before each resumed send,
  never sent under another player's token (`player_changed`). Automatic resume after sign-in is off by default.
- 3xx answers are refused (`redirect_refused`) and never sent again; on UE 5.5+ an answer the HTTP stack got by
  following a redirect is refused too. `http://` only for loopback hosts unless `bAllowInsecureHttpForDevelopment`
  (renamed from `bAllowInsecureHttp`; ignored in Shipping builds).
- Numeric options are validated when the client is made (no silent clamping).
- Android: the Keystore-sealed files are excluded from Auto Backup and device transfer through the UPL.
- Tokens, keys and player ids are compared case-sensitively. Configuring the same game and API again keeps the client.
- A renewal, refresh or restore answered after a sign-out, an erase or another sign-in is never used
  (`sign_in_changed`; its new refresh token is signed out on the server); a new guest waits for a restore's guest
  sign-in.
- Two clients on one store: the stored refresh token is read again before each refresh; each Play In Editor window
  gets its own store, and `-SvStorageNamespace=<name>` separates game processes.
- Storage that exists but cannot be read (a damaged salt, sealed values without their salt) is `storage_unreadable`,
  never empty; storage that cannot be set up saves nothing (`SavedToDevice` false). Values are written atomically
  (temporary file, then one replace); a complete leftover temporary file is finished on the next read, and deletes
  remove it too.
- One send per (player, Idempotency-Key): the same change is joined, a different one is refused
  (`change_in_progress`), and a resume skips a change already being sent.
- An event handler may release the last reference to the client; `Shutdown` fails waiting renewals, guest sign-ins
  and retry delays with `client_shutdown` at once and keeps a refresh token that arrives afterwards.
- Pending store changes are read from storage once and then kept in memory. Blueprint `GetDataObjectInt` /
  `GetDataObjectString` match field names case-sensitively. The subsystem is not made on a dedicated server.
- The Fab platform allow-list names only the platforms the plugin was built for (Win64, Android).
- Every attempt of a renewal, the guest fallback, the "sign in again" path, an erase's cleanup and the refresh token
  answered by a password or email change belong to the sign-in they started under: a late 401 from before a sign-out
  never asks the game to sign in, and never renews a guest. Game sign-ins start a new sign-in before they send: a
  renewal or restore still in flight is not used, and an earlier game sign-in loses to a later one.
- Store-change errors carry `Pending` (the change is still kept: offer "Finish", never "Buy again"; Blueprint **Is
  Change Pending**) and the key, shutdown included. A different change under a kept key is refused without touching
  the kept one; the same change finishes it. Resume skips a change being sent.
- Secure storage writes each value through its own uniquely named temporary file; leftovers (old and new names) are
  recovered when newer than the value, and removed by deletes.
