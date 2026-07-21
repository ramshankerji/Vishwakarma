---
title: "Direct Support Chat"
weight: 100115
---
This page is the design document for the **direct support chat** — the real content of the
**Support view (view 4) of the Application Tab** defined in the *Tab System and the Application
Tab* chapter (tabs.md). A user types a message inside the application and developers answer;
the reply appears in the same view within a minute. Like login.md, plain-text sections are
**locked decisions**; items marked **OPEN** are still changeable.

Shape of the system: one dedicated set of tables on the existing Django server (`./server`,
`https://mv-server.ramshanker.in`), one HTTPS endpoint, one C++ file pair
`SupportRequests.h` / `SupportRequests.cpp` on the client, and one server-rendered developer
inbox page. Phase 1 is **anonymous only**: the conversation is linked to the identity system
at the installation level (Ed25519 installation key), not to a user account — accounts do not
exist yet (login.md is design-only).

## 1. What it is — and is not

* **One rolling conversation per installation.** WhatsApp-style: a single message stream,
  newest at the bottom. No tickets, no subjects, no categories, no statuses visible to the
  user, no attachments, no email bridge. If ticketing is ever needed, it grows on the
  developer side; the user-facing model stays "a chat".
* **Anonymous.** No name, no email, no phone. The installation public key
  (`api.Installation`, the telemetry identity) is the only identifier, displayed to
  developers as its first 12 characters — exactly `Installation.__str__`. Users may
  volunteer contact details in the text if they wish; the system never asks.
* **Polling, not push.** The client POSTs to the server and polls **once per minute** for
  developer replies while the conversation is active. No websockets, no push channel — the
  same "the dashboard polls" philosophy as the server admin module (server.md §4.1). Support
  latency of ≤60 s is far better than email and costs one tiny request.
* **Not VishwakarmaServer.** The chat talks to the Django telemetry/account server
  (`mv-server.ramshanker.in`, the Raspberry Pi behind Cloudflare) — the same server that
  receives `/api/logs`. VishwakarmaServer.exe (server.md) is unrelated and remains
  "Support: not applicable" per its view table.

## 2. Identity: anonymous now, account later

Phase 1 authentication reuses the telemetry chain verbatim (`server/api/crypto.py`,
`code-core` AccountManager):

```
installation key --signs--> session key --signs--> request body (X-MV-Signature)
```

A conversation therefore belongs to an `api.Installation` row. This is deliberate: the
support chat ships before login, with zero new identity machinery.

