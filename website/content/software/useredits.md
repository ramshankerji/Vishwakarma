---
title: "Website User Edits (Wiki-Style Proposals)"
weight: 100210
---

This page is the design document for **wiki-style editing of this website**. Every section of every
documentation page gets an inline [ edit ] link, exactly like MediaWiki. Clicking it turns that
section into an in-browser markdown editor. Submitting routes the proposal through our Django server
(`./server`, the same hardened Raspberry Pi that runs telemetry), which commits the change to a
proposal branch on GitHub and opens a pull request against `main` for maintainer approval.

The reader never needs a GitHub account, never forks, never learns git. The maintainer never
receives anything except an ordinary reviewable pull request.

---

## 1. Relationship to the Contribution Policy

The *Contribution & CLA Workflow* document defines two tiers: PRs touching only `website/` need
**no signed DCO**; everything else does. Every proposal produced by this system touches exactly one
file under `website/content/` — permanently inside the free tier. `vishwakarma-bot` (the GitHub App
defined there) will therefore never block these PRs, and the same App's credentials are what the
server uses to create them (§8.3). No new GitHub identity or secret is introduced.

## 2. The Simpler Alternative, Considered

A plain "Edit this page on GitHub" link per page (`github.com/.../edit/main/website/content/...`)
costs one line of template and zero server code. GitHub handles fork, branch, and PR itself. It is
rejected as the *primary* mechanism for one reason: it demands a GitHub account and the fork/PR
mental model, which excludes most of the audience this website is written for. It is however kept
as a complementary footer link on every page — contributors who *do* have GitHub accounts get the
full-file editor for free, and it is the fallback whenever the in-place system is down.

## 3. End-to-End Flow

1. Hugo (build time) publishes each page's **raw markdown** next to its HTML (§5.1) and stamps the
   page with its source path (§5.2).
2. A single static JavaScript file (the "bolt-on", §5.3) injects [ edit ] links after each rendered
   heading, plus one whole-page link.
3. Click → fetch the same-origin raw markdown → slice out the clicked section (§6) → replace the
   section's DOM with an editor (§7).
4. Submit → `POST https://mv-server.ramshanker.in/api/edits` with page path, section address, a
   hash of the unedited section, and the replacement text (§8).
5. The server validates everything, re-slices the file as it exists on `main` **right now**,
   verifies the base hash, splices in the replacement, and — through the GitHub REST API, no local
   clone — creates a proposal branch, one commit, and one pull request (§9).
6. The response carries the PR URL; the editor shows it as "Thank you — track your proposal here."
7. Maintainer reviews the PR like any other. Merge → the existing deployment republishes the page.
   Close → branch auto-deleted. Either way the system holds no state that can go stale.

## 4. Editor Library — Survey and Decision

The candidates, current state of the art for in-browser markdown editing:

| Library | Model | ~Gzipped | Distribution | Assessment |
|---|---|---|---|---|
| **CodeMirror 6** | Source editor, markdown syntax highlight | ~130 KB | ESM packages, needs one-time bundling | The modern standard (Chrome DevTools, Obsidian, Replit). Actively developed, MIT, excellent mobile/IME support. |
| Monaco | Source editor (VS Code core) | >2 MB | AMD/ESM | Desktop-grade power, poor on mobile, absurd weight for one section of prose. |
| ProseMirror | WYSIWYG toolkit + `prosemirror-markdown` | ~70 KB core | ESM, significant assembly | A toolkit, not an editor; weeks of UI work we don't want to own. |
| TipTap | WYSIWYG (ProseMirror wrapper) | ~100 KB+ | ESM, build tooling expected | Polished, MIT core, but markdown is an add-on and the ecosystem leans on frameworks + paid extensions. |
| Milkdown | WYSIWYG markdown (ProseMirror + remark) | ~120 KB+ | ESM plugins | Elegant, MIT, but see the WYSIWYG objection below. |
| Lexical (Meta) | WYSIWYG framework | ~50 KB core | ESM | React-leaning in practice; markdown via conversion; same objection. |
| Toast UI Editor | Dual markdown/WYSIWYG | ~180 KB | Single UMD file | Batteries included, but releases have stalled since 2023 — a maintenance risk. |
| EasyMDE | Source editor (CodeMirror **5**) | ~90 KB | Single file | Simple, but built on the legacy CM5 line. |
| Plain `<textarea>` | Source, no highlighting | 0 KB | — | What Wikipedia and GitHub shipped for a decade. |

**The WYSIWYG objection.** Every WYSIWYG candidate round-trips the section through parse →
document model → re-serialize. That rewrites formatting the user never touched — list markers,
emphasis style, hard-wrap positions — so the PR diff no longer isolates the human's change. In a
system whose entire output is a *reviewable diff*, serializer noise is disqualifying. Source-mode
editing preserves untouched bytes exactly.

