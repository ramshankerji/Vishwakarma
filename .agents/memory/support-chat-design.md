---
name: support-chat-design
description: "DESIGNED ONLY (website support.md) — anonymous in-app support chat: server `support` Django app, single /api/support/sync endpoint, client SupportRequests.h/.cpp thread"
metadata: 
  node_type: memory
  type: project
  originSessionId: 3fdbe4de-12f9-4938-8145-29539939feb0
  modified: 2026-07-21T01:34:02.603Z
---

Direct support chat design doc lives at `website/content/software/support.md` (written 2026-07-21). DESIGNED ONLY — nothing implemented. It is the real content of the Support view (view 4) of [[application-tab-plan]].

Key locked decisions:
- One rolling anonymous conversation per `api.Installation` (Ed25519 chain reused verbatim from [[telemetry-system]]); accounts linkage deferred until [[login-design]] ships — no conversation merge, no nullable FK pre-created.
- Server: new Django app `support` (api stays telemetry-only), TWO tables — SupportConversation (status OPEN/BLOCKED, last_user/dev_utc) + SupportMessage (client_msg_id idempotency à la UsageRecord). Ram asked for "a dedicated table"; the doc justifies the second row (block switch needs a home) — flagged explicitly, he may push back.
- One endpoint `POST /api/support/sync`: upload + poll in one round trip (TryUpload shape), afterId cursor paging (200/page), delivered_utc stamped only when a later afterId proves receipt.
- Client: single file SupportRequests.h/.cpp, ImprovementDataThread pattern (1s loop, shutdownSignal), server-authoritative history (afterId=0 full refetch each launch, NO local SQLite), marker file `%LOCALAPPDATA%\Mission Vishwakarma\SupportChat.txt` holds last-read id; no marker + nothing queued = zero network. Cadence: immediate on send, 60s hot (<1h activity or view visible), 900s cold, never when unused/blocked. WinHTTP + JsonEscape helpers duplicated locally (single-file constraint; hoist at 3rd user).
- Developer inbox: NOT django.contrib.admin (settings.py locks "no admin, no auth, no sessions") — two stats.html-style pages under /api/support/inbox, HTTP Basic vs MV_SUPPORT_TOKEN env, views must override the middleware CSP (`form-action 'self'` — global default is 'none' via setdefault).
- v1 chat text is Latin-only (MSDF atlas, no shaping — same limit as the Devanagari word-mark).

**Why:** support chat ships before login; anchoring on Installation gives the inbox free OS/GPU context from existing telemetry.
**How to apply:** implement Phase A (server, curl-testable) independent of tabs.md; Phases B/D need tab 0 to exist.