When the accounts system of login.md ships, the upgrade path is already defined there: a
`DeviceDelegation` binds an installation key to an account key. The support conversation
**stays keyed on the installation** — the inbox merely gains a display label ("delegated to
account Ab3…, display name …") by joining through the delegation table. No conversation
merging, mirroring the no-merge doctrine of login.md: two installations of the same person
remain two conversations. A nullable `account` foreign key on the conversation is added only
when the `accounts` app actually exists — nothing is pre-created now.

## 3. Server data model — new Django app `support`

Following the login.md precedent ("the `api` app stays telemetry-only"), the chat lives in a
new Django app `support` inside `./server`. It imports `api.crypto.verify_request` and
references `api.Installation`; the `api` app is not modified.

The request that produced this document asked for "a dedicated table". The message stream is
that table; a small companion row per installation exists because *blocked* and
last-activity state need a home that is not a message. Collapsing to literally one table is
possible only by giving up the block switch — not worth it for an anonymous public endpoint.

```python
class SupportConversation(models.Model):
    """One rolling chat per installation. Created lazily on the first message."""
    installation = models.OneToOneField("api.Installation", on_delete=models.CASCADE,
                                        related_name="support_conversation")
    status = models.IntegerField(default=1)          # 1 OPEN, 2 BLOCKED.
    created_utc = models.DateTimeField(auto_now_add=True)
    last_user_utc = models.DateTimeField(null=True)  # Newest user message.
    last_dev_utc = models.DateTimeField(null=True)   # Newest developer reply.

class SupportMessage(models.Model):
    conversation = models.ForeignKey(SupportConversation, on_delete=models.CASCADE,
                                     related_name="messages")
    direction = models.IntegerField()                # 1 USER, 2 DEVELOPER.
    client_msg_id = models.BigIntegerField(null=True)  # Idempotency; NULL for replies.
    text = models.TextField()                        # ≤ 4000 chars, server-truncated.
    created_utc = models.DateTimeField(auto_now_add=True)
    delivered_utc = models.DateTimeField(null=True)  # Replies: proven received (§4).

    class Meta:
        constraints = [
            models.UniqueConstraint(fields=["conversation", "client_msg_id"],
                                    name="unique_conversation_client_msg"),
        ]
        indexes = [models.Index(fields=["conversation", "id"])]
```

The idempotency construction is the `UsageRecord.client_row_id` pattern: a retried upload
re-sends the same `client_msg_id` and `get_or_create` makes the insert a no-op, so a lost
response never duplicates a message. "Needs reply" on the inbox is simply
`last_user_utc > last_dev_utc` — no extra bookkeeping.

## 4. API — one endpoint, `POST /api/support/sync`

One endpoint does everything: it uploads any pending user messages **and** returns new
developer replies in the same round trip (the `TryUpload` shape from ImprovementData.cpp).
A poll is a sync with an empty message list. Routed via
`path("api/support/", include("support.urls"))` in the project `urls.py`; the CORS regex
(`^/api/login$`) is untouched, so the endpoint is native-app-only.

Request body (signed exactly like `/api/logs` — same key fields, `X-MV-Signature` header
over the exact body bytes):

```json
{
  "installationPublicKey": "…", "sessionPublicKey": "…", "sessionKeySignature": "…",
  "appVersion": 123,
  "afterId": 0,
  "messages": [ { "clientMsgId": 8412337804, "text": "The DXF import fails when …" } ]
}
```

Server processing, in order:

1. Size cap (Django's 1 MB), JSON parse, `verify_request` chain — identical rejection
   behavior to `/api/logs` (401 on bad chain).
2. Rate limit: the existing `_rate_limited` helper with a **separate cache bucket**
   (`"support:" + key`), same 120/hour budget — one poll per minute plus sends fits; the
   telemetry bucket is unaffected.
3. `get_or_create` the `Installation` (updating `app_version` / `last_seen`, as logs does)
   and its `SupportConversation`. If `status == BLOCKED`: discard input, return
   `{"status": "blocked"}` — the client stops polling for this launch.
4. Insert user messages idempotently (≤ 32 per request, text truncated to 4000 chars),
   update `last_user_utc`, collect `ackClientMsgIds`.
5. **Delivery proof:** any developer reply with `id ≤ afterId` was demonstrably received by
   the client (the client only advances `afterId` over messages it has stored), so stamp
   `delivered_utc` where NULL. Optimism-free: a lost response never marks anything.
6. Return replies and messages after the cursor: up to 200 rows with `id > afterId`,
   ordered by `id` — the client pages by looping until fewer than 200 arrive.

```json
{
  "status": "ok",
  "ackClientMsgIds": [8412337804],
  "messages": [ { "id": 5711, "from": 2, "text": "Fixed in build 124 — …", "utc": "2026-07-21T09:14:03Z" } ]
}
```

Note that the response stream contains **both directions** (`from`: 1 user, 2 developer).
The server is the single source of truth for history; the client rebuilds the whole
conversation from `afterId = 0` at every launch (§5) and needs no local chat database.

## 5. C++ client — `SupportRequests.h` / `SupportRequests.cpp`

A single file pair, modeled line-for-line on the ImprovementData pattern: a dedicated
thread started from `wWinMain`, a 1-second `Sleep` loop, exit on the global
`shutdownSignal`. The file owns state and networking only — **no drawing code** (rendering
belongs to `ApplicationTab.cpp`, tabs.md Decision 5).

```cpp
// SupportRequests.h — public surface.
struct SupportChatMessage {
    uint64_t serverId;      // 0 while pending (not yet acknowledged).
    bool fromDeveloper;
    long long epochUtc;
    std::string utf8Text;
};

namespace SupportRequests {
// UI thread, on Send button: caps at 4000 chars, assigns a random 63-bit clientMsgId,
// queues, and requests an immediate sync (≤ 1 s latency instead of the 60 s cadence).
void QueueOutgoingMessage(const std::string& utf8Text);

// Render thread. Bumps on every state change; the overlay re-copies the snapshot only
// when the version moved, so per-frame cost is one atomic load.
uint64_t StateVersion();
// Confirmed history (server order) followed by pending messages, plus reachability flag.
void GetSnapshot(std::vector<SupportChatMessage>& out, bool& serverReachable);

// Render thread, each frame the Support view is visible: drives the fast poll cadence
// and marks replies read (persists the read cursor).
void NoteSupportViewVisible();
int UnreadReplyCount();   // For a future badge on the Support view button.
}

void SupportRequestsThread();   // Started from wWinMain, like ImprovementDataThread.
```

Locked implementation decisions:

* **Server-authoritative history, in-memory client.** No local SQLite. On the first sync of
  a launch the client sends `afterId = 0` and receives the full conversation (small — this
  is human-typed support text). Pending outbound messages live in memory and are retried
  every cycle; at shutdown the thread makes one final short-timeout sync attempt (the
  ImprovementData "final partial row" idea). A message typed and never delivered before a
  kill is lost — accepted for v1.
* **Marker file** `%LOCALAPPDATA%\Mission Vishwakarma\SupportChat.txt` (next to
  `ImprovementStatistics.db`), containing one number: the last-read server message id.
  Its existence means "this installation has a conversation". **No marker and nothing
  queued → the thread does nothing at all** — the 99 % of installations that never
  contact support generate zero support traffic, the same airtight-idleness standard as
  tab 0's never-pushed queues. The file is created on the first send; the read cursor in
  it feeds `UnreadReplyCount` across launches.
* **Cadence** (constants at the top of the file, tunable):

  | Condition | Sync interval |
  |---|---|
  | `QueueOutgoingMessage` called | immediately (next loop tick, ≤ 1 s) |
  | Hot: view visible recently, unsent messages pending, or last activity < 1 hour | `kSupportSyncFastSeconds = 60` |
  | Cold: conversation exists, nothing recent | `kSupportSyncSlowSeconds = 900` |
  | No conversation, nothing queued | never |
  | Server answered `"blocked"` | never again this launch |

  "Once per minute" therefore holds exactly when someone is actually waiting for an
  answer; an old conversation still catches a late reply within 15 minutes of any launch.
* **Networking:** the ~45-line WinHTTP `HttpPostJson` helper and the `JsonEscape` helper
  are duplicated from ImprovementData.cpp as local statics — the price of the single-file
  requirement. If a third file ever needs them, hoist all three users to a small shared
  unit; do not pre-abstract now. URL constants follow the same `#ifdef _DEBUG` split:
  `http://127.0.0.1:8000/api/support/sync` vs
  `https://mv-server.ramshanker.in/api/support/sync`.
* **Threading contract:** one mutex over the message vectors; UI thread calls
  `QueueOutgoingMessage`, render threads copy snapshots, the support thread does all
  network I/O and state mutation. The data is tiny; no lock-free machinery.
* `clientMsgId`: 63-bit from OpenSSL `RAND_bytes` (fits the server's signed
  `BigIntegerField`).

## 6. The Support view UI

Depends on tabs.md Phase 3 (stub panels exist). The Support stub in
`BuildApplicationTabOverlay` becomes: an anonymity notice line ("This chat is anonymous —
identified only by installation key Ab3…"), the scrollable message list (developer replies
left, user messages right, pending ones greyed with "sending…", a "server unreachable —
will retry" line when applicable), a text input using the existing overlay text-field
machinery, and a Send button that calls `QueueOutgoingMessage`. The view calls
`NoteSupportViewVisible()` each frame it draws. All interaction rides the standard
`UIInput` snapshot; nothing touches tab 0's queues (tabs.md Decision 3).

Known v1 limitation: the overlay text stack renders the codepoint set of the MSDF atlas and
has no complex shaping — the same constraint that forces the Devanagari word-mark to be an
SVG (tabs.md Decision 7). Support chat text is therefore effectively Latin-script in v1;
full Unicode chat follows the general text-shaping roadmap, not this document.

## 7. Developer reply surface — the inbox

`settings.py` records a deliberate decision: *"Deliberately minimal application surface: no
admin, no auth, no sessions."* That stands. **`django.contrib.admin` is not enabled** — it
would drag in auth, sessions, contenttypes and staticfiles on a hardened public server to
serve exactly two developers. Instead, the `support` app renders two small pages in the
`stats.html` style (own templates, inline styles, no JavaScript needed):

* `GET /api/support/inbox` — conversations ordered needs-reply-first, then by newest
  activity. Each row: key prefix, app version, message counts, last activity, and — joined
  from telemetry already on hand (`Installation.app_version`, latest `HardwareReport`) —
  OS and GPU. That join is the payoff of anchoring identity on `Installation`: developers
  see the machine context of a bug report without asking, and without collecting anything
  new.
* `GET /api/support/inbox/<installation_pk>` — the thread, a reply `<form>`, and a
  block/unblock button. `POST` of the form inserts a `direction=DEVELOPER` row and stamps
  `last_dev_utc`. Undelivered replies (`delivered_utc` NULL) render marked, so a developer
  knows the user has not seen the answer yet.

Access control: **HTTP Basic auth** checked in the view against a long random
`MV_SUPPORT_TOKEN` environment variable (constant-time compare, 401 +
`WWW-Authenticate` otherwise, failures rate-limited through the cache limiter). Browser
native, zero session machinery, TLS via Cloudflare, secret handled like `MV_SECRET_KEY`
(env file, never in git). Two implementation notes from reading the existing code:
`SecurityHeadersMiddleware` uses `setdefault`, and its global CSP declares
`form-action 'none'` — the inbox views must set their own CSP header (identical but
`form-action 'self'`) or the reply form will be blocked. Django template auto-escaping
covers XSS from hostile message text; `X-Robots-Tag: noindex` is already global.

Developer notification of new messages (email/webhook on first unanswered message) is
**OPEN** — it wants the transactional-email decision of login.md §16. Until then the inbox
is pull-only.

## 8. Abuse, privacy, retention

* **Abuse surface:** anyone can mint an Ed25519 key pair, so the signature chain gates
  malformed traffic, not determined spam — the same accepted exposure as `/api/logs`.
  Layers: Cloudflare in front, per-key rate bucket (120/h), 32 messages per request,
  4000 chars per message, 1 MB body cap, and the per-conversation BLOCKED switch as the
  human backstop. A blocked client stops polling by protocol (§4.3, §5).
* **Privacy:** the system collects nothing new about the user. Chat text is voluntary,
  user-typed content, transmitted over TLS, stored in the same SQLite database and trust
  domain as telemetry, visible only to developers. The client never auto-attaches logs,
  files, or diagnostics; an explicit opt-in "attach diagnostics" button is a possible
  future, **OPEN**.
* **Retention:** conversations persist indefinitely for now (they are the support
  history). A pruning policy (e.g. delete conversations idle > 1 year) is **OPEN** and
  would ride the maintenance job that login.md also assumes.

## 9. Phases

Server phases are independently testable with `curl` as the client; C++ phases depend on
tabs.md (Phase B only needs the app to start a thread; Phase D needs tabs.md Phase 3).

* **Phase A — server core.** `support` app, models + migration, `/api/support/sync`.
  Verify: signed request round-trip; unsigned/invalid chain → 401; re-sending the same
  `clientMsgId` inserts once; `afterId` paging; `delivered_utc` stamps only after the
  cursor passes; rate limit and BLOCKED behavior.
* **Phase B — client core.** `SupportRequests.h/.cpp`, thread launched from `wWinMain`
  next to `ImprovementDataThread`. Against the local debug server: send from a temporary
  debug hook, watch rows appear; kill the network mid-send and watch the retry;
  delete the marker file and prove **zero requests** across a full session (the tab-0
  "queues stay empty" standard of proof).
* **Phase C — inbox.** Both pages + Basic auth + reply. Verify end-to-end on one machine:
  app message → inbox → reply → visible in the client's next sync ≤ 60 s; block stops the
  client's polling.
* **Phase D — Support view UI.** Replace the stub panel per §6. Verify: full manual chat
  round trip inside the application; unread count survives a restart via the marker file.

## 10. File touch list

| File | Change |
|------|--------|
| `server/support/` (`models.py`, `views.py`, `urls.py`, `templates/support/…`, `migrations/`) | **New app.** Everything in §3, §4, §7. |
| `server/improvement_server/settings.py` | `INSTALLED_APPS += ["support"]`; document `MV_SUPPORT_TOKEN`. |
| `server/improvement_server/urls.py` | `path("api/support/", include("support.urls"))`. |
| `server/deploy/mv-telemetry.env.example` | `MV_SUPPORT_TOKEN=` line. |
| `code-core/SupportRequests.h` / `.cpp` | **New.** Everything in §5. |
| `code-core/Main.cpp` | Start/join `SupportRequestsThread` alongside `ImprovementDataThread`. |
| `code-core/ApplicationTab.cpp` | Support view panel replacing the stub (Phase D). |
| `Vishwakarma.vcxproj` | Add the new file pair. |

## 11. OPEN items

* Developer notification channel for new messages (blocked on the email-provider decision).
* Retention/pruning policy for idle conversations.
* Poll cadence constants (60 s / 900 s / 1 h hot window) — confirm at implementation.
* Non-Latin chat text — waits on the general text-shaping roadmap (§6).
* Opt-in diagnostics attachment from the Support view.
* Whether the future account linkage (§2) should also show account display names to
  developers by default, or only on user consent.
