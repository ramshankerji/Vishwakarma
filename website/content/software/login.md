---
title: "Login and User Accounts"
weight: 100113
---
This page is the design document for the user account and login subsystem. It extends the
existing Django server in `./server` (currently telemetry-only, `/api/logs`, `/api/stats`) to
authenticate users of the static website `https://mv.ramshanker.in` and of the desktop
application. Like `undo-redo.md`, sections written in plain text are **locked decisions**;
items marked **OPEN** are still under debate and must not be treated as settled by an
implementer.

The central claim of this document: **an account is not a row with a username, it is an
immutable Ed25519 key pair.** Login providers (Google, Baidu, Facebook, LinkedIn, Email,
Mobile) are merely write-once pointers to that key pair. This mirrors the already-shipped
telemetry design, where an installation *is* its Ed25519 public key and every request is
authenticated by a signature chain (`server/api/crypto.py`, `code-core` AccountManager).

## 1. Design Goals and Non-Negotiables

* One account = one immutable Ed25519 key pair + one immutable per-account random salt.
* Six login providers at launch: Google, Baidu, Facebook, LinkedIn, Email (OTP), Mobile (SMS OTP).
* A provider credential, once connected to an account, can be **disabled but never changed**.
  Disabling stores a salted hash (tombstone) of the previous identity. The provider slot on
  that account is burnt forever.
* **There is no account merger. Ever.** Not as an admin tool, not as a support workflow, not
  as a database migration. Two accounts created by mistake remain two accounts.
* Passwordless. We never store or accept passwords. This removes the entire
  credential-stuffing / password-breach / password-reset attack class at the design level.
* Minimal PII, hashed or encrypted at rest. The server must remain safe to run on the same
  small machine that hosts telemetry today.
* Every anonymous, cost-incurring endpoint is protected by Cloudflare Turnstile.
* Same login system serves both surfaces: browser (static site → CORS → Django) and the
  desktop application (system browser + Ed25519 device delegation).

## 2. Identity Model: an Account is a Key Pair

At account creation the server generates:

* `account_key` — an Ed25519 key pair. The **public key is the account identifier**
  everywhere (URLs, logs, foreign keys, support conversations). Base64 of 32 raw bytes,
  same convention as `Installation.public_key`.
* `account_salt` — 32 random bytes, generated once, never rotated, never reused across
  accounts. Used only for credential tombstones and other deliberately non-lookupable
  hashes (§5).

Both are immutable for the life of the account. There is no rename, no re-key, no salt
rotation. An account that must be abandoned is closed (§12) and a new one created.

The private key is stored server-side only, encrypted at rest with a key-encryption-key
(KEK) supplied via environment variable, exactly like provider client secrets. It never
leaves the server and is used exclusively to sign:

1. **Credential link events** — "identity hash H of provider P belongs to this account",
   signed at link time. Makes the credential list tamper-evident against direct DB edits.
2. **Device delegations** — "installation public key I may act as this account" (§7.4).
3. **Account event chain** — an append-only, hash-chained audit log per account (§6,
   `AccountEvent`), the same chain aesthetic as the telemetry request chain.

We deliberately do **not** use `django.contrib.auth`. It is password-centric, its User
model fights the key-pair identity, and the telemetry endpoints already bypass it. The
accounts app defines its own models and its own session mechanism.

## 3. Login Providers

Fixed numeric provider IDs, stable forever (same philosophy as ObjectType IDs):

| ID | Provider  | Protocol                                   | Subject (`sub`) source          |
|----|-----------|--------------------------------------------|---------------------------------|
| 1  | GOOGLE    | OpenID Connect, code + PKCE                | `id_token.sub`                  |
| 2  | BAIDU     | OAuth 2.0 code flow                        | `openid` from user-info API     |
| 3  | FACEBOOK  | Facebook Login (OAuth 2.0) + appsecret_proof | `/me` `id` (app-scoped)       |
| 4  | LINKEDIN  | OpenID Connect ("Sign In with LinkedIn")   | `id_token.sub`                  |
| 5  | EMAIL     | One-time code to mailbox                   | normalized address (§5)         |
| 6  | MOBILE    | One-time code over SMS                     | E.164 number                    |
| 7  | PASSKEY   | *reserved* — WebAuthn, not designed yet    | credential ID                   |
| 8  | RECOVERY  | One-time recovery codes (§12)              | n/a (verifier hashes)           |

Provider notes:

* Google and LinkedIn are plain OIDC: validate `iss`, `aud`, `exp`, `nonce` and the JWKS
  signature of the `id_token`. Never accept an unverified `email` claim as identity.
* Baidu has no OIDC; after the code→token exchange, call the passport user-info endpoint
  and use its stable user ID. Baidu exists in the list because Google is unreachable for
  users in China — the two cover complementary markets.
* Facebook returns app-scoped IDs; all token-based Graph calls attach `appsecret_proof`.
* Email addresses and phone numbers from OAuth providers are treated as display metadata
  only. **They are never used to match, link, or deduplicate accounts** — silent
  email-based auto-linking is the classic OAuth account-takeover bug, and our no-merge
  rule kills it by construction.

## 4. Credential Slots: Write-Once, Disable-Only

Each account has at most **one credential row per provider, ever**. The row is created at
link time and is immutable except for the single one-way transition ACTIVE → DISABLED.

* Linking writes: provider, active lookup hash (§5), encrypted display identifier
  ("r…@yahoo.com", "+91 98……21"), link timestamp, and the link signature by the account key.
* Disabling writes: disabled timestamp, the per-account-salted tombstone hash of the
  identity, and **erases** the active lookup hash and the encrypted display identifier.
  What remains proves — if the original identity is ever presented again — that it *was*
  connected here, without being lookupable, reversible, or reusable.
* Re-linking the same provider on that account is impossible — the `(account, provider)`
  uniqueness plus the write-once rule means the slot is burnt. A user who disables their
  Google credential can never attach any Google identity to that account again.
* A disabled external identity is free to sign in fresh later — that creates a **new**
  account (no merge), after the explicit consent screen of §12.
* The last active credential of an account cannot be disabled. The only way out is
  account closure (§12). This prevents silently orphaning an account.

Enforcement is layered: application code has no update path for the identity columns, a
database `UniqueConstraint` covers `(account, provider)`, active lookup hashes are unique
among active rows, and the signed link event makes offline tampering detectable.

## 5. Hashing: One Pepper, One Salt per Account

Two different hashing needs, two different constructions:

* **Active lookup hash** — at login we must find the account owning `(provider, subject)`
  in O(1), so the hash must be deterministic across accounts:
  `HMAC-SHA256(server_pepper, provider_id ‖ 0x00 ‖ subject)`.
  `server_pepper` is a 32-byte secret in the environment (not in the DB, not in git — same
  handling as the Ed25519 manifest key in `release.md`). A stolen database alone cannot be
  joined against provider user IDs, email addresses, or phone numbers.
* **Tombstone hash** — must *not* be lookupable, so it uses the immutable per-account salt:
  `SHA256(account_salt ‖ provider_id ‖ 0x00 ‖ subject)`.
  Verifiable only when both the specific account and the candidate identity are already in
  hand; useless for scanning either direction.

Subjects are normalized before hashing, once, in one function, or the same user will get
two accounts: email → trim, Unicode NFC, lowercase; phone → E.164 digits only; OAuth
subjects → the raw provider string, untouched.

High-entropy secrets we generate ourselves (session tokens, OTP codes, recovery codes) are
stored as plain SHA-256 hashes — key-stretching (Argon2/bcrypt) exists to protect
low-entropy human passwords, which we do not have.

## 6. Database Schema

New Django app `accounts` in `./server` (the `api` app stays telemetry-only). Proposed
models, in the concise style of `api/models.py`:

```python
class Account(models.Model):
    public_key = models.CharField(max_length=64, unique=True)   # Base64, 32 raw bytes.
    private_key_encrypted = models.BinaryField()                # Sealed with env KEK.
    salt = models.BinaryField()                                 # 32 bytes, immutable.
    status = models.IntegerField(default=1)                     # 1 ACTIVE, 2 CLOSED.
    created_utc = models.DateTimeField(auto_now_add=True)
    last_login_utc = models.DateTimeField(null=True)
    display_name = models.CharField(max_length=64, blank=True, default="")

class Credential(models.Model):
    account = models.ForeignKey(Account, on_delete=models.PROTECT,
                                related_name="credentials")
    provider = models.IntegerField()                            # IDs of §3, fixed forever.
    status = models.IntegerField(default=1)                     # 1 ACTIVE, 2 DISABLED.
    lookup_hash = models.CharField(max_length=64, null=True)    # HMAC hex; NULL once disabled.
    tombstone_hash = models.CharField(max_length=64, blank=True, default="")
    identifier_encrypted = models.BinaryField(null=True)        # Display only; erased on disable.
    link_signature = models.CharField(max_length=96)            # Account key over link record.
    linked_utc = models.DateTimeField(auto_now_add=True)
    disabled_utc = models.DateTimeField(null=True)

    class Meta:
        constraints = [
            models.UniqueConstraint(fields=["account", "provider"],
                                    name="one_credential_per_provider_forever"),
            models.UniqueConstraint(fields=["lookup_hash"],
                                    condition=models.Q(status=1),
                                    name="unique_active_identity"),
        ]

class LoginChallenge(models.Model):
    """Pending email/SMS OTP, and pending OAuth state. Short-lived, aggressively pruned."""
    kind = models.IntegerField()                # 1 EMAIL_OTP, 2 SMS_OTP, 3 OAUTH_STATE.
    lookup_hash = models.CharField(max_length=64)               # Identifier being proven.
    secret_hash = models.CharField(max_length=64)               # SHA-256 of code / state.
    payload = models.JSONField(default=dict)    # PKCE verifier, nonce, return target, etc.
    attempts = models.IntegerField(default=0)
    created_utc = models.DateTimeField(auto_now_add=True)
    expires_utc = models.DateTimeField()

class WebSession(models.Model):
    token_hash = models.CharField(max_length=64, unique=True)   # SHA-256 of opaque token.
    account = models.ForeignKey(Account, on_delete=models.CASCADE,
                                related_name="web_sessions")
    created_utc = models.DateTimeField(auto_now_add=True)
    last_used_utc = models.DateTimeField(auto_now=True)
    expires_utc = models.DateTimeField()                        # Absolute cap.
    revoked_utc = models.DateTimeField(null=True)
    user_agent = models.CharField(max_length=128, blank=True, default="")

class DeviceDelegation(models.Model):
    """Account key's signed statement that an installation may act as the account."""
    account = models.ForeignKey(Account, on_delete=models.CASCADE,
                                related_name="delegations")
    installation = models.ForeignKey("api.Installation", on_delete=models.CASCADE)
    certificate = models.JSONField()            # Canonical signed delegation record.
    signature = models.CharField(max_length=96) # Account key over the canonical bytes.
    issued_utc = models.DateTimeField(auto_now_add=True)
    expires_utc = models.DateTimeField()
    revoked_utc = models.DateTimeField(null=True)
    label = models.CharField(max_length=64, blank=True, default="")

class AccountEvent(models.Model):
    """Append-only per-account audit chain: created / linked / disabled / delegation
    issued / delegation revoked / session revoked / closed."""
    account = models.ForeignKey(Account, on_delete=models.PROTECT,
                                related_name="events")
    seq = models.IntegerField()                 # 0,1,2,... dense per account.
    event_type = models.IntegerField()
    payload = models.JSONField(default=dict)    # Hashes only, never plaintext identifiers.
    prev_hash = models.CharField(max_length=64) # SHA-256 chain within the account.
    signature = models.CharField(max_length=96) # Account key over (prev_hash ‖ payload).

    class Meta:
        constraints = [models.UniqueConstraint(fields=["account", "seq"],
                                               name="dense_event_chain")]
```

`on_delete=PROTECT` on `Credential` and `AccountEvent` is intentional: accounts are closed,
never deleted, so history and tombstones survive.

## 7. Authentication Flows

All flows converge on one internal function:
`resolve_identity(provider, subject) → existing account | consent-to-create`. Nothing else
creates accounts or matches credentials.

### 7.1 OAuth Providers (Google / Baidu / Facebook / LinkedIn)

Authorization Code flow with PKCE (S256), server-side confidential client, current best
practice even with a client secret. No implicit flow, no hybrid flow, no embedded webviews
(Google actively blocks them; the desktop app uses the system browser, §7.4).

1. Browser hits `GET /api/login/oauth/<provider>/start?return=…`. Server creates a
   `LoginChallenge(kind=OAUTH_STATE)` holding `state`, `nonce`, PKCE verifier and the
   validated return target (allow-list: `https://mv.ramshanker.in/*` or the app loopback,
   §7.4), then 302-redirects to the provider.
2. Provider redirects to `GET /api/login/oauth/<provider>/callback`. Server checks `state`
   (single-use, expiring), exchanges the code (with PKCE verifier and client secret),
   validates the identity per §3, and derives `subject`.
3. `resolve_identity` → existing account: session issued, redirect to return target.
   Unknown identity: consent screen (§12) before any account is created.

