# SplatterVault studio API mock

> **What this is.** A local stand-in for the public SplatterVault studio API, used by this SDK's tests. It mirrors the
> player-facing operations of the [API reference](https://splattervault.com/developers) (the copy it checks itself
> against is `studio-api.json`). It never talks to SplatterVault: every key, game key, code, ticket and token below is
> a **fake test value** that works only against this mock. The same files ship in every SplatterVault SDK repository.

A local, stateful, dependency-free (Node 18+, ESM, `node:http`) mock of the **player-facing** studio API: every
operation of the API description tagged `Player sign-in`, `Player data` or `Store` that a game client may call
(`playerToken` or no credential), mounted under `/rest`. Where the API reference leaves a behaviour open, the mock
does what the real API does; the differences are listed below.

From the repository root (in the Unity SDK the folder is `Tools~/sdk-mock`, which Unity does not import):

```sh
node tools/sdk-mock/server.mjs --port 8787 --host 127.0.0.1 [--token-ttl 300] [--token-leeway 30] [--refresh-ttl 2592000] [--rate-limits] [--quiet]
node tools/sdk-mock/selftest.mjs [--verbose] [--spec <file>]   # honesty check: a full flow, every response validated against the spec
```

- Base URL: `http://127.0.0.1:8787/rest`; the test controls are at `http://127.0.0.1:8787/__mock/*`.
- The self-test reads `studio-api.json` next to it (in the Node SDK: `openapi/studio-api.json`), or `--spec <file>`.
- `--token-ttl` access-token lifetime in seconds (default 300; the real API uses 3600). `--token-leeway` the
  clock skew a token is still accepted after `exp` (default 30, like the API). `--refresh-ttl` refresh-token lifetime
  (default 30 days). `--rate-limits` turns on simplified rate limits (off by default; use faults for 429s).
- State is in memory. `POST /__mock/reset` clears it; a restart does too. The token signing key is fixed (Ed25519
  from a constant seed), so the JWKS and `kid` never change.
- `selftest.mjs` starts its own mock on an ephemeral port (it never touches a running instance), drives guest create
  → device sign-in → me → keys (verifies the token signature offline) → email code sign-up → password → refresh
  rotation / grace / reuse detection → password change → link / unlink (Steam, email add/change/remove) → player data
  write / read / conflict / limits → game and public data with ETag/304 → catalog/304 → purchase / replay /
  idempotency conflict / price mismatch / insufficient funds / bundle / durable → consume by instance and by SKU →
  history paging → Meta sign-in and purchase sync → every switch → faults (503 + Retry-After, dropped socket +
  replay, expired token, raw HTML 502, 429) → expire-tokens → sign-out → erase, plus a short-TTL instance. Every
  response is checked: the status must be listed for the operation, the body must match that status's schema (a
  small validator for the spec's subset: `$ref`, `type`, `properties`, `required`, `items`, `enum`, `nullable`,
  `oneOf`/`anyOf`/`allOf`, `additionalProperties`, `pattern`, `format`, min/max), an error's `error` must be in that
  status's `x-error-codes`, and `Cache-Control` must be what the API sends. Finally the request log is searched for
  every token, refresh token, device secret and password the run used (none may appear). Two deliberate deviations
  are reported, not failed: `GET /store/catalog` and `GET /player-auth/me` answer 304 (the spec lists no 304; the API
  does, see below).

## Test game and fixed credentials

| | |
|---|---|
| gameId | `4242` (organization 77) |
| gameKey | `org_SvMockGame_1763164800000_5eed0001` |
| email code | `12345678` (8 digits, as the real API; request it first with `POST /player-auth/email/code`, any address; sign-up is on). `GET /__mock/emails` lists what was "sent". |
| guest | `POST /player-auth/device/new` answers a `deviceSecret` (`svpd1_` + 43 chars) for `POST /player-auth/device` |
| Meta | `userId`: any 1-20 digits; `nonce`: `test-nonce` (reusable) or `test-nonce-<anything>` (single use: a second use is 401 `proof_replayed`) |
| Steam | `ticket`: hex `deadbeef` (one account) or `deadbeef<more hex>` (even length; one account per suffix). `0bad0bad` → 403 `not_entitled`, `0fa5a11e` → 403 `family_sharing_refused`, `0bb0bb` → 502 `provider_unavailable` |
| Xsolla / Supabase | `token`: `test-token` (one account) or `test-token:<suffix>` (one account per suffix) |
| PlayFab | `sessionTicket`: `test-token` or `test-token:<suffix>` |
| any Meta nonce / token | `not-entitled` → 403 `not_entitled`, `provider-unavailable` → 502 `provider_unavailable`, `banned-by-publisher` → 403 `publisher_banned`; anything else → 401 `proof_invalid` |
| app ids (optional) | meta `1234567890123456`, steam `480`, xsolla `mock-xsolla-login`, playfab `MOCK1`, supabase `mockprojectref`; any other `appId` → 404 `provider_not_enabled` |
| studio-issued token | `POST /__mock/studio-token {"externalId":"abc","displayName":"x"}` answers what `POST /player-auth/studio` (an org-key route) gives a studio backend |

