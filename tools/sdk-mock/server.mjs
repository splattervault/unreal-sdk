#!/usr/bin/env node
// SplatterVault studio API mock: the player-facing operations (Player sign-in, Player data, Store) of
// the studio API description (studio-api.json), mounted under /rest, plus test controls under /__mock.
// Dependency-free (Node 18+, ESM).
//
//   node server.mjs --port 8787 --host 127.0.0.1 [--token-ttl 300] [--token-leeway 30] [--refresh-ttl 2592000]
//                   [--rate-limits] [--quiet]
//
// Behaviour mirrors the real API where the API reference leaves it open; see README.md for the list of operations,
// the fixed (fake) test credentials, the controls and the known differences.
import http from "node:http";
import crypto from "node:crypto";
import { pathToFileURL } from "node:url";

export const MOCK_VERSION = "0.1.0";

// ---------------------------------------------------------------------------------------------------------------
// Fixed test fixtures
// ---------------------------------------------------------------------------------------------------------------
export const TEST = Object.freeze({
  gameId: 4242,
  gameKey: "org_SvMockGame_1763164800000_5eed0001",
  orgId: 77,
  emailCode: "12345678",
  metaAppId: "1234567890123456",
  steamAppId: "480",
  xsollaAppId: "mock-xsolla-login",
  playfabAppId: "MOCK1",
  supabaseAppId: "mockprojectref",
  metaNonce: "test-nonce",
  steamTicket: "deadbeef",
  platformToken: "test-token"
});

const SEED_CURRENCIES = [
  { code: "COINS", name: "Coins", cap: 1000000, archived: false },
  { code: "GEMS", name: "Gems", cap: 500, archived: false }
];
const SEED_ITEMS = [
  { sku: "sword_basic", name: "Basic Sword", kind: "item", stackable: false, maxOwned: 1,
    prices: [{ currency: "COINS", amount: 250 }], contents: [], tags: ["weapon", "durable"], custom: { damage: 10 },
    playerPurchasable: true, playerConsumable: false },
  { sku: "potion_small", name: "Small Potion", kind: "item", stackable: true, maxOwned: null,
    prices: [{ currency: "COINS", amount: 20 }], contents: [], tags: ["consumable"], custom: { heal: 25 },
    playerPurchasable: true, playerConsumable: true },
  { sku: "raffle_ticket", name: "Raffle Ticket", kind: "item", stackable: false, maxOwned: null,
    prices: [{ currency: "COINS", amount: 50 }], contents: [], tags: ["consumable", "unique"], custom: null,
    playerPurchasable: true, playerConsumable: true },
  { sku: "hat_red", name: "Red Hat", kind: "item", stackable: false, maxOwned: 1,
    prices: [{ currency: "GEMS", amount: 25 }, { currency: "COINS", amount: 300 }], contents: [], tags: ["cosmetic"],
    custom: { color: "#ff0033" }, playerPurchasable: true, playerConsumable: false },
  { sku: "starter_bundle", name: "Starter Bundle", kind: "bundle", stackable: false, maxOwned: null,
    prices: [{ currency: "COINS", amount: 500 }],
    contents: [{ sku: "potion_small", quantity: 5 }, { sku: "raffle_ticket", quantity: 2 }, { currency: "GEMS", quantity: 50 }],
    tags: ["bundle"], custom: null, playerPurchasable: true, playerConsumable: false },
  { sku: "vip_pass", name: "VIP Pass", kind: "item", stackable: false, maxOwned: 1, prices: [], contents: [],
    tags: ["studio-only"], custom: null, playerPurchasable: false, playerConsumable: false }
];
const STARTING_BALANCES = [{ currency: "COINS", amount: 1000 }];
const SEED_GAME_DATA = [
  ["motd", "Welcome to the SplatterVault mock!"],
  ["config", { maxPlayers: 8, mode: "ffa" }],
  ["event.weekend", { active: false, multiplier: 2 }]
];
// Meta product sku -> store grant. Every Meta-linked player owns these two purchases unless /__mock/meta-purchases
// replaces the list.
const META_PRODUCT_MAP = { meta_potion_pack: { sku: "potion_small", quantity: 10 }, meta_hat: { sku: "hat_red", quantity: 1 } };
const DEFAULT_META_PURCHASES = [
  { id: "meta-purchase-1", sku: "meta_potion_pack", consumable: false },
  { id: "meta-purchase-2", sku: "meta_unmapped_product", consumable: false }
];

// ---------------------------------------------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------------------------------------------
const CODE_STATUS = {
  bad_request: 400, weak_password: 400, key_invalid: 400, value_invalid: 400, idempotency_required: 400,
  unauthorized: 401, token_invalid: 401, token_expired: 401, player_deleted: 401, proof_invalid: 401,
  proof_replayed: 401, invalid_credentials: 401, code_invalid: 401, refresh_invalid: 401,
  banned: 403, forbidden: 403, not_entitled: 403, publisher_banned: 403, family_sharing_refused: 403,
  reauth_required: 403, purchases_disabled: 403, consume_disabled: 403,
  not_found: 404, game_not_found: 404, provider_not_enabled: 404, player_data_not_enabled: 404, store_not_enabled: 404,
  item_not_found: 404, currency_not_found: 404, instance_not_found: 404,
  identity_in_use: 409, provider_already_linked: 409, last_identity: 409, version_conflict: 409, out_of_range: 409,
  idempotency_conflict: 409, insufficient_funds: 409, insufficient_items: 409, currency_cap: 409, already_owned: 409,
  inventory_full: 409, price_changed: 409, meta_not_linked: 409,
  value_too_large: 413, limit_exceeded: 422, rate_limited: 429, internal_error: 500, provider_unavailable: 502,
  player_sign_in_disabled: 503, email_unavailable: 503, data_busy: 503, platform_not_available: 503
};
// Default messages: the ones the API sends for each error code (sign-in, player data and store).
const MESSAGES = {
  player_sign_in_disabled: "Player sign-in is not available right now.",
  game_not_found: "No such game.",
  provider_not_enabled: "This sign-in method is not turned on for this game.",
  bad_request: "The request is not valid.",
  proof_invalid: "The sign-in proof was not accepted.",
  proof_replayed: "This sign-in proof was already used. Get a new one and try again.",
  not_entitled: "This account does not own the game.",
  publisher_banned: "This account is banned from the game.",
  family_sharing_refused: "This game does not allow sign-in through Steam Family Sharing.",
  provider_unavailable: "The platform could not confirm the sign-in. Try again shortly.",
  identity_in_use: "That account is already signed in as a different player of this studio.",
  provider_already_linked: "This player already has an account of that kind linked.",
  last_identity: "A player must keep at least one way to sign in.",
  token_invalid: "The player token is not valid.",
  token_expired: "Token expired",
  player_deleted: "This player no longer exists.",
  forbidden: "This key cannot do that.",
  rate_limited: "Too many requests. Try again shortly.",
  invalid_credentials: "The email or password is not right.",
  code_invalid: "That code is not right, or it has expired. Ask for a new one.",
  refresh_invalid: "This sign-in has ended. Sign in again.",
  weak_password: "The password must be 8 to 72 bytes long and must not be the email address.",
  reauth_required: "Confirm it is you: send your current password, or sign in again first.",
  email_unavailable: "Email is not available right now.",
  player_data_not_enabled: "Player data is not turned on for this game. Turn it on in the game console (Player data tab).",
  key_invalid: "A key is not valid.",
  value_invalid: "A value is not valid.",
  value_too_large: "A value is too large.",
  version_conflict: "The data changed since it was read (ifVersion did not match). Read it again and retry.",
  out_of_range: "The counter would go out of its allowed range.",
  limit_exceeded: "This would go over a storage limit.",
  data_busy: "Player data is busy. Try again in a moment.",
  store_not_enabled: "The store is not turned on for this game.",
  idempotency_required: "Send an Idempotency-Key header (1 to 128 visible ASCII characters) with every store change.",
  idempotency_conflict: "This Idempotency-Key was already used for a different request.",
  insufficient_funds: "The player does not have enough of that currency.",
  insufficient_items: "The player does not hold enough of that item.",
  currency_cap: "That would take the player's balance over the currency's cap.",
  already_owned: "The player already holds the most of this item they may own.",
  inventory_full: "The player's inventory is full.",
  price_changed: "The price changed. Refresh the catalog and try again.",
  item_not_found: "No such item in this store.",
  currency_not_found: "No such currency in this store.",
  instance_not_found: "The player holds no such item.",
  purchases_disabled: "Players cannot buy this item in the game.",
  consume_disabled: "Players cannot use up this item in the game.",
  meta_not_linked: "This player has no Meta account linked for that app.",
  platform_not_available: "Platform purchase checks are not available right now.",
  not_found: "No such route. Check the method and the path.",
  unauthorized: "Invalid, expired, or revoked organization API key",
  banned: "This address or account is banned from SplatterVault.",
  internal_error: "Something went wrong on our side. Please try again."
};

class ApiError extends Error {
  constructor(code, message, details, headers) {
    super(message ?? MESSAGES[code] ?? code);
    this.code = code;
    this.status = CODE_STATUS[code] ?? 400;
    this.details = details;
    this.headers = headers;
  }
}
const fail = (code, message, details, headers) => { throw new ApiError(code, message, details, headers); };

// ---------------------------------------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------------------------------------
const b64url = (buf) => Buffer.from(buf).toString("base64url");
const now = () => Date.now();
const iso = (ms) => new Date(ms).toISOString();
const isObj = (v) => v !== null && typeof v === "object" && !Array.isArray(v) &&
  (Object.getPrototypeOf(v) === Object.prototype || Object.getPrototypeOf(v) === null);