Redirect URIs are registered with each provider as exact strings. Client IDs and secrets
live in the environment, per provider.

### 7.2 Email Login

1. `POST /api/login/email/start` with `{email, turnstileToken}`. Server verifies Turnstile
   (§9), rate-limits (§11), and — regardless of whether the address is known — answers
   uniformly ("if the address is valid a code was sent") to prevent account enumeration.
2. A 6-digit code (CSPRNG) is emailed; `LoginChallenge` stores its SHA-256 with a 10-minute
   expiry and 5-attempt budget, constant-time comparison on verify.
3. `POST /api/login/email/verify` with `{email, code}` → `resolve_identity(EMAIL, addr)`.

Outbound mail must go through a transactional provider with SPF/DKIM/DMARC alignment — the
hosting machine must never speak SMTP to the world directly (OPEN, §16).

### 7.3 Mobile Login

Same two-step shape as email (`/api/login/mobile/start`, `/verify`), plus SMS-specific
abuse controls, because SMS costs money and "SMS pumping" fraud is the dominant
contemporary attack on OTP endpoints:

* Turnstile verification **before** any SMS is sent, no exceptions.
* Numbers normalized to E.164; premium-rate and unallocated ranges rejected.
* Hard caps: per number per day, per IP per day, and a global daily SMS budget with
  alerting — if the budget trips, mobile login degrades gracefully to the other five
  providers.
* Resend cooldown (60 s) and the same 5-attempt / 10-minute code policy as email.

### 7.4 Desktop Application Login

The application never embeds a browser and never sees provider tokens. It reuses the
machinery that already authenticates telemetry: the installation Ed25519 key pair and the
per-launch session key (`crypto.py` chain).

1. App opens the **system browser** at
   `https://mv-server.ramshanker.in/api/login/device/start?install_key=<pub>&port=<n>`
   with a fresh CSRF-style challenge. The user logs in with any provider (full §7.1–7.3
   flows, Turnstile included, in a real browser where it belongs).
2. The server shows a consent page: "Authorize this installation of Vishwakarma
   (key `Ab3…`, first seen …) to use this account?" On approval it creates a
   `DeviceDelegation`: a canonical record `(account_pub, installation_pub, issued, expires)`
   signed by the **account** private key, and delivers it to the app via the loopback
   redirect `http://127.0.0.1:<port>/…` (RFC 8252 native-app pattern).
3. From then on, authenticated app requests carry the delegation certificate alongside the
   existing chain. Verification extends `crypto.py` naturally:
   account key --signs--> installation key --signs--> session key --signs--> body.
   The server additionally checks the `DeviceDelegation` row is unexpired and unrevoked,
   so revocation is immediate despite the signed certificate.

The app stores only the delegation certificate — it never holds the account private key,
provider tokens, cookies, or OTPs. Users see and revoke their devices in the account
dashboard; revocation just stamps `revoked_utc`.

Delegations expire after 1 year and renew silently on use past the halfway mark.

## 8. Sessions: Website

`mv.ramshanker.in` (static Hugo) and `mv-server.ramshanker.in` (Django) are different
origins but the **same site** (same registrable domain), which makes the cookie story
clean:

* Session cookie: opaque 256-bit random token, stored server-side as SHA-256
  (`WebSession.token_hash`). `HttpOnly; Secure; SameSite=Lax; Path=/api/`, host-only on
  `mv-server.ramshanker.in` — it is never sent to the static host or any other subdomain.
* Static-site JavaScript calls the API with `fetch(…, {credentials: "include"})`. CORS on
  the Django side: `Access-Control-Allow-Origin: https://mv.ramshanker.in` (exact, no
  wildcard), `Access-Control-Allow-Credentials: true`, `Vary: Origin`.
* CSRF defense in depth: every state-changing request must carry the custom header
  `X-MV-Request: 1` (its presence forces a CORS preflight), and the server rejects any
  such request whose `Origin` is not exactly `https://mv.ramshanker.in`.
* Lifetimes: 30-day idle timeout, 1-year absolute cap, token rotated on every login and
  on credential disable. Logout deletes server-side, not just the cookie.
* No JWTs for browser sessions — opaque tokens are trivially revocable and there is
  exactly one verifier (our own server), so JWT buys nothing but foot-guns.

The account dashboard (list credentials, disable credential, list/revoke devices and
sessions, recovery codes, close account) is a small JS page on the static site consuming
these APIs — the server renders no HTML except the OAuth consent/device pages it must.