Seed data:

- Currencies `COINS` (cap 1,000,000) and `GEMS` (cap 500). Every new player starts with **1000 COINS**, recorded as
  a `grant` txn (the oldest history entry).
- Items: `sword_basic` (durable, COINS 250, maxOwned 1), `potion_small` (stackable, consumable, COINS 20),
  `raffle_ticket` (unique, consumable, COINS 50), `hat_red` (durable, GEMS 25 or COINS 300, maxOwned 1),
  `starter_bundle` (bundle, COINS 500 = 5 `potion_small` + 2 `raffle_ticket` + 50 GEMS), `vip_pass` (not purchasable
  by players: 403 `purchases_disabled`).
- Meta purchases of every Meta-linked player: `meta_potion_pack` (mapped: 10 `potion_small`) and
  `meta_unmapped_product` (unmapped). First sync: `{granted:1, alreadyGranted:0, unmapped:1, consumePending:0}`.
- Player data: every new player has readonly key `profile.tier` = `"bronze"`. Game data: `motd`, `config`,
  `event.weekend`.

## Operations covered (all 30 player-facing operations)

Player sign-in: `POST /player-auth/email/code`, `POST /player-auth/email/code/verify`, `POST /player-auth/email/password`,
`POST /player-auth/device/new`, `POST /player-auth/device`, `POST /player-auth/refresh`, `POST /player-auth/sign-out`,
`GET /player-auth/keys`, `POST /player-auth/{xsolla,meta,steam,playfab,supabase}`, `GET /player-auth/me`,
`DELETE /player-auth/me`, `POST /player-auth/link/{provider}` (incl. `email`), `DELETE /player-auth/link/{provider}`,
`POST /player-auth/link/email/code` (incl. `{unlink:true}`), `POST /player-auth/link/email/remove`,
`PUT /player-auth/me/password`.

Player data: `GET /player-data/me`, `PATCH /player-data/me`, `GET /player-data/game`, `GET /player-data/games/{game}/public`.

Store: `GET /store/catalog`, `GET /store/me`, `GET /store/me/history`, `POST /store/me/purchases`,
`POST /store/me/consume`, `POST /store/me/platform-purchases/meta`.

**Not served** (404 `not_found`): every studio-backend operation (`orgApiKey`: `/player-auth/studio*`,
`/store/studio/*`, `/studio-data/*`) and the platform operations (sessions, billing, server pools). Game clients must
never call those.

## Behaviour mirrored from the real API

- Errors are `{error, message}` (+ `details` only where the API sends it), `Cache-Control: no-store` on everything
  except `GET /player-auth/keys` 200 (`private, max-age=300`) and the public data 200 (`public, max-age=30`).
  Every 429 carries `Retry-After` (`60` unless a fault says otherwise). Default messages are the API's
  (`token_expired` is exactly `Token expired`).
- Player tokens: Ed25519 JWS (`alg EdDSA`, `kid`), claims `iss splattervault`, `aud sv-player:4242`, `sub`, `scope`,
  `game`, `gk`, `org`, `amr`, `jti`, `iat`, `exp` (+ `app` for platform sign-ins). Verify offline with `GET /player-auth/keys`.
  Missing/garbled header → 401 `token_invalid` "A player token is required (Authorization: Bearer <token>)."; bad
  signature → `token_invalid`; past `exp + leeway` → `token_expired`; erased player → `player_deleted`.
