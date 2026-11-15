#!/usr/bin/env node
// Honesty check for the mock: drives a full player flow against a fresh in-process mock (an ephemeral port, never
// a running :8787 instance) and validates EVERY response against studio-api.json: the status must be one the
// operation lists, the body must match that status's schema, and an error's code must be in its x-error-codes.
// Places where the mock deliberately follows the API over the spec are listed in KNOWN_DEVIATIONS and
// reported (not failed). Any other mismatch fails the run (exit 1).
//
//   node selftest.mjs [--verbose] [--spec <studio-api.json>]
import { existsSync, readFileSync } from "node:fs";
import crypto from "node:crypto";
import { fileURLToPath } from "node:url";
import { startMock } from "./server.mjs";
import { validate, findOperation } from "./schema.mjs";

// The API description: --spec <file>, else studio-api.json next to this file, else the copy the repository already
// keeps at openapi/studio-api.json (the Node SDK). Paths are relative to this folder, never to the working directory.
const specAt = process.argv.indexOf("--spec");
const specCandidates = specAt > 0 && process.argv[specAt + 1]
  ? [process.argv[specAt + 1]]
  : [fileURLToPath(new URL("./studio-api.json", import.meta.url)), fileURLToPath(new URL("../../openapi/studio-api.json", import.meta.url))];
const specFile = specCandidates.find((f) => existsSync(f));
if (!specFile) {
  console.error("selftest: no API description found (studio-api.json next to selftest.mjs, or --spec <file>)");
  process.exit(2);
}
const spec = JSON.parse(readFileSync(specFile, "utf8"));
const verbose = process.argv.includes("--verbose");

// Deviations the mock reproduces on purpose (the API does this; the spec does not say so).
const KNOWN_DEVIATIONS = {
  "GET /store/catalog 304": "the API's weak ETag + If-None-Match on every JSON GET; spec lists only 200",
  "GET /player-auth/me 304": "the API's weak ETag on every JSON GET; spec lists no 304",
  "GET /store/me 304": "the API's weak ETag on every JSON GET; spec lists no 304"
};

let checks = 0;
let failures = 0;
const deviations = [];
const issued = new Set(); // every token/secret the mock handed out (must never show up in the request log)

function ok(cond, what) {
  checks++;
  if (!cond) { failures++; console.log("  FAIL " + what); } else if (verbose) console.log("  ok   " + what);
  return cond;
}

let base;
async function call(method, path, { body, token, headers = {}, expect, label, rawBody } = {}) {
  const h = { ...headers };
  if (token) h.Authorization = "Bearer " + token;
  let payload;
  if (rawBody !== undefined) payload = rawBody;
  else if (body !== undefined) { payload = JSON.stringify(body); h["Content-Type"] = "application/json"; }
  const res = await fetch(base + "/rest" + path, { method, headers: h, body: payload });
  const text = await res.text();
  let json;
  try { json = text ? JSON.parse(text) : undefined; } catch { json = undefined; }
  const name = label ?? `${method} ${path.split("?")[0]}`;
  const status = res.status;
  if (expect !== undefined) ok(status === expect, `${name}: status ${status} (expected ${expect})${status >= 400 && json ? " " + JSON.stringify(json) : ""}`);

  // --- spec check ---
  const found = findOperation(spec, method, path.split("?")[0]);
  if (!ok(!!found, `${name}: an operation in the spec`)) return { status, json, headers: res.headers };
  const resp = found.op.responses[String(status)];
  const devKey = `${method} ${found.template} ${status}`;
  if (!resp) {
    if (KNOWN_DEVIATIONS[devKey]) deviations.push(`${devKey}: ${KNOWN_DEVIATIONS[devKey]}`);
    else ok(false, `${name}: status ${status} is not listed for ${found.op.operationId}`);
  } else {
    const media = resp.content?.["application/json"];
    if (media) {
      ok(json !== undefined, `${name}: a JSON body for ${status}`);
      const errs = validate(spec, media.schema, json);
      ok(errs.length === 0, `${name}: body matches the ${status} schema${errs.length ? "\n       " + errs.join("\n       ") : ""}`);
      const codes = resp["x-error-codes"];
      if (codes && json && typeof json.error === "string") ok(codes.includes(json.error), `${name}: error "${json.error}" is listed for ${status} (${codes.join(",")})`);
    } else {
      ok(text === "", `${name}: no body for ${status}`);
    }
    for (const hname of Object.keys(resp.headers ?? {})) {
      if (hname === "Retry-After" || hname === "Cache-Control" || hname === "ETag") ok(res.headers.get(hname) !== null, `${name}: ${hname} header present`);
    }
  }
  if (status !== 304) {
    const cc = res.headers.get("cache-control");
    const expected = found.template === "/player-auth/keys" && status === 200 ? "private, max-age=300"
      : found.template === "/player-data/games/{game}/public" && status === 200 ? "public, max-age=30" : "no-store";
    ok(cc === expected, `${name}: Cache-Control ${cc} (expected ${expected})`);
  }
  if (json) for (const k of ["token", "refreshToken", "deviceSecret"]) if (typeof json[k] === "string") issued.add(json[k]);
  return { status, json, headers: res.headers };
}
const control = async (method, path, body) => {
  const res = await fetch(base + "/__mock" + path, { method, headers: body ? { "Content-Type": "application/json" } : {}, body: body ? JSON.stringify(body) : undefined });
  return res.json();
};
const G = { gameId: 4242 };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function main() {
  const mock = await startMock({ port: 0, host: "127.0.0.1", quiet: true });
  base = `http://127.0.0.1:${mock.port}`;
  console.log("selftest: mock on " + base);
  try {
    await flow(mock);
    await ttlFlow();
  } finally {
    mock.server.close();
  }
  console.log(`\nselftest: ${checks} checks, ${failures} failures, ${deviations.length} known deviations (API over spec):`);
  for (const d of [...new Set(deviations)]) console.log("  - " + d);
  if (failures) { console.log("SELFTEST FAILED"); process.exit(1); }
  console.log("SELFTEST PASSED");
}