## 9. Cloudflare Turnstile

Both hosts already sit behind Cloudflare. Turnstile (managed, mostly-invisible mode) is
the anti-automation layer for every anonymous POST that costs money or mutates state:

* email OTP send, SMS OTP send (the expensive ones),
* the account-creation consent submission (§12),
* OTP verify endpoints once an identifier crosses the failure threshold of §11.

The widget runs on the static site and on server-rendered consent pages; the token travels
in the request body; the server calls `siteverify` with the secret key and the client IP,
and every token is single-use. OAuth `start` redirects are not Turnstile-gated — they cost
us nothing and the provider runs its own bot defenses. Desktop login needs no special case
because §7.4 routes through a real browser anyway.

Turnstile failure responses are indistinguishable from other validation failures, to avoid
teaching bots which layer rejected them.

## 10. URL Map

Everything stays under the existing `/api/` prefix; the reserved `/api/login` stub in
`api/views.py` is retired in favor of this family, routed to the new `accounts` app:

| Method | URL | Purpose |
|--------|-----|---------|
| GET  | `/api/login/session`                    | Who am I (account pub key, display name) |
| POST | `/api/login/logout`                     | Revoke current session |
| GET  | `/api/login/oauth/<provider>/start`     | Begin OAuth, 302 to provider |
| GET  | `/api/login/oauth/<provider>/callback`  | Code exchange, identity resolution |
| POST | `/api/login/email/start`                | Send email OTP (Turnstile) |
| POST | `/api/login/email/verify`               | Verify OTP → session |
| POST | `/api/login/mobile/start`               | Send SMS OTP (Turnstile) |
| POST | `/api/login/mobile/verify`              | Verify OTP → session |
| POST | `/api/login/create`                     | Consent-confirmed account creation (§12) |
| GET  | `/api/login/credentials`                | List credential slots and their status |
| POST | `/api/login/credentials/link`           | Begin linking a provider to the session's account |
| POST | `/api/login/credentials/disable`        | One-way disable (writes tombstone) |
| GET  | `/api/login/devices`                    | List delegations and web sessions |
| POST | `/api/login/devices/revoke`             | Revoke a delegation or session |
| GET  | `/api/login/device/start`               | Desktop app authorization page (§7.4) |
| POST | `/api/login/recovery/generate`          | Issue one-time recovery codes (§12) |
| POST | `/api/login/recovery/verify`            | Redeem a recovery code → session |
| POST | `/api/login/close`                      | Close the account (§12) |

## 11. Abuse Controls and Rate Limits

Extending the telemetry server's existing cache-based limiter (`_rate_limited`):

* Per IP: 30 login-family POSTs/hour. Per identifier hash: 5 OTP sends/hour, 10/day.
* Per identifier: after 5 failed verifications the challenge is destroyed; after 3
  destroyed challenges in 24 h the identifier is Turnstile-gated even on verify.
* OAuth `state`, OTP challenges, and consent tokens are single-use with short expiry
  (10 min) and are pruned by the existing daily maintenance job.
* Uniform, delay-equalized responses on every path that could confirm whether an
  identifier or account exists.
* All limits are enforced in Django, not only in Cloudflare rules — Cloudflare is a
  shield, not the source of truth.

## 12. Account Lifecycle

**Creation.** Only through the consent screen. When `resolve_identity` finds no account,
the server issues a short-lived signed consent token and the UI must display, verbatim in
spirit: *"No account exists for this identity. Create a new one? If you already have an
account, sign in with that method instead — accounts can never be merged."* Only
`POST /api/login/create` with that token (plus Turnstile) generates the key pair and salt.
This friction is deliberate: since merging is impossible, accidental duplicate accounts
are the one mistake the design cannot undo.

**Linking.** A logged-in user may link additional providers (each burns that provider's
slot per §4). The dashboard nags until at least two active credentials exist, because:

**Recovery.** There is no "forgot password" (no passwords) and no support-driven identity
override (that would be a merge/takeover vector). Recovery = having a second active
credential, or redeeming a one-time recovery code (provider slot 8): 10 codes of 128-bit
entropy, shown once, stored hashed; regenerating invalidates the previous set. An account
whose every credential is lost is permanently inaccessible — this is stated plainly in the
UI at creation time. The design accepts this cost in exchange for making social-engineering
takeover structurally impossible.