- Refresh tokens (`svpr1_` + 43): only email code / password sign-ins, `PUT /me/password` and an email **change**
  return one (guests and platform sign-ins do not). Each refresh rotates; the previous token is accepted again within
  30 s while its newer token is unused (the unused one is then dead); presenting a dead or rotated token outside that
  window revokes the whole sign-in → 401 `refresh_invalid`. Lifetime 30 days sliding, capped at 90 days from the
  first sign-in. A malformed token is 401 `refresh_invalid`; a non-string is 400. Sign-out is always 204 (a missing
  body is 400). Password change / reset, email change and email removal end every other session.
- Email codes are 8 digits, 10 minutes, newest only, 5 wrong tries kill one; a malformed code is 401
  `code_invalid` (not 400). Passwords 8-72 UTF-8 bytes and not the address (400 `weak_password`).
- Player data: ops apply in order (a later op sees earlier ones), all or nothing; `ifVersion` 0 = must not exist;
  every mismatch is listed in `details` `[{store,key,version}]`. Versions come from one counter per player shared by
  all keys and stores (so they jump, and are never reused). `inc` on a missing key/field starts at 0, refuses
  non-integers (400 `value_invalid`) and min/max overflow (409 `out_of_range`, `details {op,store,key,min?,max?}`).
  `store` `readonly`/`internal` → 403 `forbidden`; other stores → 400. Keys: the DataKey pattern, never `sv.` (any
  case) or `__proto__`. Values ≤ 32 KiB (413, `details {op,maxBytes}`), ≤ 32 deep; > 25 ops → 400 `details {maxOps:25}`;
  100 keys / 256 KiB per player store → 422 `limit_exceeded`. Game/public data ETag `W/"pd-…"`.
- ETag / 304: like the API, **every** JSON answer carries a weak ETag and **every** 2xx GET answers 304 when
  `If-None-Match` matches, unless the request says `Cache-Control: no-cache`. Node's `fetch` (undici) adds exactly
  that header to any request with `If-None-Match`, so a Node client gets no 304 from the real API (or this mock)
  unless it sets its own `Cache-Control` (e.g. `max-age=0`).
- Store: purchase checks run in the API's order (Idempotency-Key → body → store on → fields → replay → player
  purchases switch → item → item flag → currency → price exists → `expectedPrice` → funds → grant: `already_owned`,
  `currency_cap`, `inventory_full`). The answer's `changes` list the debit first, `balances` the touched currencies
  (sorted), `items` the touched rows that still exist. Idempotency is per player: the same key and the same
  canonical request (sku, currency, quantity default 1, expectedPrice / the consume line, instanceId lower-cased)
  answers the stored result with 200 `replayed:true`; a different request is 409 `idempotency_conflict`; a refused
  request stores nothing (the key stays usable). A missing or malformed key is 400 `idempotency_required`.
  Consume: `{instanceId}` (a unique item), `{instanceId, quantity}` (part of a stack: quantity required),
  `{sku, quantity}` (oldest first); currency → 400; a stack at 0 is deleted. History: newest first, 50 a page,
  `next` = last id when more exist, `before` must be a uuid (400) and an unknown one gives an empty page.

## Test controls (`/__mock/*`, never logged, never faulted)

| Control | What it does |
|---|---|
| `GET /__mock/health` | `{ok, version, gameId, gameKey, tokenTtl, tokenLeeway, refreshTtl, rateLimits, players, faults, uptimeSeconds}` |
| `POST /__mock/reset` | Clears all state (players, tokens, data, store, faults, request log) |
| `POST /__mock/fault` | Queues a fault (below). `{"clear":true}` drops all. `GET /__mock/faults` lists, `DELETE /__mock/faults` clears |
| `GET /__mock/requests?since=N&path=sub` | The request log (last 5000): `seq, at, method, path, query, headers {authorization: bool, idempotencyKey, ifNoneMatch, xApiKey: bool, contentType, cacheControl}, body, status, fault`. Secret body fields (refreshToken, deviceSecret, password, currentPassword, newPassword, token, ticket, sessionTicket, nonce, code, oldCode) are replaced by `"[redacted sha256:xxxxxxxx]"` (a fingerprint to compare resends, never the value). `DELETE` clears it |
| `POST /__mock/expire-tokens` | Every access token issued so far now answers 401 `token_expired`. `{"refresh":true}` also expires every refresh token |
| `POST /__mock/config` | Switches: `{playerSignIn, signUp, store, playerData, playerPurchases, playerConsume, emailAvailable, gameActive, providers: {email, emailCode, emailPassword, device, meta, steam, xsolla, playfab, supabase}}` (booleans) |
| `POST /__mock/grant` | `{playerId, currency, amount}` or `{playerId, sku, quantity}`: a studio-style grant (a `grant` txn) |
| `POST /__mock/catalog` | `{sku, prices?, playerPurchasable?, playerConsumable?, archived?, maxOwned?}` or `{currency, archived?, cap?}` |
| `POST /__mock/game-data` | `{key, value}` or `{key, delete:true}` (changes the game data ETag) |
| `POST /__mock/readonly` | `{playerId, key, value}` or `{playerId, key, delete:true}`: the readonly store a studio backend writes |
| `POST /__mock/meta-purchases` | `{userId, purchases:[{id, sku}]}` replaces that Meta user's purchases (mapped skus: `meta_potion_pack`, `meta_hat`) |
| `POST /__mock/studio-token` | `{externalId, displayName?}` → a `PlayerSignIn` for a `studio` identity |
| `GET /__mock/emails`, `GET /__mock/players` | What was "emailed"; a summary of players |

