---
title: "Contribution & CLA Workflow"
weight: 100209
---

This page is the design document for accepting external pull requests. It covers (a) the legal
instrument every code contributor signs — a Developer Certificate of Origin, executed electronically
through the **Aadhaar eSign** facility and brokered by our Django server — and (b) **vishwakarma-bot**,
the GitHub App that enforces the policy automatically on the repository.

---

## Contribution Policy (Two Tiers)

| Pull request touches | Requirement |
|----------------------|-------------|
| Only files under `website/` | None. Documentation fixes are welcome from anyone. |
| Anything outside `website/` | An Aadhaar-eSigned Developer Certificate of Origin must already be on file for the PR author's GitHub account. Otherwise the bot closes the PR with an explanation. |

The signed document itself is **private**: it is stored on our server (the same hardened Raspberry Pi
that runs the telemetry server, see `server/README.md`) and is never exposed publicly. The only thing
the outside world can observe is the bot's boolean decision on a PR.

---

## Contributor Identity: the GitHub Numeric User ID

GitHub usernames are mutable — an account can be renamed at any time, and after a rename the old
username becomes available for **someone else** to claim. A username is therefore unusable as a legal
identity key.

GitHub also assigns every account an **immutable numeric user ID** which is never changed and never
reused. It is available everywhere we need it:

*   REST: `GET https://api.github.com/users/<username>` → the `id` field (e.g. Ram's account has one fixed integer for life).
*   Webhooks: every `pull_request` payload carries `pull_request.user.id`.
*   OAuth: `GET /user` with the signer's token returns the same `id`.

**Design rule:** the numeric ID is the primary key of the signature database and is printed inside
the signed PDF. The username is stored only as a display snapshot taken at signing time.

---

## The Document Being Signed

The signer executes a single PDF, generated per contributor by our server, containing:

1.  The verbatim **Developer Certificate of Origin v1.1** text (developercertificate.org).
2.  A short preamble binding the DCO to this repository and to the project's source-available
    license, so one signature covers all future contributions (a "CLA-lite").
3.  The contributor's identity block: **GitHub numeric user ID**, username snapshot, email,
    date, and the agreement **version**.

The agreement text is versioned. If the text ever changes materially, existing signatures remain
valid for past contributions but the bot can be flipped to require the new version for new PRs.

---

## Aadhaar eSign: Legal Basis and Provider Options

Aadhaar eSign is an online electronic-signature service recognised under the Indian IT Act, 2000
(Section 3A read with the Second Schedule), administered by the CCA. The signer authenticates with
an Aadhaar OTP at the eSign Service Provider's (ESP) gateway; the ESP's certifying authority issues
a short-lived (~30 minute, single-use) signing certificate in the signer's name and returns a
PKCS#7/CMS signature over the document hash. Two important properties for us:

*   **The Aadhaar number never reaches us.** The ESP handles UIDAI authentication; we receive only
    the signature and a certificate carrying the signer's name. We store no Aadhaar data at all.
*   The signature is legally equivalent to a wet-ink signature in India.

Two integration routes:

1.  **Direct ESP empanelment** (Protean/NSDL, eMudhra, C-DAC, …): we register as an Application
    Service Provider (ASP), implement the CCA eSign API (v2.1/v3.2 XML request/response, form-POST
    redirect to the gateway). Paperwork-heavy; designed for volume.
2.  **Commercial aggregator** (Digio, Leegality, SignDesk, Signzy, Zoop, …): REST API + hosted
    signing page + webhook callback, priced per signature (tens of rupees). Onboarding is days,
    not months.

**Decision: start with an aggregator.** Contribution volume will be tiny; per-signature pricing is
ideal. The broker flow below is written provider-neutral so switching to direct ESP later only
replaces the gateway adapter.

*Open point:* Aadhaar covers Indian residents only. Foreign contributors cannot use this flow. For
now that is acceptable; a manual fallback (signed PDF exchanged over email at the maintainer's
discretion, recorded in the same table with `method = "manual"`) can be added without any schema
change.

---

## Signing Flow (Brokered by Our Server)

```text
Contributor                 mv-server.ramshanker.in            eSign Gateway (ESP/aggregator)
    |                                |                                   |
    | 1. open /cla/                  |                                   |
    |------------------------------->|                                   |
    | 2. redirect to GitHub OAuth    |                                   |
    |<------------------------------>|   (proves control of account,     |
    |                                |    yields numeric id + login)     |
    | 3. shown generated DCO PDF     |                                   |
    |<-------------------------------|                                   |
    | 4. click "Sign with Aadhaar"   |                                   |
    |------------------------------->| 5. doc hash + sign request        |
    |                                |---------------------------------->|
    | 6. redirected to gateway; Aadhaar OTP entered at ESP page          |
    |<------------------------------------------------------------------>|
    |                                | 7. PKCS#7 signature callback      |
    |                                |<----------------------------------|
    |                                | 8. embed into PDF, verify,        |
    |                                |    store privately, insert DB row |
    | 9. confirmation page           |                                   |
    |<-------------------------------|                                   |
```

Step details:

1.  **GitHub identification.** We use the GitHub App's own user-authorization (OAuth web flow, no
    scopes — public profile is enough). `GET /user` gives the immutable `id` and current `login`.
    No separate OAuth App is needed and no GitHub password ever touches our server.