**Decision: CodeMirror 6**, with a plain `<textarea>` as both the phase-1 editor and the permanent
fallback. CM6 is bundled **once** offline (rollup: `@codemirror/state`, `view`, `commands`,
`language`, `lang-markdown`, a minimal theme) into a single committed file
`website/static/js/mv-editor-cm6.js` — the PocketPy vendoring pattern: reviewed bytes in the repo,
no npm, no CDN, no build step at deploy time. The bundle is **lazy-loaded** on the first [ edit ]
click, so readers who never edit download nothing. If the module fails to load, the textarea is
used silently.

## 5. Hugo-Side Changes

### 5.1 Raw markdown output format

The editor must edit the page's true source, not scraped HTML. Hugo publishes it natively:

```toml
[mediaTypes."text/markdown"]
  suffixes = ["md"]
[outputFormats.RawMd]
  mediaType = "text/markdown"
  baseName = "index"
  isPlainText = true
[outputs]
  page = ["HTML", "RawMd"]
```

with layout `layouts/_default/single.rawmd.md` containing exactly `{{ .RawContent }}`. Every page
then serves its body markdown at `<permalink>/index.md`, same origin — no CORS, no GitHub fetch,
byte-identical to what the deployed HTML was built from. (`.RawContent` excludes front matter,
which is deliberately not editable.)

### 5.2 Edit metadata

`single.html` stamps `<main>` with two attributes: `data-edit-source="{{ .File.Path }}"` (e.g.
`content/software/graphics.md`) and `data-edit-raw="{{ .RelPermalink }}index.md"`. Pages opt out
with front matter `disableUserEdits: true` (omits the attributes); section list pages (`_index.md`)
and the home page are excluded in v1.

### 5.3 The bolt-on script

One static file `website/static/js/useredits.js`, referenced with `defer` from `baseof.html`,
hand-written vanilla JS like everything else on this site. On load, if `<main>` carries the
metadata, it appends a small `[ edit ]` anchor after every `h2`–`h4` inside the content div, and a
"✏ Propose an edit to this page" link at the bottom (several design documents — this site's older
chapters use plain numbered paragraphs, not markdown headings — get only the whole-page link).
Injected CSS is a few rules in the same file: links faint, visible on heading hover, hidden in
print.

## 6. Section Addressing and Slicing

Mapping a rendered heading back to source lines must be exact:

* The script scans the raw markdown for ATX headings (`^#{1,6} `), **tracking fenced code blocks**
  so a `# comment` inside a fence is not a heading. Goldmark preserves order, and fences cannot emit
  headings, so source headings and rendered `h1`–`h6` elements correspond **by index**. The clicked
  heading is simply the Nth rendered heading → the Nth source heading.
* Safety guard: before injecting any links, the script compares the plain text of every rendered
  heading against its source counterpart. Any mismatch (a shortcode that emits headings would cause
  one) disables section links for that page, leaving only the whole-page link.
* The slice runs from the heading line to the line before the next heading of the same or higher
  level (or EOF). The whole-page slice is the entire `.RawContent`.
* `baseHash` = SHA-256 (WebCrypto) over the exact slice bytes, LF-normalized. The section address
  sent to the server is `(headingIndex, level, headingText)` — or `whole-page` — plus `baseHash`.

## 7. Editor UI

The section's DOM nodes are replaced in place (restored on Cancel) by:

* The editor (CM6 or textarea) pre-filled with the slice, auto-growing, monospace.
* One-line **summary** (required, ≤ 120 chars — becomes the commit subject).
* Optional **name** and **email** — with the explicit warning that they become **public** in the
  commit (`Co-authored-by:`). Left blank, the proposal is anonymous and bot-authored only.
* A fixed notice: "Your proposal becomes a public pull request on GitHub and is licensed under this
  website's license." (Tier-1 of the contribution policy — no DCO involved.)
* Submit / Cancel. Success replaces the editor with the PR link; the page section is restored from
  the pristine DOM (the live site updates only after merge + redeploy).
* No live preview in v1. An approximate client-side preview (vendored `markdown-it`; Hugo's
  goldmark differs slightly) is a phase-4 option — the PR diff is the authoritative preview.

## 8. Server Side (`./server`)

### 8.1 Endpoint

A new Django app `edits` beside `api`, one route: **`POST /api/edits`**. CORS allowed only from
`https://mv.ramshanker.in` (django-cors-headers is already installed and configured this way for
`/api/login`). Request JSON:

```json
{
  "path": "content/software/graphics.md",
  "section": {"index": 4, "level": 2, "text": "Group table"},
  "baseHash": "hex sha-256 of the unedited slice",
  "newText": "replacement markdown for the slice",
  "summary": "Fix typo in group table",
  "name": "", "email": ""
}
```

`section: null` means whole-page. Response: `201 {"pr": "https://github.com/..."} `, or a typed
error (§10).

### 8.2 Validation (all server-side, client checks are cosmetic)

* Path: must match `^content/[a-z0-9/_.-]+\.md$` after normalization, no `..`, resolved under
  `website/`, and the file must exist on `main`. No file creation, no deletion, one file per
  proposal.
