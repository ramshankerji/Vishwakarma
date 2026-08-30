---
name: login-design
description: "Login/accounts design doc (website/content/software/login.md) — DESIGNED ONLY, nothing implemented; account = immutable Ed25519 keypair + per-account salt"
metadata: 
  node_type: memory
  type: project
  originSessionId: edb906a7-faa9-4a77-b8ce-6583ab511da1
  modified: 2026-07-20T19:14:06.452Z
---

`website/content/software/login.md` (written 2026-07-21) designs the account/login system
for the Django `./server` — DESIGNED ONLY, no code exists yet; `/api/login` stub in
`api/views.py` still returns 501.

Load-bearing decisions (Ram's requirements, locked):
- Account = server-generated immutable Ed25519 keypair (public key IS the account ID,
  matching `Installation.public_key`) + immutable 32-byte per-account salt. Private key
  server-side only, encrypted with env KEK; signs credential links, device delegations,
  and a per-account hash-chained AccountEvent audit log.
- Providers fixed IDs: 1 Google, 2 Baidu, 3 Facebook, 4 LinkedIn, 5 Email OTP,
  6 Mobile SMS OTP, 7 reserved passkey, 8 recovery codes. Passwordless everywhere.
- One credential row per (account, provider) EVER — disable-only (tombstone =
  SHA256(account_salt‖provider‖subject)), slot burns permanently. **No account merger,
  ever** — no email-based auto-linking; unknown identity → explicit consent screen.
- Two hash constructions: active login lookup = HMAC-SHA256(env pepper, provider‖subject)
  (O(1) lookup, DB-leak safe); tombstones use the per-account salt (non-lookupable).
- Desktop app login extends the existing `crypto.py` chain: account key signs installation
  key (DeviceDelegation cert via system browser + loopback, RFC 8252); app never holds
  account private key.
- Website sessions: opaque hashed tokens, host-only cookie on mv-server.ramshanker.in,
  SameSite=Lax works because mv.ramshanker.in is same-site; exact-origin CORS +
  X-MV-Request header CSRF check. Cloudflare Turnstile on OTP sends and account creation.
- New Django app `accounts` (api app stays telemetry-only); django.contrib.auth NOT used.

Related: [[telemetry-system]] (the server being extended), [[release-signing-setup]]
(secret-handling precedent for pepper/KEK), [[undo-redo-design]] (same designed-only status).