2.  **PDF generation.** Server fills the versioned agreement template with the identity block and
    computes its SHA-256. The unsigned PDF is shown to the signer for review.
3.  **eSign hand-off.** Server calls the aggregator API with the document (or its hash, per
    provider), receives a gateway URL, and redirects the signer. Aadhaar OTP happens entirely on
    the provider's page.
4.  **Callback.** Provider posts the signed artefact back to our callback URL. Server verifies the
    signature and the certificate chain, embeds timestamp + revocation data (LTV) so the PDF stays
    verifiable long after the 30-minute certificate expires, and only then commits.
5.  **Record.** One row per GitHub numeric ID (re-signing a newer agreement version updates it).

---

## Server-Side Additions

A new Django app `cla/` beside the existing `api/` app, same project, same hardening posture
(no Django admin/auth/session apps; the OAuth round-trip is stateless via a signed `state` token).

| URL                  | Method | Purpose |
|----------------------|--------|---------|
| `/cla/`              | GET    | Entry page: policy explanation + "Sign in with GitHub". |
| `/cla/callback/github` | GET  | OAuth redirect; establishes numeric ID, shows the PDF for review. |
| `/cla/sign`          | POST   | Starts the eSign transaction, redirects to the gateway. |
| `/cla/callback/esign`| POST   | Provider callback: verify, store, confirm. |
| `/api/github/webhook`| POST   | vishwakarma-bot webhook receiver (below). |

There is deliberately **no public "is signed?" endpoint**: the webhook handler and the signature
table live in the same Django project, so the bot's check is a direct DB query — nothing to leak,
nothing extra to authenticate.

**Signature table** (SQLite, same database file strategy as telemetry):

```text
cla_signature
  github_user_id   INTEGER PRIMARY KEY  -- immutable numeric GitHub ID
  github_login     TEXT                 -- snapshot at signing time, display only
  signer_name      TEXT                 -- name from the eSign certificate
  email            TEXT
  agreement_version TEXT
  document_sha256  TEXT
  esign_txn_id     TEXT                 -- provider transaction reference
  method           TEXT                 -- "aadhaar-esign" | "manual"
  signed_at_utc    TEXT
  pdf_path         TEXT                 -- private path, see below
```

**Privacy and storage.** Signed PDFs go to a directory under the service's writable state dir
(e.g. `/var/lib/mv-telemetry/cla/`), owned by the service user, mode 0600, **outside any web root**
— gunicorn has no route that serves files from it. The PDF contains a real legal name, so under the
DPDP Act 2023 we keep collection minimal (no Aadhaar data exists to store), purpose-limited to
contribution provenance, and never published. Backups of this directory follow the same rule.

---

## vishwakarma-bot (GitHub App)

A **GitHub App** — not a personal "bot account" (against GitHub ToS for automation at scale, and a
password liability) and not a GitHub Actions workflow (the signature DB is private on our Pi; we
would have to expose a query endpoint to Actions, which the webhook design avoids entirely).
GitHub Apps get the `vishwakarma-bot[bot]` identity, fine-grained permissions, and short-lived
tokens.

### Registration steps

1.  On the repository owner account: *Settings → Developer settings → GitHub Apps → New GitHub App*.
    Name `vishwakarma-bot`, homepage `https://mv.ramshanker.in`.