async function flow(mock) {
  console.log("1. guest");
  const guest = (await call("POST", "/player-auth/device/new", { body: { gameKey: mock.test.gameKey, displayName: "Guest One" }, expect: 200 })).json;
  ok(guest.player.newPlayer === true && /^svpd1_[A-Za-z0-9_-]{43}$/.test(guest.deviceSecret), "guest: new player + device secret format");
  const g2 = (await call("POST", "/player-auth/device", { body: { ...G, deviceSecret: guest.deviceSecret }, expect: 200 })).json;
  ok(g2.player.id === guest.player.id && g2.player.newPlayer === false, "device sign-in: same player");
  await call("POST", "/player-auth/device", { body: { ...G, deviceSecret: "svpd1_" + "A".repeat(43) }, expect: 401 });
  await call("POST", "/player-auth/device", { body: { ...G, deviceSecret: "nope" }, expect: 400 });
  await call("POST", "/player-auth/device/new", { body: { gameId: 4242, gameKey: mock.test.gameKey }, expect: 400 });
  await call("POST", "/player-auth/device/new", { body: { gameId: 999 }, expect: 404 });
  const me = (await call("GET", "/player-auth/me", { token: g2.token, expect: 200 })).json;
  ok(me.player.id === guest.player.id && me.game.gameKey === mock.test.gameKey && me.identities[0].provider === "device", "me: profile");
  await call("GET", "/player-auth/me", { expect: 401 });
  await call("GET", "/player-auth/me", { token: "abc.def.ghi", expect: 401 });

  console.log("2. keys + offline verification");
  const keys = (await call("GET", "/player-auth/keys", { expect: 200 })).json;
  const [hb, cb, sb] = g2.token.split(".");
  const header = JSON.parse(Buffer.from(hb, "base64url"));
  const claims = JSON.parse(Buffer.from(cb, "base64url"));
  const jwk = keys.keys.find((k) => k.kid === header.kid);
  ok(!!jwk, "token kid is in the JWKS");
  const pub = crypto.createPublicKey({ key: { kty: jwk.kty, crv: jwk.crv, x: jwk.x }, format: "jwk" });
  ok(crypto.verify(null, Buffer.from(hb + "." + cb), pub, Buffer.from(sb, "base64url")), "token signature verifies with the JWKS");
  ok(claims.iss === "splattervault" && claims.aud === "sv-player:4242" && claims.sub === guest.player.id, "token claims iss/aud/sub");
  ok(new Date(g2.expiresAt).getTime() === claims.exp * 1000, "expiresAt = exp");

  console.log("3. email code sign-up, password, refresh rotation");
  const email = "player.one@example.com";
  await call("POST", "/player-auth/email/code", { body: { ...G, email }, expect: 202 });
  await call("POST", "/player-auth/email/code", { body: { ...G, email: "not-an-email" }, expect: 400 });
  await call("POST", "/player-auth/email/code/verify", { body: { ...G, email, code: "00000000" }, expect: 401 });
  await call("POST", "/player-auth/email/code/verify", { body: { ...G, email, code: "12345678", password: "short" }, expect: 400 });
  const es = (await call("POST", "/player-auth/email/code/verify", { body: { ...G, email, code: "12345678", password: "correct horse 1", displayName: "Email One" }, expect: 200 })).json;
  ok(es.player.newPlayer === true && /^svpr1_/.test(es.refreshToken), "code verify: new player + refresh token");
  await call("POST", "/player-auth/email/code/verify", { body: { ...G, email, code: "12345678" }, expect: 401, label: "code reuse" });
  const pw = (await call("POST", "/player-auth/email/password", { body: { ...G, email, password: "correct horse 1" }, expect: 200 })).json;
  ok(pw.player.id === es.player.id && pw.player.newPlayer === false, "password sign-in: same player");
  await call("POST", "/player-auth/email/password", { body: { ...G, email, password: "wrong password" }, expect: 401 });

  const r1 = (await call("POST", "/player-auth/refresh", { body: { refreshToken: es.refreshToken }, expect: 200 })).json;
  ok(r1.refreshToken !== es.refreshToken && r1.player.newPlayer === false, "refresh: rotated");
  const rGrace = (await call("POST", "/player-auth/refresh", { body: { refreshToken: es.refreshToken }, expect: 200, label: "refresh: old token within 30 s grace" })).json;
  await call("POST", "/player-auth/refresh", { body: { refreshToken: r1.refreshToken }, expect: 401, label: "refresh: replaced token is reuse -> family revoked" });
  await call("POST", "/player-auth/refresh", { body: { refreshToken: rGrace.refreshToken }, expect: 401, label: "refresh: family revoked" });
  await call("POST", "/player-auth/refresh", { body: { refreshToken: "svpr1_garbage" }, expect: 401 });
  await call("POST", "/player-auth/refresh", { body: {}, expect: 400 });

  console.log("4. change password, link/unlink");
  let sess = (await call("POST", "/player-auth/email/password", { body: { ...G, email, password: "correct horse 1" }, expect: 200 })).json;
  await call("PUT", "/player-auth/me/password", { token: sess.token, body: { currentPassword: "nope nope", newPassword: "battery staple 2" }, expect: 401 });
  await call("PUT", "/player-auth/me/password", { token: sess.token, body: { newPassword: "battery staple 2" }, expect: 403 });
  const grant = (await call("PUT", "/player-auth/me/password", { token: sess.token, body: { currentPassword: "correct horse 1", newPassword: "battery staple 2" }, expect: 200 })).json;
  await call("POST", "/player-auth/refresh", { body: { refreshToken: sess.refreshToken }, expect: 401, label: "refresh: old session ended by password change" });
  await call("POST", "/player-auth/refresh", { body: { refreshToken: grant.refreshToken }, expect: 200, label: "refresh: new grant works" });
  const linked = (await call("POST", "/player-auth/link/steam", { token: g2.token, body: { ticket: "deadbeef" }, expect: 200 })).json;
  ok(linked.identities.map((i) => i.provider).join() === "device,steam", "link steam: identities");
  await call("POST", "/player-auth/link/steam", { token: sess.token, body: { ticket: "deadbeef" }, expect: 409, label: "link steam held by another player" });
  await call("POST", "/player-auth/link/steam", { token: g2.token, body: { ticket: "deadbeef02" }, expect: 409, label: "second steam account" });
  await call("POST", "/player-auth/link/steam", { token: g2.token, body: { ticket: "cafe" }, expect: 401 });
  await call("POST", "/player-auth/link/device", { token: g2.token, body: {}, expect: 400 });
  await call("DELETE", "/player-auth/link/steam", { token: g2.token, expect: 200 });
  await call("DELETE", "/player-auth/link/device", { token: g2.token, expect: 409, label: "unlink last identity" });
  await call("POST", "/player-auth/link/email/code", { token: g2.token, body: { email: "guest.one@example.com" }, expect: 202 });
  const ge = (await call("POST", "/player-auth/link/email", { token: g2.token, body: { email: "guest.one@example.com", code: "12345678" }, expect: 200 })).json;
  ok(ge.identities.some((i) => i.provider === "email") && ge.refreshToken === undefined, "link email to guest: no refresh token");
  await call("POST", "/player-auth/link/email/code", { token: g2.token, body: { unlink: true }, expect: 202 });
  await call("POST", "/player-auth/link/email/remove", { token: g2.token, body: {}, expect: 403 });
  await call("POST", "/player-auth/link/email/remove", { token: g2.token, body: { oldCode: "12345678" }, expect: 200 });

  console.log("4b. email change (password account) answers a new refresh token");
  sess = (await call("POST", "/player-auth/email/password", { body: { ...G, email, password: "battery staple 2" }, expect: 200 })).json;
  await call("POST", "/player-auth/link/email/code", { token: sess.token, body: { email: "player.two@example.com" }, expect: 202 });
  await call("POST", "/player-auth/link/email", { token: sess.token, body: { email: "player.two@example.com", code: "12345678" }, expect: 403, label: "email change without currentPassword" });
  await call("POST", "/player-auth/link/email", { token: sess.token, body: { email: "player.two@example.com", code: "12345678", currentPassword: "wrong one" }, expect: 401, label: "email change, wrong password" });
  const changed = (await call("POST", "/player-auth/link/email", { token: sess.token, body: { email: "player.two@example.com", code: "12345678", currentPassword: "battery staple 2" }, expect: 200, label: "email change" })).json;
  ok(typeof changed.refreshToken === "string" && typeof changed.refreshExpiresAt === "string", "email change: new refresh grant");
  await call("POST", "/player-auth/refresh", { body: { refreshToken: sess.refreshToken }, expect: 401, label: "email change ended the other sessions" });
  const email2 = (await call("POST", "/player-auth/email/password", { body: { ...G, email: "player.two@example.com", password: "battery staple 2" }, expect: 200, label: "sign in with the new address" })).json;
  ok(email2.player.id === es.player.id, "email change: same player");
  await call("POST", "/player-auth/link/email/remove", { token: email2.token, body: { currentPassword: "battery staple 2" }, expect: 409, label: "remove the only identity" });
  await call("POST", "/player-auth/sign-out", { expect: 400, label: "sign-out without a body" });

  console.log("5. player data");
  const t = g2.token;
  let d = (await call("GET", "/player-data/me", { token: t, expect: 200 })).json;
  ok(d.data.player && d.data.readonly && d.data.readonly["profile.tier"]?.value === "bronze", "data: both stores + seeded readonly");
  let w = (await call("PATCH", "/player-data/me", { token: t, body: { ops: [{ op: "set", store: "player", key: "settings", value: { fov: 90 }, ifVersion: 0 },
    { op: "inc", store: "player", key: "stats", field: "wins", by: 2 }, { op: "inc", store: "player", key: "coins", by: 5, max: 10 }] }, expect: 200 })).json;
  const vSettings = w.results[0].version;
  ok(w.results[1].value.wins === 2 && w.results[2].value === 5, "data: inc results carry the value");
  d = (await call("GET", "/player-data/me?stores=player&keys=settings", { token: t, expect: 200 })).json;
  ok(d.data.player.settings.value.fov === 90 && d.data.player.settings.version === vSettings && !d.data.readonly, "data: read back by key");
  d = (await call("GET", "/player-data/me?keysOnly=true", { token: t, expect: 200 })).json;
  ok(d.data.player.settings.value === undefined && d.data.player.settings.version === vSettings, "data: keysOnly");
  const conflict = (await call("PATCH", "/player-data/me", { token: t, body: { ops: [{ op: "set", store: "player", key: "settings", value: { fov: 100 }, ifVersion: vSettings - 1 }] }, expect: 409 })).json;
  ok(conflict.error === "version_conflict" && conflict.details[0].version === vSettings, "data: version_conflict details");
  await call("PATCH", "/player-data/me", { token: t, body: { ops: [{ op: "inc", store: "player", key: "coins", by: 6, max: 10 }] }, expect: 409, label: "data: out_of_range" });
  await call("PATCH", "/player-data/me", { token: t, body: { ops: [{ op: "set", store: "readonly", key: "x", value: 1 }] }, expect: 403 });
  await call("PATCH", "/player-data/me", { token: t, body: { ops: [{ op: "set", store: "player", key: "sv.secret", value: 1 }] }, expect: 400 });
  await call("PATCH", "/player-data/me", { token: t, body: { ops: [{ op: "set", store: "player", key: "big", value: "x".repeat(33000) }] }, expect: 413 });
  w = (await call("PATCH", "/player-data/me", { token: t, body: { ops: [{ op: "delete", store: "player", key: "settings", ifVersion: vSettings }] }, expect: 200 })).json;
  ok(w.results[0].version === null, "data: delete answers version null");

  console.log("6. game data ETag / 304");
  const gd = await call("GET", "/player-data/game", { token: t, expect: 200 });
  ok(gd.json.data.motd && gd.headers.get("etag")?.startsWith('W/"pd-'), "game data + ETag");
  await call("GET", "/player-data/game", { token: t, headers: { "If-None-Match": gd.headers.get("etag"), "Cache-Control": "max-age=0" }, expect: 304 });
  const pub1 = await call("GET", "/player-data/games/" + mock.test.gameKey + "/public", { expect: 200 });
  await call("GET", "/player-data/games/4242/public", { headers: { "If-None-Match": pub1.headers.get("etag"), "Cache-Control": "max-age=0" }, expect: 304 });
  await call("GET", "/player-data/games/nope/public", { expect: 404 });
  // Node fetch adds Cache-Control: no-cache to a request with If-None-Match unless one is set; the real API
  // (and so the mock) then never answers 304.
  await call("GET", "/player-data/game", { token: t, headers: { "If-None-Match": gd.headers.get("etag") }, expect: 200, label: "If-None-Match + fetch default no-cache -> 200" });
  await control("POST", "/game-data", { key: "motd", value: "changed" });
  await call("GET", "/player-data/game", { token: t, headers: { "If-None-Match": gd.headers.get("etag") }, expect: 200, label: "game data changed -> 200" });

  console.log("7. store catalog / holdings");
  const cat = await call("GET", "/store/catalog", { token: t, expect: 200 });
  ok(cat.json.items.some((i) => i.sku === "starter_bundle" && i.kind === "bundle") && cat.headers.get("etag"), "catalog + ETag");
  await call("GET", "/store/catalog", { token: t, headers: { "If-None-Match": cat.headers.get("etag"), "Cache-Control": "max-age=0" }, expect: 304 });
  let hold = (await call("GET", "/store/me", { token: t, expect: 200 })).json;
  ok(hold.balances.find((b) => b.currency === "COINS")?.amount === 1000, "holdings: starting balance 1000 COINS");

  console.log("8. purchases: idempotency, mismatch, funds");
  const key1 = crypto.randomUUID();
  const buy = { sku: "potion_small", currency: "COINS", quantity: 3, expectedPrice: 20 };
  const p1 = (await call("POST", "/store/me/purchases", { token: t, headers: { "Idempotency-Key": key1 }, body: buy, expect: 201 })).json;
  ok(p1.replayed === false && p1.changes[0].delta === -60 && p1.items[0].quantity === 3, "purchase: debit + stack");
  const p1r = (await call("POST", "/store/me/purchases", { token: t, headers: { "Idempotency-Key": key1 }, body: buy, expect: 200, label: "purchase replay" })).json;
  ok(p1r.replayed === true && p1r.txn.id === p1.txn.id, "replay: same txn, replayed true");
  await call("POST", "/store/me/purchases", { token: t, headers: { "Idempotency-Key": key1 }, body: { ...buy, quantity: 4 }, expect: 409, label: "same key, other body" });
  await call("POST", "/store/me/purchases", { token: t, body: buy, expect: 400, label: "no Idempotency-Key" });
  const pc = (await call("POST", "/store/me/purchases", { token: t, headers: { "Idempotency-Key": crypto.randomUUID() }, body: { ...buy, expectedPrice: 19 }, expect: 409 })).json;
  ok(pc.error === "price_changed", "price_changed");
  const nf = (await call("POST", "/store/me/purchases", { token: t, headers: { "Idempotency-Key": crypto.randomUUID() }, body: { ...buy, quantity: 100 }, expect: 409 })).json;
  ok(nf.error === "insufficient_funds", "insufficient_funds");
  await call("POST", "/store/me/purchases", { token: t, headers: { "Idempotency-Key": crypto.randomUUID() }, body: { ...buy, sku: "vip_pass", expectedPrice: 0 }, expect: 403 });
  await call("POST", "/store/me/purchases", { token: t, headers: { "Idempotency-Key": crypto.randomUUID() }, body: { ...buy, sku: "no_such" }, expect: 404 });
  const bundle = (await call("POST", "/store/me/purchases", { token: t, headers: { "Idempotency-Key": crypto.randomUUID() }, body: { sku: "starter_bundle", currency: "COINS", expectedPrice: 500 }, expect: 201 })).json;
  ok(bundle.balances.find((b) => b.currency === "GEMS")?.amount === 50, "bundle: gems credited");
  const sword = (await call("POST", "/store/me/purchases", { token: t, headers: { "Idempotency-Key": crypto.randomUUID() }, body: { sku: "sword_basic", currency: "COINS", expectedPrice: 250 }, expect: 201 })).json;
  ok(sword.items.length === 1, "durable bought");
  await call("POST", "/store/me/purchases", { token: t, headers: { "Idempotency-Key": crypto.randomUUID() }, body: { sku: "sword_basic", currency: "COINS", expectedPrice: 250 }, expect: 409, label: "already_owned" });

  console.log("9. consume");
  hold = (await call("GET", "/store/me", { token: t, expect: 200 })).json;
  const ticket = hold.items.find((i) => i.sku === "raffle_ticket");
  const c1 = (await call("POST", "/store/me/consume", { token: t, headers: { "Idempotency-Key": crypto.randomUUID() }, body: { instanceId: ticket.instanceId }, expect: 201 })).json;
  ok(c1.changes[0].after === 0 && c1.items.length === 0, "consume unique by instanceId");
  const ck = crypto.randomUUID();
  const c2 = (await call("POST", "/store/me/consume", { token: t, headers: { "Idempotency-Key": ck }, body: { sku: "potion_small", quantity: 2 }, expect: 201 })).json;
  ok(c2.items[0].quantity === 6, "consume by sku (3 + 5 - 2 = 6)");
  ok((await call("POST", "/store/me/consume", { token: t, headers: { "Idempotency-Key": ck }, body: { sku: "potion_small", quantity: 2 }, expect: 200 })).json.replayed === true, "consume replay");
  await call("POST", "/store/me/consume", { token: t, headers: { "Idempotency-Key": crypto.randomUUID() }, body: { sku: "potion_small", quantity: 99 }, expect: 409 });
  await call("POST", "/store/me/consume", { token: t, headers: { "Idempotency-Key": crypto.randomUUID() }, body: { instanceId: crypto.randomUUID() }, expect: 404 });
  await call("POST", "/store/me/consume", { token: t, headers: { "Idempotency-Key": crypto.randomUUID() }, body: { sku: "sword_basic", quantity: 1 }, expect: 403 });

  console.log("10. history paging");
  await control("POST", "/grant", { playerId: guest.player.id, currency: "COINS", amount: 5000 });
  for (let i = 0; i < 50; i++) {
    await call("POST", "/store/me/purchases", { token: t, headers: { "Idempotency-Key": "page-" + i }, body: { sku: "potion_small", currency: "COINS", expectedPrice: 20 }, expect: 201, label: "purchase " + i });
  }
  const h1 = (await call("GET", "/store/me/history", { token: t, expect: 200 })).json;
  ok(h1.txns.length === 50 && typeof h1.next === "string", "history page 1: 50 + next");
  const h2 = (await call("GET", "/store/me/history?before=" + h1.next, { token: t, expect: 200 })).json;
  ok(h2.txns.length > 0 && h2.next === null && h2.txns[h2.txns.length - 1].kind === "grant", "history page 2 ends at the starting grant");
  await call("GET", "/store/me/history?before=nope", { token: t, expect: 400 });

  console.log("11. Meta sign-in + purchase sync");
  const meta = (await call("POST", "/player-auth/meta", { body: { ...G, userId: "1001", nonce: "test-nonce" }, expect: 200 })).json;
  await call("POST", "/player-auth/meta", { body: { ...G, userId: "1001", nonce: "bogus" }, expect: 401 });
  await call("POST", "/player-auth/meta", { body: { ...G, userId: "1001", nonce: "test-nonce-once" }, expect: 200 });
  await call("POST", "/player-auth/meta", { body: { ...G, userId: "1001", nonce: "test-nonce-once" }, expect: 401, label: "meta nonce replay" });
  const s1 = (await call("POST", "/store/me/platform-purchases/meta", { token: meta.token, body: {}, expect: 200 })).json;
  ok(s1.granted === 1 && s1.unmapped === 1 && s1.alreadyGranted === 0, "meta sync: first");
  const s2 = (await call("POST", "/store/me/platform-purchases/meta", { token: meta.token, expect: 200 })).json;
  ok(s2.granted === 0 && s2.alreadyGranted === 1, "meta sync: second");
  await call("POST", "/store/me/platform-purchases/meta", { token: t, body: {}, expect: 409, label: "meta sync without Meta" });
  for (const [prov, body] of [["xsolla", { token: "test-token" }], ["playfab", { sessionTicket: "test-token" }], ["supabase", { token: "test-token:abc" }], ["steam", { ticket: "deadbeef01" }]]) {
    await call("POST", "/player-auth/" + prov, { body: { ...G, ...body }, expect: 200 });
    await call("POST", "/player-auth/" + prov, { body: { ...G, ...(prov === "playfab" ? { sessionTicket: "wrong" } : prov === "steam" ? { ticket: "abcd" } : { token: "wrong" }) }, expect: 401 });
  }

  console.log("11b. switches");
  await control("POST", "/config", { store: false });
  ok((await call("GET", "/store/catalog", { token: t, expect: 404 })).json.error === "store_not_enabled", "store off: store_not_enabled");
  await control("POST", "/config", { store: true, playerData: false });
  ok((await call("GET", "/player-data/me", { token: t, expect: 404 })).json.error === "player_data_not_enabled", "player data off");
  await call("GET", "/player-data/games/4242/public", { expect: 404, label: "public data off: game_not_found" });
  await control("POST", "/config", { playerData: true, playerPurchases: false, playerConsume: false });
  ok((await call("POST", "/store/me/purchases", { token: t, headers: { "Idempotency-Key": crypto.randomUUID() }, body: buy, expect: 403 })).json.error === "purchases_disabled", "purchases_disabled");
  ok((await call("POST", "/store/me/consume", { token: t, headers: { "Idempotency-Key": crypto.randomUUID() }, body: { sku: "potion_small", quantity: 1 }, expect: 403 })).json.error === "consume_disabled", "consume_disabled");
  const catOff = (await call("GET", "/store/catalog", { token: t, expect: 200 })).json;
  ok(catOff.settings.playerPurchases === false && catOff.items.every((i) => i.purchasable === false), "catalog reflects the switches");
  await control("POST", "/config", { playerPurchases: true, playerConsume: true, playerSignIn: false });
  await call("GET", "/player-auth/keys", { expect: 503 });
  await call("GET", "/player-auth/me", { token: t, expect: 503 });
  await control("POST", "/config", { playerSignIn: true });
  await control("POST", "/catalog", { sku: "potion_small", prices: [{ currency: "COINS", amount: 25 }] });
  const cat2 = await call("GET", "/store/catalog", { token: t, headers: { "If-None-Match": cat.headers.get("etag"), "Cache-Control": "max-age=0" }, expect: 200, label: "catalog changed -> 200" });
  ok(cat2.json.items.find((i) => i.sku === "potion_small").prices[0].amount === 25, "catalog price change visible");
  ok((await call("POST", "/store/me/purchases", { token: t, headers: { "Idempotency-Key": crypto.randomUUID() }, body: buy, expect: 409 })).json.error === "price_changed", "old price -> price_changed");
  await control("POST", "/catalog", { sku: "potion_small", prices: [{ currency: "COINS", amount: 20 }] });
  const meE = await call("GET", "/player-auth/me", { token: t, expect: 200 });
  await call("GET", "/player-auth/me", { token: t, headers: { "If-None-Match": meE.headers.get("etag"), "Cache-Control": "max-age=0" }, expect: 304, label: "me 304 (weak ETag)" });
  await control("POST", "/fault", { match: "/player-data/me", status: 429 });
  const r429 = await call("GET", "/player-data/me", { token: t, expect: 429, label: "fault 429" });
  ok(r429.headers.get("retry-after") === "60", "429 carries Retry-After 60");
  await control("POST", "/fault", { match: "/store/me", method: "GET", status: 502, raw: "<html>bad gateway</html>" });
  const raw = await fetch(base + "/rest/store/me", { headers: { Authorization: "Bearer " + t } });
  ok(raw.status === 502 && (await raw.text()).startsWith("<html>") && raw.headers.get("content-type").startsWith("text/html"), "fault: raw non-JSON body");

  console.log("12. faults, expiry, sign-out, erase");
  await control("POST", "/fault", { match: "/store/me/purchases", times: 2, status: 503, retryAfter: 1 });
  const f1 = await call("POST", "/store/me/purchases", { token: t, headers: { "Idempotency-Key": "fault-1" }, body: buy, expect: 503, label: "fault 503 #1" });
  ok(f1.headers.get("retry-after") === "1", "fault: Retry-After");
  await call("POST", "/store/me/purchases", { token: t, headers: { "Idempotency-Key": "fault-1" }, body: buy, expect: 503, label: "fault 503 #2" });
  await call("POST", "/store/me/purchases", { token: t, headers: { "Idempotency-Key": "fault-1" }, body: buy, expect: 201, label: "after faults" });
  await control("POST", "/fault", { match: "/store/me/purchases", drop: true, after: true });
  let dropped = false;
  try { await fetch(base + "/rest/store/me/purchases", { method: "POST", headers: { Authorization: "Bearer " + t, "Idempotency-Key": "lost-answer", "Content-Type": "application/json" }, body: JSON.stringify(buy) }); }
  catch { dropped = true; }
  ok(dropped, "fault: dropped socket is a network error");
  const lost = (await call("POST", "/store/me/purchases", { token: t, headers: { "Idempotency-Key": "lost-answer" }, body: buy, expect: 200, label: "retry after a lost answer replays" })).json;
  ok(lost.replayed === true, "lost answer: replayed");
  await control("POST", "/fault", { tokenExpired: true });
  const fe = (await call("GET", "/store/me", { token: t, expect: 401, label: "fault: expired token" })).json;
  ok(fe.error === "token_expired" && fe.message === "Token expired", "fault: token_expired body");
  await control("POST", "/expire-tokens", {});
  await call("GET", "/player-auth/me", { token: t, expect: 401, label: "expire-tokens" });
  const fresh = (await call("POST", "/player-auth/device", { body: { ...G, deviceSecret: guest.deviceSecret }, expect: 200 })).json;
  await call("GET", "/player-auth/me", { token: fresh.token, expect: 200, label: "new token after expire-tokens" });
  const sOut = (await call("POST", "/player-auth/email/password", { body: { ...G, email: "player.two@example.com", password: "battery staple 2" }, expect: 200 })).json;
  await call("POST", "/player-auth/sign-out", { body: { refreshToken: sOut.refreshToken }, expect: 204 });
  await call("POST", "/player-auth/refresh", { body: { refreshToken: sOut.refreshToken }, expect: 401, label: "refresh after sign-out" });
  await call("POST", "/player-auth/sign-out", { body: { refreshToken: "whatever" }, expect: 204, label: "sign-out unknown token" });
  await call("DELETE", "/player-auth/me", { token: sOut.token, expect: 204 });
  const gone = (await call("GET", "/player-auth/me", { token: sOut.token, expect: 401 })).json;
  ok(gone.error === "player_deleted", "erased: player_deleted");
  await call("POST", "/player-auth/email/password", { body: { ...G, email: "player.two@example.com", password: "battery staple 2" }, expect: 401, label: "erased email gone" });
  const nr = await fetch(base + "/rest/player-auth/nope");
  const nrb = await nr.json();
  ok(nr.status === 404 && nrb.error === "not_found", "unknown route: 404 not_found (not a spec operation)");

  console.log("13. request log never holds secrets");
  const log = JSON.stringify(await control("GET", "/requests"));
  ok([...issued].every((s) => !log.includes(s)), `request log: none of ${issued.size} issued tokens/secrets appear`);
  ok(!log.includes("correct horse") && !log.includes("battery staple"), "request log: no passwords");
  const reqs = JSON.parse(log).requests;
  ok(reqs.some((r) => r.headers.idempotencyKey === key1) && reqs.some((r) => r.headers.authorization === true), "request log: Idempotency-Key + Authorization presence");
}

async function ttlFlow() {
  console.log("14. short token TTL (separate mock, --token-ttl 1 --token-leeway 0)");
  const m = await startMock({ port: 0, host: "127.0.0.1", tokenTtl: 1, tokenLeeway: 0, quiet: true });
  const saved = base;
  base = `http://127.0.0.1:${m.port}`;
  try {
    const g = (await call("POST", "/player-auth/device/new", { body: G, expect: 200 })).json;
    await call("GET", "/player-auth/me", { token: g.token, expect: 200 });
    await sleep(2100);
    const e = (await call("GET", "/player-auth/me", { token: g.token, expect: 401 })).json;
    ok(e.error === "token_expired", "short TTL: token_expired after 2 s");
  } finally {
    m.server.close();
    base = saved;
  }
}

main().catch((e) => { console.error("selftest crashed:", e); process.exit(2); });