**Disable.** §4. Requires an active session; re-verifies Turnstile; writes the tombstone
and an `AccountEvent`; revokes nothing else (sessions and delegations continue — they
belong to the account, not the credential).

**Closure.** Soft: `status=CLOSED`, all sessions and delegations revoked, all credentials
disabled with tombstones, display name and encrypted identifiers erased. The key pair,
salt, tombstones, and event chain are retained forever — they are non-reversible and are
the proof fabric that prevents identity reuse confusion. A closed account's identities may
create fresh accounts (no merge, no resurrection).

## 13. Data at Rest and Privacy

Following the telemetry precedent ("nothing personally identifiable, the public key is the
only identifier"):

* Stored in plaintext: account public keys, timestamps, provider IDs, statuses.
* Stored hashed: subjects (peppered lookup / salted tombstone), session tokens, OTPs,
  recovery codes.
* Stored encrypted (env KEK): account private keys, display identifiers.
* Not stored at all: passwords (none exist), OAuth access/refresh tokens (discarded after
  identity extraction — we fetch nothing from providers post-login), raw IPs in login
  tables (only coarse rate-limit counters in cache), Turnstile tokens.
* Backups inherit safety: a leaked dump yields no identities without the environment
  secrets (pepper + KEK), which are handled like the release-signing PFX — never in git.

## 14. Best-Practice Checklist

For reviewers, the contemporary practices this design commits to: authorization code +
PKCE only; exact redirect URI registration; `state` + `nonce` single-use; ID-token
signature/`iss`/`aud`/`exp` validation against provider JWKS; `appsecret_proof` for
Facebook; system browser + loopback for native app (RFC 8252); no embedded webviews; no
password storage; CSPRNG OTPs, hashed, constant-time compared, attempt-capped,
short-lived; SMS-pumping caps and budget alarm; anti-enumeration uniform responses;
opaque revocable session tokens, hashed at rest; `HttpOnly/Secure/SameSite` cookies;
exact-origin CORS with credentials; custom-header + Origin CSRF check; Turnstile with
server-side `siteverify`; per-IP and per-identifier rate limits in the application; append-
only signed audit chain; secrets in environment only; HSTS via Cloudflare; no account
enumeration, no account merger, no email-claim auto-linking.

## 15. Explicitly Out of Scope

* Account merger, identity transfer, admin identity override — never, by design.
* Usernames, profiles, avatars, social features. `display_name` is the entire profile.
* Password login in any form, including "set a password later".
* SAML / enterprise SSO / multi-tenant orgs.
* Using login to gate the documentation — the site stays fully readable anonymously;
  login exists for future per-account features (licensing, sync, collaboration per
  `storage.md`).

## 16. OPEN Items

* **Email delivery provider** — transactional service choice (SES / Mailgun / Resend / …);
  driven by cost and India + China deliverability.
* **SMS gateway** — Twilio vs MSG91 vs provider-per-region; China deliverability again.
* **Passkeys (provider 7)** — strong candidate for the seventh method; needs its own
  design pass (WebAuthn ceremonies, platform authenticator UX in the desktop app).
* **TOTP second factor** — whether high-value future actions (license management) require
  a second active credential re-verification instead of classic 2FA.
* **KEK custody** — env variable now; evaluate OS keyring / TPM on the server later.
* **Django native vs library** — hand-rolled OAuth per §7.1 vs `authlib`. Leaning
  hand-rolled for the four fixed providers, consistent with the project's
  avoid-dependencies philosophy, but `authlib` is the fallback if provider quirks bloat.

## 17. Implementation Order (MVP)

1. `accounts` app skeleton: models + migrations + KEK sealing helpers → verify: unit
   tests for hash constructions, immutability constraints, and event-chain signing.
2. Email OTP end-to-end with Turnstile on the static site → verify: create account,
   session cookie works cross-subdomain from `mv.ramshanker.in`.
3. Google OIDC (the reference OAuth implementation) → verify: link + disable + tombstone
   round-trip; consent screen blocks silent account creation.
4. Desktop delegation flow, extending `crypto.py` verification → verify: app request
   accepted with valid chain, rejected instantly after dashboard revocation.
5. Remaining OAuth providers (LinkedIn OIDC ≈ Google; Facebook; Baidu last — needs a
   registered developer account) and SMS OTP behind its budget guards.
6. Dashboard page, recovery codes, account closure.

Each step ships behind the existing deploy pipeline and leaves `/api/logs` and
`/api/stats` untouched.