2.  Webhook URL `https://mv-server.ramshanker.in/api/github/webhook`; generate a strong webhook
    secret (stored in `/etc/mv-telemetry.env` like the other secrets).
3.  Permissions: **Pull requests: Read & write** (comment + close + label), **Contents: Read-only**,
    **Metadata: Read-only**. Subscribe to the **Pull request** event. Enable *Request user
    authorization (OAuth)* — this same App provides the `/cla/` GitHub sign-in.
4.  Generate the App **private key** (PEM). Store root-only on the Pi alongside the env file. Note
    the App ID.
5.  Install the App on the Vishwakarma repository only (first on a scratch repository for testing).
6.  API authentication at runtime: sign a short-lived JWT (RS256, App ID, ≤10 min) → 
    `POST /app/installations/{id}/access_tokens` → 1-hour installation token, cached and refreshed.

### Webhook handling

On `pull_request` events with action `opened`, `reopened`, `synchronize`, or `ready_for_review`:

1.  Verify `X-Hub-Signature-256` (HMAC of the raw body with the webhook secret); reject otherwise.
2.  List changed files: `GET /repos/{owner}/{repo}/pulls/{n}/files?per_page=100`, paginated. A PR
    is *website-only* iff every entry's `filename` **and** `previous_filename` (present on renames —
    a file renamed *out of* or *into* `website/` counts as touching outside) starts with `website/`.
    The API caps the listing at 3000 files; if `changed_files` exceeds that, treat the PR
    conservatively as touching outside.
3.  Decide:

| PR touches | `pull_request.user.id` in `cla_signature`? | Action |
|------------|--------------------------------------------|--------|
| only `website/` | (not checked) | Leave open. Label `website-only`. |
| outside `website/` | yes | Leave open. Label `dco-verified`. |
| outside `website/` | no  | Post the explanation comment, then `PATCH …/pulls/{n}` with `state: "closed"`. |

4.  The explanation comment links to `https://mv-server.ramshanker.in/cla/`, states that
    documentation-only PRs need no signature, and tells the author to **reopen the PR after
    signing** — the `reopened` event re-runs the check, which will then pass. Closure is therefore
    never permanent and no extra "recheck" command is needed.

### Defense in depth

Closing a PR is advisory — a maintainer could reopen and merge by mistake. Additionally publish a
**check run** (`vishwakarma-bot/dco`) with success/failure on the head SHA, and mark it a required
status check in branch protection for `main`. Then even an open PR cannot merge until the check
passes.

### Edge cases

*   **Allowlist** (by numeric ID): the maintainer's own account and known automation such as
    `dependabot[bot]` skip the check.
*   **Multi-author PRs:** the check keys on the PR author only. Commits authored by others inside
    the PR are accepted under the PR author's certification (that is exactly what DCO clause (b)/(c)
    covers). Revisit only if it becomes a real problem.
*   **Bot outage:** webhooks missed while the server is down are visible under the App's *Advanced →
    Recent Deliveries* and can be redelivered manually; the required check (unset = pending) blocks
    merging in the meantime.

---

## Implementation Order

1.  Aggregator selection + ASP onboarding (commercial step, longest lead time — start first).
2.  `cla/` Django app: GitHub OAuth identification + PDF generation, with a stub "sign" step;
    schema + private storage in place. → verify: full flow locally with the stub.
3.  Real eSign adapter behind the stub interface. → verify: one real signature end-to-end (self).
4.  GitHub App registration + webhook endpoint + decision logic. → verify: on a scratch repository,
    all four cases of the decision table.
5.  Check-run publication + branch protection on the real repository. → verify: unsigned test
    account's PR is closed and unmergeable; after signing and reopening it passes.
6.  Public announcement: short contributor-facing page on the website linking to `/cla/`.

---

## Open Questions

*   Non-Aadhaar (foreign) contributors — manual fallback exists on paper; formalise only when the
    first such contributor actually appears.
*   Agreement version bumps — current design keeps old signatures valid; whether a re-sign is ever
    forced is a policy decision deferred until the text actually changes.
*   Whether `website/`-only PRs should also get a gentle nudge comment about the DCO for future
    code contributions (cosmetic, decide during implementation).