const isSafeInt = (v) => typeof v === "number" && Number.isSafeInteger(v);
const UUID_RE = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;
const EMAIL_RE = /^[^\s@<>()[\]\\,;:"]+@[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?(?:\.[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?)+$/;
const KEY_RE = /^[A-Za-z0-9_][A-Za-z0-9_.:-]{0,127}$/;
const SKU_RE = /^[A-Za-z0-9_.:-]{1,64}$/;
const CURRENCY_RE = /^[A-Z0-9]{2,8}$/;
const IDEM_RE = /^[\x21-\x7e]{1,128}$/;
const sha = (s, alg = "sha256") => crypto.createHash(alg).update(s).digest();
const randomToken = (prefix) => prefix + b64url(crypto.randomBytes(32)); // 43 base64url chars
const clone = (v) => (v === undefined ? undefined : JSON.parse(JSON.stringify(v)));
const byCode = (a, b) => (a.currency < b.currency ? -1 : a.currency > b.currency ? 1 : 0);

/** Canonical JSON (sorted keys) for idempotency comparisons. */
function canonical(v) {
  if (Array.isArray(v)) return "[" + v.map(canonical).join(",") + "]";
  if (v && typeof v === "object") return "{" + Object.keys(v).sort().map((k) => JSON.stringify(k) + ":" + canonical(v[k])).join(",") + "}";
  return JSON.stringify(v);
}

/** The API's weak ETag: W/"<len hex>-<sha1 base64, 27 chars>". */
function weakEtag(text) {
  const len = Buffer.byteLength(text, "utf8");
  if (len === 0) return 'W/"0-2jmj7l5rSw0yVb/vlWAYkK/YBwk"';
  return 'W/"' + len.toString(16) + "-" + sha(text, "sha1").toString("base64").substring(0, 27) + '"';
}
/** The API's If-None-Match test (weak comparison). */
function etagMatches(ifNoneMatch, etag) {
  if (!ifNoneMatch || !etag) return false;
  const v = String(ifNoneMatch).trim();
  if (v === "*") return true;
  return v.split(",").map((s) => s.trim()).some((m) => m === etag || m === "W/" + etag || "W/" + m === etag);
}

function redactBody(body) {
  const SECRET = new Set(["refreshToken", "deviceSecret", "password", "currentPassword", "newPassword", "token",
    "ticket", "sessionTicket", "nonce", "code", "oldCode"]);
  const walk = (v) => {
    if (Array.isArray(v)) return v.map(walk);
    if (v && typeof v === "object") {
      const out = {};
      for (const [k, x] of Object.entries(v)) {
        out[k] = SECRET.has(k) && typeof x === "string" ? `[redacted sha256:${sha(x).toString("hex").slice(0, 8)}]` : walk(x);
      }
      return out;
    }
    return v;
  };
  return walk(body);
}

// ---------------------------------------------------------------------------------------------------------------
// The mock
// ---------------------------------------------------------------------------------------------------------------
export function createMock(options = {}) {
  const opt = {
    tokenTtl: options.tokenTtl ?? 300,
    tokenLeeway: options.tokenLeeway ?? 30,
    refreshTtl: options.refreshTtl ?? 30 * 24 * 3600,
    refreshMaxAge: options.refreshMaxAge ?? 90 * 24 * 3600,
    refreshGrace: options.refreshGrace ?? 30,
    rateLimits: options.rateLimits ?? false,
    quiet: options.quiet ?? true
  };

  // Ed25519 key from a fixed seed so the kid (and JWKS) is stable across restarts.
  const seed = sha("sv-mock-player-token-key-v1");
  const privateKey = crypto.createPrivateKey({
    key: Buffer.concat([Buffer.from("302e020100300506032b657004220420", "hex"), seed]), format: "der", type: "pkcs8"
  });
  const publicKey = crypto.createPublicKey(privateKey);
  const jwk = publicKey.export({ format: "jwk" });
  const kid = sha(jwk.x).toString("hex").slice(0, 16);

  let state;
  function freshState() {
    const s = {
      cfg: { playerSignIn: true, signUp: true, store: true, playerData: true, playerPurchases: true, playerConsume: true,
        providers: { email: true, emailCode: true, emailPassword: true, device: true, meta: true, steam: true, xsolla: true,
          playfab: true, supabase: true }, emailAvailable: true, gameActive: true },
      players: new Map(), identities: new Map(), emails: new Map(), devices: new Map(),
      codes: new Map(), sentEmails: [], refresh: new Map(), families: new Map(),
      issuedJtis: new Set(), expiredJtis: new Set(), usedNonces: new Set(),
      gameData: new Map(), gameCounter: 0,
      currencies: clone(SEED_CURRENCIES), items: clone(SEED_ITEMS),
      metaPurchases: new Map(), metaClaimed: new Set(),
      faults: [], faultSeq: 0, requests: [], requestSeq: 0, rate: new Map()
    };
    for (const [k, v] of SEED_GAME_DATA) s.gameData.set(k, { value: v, version: ++s.gameCounter, updatedAt: iso(now()) });
    return s;
  }
  state = freshState();

  // -------------------------------------------------------------------------------------------------------------
  // Rate limits (opt-in: --rate-limits)
  // -------------------------------------------------------------------------------------------------------------
  function limit(bucket, max, windowMs) {
    if (!opt.rateLimits) return;
    const t = now();
    let r = state.rate.get(bucket);
    if (!r || t >= r.resetAt) { r = { count: 0, resetAt: t + windowMs }; state.rate.set(bucket, r); }
    r.count++;
    if (r.count > max) fail("rate_limited", undefined, undefined, { "Retry-After": "60" });
  }

  // -------------------------------------------------------------------------------------------------------------
  // Tokens
  // -------------------------------------------------------------------------------------------------------------
  function issueToken(player, provider, app) {
    const iat = Math.floor(now() / 1000);
    const exp = iat + opt.tokenTtl;
    const jti = crypto.randomUUID();
    const claims = { iss: "splattervault", aud: `sv-player:${TEST.gameId}`, sub: player.id, scope: `org:${TEST.orgId}`,
      game: TEST.gameId, gk: TEST.gameKey, org: TEST.orgId, amr: [provider], jti, iat, exp };
    if (app) claims.app = app;
    const input = b64url(JSON.stringify({ alg: "EdDSA", typ: "JWT", kid })) + "." + b64url(JSON.stringify(claims));
    const token = input + "." + b64url(crypto.sign(null, Buffer.from(input), privateKey));
    state.issuedJtis.add(jti);
    return { token, expiresAt: iso(exp * 1000) };
  }

  function verifyToken(token) {
    const parts = token.split(".");
    if (parts.length !== 3) fail("token_invalid");
    let header, claims;
    try {
      header = JSON.parse(Buffer.from(parts[0], "base64url").toString("utf8"));
      claims = JSON.parse(Buffer.from(parts[1], "base64url").toString("utf8"));
    } catch { fail("token_invalid"); }
    if (!header || header.alg !== "EdDSA" || header.kid !== kid) fail("token_invalid");
    let ok = false;
    try { ok = crypto.verify(null, Buffer.from(parts[0] + "." + parts[1]), publicKey, Buffer.from(parts[2], "base64url")); } catch { ok = false; }
    if (!ok) fail("token_invalid");
    if (claims.iss !== "splattervault" || claims.aud !== `sv-player:${TEST.gameId}` || typeof claims.sub !== "string") fail("token_invalid");
    const t = now() / 1000;
    if (typeof claims.iat !== "number" || claims.iat > t + 30) fail("token_invalid");
    if (typeof claims.exp !== "number" || t > claims.exp + opt.tokenLeeway) fail("token_expired");
    if (state.expiredJtis.has(claims.jti)) fail("token_expired");
    return claims;
  }

  function authenticate(ctx) {
    if (!state.cfg.playerSignIn) fail("player_sign_in_disabled");
    const m = /^Bearer ([A-Za-z0-9_.-]+)$/i.exec(String(ctx.headers.authorization ?? ""));
    if (!m) fail("token_invalid", "A player token is required (Authorization: Bearer <token>).");
    const claims = verifyToken(m[1]);
    limit("player:" + claims.sub, 60, 60000);
    const player = state.players.get(claims.sub);
    if (!player || player.erased) fail("player_deleted");
    return { player, claims };
  }

  function newRefresh(player, family) {
    const t = now();
    let fam = family ? state.families.get(family) : null;
    if (!fam) { fam = { id: crypto.randomUUID(), playerId: player.id, startedAt: t, revoked: false }; state.families.set(fam.id, fam); }
    const expiresAt = Math.min(t + opt.refreshTtl * 1000, fam.startedAt + opt.refreshMaxAge * 1000);
    const rec = { token: randomToken("svpr1_"), family: fam.id, playerId: player.id, createdAt: t, expiresAt,
      rotatedAt: null, child: null, revokedAt: null, graceRevokedAt: null };
    state.refresh.set(rec.token, rec);
    return { refreshToken: rec.token, refreshExpiresAt: iso(expiresAt) };
  }
  function revokeFamily(familyId) {
    const fam = state.families.get(familyId);
    if (fam) fam.revoked = true;
  }
  function revokeAllSessions(playerId, exceptFamily) {
    for (const fam of state.families.values()) if (fam.playerId === playerId && fam.id !== exceptFamily) fam.revoked = true;
  }

  // -------------------------------------------------------------------------------------------------------------
  // Players
  // -------------------------------------------------------------------------------------------------------------
  function cleanDisplayName(v) {
    if (typeof v !== "string") return null;
    const s = v.trim();
    if (!s || [...s].length > 64 || /[\p{Cc}\p{Cf}]/u.test(s)) return null;
    return s;
  }

  function createPlayer(displayName) {
    const t = now();
    const p = { id: crypto.randomUUID(), displayName: cleanDisplayName(displayName), createdAt: iso(t), lastSignedInAt: null,
      identities: [], email: null, deviceSecret: null, erased: false,
      data: { player: new Map(), readonly: new Map(), internal: new Map() }, dataCounter: 0,
      balances: new Map(state.currencies.map((c) => [c.code, 0])), items: [], txns: [], idem: new Map(), metaClaims: new Set() };
    // Seed: a readonly key (only a studio backend writes that store) and the starting balance (a grant).
    p.data.readonly.set("profile.tier", { value: "bronze", version: ++p.dataCounter, updatedAt: iso(t) });
    state.players.set(p.id, p);
    if (STARTING_BALANCES.length) {
      const changes = [];
      for (const b of STARTING_BALANCES) {
        p.balances.set(b.currency, (p.balances.get(b.currency) ?? 0) + b.amount);
        changes.push({ currency: b.currency, delta: b.amount, after: p.balances.get(b.currency) });
      }
      recordTxn(p, "grant", changes);
    }
    return p;
  }

  function addIdentity(p, provider, subject, appId) {
    p.identities.push({ provider, subject, appId: appId ?? null, linkedAt: iso(now()) });
    if (provider !== "email" && provider !== "device") state.identities.set(provider + "|" + subject, p.id);
  }
  function removeIdentity(p, provider) {
    const i = p.identities.findIndex((x) => x.provider === provider);
    if (i < 0) return;
    const [idn] = p.identities.splice(i, 1);
    if (provider === "email") { state.emails.delete(p.email?.address); p.email = null; }
    else if (provider === "device") { if (p.deviceSecret) state.devices.delete(p.deviceSecret); p.deviceSecret = null; }
    else state.identities.delete(provider + "|" + idn.subject);
  }
  const identitiesOut = (p) => p.identities.map((x) => ({ provider: x.provider, linkedAt: x.linkedAt }));

  function signInAnswer(p, provider, { newPlayer = false, app = null, refresh = false, family = null, touch = true } = {}) {
    if (touch) p.lastSignedInAt = iso(now());
    const t = issueToken(p, provider, app);
    const out = { token: t.token, expiresAt: t.expiresAt,
      player: { id: p.id, displayName: p.displayName, createdAt: p.createdAt, newPlayer }, identities: identitiesOut(p) };
    if (refresh) Object.assign(out, newRefresh(p, family));
    return out;
  }
  const profile = (p) => ({ player: { id: p.id, displayName: p.displayName, createdAt: p.createdAt, lastSignedInAt: p.lastSignedInAt },
    game: { gameKey: TEST.gameKey }, identities: identitiesOut(p) });

  function erasePlayer(p) {
    for (const idn of [...p.identities]) removeIdentity(p, idn.provider);
    p.erased = true;
    p.displayName = null;
    p.data = { player: new Map(), readonly: new Map(), internal: new Map() };
    p.balances = new Map(); p.items = []; p.txns = []; p.idem = new Map();
    revokeAllSessions(p.id);
  }

  // -------------------------------------------------------------------------------------------------------------
  // Game resolution and request field checks (as the API does them)
  // -------------------------------------------------------------------------------------------------------------
  function requireBodyObject(body) {
    if (!isObj(body)) fail("bad_request", "The request body must be a JSON object.");
    return body;
  }
  function resolveGame(body) {
    if (!state.cfg.playerSignIn) fail("player_sign_in_disabled");
    requireBodyObject(body);
    const hasId = body.gameId !== undefined && body.gameId !== null;
    const hasKey = body.gameKey !== undefined && body.gameKey !== null;
    if (hasId === hasKey) fail("bad_request", "Name the game with exactly one of gameId or gameKey.");
    if (hasId) {
      const v = body.gameId;
      const n = typeof v === "number" ? v : typeof v === "string" && /^[0-9]{1,10}$/.test(v) ? Number(v) : NaN;
      if (!Number.isSafeInteger(n) || n < 1 || n > 2147483647) fail("bad_request", "gameId must be a positive integer.");
      if (n !== TEST.gameId || !state.cfg.gameActive) fail("game_not_found");
    } else {
      const v = body.gameKey;
      if (typeof v !== "string" || v.length < 1 || v.length > 255 || v.trim() !== v) fail("bad_request", "gameKey must be the game's key.");
      if (v !== TEST.gameKey || !state.cfg.gameActive) fail("game_not_found");
    }
  }
  function normalizeAppId(v) {
    if (v === undefined || v === null) return null;
    if (isSafeInt(v) && v >= 0) return String(v);
    if (typeof v === "string" && /^[\x21-\x7e]{0,64}$/.test(v)) return v;
    fail("bad_request", "appId must be the platform app id as a string.");
  }
  function checkEmail(v) {
    if (typeof v !== "string" || v.length > 254 || !EMAIL_RE.test(v.trim())) fail("bad_request", "email must be a plain email address (name@example.com).");
    return v.trim().toLowerCase();
  }
  function checkNewPassword(pw, email) {
    if (typeof pw !== "string") fail("bad_request", "password must be a string.");
    const bytes = Buffer.byteLength(pw, "utf8");
    if (bytes < 8 || bytes > 72 || (email && pw.trim().toLowerCase() === email.toLowerCase())) fail("weak_password");
  }
  const validSignInPassword = (v) => typeof v === "string" && v.length > 0 && v.length <= 1024;
  const hashPw = (pw) => sha("pw|" + pw).toString("hex");

  // -------------------------------------------------------------------------------------------------------------
  // Platform proofs (fixed test credentials)
  // -------------------------------------------------------------------------------------------------------------
  const PROVIDER_APP = { meta: TEST.metaAppId, steam: TEST.steamAppId, xsolla: TEST.xsollaAppId, playfab: TEST.playfabAppId, supabase: TEST.supabaseAppId };

  function checkProof(provider, body) {
    const appId = normalizeAppId(body.appId); // 400 before the provider lookup, as the API does
    if (!state.cfg.providers[provider]) fail("provider_not_enabled");
    if (appId !== null && appId !== "" && appId !== PROVIDER_APP[provider]) fail("provider_not_enabled");
    const app = PROVIDER_APP[provider];
    const special = (v) => {
      if (v === "not-entitled") fail("not_entitled");
      if (v === "provider-unavailable") fail("provider_unavailable");
      if (v === "banned-by-publisher") fail("publisher_banned");
    };
    if (provider === "meta") {
      if (typeof body.userId !== "string" || !/^[0-9]{1,20}$/.test(body.userId)) fail("bad_request", "userId must be the Meta user id (digits).");
      if (typeof body.nonce !== "string" || body.nonce.length < 1 || body.nonce.length > 512) fail("bad_request", "nonce must be the GetUserProof nonce.");
      special(body.nonce);
      if (body.nonce === TEST.metaNonce) return { subject: body.userId, app };
      if (body.nonce.startsWith(TEST.metaNonce + "-")) {
        if (state.usedNonces.has(body.nonce)) fail("proof_replayed");
        state.usedNonces.add(body.nonce);
        return { subject: body.userId, app };
      }
      fail("proof_invalid");
    }
    if (provider === "steam") {
      const t = body.ticket;
      if (typeof t !== "string" || !/^[0-9A-Fa-f]+$/.test(t) || t.length % 2 !== 0 || t.length < 2 || t.length > 5120) fail("bad_request", "ticket must be the Web API ticket in hex.");
      const low = t.toLowerCase();
      if (low === "0bad0bad") fail("not_entitled");
      if (low === "0fa5a11e") fail("family_sharing_refused");
      if (low === "0bb0bb") fail("provider_unavailable");
      if (!low.startsWith(TEST.steamTicket)) fail("proof_invalid");
      // deadbeef -> one Steam account; deadbeef<more hex> -> one account per suffix.
      return { subject: "7656119" + String(BigInt("0x" + (low.slice(8) || "1")) % 10000000000n).padStart(10, "0"), app };
    }
    const field = provider === "playfab" ? "sessionTicket" : "token";
    const v = body[field];
    const maxLen = provider === "playfab" ? 1024 : provider === "supabase" ? 8192 : 4096;
    if (typeof v !== "string" || v.length < 1 || v.length > maxLen) fail("bad_request", `${field} must be a string of 1 to ${maxLen} characters.`);
    special(v);
    if (v === TEST.platformToken) return { subject: provider + "-user-1", app };
    if (v.startsWith(TEST.platformToken + ":")) return { subject: provider + "-user-" + v.slice(TEST.platformToken.length + 1), app };
    fail("proof_invalid");
  }

  // -------------------------------------------------------------------------------------------------------------
  // Email codes
  // -------------------------------------------------------------------------------------------------------------
  function sendCode(purpose, email, playerId) {
    const t = now();
    const key = purpose + "|" + (playerId ?? "") + "|" + email;
    state.codes.set(key, { code: TEST.emailCode, expiresAt: t + 600000, attempts: 0 });
    state.sentEmails.push({ to: email, purpose, code: TEST.emailCode, at: iso(t) });
    if (state.sentEmails.length > 500) state.sentEmails.shift();
  }
  function useCode(purpose, email, playerId, code, consume = true) {
    const c = typeof code === "string" ? code.trim() : "";
    if (!/^[0-9]{8}$/.test(c)) fail("code_invalid");
    const key = purpose + "|" + (playerId ?? "") + "|" + email;
    const rec = state.codes.get(key);
    if (!rec || rec.expiresAt < now() || rec.attempts >= 5) fail("code_invalid");
    if (rec.code !== c) { rec.attempts++; fail("code_invalid"); }
    if (consume) state.codes.delete(key);
  }

  // -------------------------------------------------------------------------------------------------------------
  // Player data
  // -------------------------------------------------------------------------------------------------------------
  function requireGameData(kind) {
    if (!state.cfg.gameActive) fail("game_not_found", kind === "public" ? "No such game, or it has no public data." : undefined);
    if (!state.cfg.playerData) {
      if (kind === "public") fail("game_not_found", "No such game, or it has no public data.");
      fail("player_data_not_enabled");
    }
  }
  const validKey = (k) => typeof k === "string" && KEY_RE.test(k) && !k.toLowerCase().startsWith("sv.") && k !== "__proto__";
  function parseKeys(q) {
    if (q === null) return null;
    const keys = [...new Set(q.split(",").map((s) => s.trim()))];
    if (keys.length > 100 || keys.some((k) => !validKey(k))) fail("key_invalid", "keys must be a comma-separated list of at most 100 valid keys.");
    return keys;
  }
  const entryOut = (e, keysOnly) => (keysOnly ? { version: e.version, updatedAt: e.updatedAt } : { value: clone(e.value), version: e.version, updatedAt: e.updatedAt });
  function storeOut(map, keys, keysOnly) {
    const out = {};
    for (const [k, e] of map) if (!keys || keys.includes(k)) out[k] = entryOut(e, keysOnly);
    return out;
  }
  function gameDataEtag(keys) {
    const parts = [String(TEST.gameId)];
    for (const [k, e] of [...state.gameData].sort((a, b) => (a[0] < b[0] ? -1 : 1))) if (!keys || keys.includes(k)) parts.push(`${k}:${e.version}:${e.updatedAt}`);
    return 'W/"pd-' + sha(parts.join("\n")).toString("base64url").slice(0, 27) + '"';
  }
  function jsonDepth(v, d = 0) {
    if (d > 32) return d;
    if (Array.isArray(v)) return Math.max(d + 1, ...v.map((x) => jsonDepth(x, d + 1)));
    if (v && typeof v === "object") return Math.max(d + 1, ...Object.values(v).map((x) => jsonDepth(x, d + 1)));
    return d;
  }

  function validateOps(body) {
    requireBodyObject(body);
    for (const k of Object.keys(body)) if (k !== "ops") fail("bad_request", "The body is { ops: [...] }.");
    if (!Array.isArray(body.ops) || body.ops.length < 1) fail("bad_request", "ops must be a list of 1 to 25 ops.");
    if (body.ops.length > 25) fail("bad_request", "At most 25 ops in one request.", { maxOps: 25 });
    const ALLOWED = { set: ["op", "store", "key", "value", "ifVersion"], delete: ["op", "store", "key", "ifVersion"],
      inc: ["op", "store", "key", "by", "field", "min", "max", "ifVersion"] };
    body.ops.forEach((op, i) => {
      if (!isObj(op)) fail("bad_request", `ops[${i}] must be an object.`, { op: i });
      if (!ALLOWED[op.op]) fail("bad_request", `ops[${i}].op must be set, delete or inc.`, { op: i });
      for (const k of Object.keys(op)) if (!ALLOWED[op.op].includes(k)) fail("bad_request", `ops[${i}] has an unknown field: ${k}.`, { op: i });
      if (!["player", "readonly", "internal"].includes(op.store)) fail("bad_request", `ops[${i}].store must be player.`, { op: i });
      if (!validKey(op.key)) fail("key_invalid", `ops[${i}].key is not a valid key.`, { op: i });
      if (op.ifVersion !== undefined && !(isSafeInt(op.ifVersion) && op.ifVersion >= 0)) fail("bad_request", `ops[${i}].ifVersion must be a whole number of at least 0.`, { op: i });
      if (op.op === "set") {
        if (op.value === undefined || op.value === null) fail("value_invalid", `ops[${i}].value must be a JSON value (not null).`, { op: i });
        if (jsonDepth(op.value) > 32) fail("value_invalid", `ops[${i}].value is nested too deep.`, { op: i });
        if (Buffer.byteLength(JSON.stringify(op.value), "utf8") > 32768) fail("value_too_large", undefined, { op: i, maxBytes: 32768 });
      }
      if (op.op === "inc") {
        if (!isSafeInt(op.by)) fail("bad_request", `ops[${i}].by must be a whole number.`, { op: i });
        if (op.field !== undefined && !validKey(op.field)) fail("key_invalid", `ops[${i}].field is not a valid key.`, { op: i });
        for (const f of ["min", "max"]) if (op[f] !== undefined && !isSafeInt(op[f])) fail("bad_request", `ops[${i}].${f} must be a whole number.`, { op: i });
        if (op.min !== undefined && op.max !== undefined && op.min > op.max) fail("bad_request", `ops[${i}]: min is above max.`, { op: i });
      }
    });
    if (body.ops.some((op) => op.store !== "player")) fail("forbidden", "Players can write only their player store.");
    return body.ops;
  }

  function applyOps(p, ops) {
    const work = new Map([...p.data.player].map(([k, e]) => [k, { ...e }]));
    let counter = p.dataCounter;
    const conflicts = [];
    const results = [];
    const t = iso(now());
    ops.forEach((op, i) => {
      const cur = work.get(op.key);
      if (op.ifVersion !== undefined && (cur ? cur.version : 0) !== op.ifVersion) {
        conflicts.push({ store: op.store, key: op.key, version: cur ? cur.version : null });
        results.push(null);
        return;
      }
      if (op.op === "set") {
        const e = { value: clone(op.value), version: ++counter, updatedAt: t };
        work.set(op.key, e);
        results.push({ store: op.store, key: op.key, version: e.version });
      } else if (op.op === "delete") {
        if (cur) work.delete(op.key);
        results.push({ store: op.store, key: op.key, version: null });
      } else {
        const msg = "inc applies only to a whole number (or a missing value), or to a whole-number field of an object.";
        let value;
        let base;
        if (op.field !== undefined) {
          const obj = cur ? cur.value : {};
          if (!isObj(obj)) fail("value_invalid", msg, { op: i, store: op.store, key: op.key });
          base = obj[op.field] === undefined ? 0 : obj[op.field];
          if (!isSafeInt(base)) fail("value_invalid", msg, { op: i, store: op.store, key: op.key });
          value = { ...clone(obj) };
        } else {
          base = cur ? cur.value : 0;
          if (!isSafeInt(base)) fail("value_invalid", msg, { op: i, store: op.store, key: op.key });
        }
        const next = base + op.by;
        if ((op.min !== undefined && next < op.min) || (op.max !== undefined && next > op.max) || !Number.isSafeInteger(next)) {
          const d = { op: i, store: op.store, key: op.key };
          if (op.min !== undefined) d.min = op.min;
          if (op.max !== undefined) d.max = op.max;
          fail("out_of_range", undefined, d);
        }
        if (op.field !== undefined) value[op.field] = next; else value = next;
        const e = { value, version: ++counter, updatedAt: t };
        work.set(op.key, e);
        results.push({ store: op.store, key: op.key, version: e.version, value: clone(value) });
      }
    });
    if (conflicts.length) fail("version_conflict", undefined, conflicts);
    // Limits (only when usage grows).
    const bytes = [...work].reduce((n, [k, e]) => n + Buffer.byteLength(k) + Buffer.byteLength(JSON.stringify(e.value)), 0);
    const oldBytes = [...p.data.player].reduce((n, [k, e]) => n + Buffer.byteLength(k) + Buffer.byteLength(JSON.stringify(e.value)), 0);
    if (work.size > 100 && work.size > p.data.player.size) fail("limit_exceeded", undefined, { limit: "player_store_keys", max: 100 });
    if (bytes > 262144 && bytes > oldBytes) fail("limit_exceeded", undefined, { limit: "player_store_bytes", max: 262144 });
    p.data.player = work;
    p.dataCounter = counter;
    return results;
  }

  // -------------------------------------------------------------------------------------------------------------
  // Store
  // -------------------------------------------------------------------------------------------------------------
  function requireStore() {
    if (!state.cfg.gameActive) fail("game_not_found");
    if (!state.cfg.store) fail("store_not_enabled");
  }
    function catalogOut() {
    const s = state.cfg;
    const active = new Set(state.currencies.filter((c) => !c.archived).map((c) => c.code));
    return {
      settings: { playerPurchases: s.playerPurchases, playerConsume: s.playerConsume },
      currencies: state.currencies.filter((c) => !c.archived).map((c) => ({ code: c.code, name: c.name, cap: c.cap })),
      items: state.items.filter((i) => !i.archived).map((i) => ({
        sku: i.sku, name: i.name, kind: i.kind, stackable: i.stackable, maxOwned: i.maxOwned,
        prices: i.prices.filter((pr) => active.has(pr.currency)).map((pr) => ({ ...pr })), contents: clone(i.contents),
        tags: [...i.tags], custom: clone(i.custom),
        purchasable: s.playerPurchases && i.playerPurchasable, consumable: s.playerConsume && i.playerConsumable
      }))
    };
  }
  const instanceOut = (x) => ({ instanceId: x.instanceId, sku: x.sku, quantity: x.quantity, custom: clone(x.custom), acquiredAt: x.acquiredAt });
  function holdingsOut(p) {
    const balances = [...p.balances].map(([currency, amount]) => ({ currency, amount })).sort(byCode);
    return { player: { id: p.id }, balances, items: [...p.items].sort((a, b) => (a.acquiredAt < b.acquiredAt ? -1 : 1)).map(instanceOut) };
  }
  function recordTxn(p, kind, changes) {
    const txn = { id: crypto.randomUUID(), kind, createdAt: iso(now()), reason: null, reverses: null };
    p.txns.unshift({ id: txn.id, kind, createdAt: txn.createdAt, reverses: null, changes: clone(changes), shortfall: [] });
    return txn;
  }
  const ownedCount = (p, sku) => p.items.filter((x) => x.sku === sku).reduce((n, x) => n + x.quantity, 0);

  /** Grants item/currency lines into p (mutates; caller works on a copy). Returns changes and touched ids. */
  function grantLines(p, lines, touched) {
    const changes = [];
    for (const line of lines) {
      if (line.currency) {
        const cur = state.currencies.find((c) => c.code === line.currency);
        const after = (p.balances.get(line.currency) ?? 0) + line.quantity;
        if (cur && cur.cap !== null && after > cur.cap) fail("currency_cap");
        p.balances.set(line.currency, after);
        touched.currencies.add(line.currency);
        changes.push({ currency: line.currency, delta: line.quantity, after });
        continue;
      }
      const item = state.items.find((i) => i.sku === line.sku);
      if (!item) fail("item_not_found");
      if (item.kind === "bundle") { changes.push(...grantLines(p, item.contents.map((c) => ({ ...c, quantity: c.quantity * line.quantity })), touched)); continue; }
      if (item.maxOwned !== null && ownedCount(p, item.sku) + line.quantity > item.maxOwned) fail("already_owned");
      if (item.stackable) {
        let stack = p.items.find((x) => x.sku === item.sku);
        if (!stack) {
          if (p.items.length >= 10000) fail("inventory_full");
          stack = { instanceId: crypto.randomUUID(), sku: item.sku, quantity: 0, custom: null, acquiredAt: iso(now()) };
          p.items.push(stack);
        }
        stack.quantity += line.quantity;
        touched.instances.add(stack.instanceId);
        changes.push({ sku: item.sku, instanceId: stack.instanceId, delta: line.quantity, after: stack.quantity });
      } else {
        if (line.quantity > 100) fail("bad_request", "At most 100 of a unique item can be added in one line.");
        for (let n = 0; n < line.quantity; n++) {
          if (p.items.length >= 10000) fail("inventory_full");
          const x = { instanceId: crypto.randomUUID(), sku: item.sku, quantity: 1, custom: null, acquiredAt: iso(now()) };
          p.items.push(x);
          touched.instances.add(x.instanceId);
          changes.push({ sku: item.sku, instanceId: x.instanceId, delta: 1, after: 1 });
        }
      }
    }
    return changes;
  }

  /** Runs fn on a copy of the player's holdings; commits only when it does not throw. */
  function storeTransaction(p, kind, fn) {
    const work = { balances: new Map(p.balances), items: p.items.map((x) => ({ ...x })) };
    const touched = { currencies: new Set(), instances: new Set() };
    const changes = fn(work, touched);
    p.balances = work.balances;
    p.items = work.items;
    const txn = recordTxn(p, kind, changes);
    return {
      txn, changes,
      balances: [...touched.currencies].sort().map((c) => ({ currency: c, amount: p.balances.get(c) ?? 0 })),
      items: p.items.filter((x) => touched.instances.has(x.instanceId)).map(instanceOut),
      shortfall: [], replayed: false
    };
  }

  function idempotencyKey(ctx) {
    const k = ctx.headers["idempotency-key"];
    if (k === undefined || k === "") fail("idempotency_required");
    if (!IDEM_RE.test(String(k))) fail("idempotency_required");
    return "p:" + k;
  }
  function replayOrNull(p, key, canon) {
    const prior = p.idem.get(key);
    if (!prior) return null;
    if (prior.canonical !== canon) fail("idempotency_conflict");
    return { status: 200, body: { ...clone(prior.result), replayed: true } };
  }

  // -------------------------------------------------------------------------------------------------------------
  // Routes
  // -------------------------------------------------------------------------------------------------------------
  const routes = [];
  const route = (method, pattern, handler, opts = {}) => {
    const names = [];
    const re = new RegExp("^" + pattern.replace(/\{(\w+)\}/g, (_, n) => { names.push(n); return "([^/]+)"; }) + "/?$", "i");
    routes.push({ method, pattern, re, names, handler, auth: !!opts.auth });
  };

  // ---- Player sign-in ------------------------------------------------------------------------------------------
  route("POST", "/rest/player-auth/email/code", (ctx) => {
    const b = ctx.body;
    resolveGame(b);
    if (!state.cfg.providers.email) fail("provider_not_enabled");
    if (!state.cfg.emailAvailable) fail("email_unavailable");
    const email = checkEmail(b.email);
    limit("email:" + email, 1, 60000);
    if (state.emails.has(email) || state.cfg.signUp) sendCode("signin", email, null);
    return { status: 202, body: { expiresInSeconds: 600, resendAfterSeconds: 60 } };
  });

  route("POST", "/rest/player-auth/email/code/verify", (ctx) => {
    const b = ctx.body;
    resolveGame(b);
    if (!state.cfg.providers.email) fail("provider_not_enabled");
    const email = checkEmail(b.email);
    const wantsPassword = b.password !== undefined && b.password !== null;
    if (!state.cfg.providers.emailCode && !wantsPassword) {
      fail("bad_request", "This game signs in with a password: send password with the code (to set a new one), or use /player-auth/email/password.");
    }
    // Sign-in code check, in the API's order: a malformed code is 401 (not 400), then the new password, then the
    // code itself (a wrong one counts an attempt); no account and no sign-up leaves the code unspent.
    if (typeof b.code !== "string" || !/^[0-9]{8}$/.test(b.code.trim())) fail("code_invalid");
    if (wantsPassword) checkNewPassword(b.password, email);
    const existingId = state.emails.get(email);
    useCode("signin", email, null, b.code, !!existingId || state.cfg.signUp);
    if (!existingId && !state.cfg.signUp) fail("code_invalid");
    let p = existingId ? state.players.get(existingId) : null;
    let isNew = false;
    if (!p) {
      p = createPlayer(b.displayName);
      p.email = { address: email, password: wantsPassword ? hashPw(b.password) : null };
      state.emails.set(email, p.id);
      addIdentity(p, "email", email);
      isNew = true;
    } else if (wantsPassword) {
      p.email.password = hashPw(b.password);
      revokeAllSessions(p.id); // a reset: the other sessions end
    }
    return { status: 200, body: signInAnswer(p, "email", { newPlayer: isNew, refresh: true }) };
  });

  route("POST", "/rest/player-auth/email/password", (ctx) => {
    const b = ctx.body;
    resolveGame(b);
    if (!state.cfg.providers.email) fail("provider_not_enabled");
    if (!state.cfg.providers.emailPassword) fail("provider_not_enabled", "Password sign-in is not turned on for this game.");
    const email = checkEmail(b.email);
    if (typeof b.password !== "string" || b.password.length < 1 || b.password.length > 1024) fail("bad_request", "password must be a string of 1 to 1024 characters.");
    const id = state.emails.get(email);
    const p = id ? state.players.get(id) : null;
    if (!p || !p.email?.password || p.email.password !== hashPw(b.password)) {
      limit("pwfail:" + email, 10, 15 * 60000);
      fail("invalid_credentials");
    }
    return { status: 200, body: signInAnswer(p, "email", { refresh: true }) };
  });

  route("POST", "/rest/player-auth/device/new", (ctx) => {
    const b = ctx.body;
    resolveGame(b);
    if (!state.cfg.providers.device) fail("provider_not_enabled");
    limit("guest:" + ctx.ip, 10, 3600000);
    const p = createPlayer(b.displayName);
    p.deviceSecret = randomToken("svpd1_");
    state.devices.set(p.deviceSecret, p.id);
    addIdentity(p, "device", p.id);
    return { status: 200, body: { ...signInAnswer(p, "device", { newPlayer: true }), deviceSecret: p.deviceSecret } };
  });

  route("POST", "/rest/player-auth/device", (ctx) => {
    const b = ctx.body;
    resolveGame(b);
    if (!state.cfg.providers.device) fail("provider_not_enabled");
    if (typeof b.deviceSecret !== "string" || !/^svpd1_[A-Za-z0-9_-]{43}$/.test(b.deviceSecret)) {
      fail("bad_request", "deviceSecret must be the secret POST /player-auth/device/new returned.");
    }
    const id = state.devices.get(b.deviceSecret);
    const p = id ? state.players.get(id) : null;
    if (!p || p.erased) fail("proof_invalid", "This guest account does not exist (or it was removed).");
    return { status: 200, body: signInAnswer(p, "device") };
  });

  route("POST", "/rest/player-auth/refresh", (ctx) => {
    if (!state.cfg.playerSignIn) fail("player_sign_in_disabled");
    const b = requireBodyObject(ctx.body);
    if (typeof b.refreshToken !== "string") fail("bad_request", "refreshToken must be a string.");
    const rec = state.refresh.get(b.refreshToken);
    if (!rec) fail("refresh_invalid");
    const fam = state.families.get(rec.family);
    const p = state.players.get(rec.playerId);
    if (!fam || fam.revoked || rec.revokedAt) fail("refresh_invalid");
    if (rec.graceRevokedAt) { revokeFamily(rec.family); fail("refresh_invalid"); }
    if (rec.expiresAt < now()) fail("refresh_invalid");
    if (!p || p.erased) fail("refresh_invalid");
    if (!state.cfg.gameActive) fail("refresh_invalid", "This game is not available. Sign in again.");
    if (!state.cfg.providers.email) fail("refresh_invalid", "Email sign-in is off for this game.");
    if (rec.rotatedAt !== null) {
      const child = rec.child ? state.refresh.get(rec.child) : null;
      const withinGrace = now() - rec.rotatedAt <= opt.refreshGrace * 1000;
      if (!withinGrace || !child || child.rotatedAt !== null || child.graceRevokedAt) { revokeFamily(rec.family); fail("refresh_invalid"); }
      child.graceRevokedAt = now(); // the unused newer token is replaced
    } else {
      rec.rotatedAt = now();
    }
    const answer = signInAnswer(p, "email", { refresh: true, family: rec.family, touch: false });
    rec.child = answer.refreshToken;
    return { status: 200, body: answer };
  });

  route("POST", "/rest/player-auth/sign-out", (ctx) => {
    if (!state.cfg.playerSignIn) fail("player_sign_in_disabled");
    requireBodyObject(ctx.body); // as the API: a missing body is 400 too
    const t = ctx.body.refreshToken;
    const rec = typeof t === "string" ? state.refresh.get(t) : null;
    if (rec) revokeFamily(rec.family);
    return { status: 204 };
  });

  route("GET", "/rest/player-auth/keys", () => {
    if (!state.cfg.playerSignIn) fail("player_sign_in_disabled", "Player sign-in is off on this server.");
    return { status: 200, headers: { "Cache-Control": "private, max-age=300" },
      body: { keys: [{ kty: "OKP", crv: "Ed25519", x: jwk.x, kid, alg: "EdDSA", use: "sig" }] } };
  });

  for (const provider of ["xsolla", "meta", "steam", "playfab", "supabase"]) {
    route("POST", "/rest/player-auth/" + provider, (ctx) => {
      const b = ctx.body;
      resolveGame(b);
      const { subject, app } = checkProof(provider, b);
      const id = state.identities.get(provider + "|" + subject);
      let p = id ? state.players.get(id) : null;
      let isNew = false;
      if (!p) { p = createPlayer(null); addIdentity(p, provider, subject, app); isNew = true; }
      return { status: 200, body: signInAnswer(p, provider, { newPlayer: isNew, app }) };
    });
  }

  route("GET", "/rest/player-auth/me", (ctx) => ({ status: 200, body: profile(ctx.auth.player) }), { auth: true });

  route("DELETE", "/rest/player-auth/me", (ctx) => {
    erasePlayer(ctx.auth.player);
    return { status: 204 };
  }, { auth: true });

  // Link / unlink email code requests
  route("POST", "/rest/player-auth/link/email/code", (ctx) => {
    const p = ctx.auth.player;
    const b = requireBodyObject(ctx.body);
    if (b.unlink === true) {
      if (!state.cfg.emailAvailable) fail("email_unavailable");
      if (!p.email) fail("bad_request", "This player has no email to unlink.");
      if (p.email.password) fail("bad_request", "This account has a password: send currentPassword with DELETE /player-auth/link/email.");
      limit("linkcode:" + p.id, 3, 3600000);
      sendCode("old", p.email.address, p.id);
      return { status: 202, body: { expiresInSeconds: 600, resendAfterSeconds: 60 } };
    }
    if (!state.cfg.providers.email) fail("provider_not_enabled");
    if (!state.cfg.emailAvailable) fail("email_unavailable");
    const email = checkEmail(b.email);
    limit("linkcode:" + p.id, 3, 3600000);
    sendCode("link", email, p.id);
    if (p.email && !p.email.password && p.email.address !== email) sendCode("old", p.email.address, p.id);
    return { status: 202, body: { expiresInSeconds: 600, resendAfterSeconds: 60 } };
  }, { auth: true });

  // Password change
  route("PUT", "/rest/player-auth/me/password", (ctx) => {
    const p = ctx.auth.player;
    const b = requireBodyObject(ctx.body);
    if (!state.cfg.providers.email) fail("provider_not_enabled");
    if (!state.cfg.providers.emailPassword) fail("provider_not_enabled", "Password sign-in is not turned on for this game.");
    if (!p.email) fail("bad_request", "Add an email to the account first (POST /player-auth/link/email).");
    checkNewPassword(b.newPassword, p.email.address);
    if (!p.email.password) {
      fail("reauth_required", "This account has no password yet. Set one with POST /player-auth/email/code/verify: ask for a code " +
        "(POST /player-auth/email/code) and send it with the new password.");
    }
    if (b.currentPassword === undefined || b.currentPassword === null) fail("reauth_required", "Send your current password.");
    if (!validSignInPassword(b.currentPassword) || p.email.password !== hashPw(b.currentPassword)) fail("invalid_credentials", "The current password is not right.");
    p.email.password = hashPw(b.newPassword);
    revokeAllSessions(p.id);
    return { status: 200, body: newRefresh(p, null) };
  }, { auth: true });

  // Email removal
  function removeEmail(p, b) {
    if (!p.email) return profile(p);
    if (p.email.password) {
      if (b.currentPassword === undefined || b.currentPassword === null) fail("reauth_required", "Send your current password (currentPassword) to remove the email.");
      if (!validSignInPassword(b.currentPassword) || p.email.password !== hashPw(b.currentPassword)) fail("invalid_credentials", "The current password is not right.");
    } else {
      if (b.oldCode === undefined || b.oldCode === null) {
        fail("reauth_required", "Send the code we email to this address (POST /player-auth/link/email/code with unlink: true) as oldCode.");
      }
      if (typeof b.oldCode !== "string" || !/^[0-9]{8}$/.test(b.oldCode.trim())) fail("code_invalid");
    }
    if (p.identities.length <= 1) fail("last_identity");
    if (!p.email.password) useCode("old", p.email.address, p.id, b.oldCode);
    removeIdentity(p, "email");
    revokeAllSessions(p.id);
    return profile(p);
  }

  route("POST", "/rest/player-auth/link/email/remove", (ctx) => {
    const b = ctx.body === undefined ? {} : requireBodyObject(ctx.body);
    return { status: 200, body: removeEmail(ctx.auth.player, b) };
  }, { auth: true });

  // Linking an email (provider email) and a platform
  function linkEmail(p, b) {
    if (!state.cfg.providers.email) fail("provider_not_enabled");
    const email = checkEmail(b.email);
    if (typeof b.code !== "string" || !/^[0-9]{8}$/.test(b.code.trim())) fail("code_invalid");
    const wantsPassword = b.password !== undefined && b.password !== null;
    if (wantsPassword) checkNewPassword(b.password, email);
    const current = p.email;
    const stored = current?.password ?? null;
    const isChange = !!current && current.address !== email;
    if (wantsPassword && current) {
      fail("bad_request", "This player already has an email account: set a first password with POST /player-auth/email/code/verify (password " +
        "with a code), or change it with PUT /player-auth/me/password.");
    }
    const oldCode = isChange && !stored && typeof b.oldCode === "string" && /^[0-9]{8}$/.test(b.oldCode.trim()) ? b.oldCode : null;
    if (isChange && stored && (b.currentPassword === undefined || b.currentPassword === null)) {
      fail("reauth_required", "Send your current password (currentPassword) to change the email.");
    }
    if (isChange && !stored && !oldCode) fail("reauth_required", "Send the code we emailed to your current address (oldCode) to change the email.");
    // The new address's code first; the current password only for a code that is right.
    const rec = state.codes.get("link|" + p.id + "|" + email);
    const codeLooksRight = !!rec && rec.expiresAt >= now() && rec.attempts < 5 && rec.code === b.code.trim();
    if (codeLooksRight && isChange && stored && (!validSignInPassword(b.currentPassword) || stored !== hashPw(b.currentPassword))) {
      fail("invalid_credentials", "The current password is not right.");
    }
    useCode("link", email, p.id, b.code, false);
    if (isChange && !stored) useCode("old", current.address, p.id, oldCode);
    const holder = state.emails.get(email);
    if (holder && holder !== p.id) fail("identity_in_use");
    useCode("link", email, p.id, b.code);
    if (!current) {
      p.email = { address: email, password: wantsPassword ? hashPw(b.password) : null };
      state.emails.set(email, p.id);
      addIdentity(p, "email", email);
      return profile(p);
    }
    if (!isChange) return profile(p); // the same address: verified again, nothing else changes
    state.emails.delete(current.address);
    current.address = email;
    state.emails.set(email, p.id);
    const idn = p.identities.find((x) => x.provider === "email");
    if (idn) idn.subject = email;
    revokeAllSessions(p.id);
    return { ...profile(p), ...newRefresh(p, null) };
  }

  route("POST", "/rest/player-auth/link/{provider}", (ctx) => {
    const p = ctx.auth.player;
    const provider = ctx.params.provider;
    limit("link:" + p.id, 60, 60000);
    if (provider === "email") return { status: 200, body: linkEmail(p, requireBodyObject(ctx.body)) };
    if (!["xsolla", "meta", "steam", "playfab", "supabase"].includes(provider)) {
      fail("bad_request", "provider must be xsolla, meta, steam, playfab, supabase or email.");
    }
    const b = requireBodyObject(ctx.body);
    const { subject, app } = checkProof(provider, b);
    const holder = state.identities.get(provider + "|" + subject);
    if (holder === p.id) return { status: 200, body: profile(p) };
    if (holder) fail("identity_in_use", "Another player of this studio holds that account.");
    if (p.identities.some((x) => x.provider === provider)) fail("provider_already_linked");
    addIdentity(p, provider, subject, app);
    return { status: 200, body: profile(p) };
  }, { auth: true });

  route("DELETE", "/rest/player-auth/link/{provider}", (ctx) => {
    const p = ctx.auth.player;
    const provider = ctx.params.provider;
    if (provider === "email") return { status: 200, body: removeEmail(p, ctx.body === undefined ? {} : requireBodyObject(ctx.body)) };
    if (!["xsolla", "meta", "steam", "playfab", "supabase", "device"].includes(provider)) {
      fail("bad_request", "provider must be xsolla, meta, steam, playfab, supabase, email or device.");
    }
    if (!p.identities.some((x) => x.provider === provider)) return { status: 200, body: profile(p) };
    if (p.identities.length <= 1) fail("last_identity");
    removeIdentity(p, provider);
    return { status: 200, body: profile(p) };
  }, { auth: true });

  // ---- Player data -----------------------------------------------------------------------------------------------
  route("GET", "/rest/player-data/me", (ctx) => {
    requireGameData();
    let stores = ["player", "readonly"];
    const sq = ctx.query.get("stores");
    if (sq !== null) {
      stores = sq.split(",").map((s) => s.trim());
      if (!stores.length || stores.some((s) => s !== "player" && s !== "readonly") || new Set(stores).size !== stores.length) {
        fail("bad_request", "stores must be a comma-separated list of player and readonly.");
      }
    }
    const keys = parseKeys(ctx.query.get("keys"));
    const ko = ctx.query.get("keysOnly");
    const keysOnly = ko === "true" || ko === "1";
    const data = {};
    for (const s of stores) data[s] = storeOut(ctx.auth.player.data[s], keys, keysOnly);
    return { status: 200, body: { data } };
  }, { auth: true });

  route("PATCH", "/rest/player-data/me", (ctx) => {
    requireGameData();
    const ops = validateOps(ctx.body);
    limit("datawrite:" + ctx.auth.player.id, 30, 60000);
    return { status: 200, body: { results: applyOps(ctx.auth.player, ops) } };
  }, { auth: true });

  route("GET", "/rest/player-data/game", (ctx) => {
    requireGameData();
    const keys = parseKeys(ctx.query.get("keys"));
    return { status: 200, headers: { ETag: gameDataEtag(keys) }, body: { data: storeOut(state.gameData, keys, false) } };
  }, { auth: true });

  route("GET", "/rest/player-data/games/{game}/public", (ctx) => {
    limit("public:" + ctx.ip, 120, 60000);
    const g = decodeURIComponent(ctx.params.game);
    const isId = /^[0-9]{1,10}$/.test(g) && Number(g) <= 2147483647;
    const ok = isId ? Number(g) === TEST.gameId : /^[A-Za-z0-9_.:-]{1,255}$/.test(g) && g === TEST.gameKey;
    if (!ok) fail("game_not_found", "No such game, or it has no public data.");
    requireGameData("public");
    const keys = parseKeys(ctx.query.get("keys"));
    return { status: 200, headers: { ETag: gameDataEtag(keys), "Cache-Control": "public, max-age=30" },
      body: { data: storeOut(state.gameData, keys, false) } };
  });

  // ---- Store -----------------------------------------------------------------------------------------------------
  route("GET", "/rest/store/catalog", () => { requireStore(); return { status: 200, body: catalogOut() }; }, { auth: true });
  route("GET", "/rest/store/me", (ctx) => { requireStore(); return { status: 200, body: holdingsOut(ctx.auth.player) }; }, { auth: true });

  route("GET", "/rest/store/me/history", (ctx) => {
    const before = ctx.query.get("before");
    if (before !== null && !UUID_RE.test(before)) fail("bad_request", "before must be an id (uuid).");
    requireStore();
    const txns = ctx.auth.player.txns;
    let start = 0;
    if (before !== null) {
      const i = txns.findIndex((t) => t.id === before.toLowerCase());
      if (i < 0) return { status: 200, body: { txns: [], next: null } };
      start = i + 1;
    }
    const page = txns.slice(start, start + 50);
    const more = txns.length > start + 50;
    return { status: 200, body: { txns: clone(page), next: more ? page[page.length - 1].id : null } };
  }, { auth: true });

  route("POST", "/rest/store/me/purchases", (ctx) => {
    const p = ctx.auth.player;
    const key = idempotencyKey(ctx);
    const b = requireBodyObject(ctx.body);
    requireStore();
    if (typeof b.sku !== "string" || !SKU_RE.test(b.sku)) fail("bad_request", "sku must be 1 to 64 characters: letters, digits and _ . : -");
    if (typeof b.currency !== "string" || !CURRENCY_RE.test(b.currency)) fail("bad_request", "currency must be a currency code: 2 to 8 capital letters or digits.");
    const quantity = b.quantity === undefined ? 1 : b.quantity;
    if (!isSafeInt(quantity) || quantity < 1 || quantity > 100) fail("bad_request", "quantity must be a whole number from 1 to 100.");
    if (b.expectedPrice === undefined || b.expectedPrice === null) fail("bad_request", "expectedPrice (the unit price the player saw) is required.");
    if (!isSafeInt(b.expectedPrice) || b.expectedPrice < 0) fail("bad_request", "expectedPrice must be a whole number of at least 0.");
    const canon = canonical({ kind: "purchase", scope: `org:${TEST.orgId}`, reason: null, capPolicy: "enforce",
      sku: b.sku, currency: b.currency, quantity, expectedPrice: b.expectedPrice });
    const replay = replayOrNull(p, key, canon);
    if (replay) return replay;
    if (!state.cfg.playerPurchases) fail("purchases_disabled");
    const item = state.items.find((i) => i.sku === b.sku);
    if (!item) fail("item_not_found", "sku: no such item in this store.");
    if (item.archived) fail("item_not_found");
    if (!item.playerPurchasable) fail("purchases_disabled");
    const cur = state.currencies.find((c) => c.code === b.currency);
    if (!cur) fail("currency_not_found", "currency: no such currency in this store.");
    if (cur.archived) fail("currency_not_found");
    const price = item.prices.find((pr) => pr.currency === b.currency);
    if (!price) fail("price_changed", "This item has no price in that currency. Refresh the catalog.");
    if (price.amount !== b.expectedPrice) fail("price_changed");
    const result = storeTransaction(p, "purchase", (work, touched) => {
      const changes = [];
      const total = price.amount * quantity;
      if (total > 0) {
        const have = work.balances.get(b.currency) ?? 0;
        if (have < total) fail("insufficient_funds");
        work.balances.set(b.currency, have - total);
        touched.currencies.add(b.currency);
        changes.push({ currency: b.currency, delta: -total, after: have - total });
      }
      changes.push(...grantLines(work, [{ sku: item.sku, quantity }], touched));
      return changes;
    });
    p.idem.set(key, { canonical: canon, result: clone(result) });
    return { status: 201, body: result };
  }, { auth: true });

  route("POST", "/rest/store/me/consume", (ctx) => {
    const p = ctx.auth.player;
    const key = idempotencyKey(ctx);
    const b = requireBodyObject(ctx.body);
    requireStore();
    if (b.currency !== undefined) fail("bad_request", "Players spend currency by buying items.");
    const named = ["sku", "instanceId"].filter((k) => b[k] !== undefined);
    if (named.length !== 1) fail("bad_request", "lines[0]: name exactly one of sku or instanceId.");
    if (b.quantity !== undefined && (!isSafeInt(b.quantity) || b.quantity < 1 || b.quantity > 1000000)) {
      fail("bad_request", "lines[0].quantity must be a whole number from 1 to 1000000.");
    }
    let line;
    if (b.instanceId !== undefined) {
      if (typeof b.instanceId !== "string" || !UUID_RE.test(b.instanceId)) fail("bad_request", "lines[0].instanceId must be an id (uuid).");
      line = { instanceId: b.instanceId.toLowerCase() };
      if (b.quantity !== undefined) line.quantity = b.quantity;
    } else {
      if (typeof b.sku !== "string" || !SKU_RE.test(b.sku)) fail("bad_request", "lines[0].sku must be 1 to 64 characters: letters, digits and _ . : -");
      if (b.quantity === undefined) fail("bad_request", "lines[0].quantity is required with sku.");
      line = { sku: b.sku, quantity: b.quantity };
    }
    const canon = canonical({ kind: "consume", scope: `org:${TEST.orgId}`, reason: null, capPolicy: "enforce", lines: [line] });
    const replay = replayOrNull(p, key, canon);
    if (replay) return replay;
    if (!state.cfg.playerConsume) fail("consume_disabled");
    const result = storeTransaction(p, "consume", (work, touched) => {
      const changes = [];
      const take = (x, q) => {
        x.quantity -= q;
        changes.push({ sku: x.sku, instanceId: x.instanceId, delta: -q, after: x.quantity });
        touched.instances.add(x.instanceId);
        if (x.quantity === 0) work.items.splice(work.items.indexOf(x), 1);
      };
      if (line.instanceId) {
        const x = work.items.find((i) => i.instanceId === line.instanceId);
        if (!x) fail("instance_not_found");
        const item = state.items.find((i) => i.sku === x.sku);
        if (!item || !item.playerConsumable) fail("consume_disabled");
        if (item.stackable) {
          if (line.quantity === undefined) fail("bad_request", "lines[0]: quantity is required to use up part of a stack.");
          if (x.quantity < line.quantity) fail("insufficient_items");
          take(x, line.quantity);
        } else {
          if (line.quantity !== undefined && line.quantity !== 1) fail("bad_request", "lines[0]: a unique item's quantity is 1.");
          take(x, 1);
        }
      } else {
        const item = state.items.find((i) => i.sku === line.sku);
        if (!item) fail("item_not_found", "lines[0]: no such item in this store.");
        if (!item.playerConsumable) fail("consume_disabled");
        if (item.kind === "bundle") fail("bad_request", "A bundle is never held: consume its items.");
        const held = work.items.filter((i) => i.sku === line.sku).sort((a, c) => (a.acquiredAt < c.acquiredAt ? -1 : 1));
        if (held.reduce((n, x) => n + x.quantity, 0) < line.quantity) fail("insufficient_items");
        let left = line.quantity;
        for (const x of held) { if (left === 0) break; const q = Math.min(left, x.quantity); take(x, q); left -= q; }
      }
      return changes;
    });
    p.idem.set(key, { canonical: canon, result: clone(result) });
    return { status: 201, body: result };
  }, { auth: true });

  route("POST", "/rest/store/me/platform-purchases/meta", (ctx) => {
    const p = ctx.auth.player;
    const b = ctx.body === undefined ? {} : requireBodyObject(ctx.body);
    requireStore();
    limit("metasync:" + p.id, 6, 60000);
    const appId = normalizeAppId(b.appId);
    const notEnabled = "Meta purchases need the game's Meta app (with its app secret) turned on in the game's Sign-in settings.";
    if (!state.cfg.providers.meta) fail("provider_not_enabled", notEnabled);
    if (appId !== null && appId !== "" && appId !== TEST.metaAppId) fail("provider_not_enabled", notEnabled);
    const meta = p.identities.find((x) => x.provider === "meta");
    if (!meta) fail("meta_not_linked");
    const purchases = state.metaPurchases.get(meta.subject) ?? DEFAULT_META_PURCHASES;
    const answer = { granted: 0, alreadyGranted: 0, unmapped: 0, consumePending: 0 };
    for (const pur of purchases) {
      const map = META_PRODUCT_MAP[pur.sku];
      if (!map) { answer.unmapped++; continue; }
      const claim = meta.subject + "|" + pur.id;
      if (state.metaClaimed.has(claim)) { answer.alreadyGranted++; continue; }
      storeTransaction(p, "platform_purchase", (work, touched) => grantLines(work, [map], touched));
      state.metaClaimed.add(claim);
      answer.granted++;
    }
    return { status: 200, body: answer };
  }, { auth: true });

  // -------------------------------------------------------------------------------------------------------------
  // Test controls (/__mock/*)
  // -------------------------------------------------------------------------------------------------------------
  const controls = new Map();
  const control = (method, path, fn) => controls.set(method + " " + path, fn);
  const findPlayer = (id) => {
    const p = state.players.get(String(id ?? ""));
    if (!p || p.erased) fail("not_found", "No such player (playerId).");
    return p;
  };

  control("GET", "/__mock/health", () => ({ ok: true, version: MOCK_VERSION, baseUrl: "/rest", gameId: TEST.gameId, gameKey: TEST.gameKey,
    tokenTtl: opt.tokenTtl, tokenLeeway: opt.tokenLeeway, refreshTtl: opt.refreshTtl, rateLimits: opt.rateLimits,
    players: state.players.size, faults: state.faults.length, uptimeSeconds: Math.round(process.uptime()) }));
  control("POST", "/__mock/reset", () => {
    // Sequence numbers stay monotonic across resets, so a client polling /__mock/requests?since=N never re-reads.
    const { requestSeq, faultSeq } = state;
    state = freshState();
    state.requestSeq = requestSeq;
    state.faultSeq = faultSeq;
    return { ok: true, last: requestSeq };
  });
  control("POST", "/__mock/expire-tokens", (b) => {
    for (const j of state.issuedJtis) state.expiredJtis.add(j);
    let refresh = 0;
    if (b?.refresh === true) for (const r of state.refresh.values()) { r.expiresAt = 0; refresh++; }
    return { ok: true, accessTokens: state.issuedJtis.size, refreshTokens: refresh };
  });
  control("POST", "/__mock/fault", (b) => {
    if (!isObj(b)) fail("bad_request", "Send a JSON object.");
    if (b.clear === true) { state.faults = []; return { ok: true, faults: [] }; }
    const f = {
      id: ++state.faultSeq, match: typeof b.match === "string" ? b.match : "", method: typeof b.method === "string" ? b.method.toUpperCase() : null,
      times: isSafeInt(b.times) && b.times > 0 ? b.times : 1, status: isSafeInt(b.status) ? b.status : null,
      error: typeof b.error === "string" ? b.error : null, message: typeof b.message === "string" ? b.message : null,
      retryAfter: b.retryAfter !== undefined ? String(b.retryAfter) : null, drop: b.drop === true,
      after: b.after === true, delayMs: isSafeInt(b.delayMs) ? b.delayMs : 0,
      tokenExpired: b.tokenExpired === true || b.expiredToken === true, raw: typeof b.raw === "string" ? b.raw : null,
      contentType: typeof b.contentType === "string" ? b.contentType : null
    };
    if (!f.status && !f.drop && !f.tokenExpired && !f.delayMs) fail("bad_request", "A fault needs status, drop, tokenExpired or delayMs.");
    state.faults.push(f);
    return { ok: true, fault: f };
  });
  control("GET", "/__mock/faults", () => ({ faults: state.faults }));
  control("DELETE", "/__mock/faults", () => { state.faults = []; return { ok: true }; });
  control("GET", "/__mock/requests", (_b, q) => {
    const since = Number(q.get("since") ?? 0);
    const path = q.get("path");
    return { requests: state.requests.filter((r) => r.seq > since && (!path || r.path.includes(path))), last: state.requestSeq };
  });
  control("DELETE", "/__mock/requests", () => { state.requests = []; return { ok: true, last: state.requestSeq }; });
  control("GET", "/__mock/emails", () => ({ emails: state.sentEmails }));
  control("GET", "/__mock/players", () => ({ players: [...state.players.values()].map((p) => ({ id: p.id, displayName: p.displayName, erased: p.erased,
    identities: p.identities.map((x) => x.provider), email: p.email?.address ?? null,
    balances: Object.fromEntries(p.balances), items: p.items.length, txns: p.txns.length })) }));
  control("POST", "/__mock/config", (b) => {
    if (!isObj(b)) fail("bad_request", "Send a JSON object.");
    for (const k of ["playerSignIn", "signUp", "store", "playerData", "playerPurchases", "playerConsume", "emailAvailable", "gameActive"]) {
      if (typeof b[k] === "boolean") state.cfg[k] = b[k];
    }
    if (isObj(b.providers)) for (const [k, v] of Object.entries(b.providers)) if (k in state.cfg.providers && typeof v === "boolean") state.cfg.providers[k] = v;
    return { ok: true, config: state.cfg };
  });
  control("POST", "/__mock/grant", (b) => {
    const p = findPlayer(b?.playerId);
    const line = b.currency ? { currency: b.currency, quantity: b.amount ?? b.quantity } : { sku: b.sku, quantity: b.quantity ?? 1 };
    if (!isSafeInt(line.quantity) || line.quantity < 1) fail("bad_request", "amount/quantity must be a whole number of at least 1.");
    const r = storeTransaction(p, "grant", (work, touched) => grantLines(work, [line], touched));
    return { ok: true, result: r };
  });
  control("POST", "/__mock/catalog", (b) => {
    if (!isObj(b)) fail("bad_request", "Send a JSON object.");
    if (b.sku !== undefined) {
      const item = state.items.find((i) => i.sku === b.sku);
      if (!item) fail("item_not_found");
      if (Array.isArray(b.prices)) item.prices = clone(b.prices);
      for (const k of ["playerPurchasable", "playerConsumable", "archived"]) if (typeof b[k] === "boolean") item[k] = b[k];
      if (b.maxOwned === null || isSafeInt(b.maxOwned)) item.maxOwned = b.maxOwned;
    }
    if (b.currency !== undefined) {
      const c = state.currencies.find((x) => x.code === b.currency);
      if (!c) fail("currency_not_found");
      if (typeof b.archived === "boolean") c.archived = b.archived;
      if (b.cap === null || isSafeInt(b.cap)) c.cap = b.cap;
    }
    return { ok: true, catalog: catalogOut() };
  });
  control("POST", "/__mock/game-data", (b) => {
    if (!isObj(b) || !validKey(b.key)) fail("bad_request", "Send { key, value } or { key, delete: true }.");
    if (b.delete === true) state.gameData.delete(b.key);
    else state.gameData.set(b.key, { value: clone(b.value), version: ++state.gameCounter, updatedAt: iso(now()) });
    return { ok: true, etag: gameDataEtag(null) };
  });
  control("POST", "/__mock/readonly", (b) => {
    const p = findPlayer(b?.playerId);
    if (!validKey(b.key)) fail("key_invalid");
    if (b.delete === true) p.data.readonly.delete(b.key);
    else p.data.readonly.set(b.key, { value: clone(b.value), version: ++p.dataCounter, updatedAt: iso(now()) });
    return { ok: true };
  });
  control("POST", "/__mock/meta-purchases", (b) => {
    if (!isObj(b) || typeof b.userId !== "string" || !Array.isArray(b.purchases)) fail("bad_request", "Send { userId, purchases: [{ id, sku }] }.");
    state.metaPurchases.set(b.userId, b.purchases.map((x, i) => ({ id: String(x.id ?? "meta-purchase-x" + i), sku: String(x.sku), consumable: !!x.consumable })));
    return { ok: true, mapped: Object.keys(META_PRODUCT_MAP) };
  });
  control("POST", "/__mock/studio-token", (b) => {
    // What a studio backend gets from POST /player-auth/studio (an org-key route this mock does not serve).
    if (!isObj(b) || typeof b.externalId !== "string" || !b.externalId) fail("bad_request", "Send { externalId, displayName? }.");
    const id = state.identities.get("studio|" + b.externalId);
    let p = id ? state.players.get(id) : null;
    let isNew = false;
    if (!p) { p = createPlayer(b.displayName ?? null); addIdentity(p, "studio", b.externalId); isNew = true; }
    return signInAnswer(p, "studio", { newPlayer: isNew });
  });

  // -------------------------------------------------------------------------------------------------------------
  // HTTP plumbing
  // -------------------------------------------------------------------------------------------------------------
  const BODY_CAP = (path) => (path.startsWith("/rest/player-auth") ? 16 * 1024 : path.startsWith("/rest/player-data") ? 64 * 1024 : 64 * 1000);

  function readBody(req, cap) {
    return new Promise((resolve) => {
      const chunks = [];
      let size = 0;
      let over = false;
      req.on("data", (c) => { size += c.length; if (size > cap) over = true; else chunks.push(c); });
      req.on("end", () => resolve({ raw: Buffer.concat(chunks).toString("utf8"), size, over }));
      req.on("error", () => resolve({ raw: "", size: 0, over: false, error: true }));
    });
  }

  function send(res, out, reqMethod, reqHeaders) {
    const headers = { "Cache-Control": "no-store", ...(out.headers ?? {}) };
    let text = null;
    if (out.raw !== undefined) {
      text = out.raw;
      headers["Content-Type"] = out.contentType ?? "text/html; charset=utf-8";
    } else if (out.body !== undefined) {
      text = JSON.stringify(out.body);
      headers["Content-Type"] = "application/json; charset=utf-8";
      if (!Object.keys(headers).some((h) => h.toLowerCase() === "etag")) headers.ETag = weakEtag(text);
    }
    let status = out.status;
    const etag = headers.ETag;
    const noCache = /(?:^|,)\s*no-cache\s*(?:,|$)/i.test(String(reqHeaders["cache-control"] ?? ""));
    if ((reqMethod === "GET" || reqMethod === "HEAD") && status >= 200 && status < 300 && etag && !noCache && etagMatches(reqHeaders["if-none-match"], etag)) {
      status = 304;
      text = null;
      delete headers["Content-Type"];
    }
    if (status === 204 || status === 304) text = null;
    if (text !== null) headers["Content-Length"] = Buffer.byteLength(text);
    res.writeHead(status, headers);
    res.end(reqMethod === "HEAD" || text === null ? undefined : text);
    return status;
  }

  const errorOut = (e) => {
    if (e instanceof ApiError) {
      const body = { error: e.code, message: e.message };
      if (e.details !== undefined) body.details = e.details;
      const headers = { ...(e.headers ?? {}) };
      if (e.status === 429 && !headers["Retry-After"]) headers["Retry-After"] = "60";
      return { status: e.status, body, headers };
    }
    if (!opt.quiet) console.error("[mock] internal error:", e?.stack ?? e);
    return { status: 500, body: { error: "internal_error", message: "Something went wrong on our side. Please try again. (ref: mock)" } };
  };

  const defaultFaultCode = (status, path) => ({ 400: "bad_request", 401: "unauthorized", 403: "forbidden", 404: "not_found", 409: "conflict",
    429: "rate_limited", 500: "internal_error", 502: "provider_unavailable",
    503: path.includes("/player-data") ? "data_busy" : "player_sign_in_disabled" })[status] ?? "error";

  function takeFault(method, path, hasAuth) {
    const i = state.faults.findIndex((f) => (!f.method || f.method === method) && path.includes(f.match) && (!f.tokenExpired || hasAuth));
    if (i < 0) return null;
    const f = state.faults[i];
    f.times--;
    if (f.times <= 0) state.faults.splice(i, 1);
    return f;
  }
  function faultOut(f, path) {
    if (f.tokenExpired) return { status: 401, body: { error: "token_expired", message: "Token expired" } };
    const headers = {};
    if (f.retryAfter !== null) headers["Retry-After"] = f.retryAfter;
    else if (f.status === 429) headers["Retry-After"] = "60";
    if (f.raw !== null) return { status: f.status, raw: f.raw, contentType: f.contentType, headers };
    const code = f.error ?? defaultFaultCode(f.status, path);
    return { status: f.status, headers, body: { error: code, message: f.message ?? MESSAGES[code] ?? "Injected fault." } };
  }

  async function handle(req, res) {
    const url = new URL(req.url, "http://mock.local");
    const method = req.method.toUpperCase();
    const path = url.pathname;
    const headers = req.headers;

    // Controls are never logged and never faulted.
    if (path.startsWith("/__mock/")) {
      const fn = controls.get(method + " " + path.replace(/\/+$/, ""));
      const { raw } = await readBody(req, 1024 * 1024);
      let body;
      try { body = raw ? JSON.parse(raw) : undefined; } catch { return send(res, { status: 400, body: { error: "bad_request", message: "The request body is not valid JSON." } }, method, headers); }
      if (!fn) return send(res, { status: 404, body: { error: "not_found", message: "No such mock control." } }, method, headers);
      try { return send(res, { status: 200, body: fn(body ?? {}, url.searchParams) }, method, headers); }
      catch (e) { return send(res, errorOut(e), method, headers); }
    }

    const entry = {
      seq: ++state.requestSeq, at: iso(now()), method, path, query: Object.fromEntries(url.searchParams),
      headers: { authorization: headers.authorization !== undefined, idempotencyKey: headers["idempotency-key"] ?? null,
        ifNoneMatch: headers["if-none-match"] ?? null, xApiKey: headers["x-api-key"] !== undefined,
        contentType: headers["content-type"] ?? null, cacheControl: headers["cache-control"] ?? null },
      body: null, status: null, fault: null
    };
    state.requests.push(entry);
    if (state.requests.length > 5000) state.requests.shift();

    const cap = BODY_CAP(path);
    const got = await readBody(req, cap);
    let body;
    let bodyError = null;
    const hasBody = got.size > 0;
    if (hasBody) {
      const ct = String(headers["content-type"] ?? "");
      if (!/^application\/json\b/i.test(ct.trim()) && !/\+json\b/i.test(ct)) bodyError = new ApiError("bad_request", "Send the request body as application/json.");
      else if (got.over) bodyError = new ApiError("bad_request", "The request body is too large.");
      else {
        try { body = JSON.parse(got.raw); entry.body = redactBody(body); }
        catch { bodyError = new ApiError("bad_request", "The request body is not valid JSON."); entry.body = "[unparsable]"; }
        if (!bodyError && !isObj(body)) bodyError = new ApiError("bad_request", "Send a JSON object.");
      }
    }

    const fault = takeFault(method, path, headers.authorization !== undefined);
    if (fault) entry.fault = fault.id;
    if (fault?.delayMs) await new Promise((r) => setTimeout(r, fault.delayMs));
    if (fault && !fault.after && (fault.drop || fault.status || fault.tokenExpired)) {
      if (fault.drop) { entry.status = "dropped"; req.socket.destroy(); return; }
      entry.status = send(res, faultOut(fault, path), method, headers);
      return;
    }

    let out;
    try {
      if (bodyError) throw bodyError;
      const lookupMethod = method === "HEAD" ? "GET" : method;
      let match = null;
      for (const r of routes) {
        if (r.method !== lookupMethod) continue;
        const m = r.re.exec(path);
        if (m) { match = { r, params: Object.fromEntries(r.names.map((n, i) => [n, m[i + 1]])) }; break; }
      }
      if (!match) fail("not_found");
      const ctx = { method, path, query: url.searchParams, headers, body, params: match.params, ip: req.socket.remoteAddress };
      if (match.r.auth) ctx.auth = authenticate(ctx);
      out = await match.r.handler(ctx);
    } catch (e) {
      out = errorOut(e);
    }
    if (fault && fault.after) {
      if (fault.drop) { entry.status = "dropped-after"; req.socket.destroy(); return; }
      if (fault.status || fault.tokenExpired) out = faultOut(fault, path);
    }
    entry.status = send(res, out, method, headers);
    if (!opt.quiet) console.log(`[mock] ${method} ${path} -> ${entry.status}${fault ? " (fault " + fault.id + ")" : ""}`);
  }

  const server = http.createServer((req, res) => {
    handle(req, res).catch((e) => {
      try { send(res, errorOut(e), req.method, req.headers); } catch { /* socket gone */ }
    });
  });
  server.keepAliveTimeout = 5000;
  return { server, options: opt, test: TEST, get state() { return state; }, routes };
}

// ---------------------------------------------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------------------------------------------
function parseArgs(argv) {
  const out = {};
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (!a.startsWith("--")) continue;
    const [k, inline] = a.slice(2).split("=", 2);
    if (inline !== undefined) out[k] = inline;
    else if (argv[i + 1] !== undefined && !argv[i + 1].startsWith("--")) out[k] = argv[++i];
    else out[k] = true;
  }
  return out;
}