* Sizes: request body ≤ 256 KB (the telemetry app's 1 MB cap pattern, tightened), `newText` ≤ 64 KB,
  valid UTF-8, CRLF normalized to LF. Front matter is unreachable by construction (§5.1).
* Rate limits, reusing the `_rate_limited` pattern: 5 proposals per IP per day, and a global
  circuit-breaker — refuse with 503 when more than 25 bot-created proposal PRs are already open.
* A small SQLite table (same philosophy as the telemetry store) records id, UTC timestamp, IP hash,
  path, PR number — for the rate limits and for `/api/stats`.

### 8.3 GitHub integration — REST only, no clone

The server keeps **no working copy**. Authenticated as the `vishwakarma-bot` GitHub App
installation (permissions: `contents: write`, `pull_requests: write` on this one repository; the
App private key lives in the systemd env file, exactly like the existing
`deploy/mv-telemetry.env.example` secrets), a submission is five API calls:

1. `GET /git/ref/heads/main` → tip SHA.
2. `GET /contents/website/<path>?ref=main` → current file.
3. Server re-runs the §6 slicing on **that** content using the submitted section address. Heading
   text must match and `baseHash` must match → otherwise `409` (the page changed since the reader
   loaded it — the deployed site may also lag `main` briefly after a merge).
4. `POST /git/refs` → create the proposal branch from the tip SHA (§9); `PUT /contents/...` on that
   branch → one commit containing the spliced file.
5. `POST /pulls` → head = proposal branch, base = `main`, label `website-edit`.

Commit message: the summary line, a body line `Website edit proposal for <page url>#<anchor>`, and
`Co-authored-by: <name> <email>` only when supplied (explicitly unverified — review applies). PR
body: page URL, section heading, the summary, and a note that the author used the website editor.

## 9. Branch Strategy — a Correction to the Premise

The request specifies a single `website-edit-proposals` branch, always in sync with `main`. A
single shared branch has a structural problem: two pending proposals stack into one PR — merging
one merges both, rejecting one blocks the other, and "in sync with main" breaks the moment any
proposal is committed to it. Two workable readings:

* **(a) Literal single branch.** One rolling branch + one rolling PR; at most one proposal may be
  pending at a time; the server refuses submissions while it is open. Simple, but a slow review
  freezes the feature for everyone.
* **(b) Namespaced per-proposal branches — recommended.** Each submission creates
  `website-edit-proposals/<NNNN>` from the current `main` tip, so every proposal branch is in sync
  with `main` by construction, proposals are approved or rejected independently, and GitHub's
  "automatically delete head branches" setting cleans up after close. The name the request asked
  for survives as the shared prefix.

This document assumes (b); §12 lists it for confirmation.

## 10. Failure Modes

| Condition | Response | Editor shows |
|---|---|---|
| Section changed on `main` since page load | `409` | "This page was updated in the meantime — please reload and re-apply your edit." |
| Validation failure | `400` + reason | The reason, editor content preserved. |
| Rate limit | `429` | "Daily proposal limit reached." |
| Open-PR circuit breaker / GitHub API down | `503` | "Try again later" — content preserved; no server-side queue in v1, deliberately. |

Nothing is ever lost silently: until a `201` arrives, the text stays in the editor.

## 11. Phased Implementation

* **Phase 0 — Hugo output.** Raw-markdown output format + metadata attributes + opt-out flag.
  Verify: `curl <page>/index.md` is byte-identical to the source body; opted-out pages carry no
  metadata.
* **Phase 1 — Bolt-on, dry run.** `useredits.js` with the `<textarea>` editor; Submit logs the
  would-be request instead of posting. Verify: on `graphics.md`, injected link count equals source
  heading count; slice → hash → splice round-trips byte-exact; the heading-mismatch guard degrades
  a doctored page to whole-page-only.
* **Phase 2 — Server + PR.** `edits` app, validation, GitHub REST flow, per-proposal branches.
  Verify: a browser submission yields one branch + one PR whose diff touches only the edited lines;
  tampered `baseHash` → 409; sixth same-day submission → 429.
* **Phase 3 — CodeMirror 6.** Vendored single-file bundle, lazy-loaded, textarea fallback path
  tested by blocking the module. Verify: no request leaves our origin; mobile editing usable.
* **Phase 4 — Hardening.** Open-PR circuit breaker live, branch auto-delete confirmed, proposal
  counts surfaced in `/api/stats`, optional anti-bot challenge and optional preview (§12).

## 12. Open Questions

* Branch model: confirm per-proposal `website-edit-proposals/<NNNN>` (§9-b) over the literal single
  branch.
* Anti-bot: rate limits + the public-PR deterrent may suffice; Cloudflare Turnstile would help but
  imports an external script — against this site's no-third-party rule (analytics is currently the
  sole exception). Decide at phase 4 based on observed abuse, not speculation.
* Anonymous-by-default attribution: acceptable, or require an email?
* `_index.md` section pages: excluded in v1; revisit.
* Future tie-in: once *Login and User Accounts* ships, a signed-in reader's Ed25519 account could
  sign proposals, giving verified attribution and per-account (not per-IP) rate limits.