Faults: `POST /__mock/fault` with

```json
{ "match": "/store/me/purchases", "method": "POST", "times": 2,
  "status": 503, "error": "player_sign_in_disabled", "message": "…", "retryAfter": 1,
  "drop": false, "after": false, "delayMs": 0, "tokenExpired": false, "raw": null, "contentType": null }
```

- `match`: a substring of the request path (`""` matches every `/rest` request); `method` optional; `times` default 1.
  Faults are consumed in the order queued, first match wins.
- `status` (+ optional `error`, `message`, `retryAfter`): answer that instead. Default codes: 400 `bad_request`,
  401 `unauthorized`, 403 `forbidden`, 404 `not_found`, 409 `conflict`, 429 `rate_limited` (Retry-After 60),
  500 `internal_error`, 502 `provider_unavailable`, 503 `data_busy` on `/player-data`, else `player_sign_in_disabled`.
- `raw` (+ `contentType`, default `text/html`): a non-JSON body, like a proxy error page.
- `drop: true`: destroy the socket (the client sees a network error).
- `tokenExpired: true`: the next matching request that carries an `Authorization` header gets the API's real
  expired-token answer, 401 `{"error":"token_expired","message":"Token expired"}`.
- `after: true`: the request is processed first, then its answer is replaced or dropped: a "lost answer" (e.g. a
  purchase that went through) to test that a retry with the same Idempotency-Key replays it.
- `delayMs`: wait before handling (alone: just slow).

## Known differences from the real API

- One game, one studio, in memory. No bans (403 `banned` never happens), no game owner changes (the owner-change
  `token_invalid` never happens), no 500s or `data_busy` except by fault. `gameActive:false` gives `game_not_found`.
- Access tokens default to 300 s (the API: 3600 s); set `--token-ttl 3600` for the real value.
- Rate limits are off by default. `--rate-limits` turns on simplified fixed windows: 60 a minute per player on all
  player-token routes, 30 data writes a minute, 1 sign-in code a minute per address, 3 link codes an hour per player,
  6 Meta syncs a minute, 10 guests an hour per client address, 120 public reads a minute per address, 10 password
  failures per 15 minutes per address. The API has more dimensions (network, game, studio, daily caps).
- No email is sent; the code is always `12345678`. The test platform proofs are fixed strings: `deadbeef` Steam tickets
  and the `test-nonce` Meta nonce are reusable (the API spends each once: 401 `proof_replayed`); use
  `test-nonce-<x>` to exercise single use.
- Public game data is served live (the API serves it from a 30 s per-process cache, so a change can take 30 s to show).
- A new player's 1000 COINS starting balance and readonly `profile.tier` are mock seed data (a real game starts
  empty until the studio grants or writes).
- Meta purchase sync uses the fixed product map above; `consumePending` is always 0 and entitlements with an
  expiration are not modelled.
- The player-wide data limits (500 keys / 1 MiB across games, studio quota) are not modelled, only the per-store ones.
- Erasing a player keeps no `erase` txn (the player is gone, so nothing can read it).
- Link request bodies may carry other providers' fields (the API may refuse fields that are not the provider's).
- No CORS headers (game engines do not need them; a browser build would).
- Every other `/rest` path is 404 `not_found` (the API serves its platform routes there).