export function startMock({ port = 8787, host = "127.0.0.1", ...options } = {}) {
  const mock = createMock(options);
  return new Promise((resolve, reject) => {
    mock.server.once("error", reject);
    mock.server.listen(port, host, () => resolve({ ...mock, port: mock.server.address().port, host }));
  });
}

const isMain = process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href;
if (isMain) {
  const a = parseArgs(process.argv.slice(2));
  const num = (v, d) => (v === undefined ? d : Number(v));
  const m = await startMock({
    port: num(a.port, 8787), host: a.host ?? "127.0.0.1",
    tokenTtl: num(a["token-ttl"], 300), tokenLeeway: num(a["token-leeway"], 30), refreshTtl: num(a["refresh-ttl"], 30 * 24 * 3600),
    rateLimits: a["rate-limits"] === true || a["rate-limits"] === "true", quiet: a.quiet === true
  });
  console.log(`[mock] SplatterVault studio API mock ${MOCK_VERSION} listening on http://${m.host}:${m.port}/rest ` +
    `(gameId ${TEST.gameId}, token ttl ${m.options.tokenTtl}s, pid ${process.pid})`);
  const stop = () => m.server.close(() => process.exit(0));
  process.on("SIGTERM", stop);
  process.on("SIGINT", stop);
}
