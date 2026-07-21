---
title: "Tab System and the Application Tab"
weight: 100113
---
This page is the Design Document for **Tab 0 — the un-closable Application Tab** — and the
longer-term Chrome-style tab strip it enables. To be referred by AI for coding as well as humans.
It is a plan; sections marked as decisions are locked, defaults are changeable until first
implementation.

The feature: today the application starts 3 engineering tabs, each with its own engineering
thread. We add a 4th tab at **tab index 0** that has no file name — its tab button shows only the
Devanagari application title **विश्वकर्मा**. It cannot be closed, cannot be extracted into another
window, and has **no engineering thread**: everything inside it is owned and serviced by the main
UI thread. It hosts 8 fixed, un-closable views (sub-tabs): Launcher, Profile, Settings, Support,
Peer Chat, Documentation, Common Geometry, and Stats. Its memory group (group 0) is reserved as
the future home of the immutable instancing geometry masters. Once tab 0 exists, a later phase
removes the OS title bar so the window becomes nearly identical to the Google Chrome tab system:
`[विश्वकर्मा][Tab][Tab][+] ................ [─][□][✕]`.

## Current state (anchor points in code)

- `Main.cpp` `wWinMain`: creates tabs 0..2 from `initialTabNames[]` (~line 744), publishes a
  3-entry tab list, then launches 3 engineering threads `विश्वकर्मा(i)` for `i = 0..2` (~line 826).
  `nextTabSlot` starts at 3.
- `CreateEngineeringTab()` (`Main.cpp` ~line 199): '+' button path; takes the next slot, publishes,
  spawns the engineering thread.
- `RequestCloseEngineeringTab()` (`Main.cpp` ~line 249): refuses when `tabCount <= 1`; picks a
  replacement tab, retargets windows, queues `ACTION_TYPE::CLOSE_TAB` to the tab's own thread.
- Tab band rendering: `UserInterface.cpp` `BuildUIOverlay` (~line 1067 onward). Every tab button
  gets an `x` close button; label text is ASCII-filtered (`if (ch <= 0x7F)`, ~line 1183), so a
  Devanagari `fileName` would currently render blank. Drag-down extracts a tab
  (`kExtractTabUIAction`) unless it is the window's last hosted tab.
- Sub-tab (view) machinery: fixed 128-slot `subTabs[]` per `DATASETTAB` with a double-buffered
  published index list (`विश्वकर्मा.h` ~line 157). Slots are engineering-thread owned, written
  under `storageObjectsMutex`, read lock-free by render threads. Open/activate/close live in
  `विश्वकर्मा.cpp` (`OpenInternalSubTab` ~line 394).
- Content per window: `RenderCompositor-DirectX12.cpp` (~line 250) branches Page2D vs Scene3D on
  the active sub-tab's container type, then draws the UI overlay on top.
- Input: `WndProc` (`Main.cpp`) pushes scene/keyboard input into the active tab's
  `userInputQueue`; ribbon commands resolve their target via `GetActiveTabForUIAction()`.
- The comment at `विश्वकर्मा.h` ~line 245 ("Tab 0 id default application launch screen tab. It
  can't be closed.") already records this intent — this document implements it.

## Locked design decisions

1. **Tab 0 reuses `DATASETTAB` slot `allTabs[0]`.** No parallel structure. Every existing reader
   (tab band, sub-tab band, compositor, data tree) keeps working on the same arrays and published
   lists. `IsApplicationTab(tabIndex)` is simply `tabIndex == kApplicationTabId` with
   `constexpr uint16_t kApplicationTabId = 0;`.
2. **No engineering thread, UI thread is the single writer.** Tab 0's `DATASETTAB` fields are
   written only by the main UI thread (startup init + `ProcessPendingUIActions`). Render threads
   read exactly as they do for other tabs (acquire loads of published lists). The 8 view slots
   are filled and published **once at startup, before render threads launch**, and never change
   afterward — the published sub-tab list of tab 0 is immutable, so there is nothing to race on.
   Only `activeInternalSubTabMemoryId` changes at runtime (UI thread writes on view activation,
   same 1-frame-staleness read contract as everywhere else).
3. **Tab 0's queues are never drained — so nothing may ever push to them.** `userInputQueue` and
   `todoCPUQueue` exist (the constructor makes them) but have no consumer. Guards (see Guard
   Inventory) stop `WndProc` and `PushSystemTodoToTab()` from pushing; otherwise memory grows
   unbounded. This is the single most important correctness rule of this feature.
4. **Views are identified by reserved synthetic container IDs.** Real container memory IDs come
   from `GetNewTempID()` which counts up from 1, so low constants would collide. Reserve the top
   range: `constexpr uint64_t kAppViewContainerIdBase = 0xFFFFFFFFFFFFFF00ULL;` and
   `AppViewKind` 1..8 maps to `kAppViewContainerIdBase + kind`. `InternalSubTab` is **not**
   modified; `containerType` stays `ObjectType::Unknown` and helpers
   `IsAppViewContainerId(uint64_t)` / `AppViewKindForContainerId(uint64_t)` derive the kind.
   Storage schema is untouched — these IDs are never persisted.
5. **Single new file pair `ApplicationTab.h` / `ApplicationTab.cpp`** hosts everything: the
   `AppViewKind` enum, synthetic ID helpers, `InitializeApplicationTab()`,
   `BuildApplicationTabOverlay()` (per-view stub panels), and later the per-view real content.
   Bifurcate into per-view files only when a view outgrows a screenful of code.
6. **Engineering tabs shift to slots 1..3; memory group 0 is reserved.** `tabNo` doubles as the
   CPU memory group number (`new (tab->tabNo)` placement allocation), so tab 0 keeping group 0
   reserves that group for the future immutable instancing masters. `nextTabSlot` starts at 4.
7. **The Devanagari title renders as an SVG word-mark icon, not shaped text.** विश्वकर्मा needs
   complex shaping (conjunct श्व, rakar र्मा) that the codepoint→glyph MSDF atlas cannot do, and
   the tab-label path filters to ASCII anyway. Author the word once as vector paths (text→path in
   Inkscape from `Fonts/NotoSansDevanagari`), commit as a new icon in `website/static/SVGIcons`
   plus one `SVGIconManifest.h` line, and draw it with `PushIcon` in tab 0's button — the exact
   mechanism the launch splash already uses for the logo (`kSplashLogoCodepoint`,
   `UserInterface.cpp` ~line 921). Full Devanagari text shaping is explicitly out of scope here.
8. **Un-closable and un-movable, everywhere.** No `x` on the tab button, close requests refused
   at the `RequestCloseEngineeringTab` level (defense in depth), no drag-out extraction, no
   drag-merge into another window. `hostWindowSlot` stays 0 forever; secondary tab-host windows
   never show tab 0. Closing the last *engineering* tab is now allowed — the main window falls
   back to the Application Tab, Chrome-style.
9. **Views are un-closable and (for now) un-extractable.** The sub-tab band draws no `x` for tab
   0's views and ignores drag-out. View activation for tab 0 is handled directly on the UI
   thread inside `ProcessPendingUIActions` (write `activeInternalSubTabMemoryId`), since there is
   no engineering thread to service `kActivateUIAction`.

## The 8 application views

| # | `AppViewKind` | Band title | MVP content (Phase 3) | Eventual content |
|---|---------------|------------|----------------------|------------------|
| 1 | `Launcher` | Launcher | Stub panel | Recent local files + online servers; click opens a new engineering tab |
| 2 | `Profile` | Profile | Stub panel | Login details, badges, achievements (AccountManager identity) |
| 3 | `Settings` | Settings | Stub panel | Application-level settings |
| 4 | `Support` | Support | Stub panel | Direct chat with support team (server: mv-server.ramshanker.in) |
| 5 | `PeerChat` | Peer Chat | Stub panel | IPMessenger-style LAN chat for the local office |
| 6 | `Documentation` | Docs | Stub panel | Local copy of the online documentation (this website) |
| 7 | `CommonGeometry` | Geometry | Stub panel | Display of the frequently-used / instanced master geometry elements |
| 8 | `Stats` | Stats | **Real data from day 1** | Tab count, window count, CPU memory pages, GPU memory pages, and whatever else is cheap to read |

Stats ships real numbers in the MVP because it is nearly free (`publishedTabCount`,
`publishedWindowCount` are atomics; CPU/GPU page counts need only small read-only counters
exposed from `MemoryManagerCPU` / `MemoryManagerGPU-DirectX12`) and it proves the whole
UI-thread-owned view pattern end to end.

> **Naming note:** the request said "Comment Geometry elements". This document interprets it as
> **Common Geometry** — the display of the immutable instancing masters described below. If the
> intent was annotation/markup elements (clouds, leaders, comments), rename the view; nothing
> else in the design changes.

## Startup sequence changes (`Main.cpp` `wWinMain`)

1. Keep the existing per-tab init loop but run it for slots 0..3: slot 0 gets
   `InitializeApplicationTab(allTabs[0])` (from the new file), slots 1..3 get the existing
   `initialTabNames[]` treatment (names unchanged). `gpu.InitD3DPerTab` and
   `InitCad2DTabResources` still run for tab 0 — empty but valid GPU state keeps every
   downstream path (matrix tables, compositor) untouched.
2. `InitializeApplicationTab` fills: `tabID = tabNo = 0`, `fileName = L"विश्वकर्मा"` (used by
   window-title contexts; the band draws the word-mark icon instead), `dataTreeView.isVisible =
   false`, then opens the 8 view slots — slot `k-1` gets `containerType = Unknown`,
   `containerMemoryId = kAppViewContainerIdBase + k`, ASCII `title` from the table above, state
   `SUBTAB_OPEN` — and publishes the 8-entry list. `activeInternalSubTabMemoryId` = Launcher.
   No lock needed: render threads do not exist yet.
3. Publish tab list `{0, 1, 2, 3}`, count 4. `nextTabSlot = 4`.
4. Engineering thread launch loop becomes `for (int i = 1; i <= 3; ++i)`. Tab 0 gets no thread.
5. `mainWindow.activeTabIndex`: **default 1 during development** (engineering demo content stays
   front on every run); switch the default to 0 (Launcher) once the Launcher has real content.

## Guard inventory

Every place that must learn about the Application Tab. Each is one small `IsApplicationTab(...)`
check; together they make "no thread + un-closable" airtight.

| Where | Guard |
|-------|-------|
| `WndProc` (`Main.cpp`, all `tab->userInputQueue->push` sites) | Skip the push when the routed tab is tab 0 (queue has no consumer — Decision 3). The per-window `uiInput` snapshot path is untouched, so all overlay UI (buttons, text fields) keeps working. |
| `PushSystemTodoToTab()` (`Main.cpp` ~line 89) | Early-return for tab 0 (same reason). Single choke point for all ribbon/system todos. |
| `RequestCloseEngineeringTab()` | Replace the `tabCount <= 1` guard with `tabID == kApplicationTabId` refusal. Closing the last engineering tab is now legal; the existing replacement logic already lands the main window on tab 0 (it is always in the published list, hosted by window 0). |
| Tab band (`UserInterface.cpp` ~line 1126) | Tab 0: draw word-mark icon instead of label; skip the `x` button and its hit test; never set `pressedTabId` (no drag-out). |
| `ExtractTabToNewWindow` / drag-merge paths | Refuse tab 0; `hostWindowSlot` stays 0. |
| Sub-tab band (`UserInterface.cpp` ~line 1400) | When the active tab is 0: skip the `x` button/hit-test and drag-out (`pressedSubTabSlot` stays -1). Activation click still emits `kActivateUIAction`. |
| `ProcessPendingUIActions` (`Main.cpp` ~line 375) | `kActivateUIAction` for tab 0: set `allTabs[0].activeInternalSubTabMemoryId = p2` directly on the UI thread. `kCloseUIAction` / `kExtractUIAction` / `kCloseViewWindowUIAction` for tab 0: ignore. |
| `GetActiveTabForUIAction()` (`Main.cpp` ~line 123) | Never return tab 0 — return `nullptr` when the action originated there. All engineering ribbon commands (save, create sphere, import…) then no-op on the Application Tab through their existing null checks. Commands that do not need a tab (e.g. `PROJECT_OPEN` → `OpenStorageFileInNewTab`) keep working. |
| Shutdown / `CleanupReleasedTabs` | No change needed — tab 0 never sets `closeRequested`, owns no engineering thread to join, and never enters the GPU-release handshake. Verify only. |

Not guards, but consequences worth knowing: tab 0 has no camera orbit, no random geometry
generation, no pick servicing (no pick requests are ever queued for it), and its data tree is
hidden. The compositor's Scene3D branch runs against empty geometry and draws only the sky
gradient behind the view panel — harmless, zero compositor changes in the MVP.

## Rendering the application views

`BuildUIOverlay` (UI overlay, render thread, immediate-mode — same model as the ribbon and the
properties pane): when `window.activeTabIndex == kApplicationTabId`, call
`ApplicationTab::BuildApplicationTabOverlay(...)` after the bands are drawn. It:

1. Draws one opaque full-content panel (from `topUITotalHeightPx` down) in the UI background
   color — this is why the compositor needs no changes.
2. Switches on `AppViewKindForContainerId(allTabs[0].activeInternalSubTabMemoryId)` and draws the
   active view: MVP = centered view name + one line of placeholder text; Stats = live numbers.
3. Receives the same `UIInput` snapshot as the rest of the overlay, so future views (Launcher
   file list, Settings toggles) get clicks and text input for free, and commit their effects via
   `PushUIAction` handled on the UI thread — never via tab queues.

Threading recap: render thread draws and hit-tests; durable state changes travel through
`PushUIAction` → `ProcessPendingUIActions` (UI thread), which is already the pattern for every
band button today.

## Devanagari tab title (word-mark details)

- New SVG: the word विश्वकर्मा converted to paths (no `<text>` element — the embedded rasterizer
  handles paths only), committed under `website/static/SVGIcons` with a manifest line in
  `SVGIconManifest.h`, ID per the existing hash-style convention.
- The icon atlas rasterizes square cells (`RenderEmbeddedSVGIcons(int pixelSize)`), so author the
  word inside a **square viewBox occupying a horizontal middle strip**. In the tab band, draw the
  icon as a square quad larger than the band height, centered vertically on the band: the strip
  lands inside the band, the overflow above/below is fully transparent pixels, and the ribbon —
  drawn after the band — paints over the lower overflow anyway. No renderer changes.
- Fallback that MVP may ship with: the existing logo icon (`IconForID(1u)`), swapped for the
  word-mark by changing one constant. Do not block Phase 1 on SVG authoring.
- If the strip trick looks bad at small DPI, the alternative is teaching the atlas one
  non-square cell (`RenderedSVGIcon` already carries independent width/height) — deferred.

## Phases

### Phase 1 — Tab 0 exists, un-closable, engineering tabs renumbered
Startup changes, all guards from the inventory, word-mark (or logo fallback) tab button.
No views yet (empty published sub-tab list is fine — band renders nothing).
**Verify:** app starts with 4 tabs; tab 0 shows the icon and no `x`; clicking tab 0 shows the
empty sky-gradient content; engineering tabs 1..3 behave exactly as before (create, close,
extract, multi-window); `+` creates slot 4+; closing all engineering tabs leaves the app on tab
0 and the app stays healthy; opening/saving files from tab 0 either works (open) or no-ops
(save); a debug counter proves tab 0's queues stay empty across a full session.

### Phase 2 — The 8 un-closable views
`InitializeApplicationTab` publishes the 8 slots; sub-tab band guards; UI-thread activation.
**Verify:** all 8 buttons visible with no `x`; clicking each switches
`activeInternalSubTabMemoryId`; drag-out does nothing; engineering tabs' own sub-tabs
(Scene3D/Page2D) still open/close/extract normally.

### Phase 3 — Stub panels + real Stats view
`BuildApplicationTabOverlay` with the opaque panel, per-view stubs, Stats with live counters.
**Verify:** each view renders its name; Stats numbers change when tabs/windows open and close and
when geometry allocates pages in engineering tabs.

## Future phases (designed direction, not scheduled)

### Instancing / Common Geometry masters
Tab 0's memory group 0 hosts the frequently-used geometry elements at all instancing levels.
Created **once at startup on the main thread before the copy/render threads consume them**, then
immutable — the same create-before-publish reasoning that makes the view slots race-free, applied
to geometry pages. The CommonGeometry view becomes their visual catalog. Engineering tabs will
reference these masters by ID for instanced draws; the graphics-pipeline side (instance buffers,
per-instance transforms) is its own future design document section in `graphics.md`.

### Chrome-style frameless window
Remove the OS title bar; final band layout `[विश्वकर्मा][tabs...][+] ... [─][□][✕]`.
Known ingredients: `WM_NCCALCSIZE` client-area extension (a commented prototype already sits in
`WndProc`, `Main.cpp` ~line 1060), `WM_NCHITTEST` returning `HTCAPTION` for empty band area (drag
to move), `HTMINBUTTON`/`HTMAXBUTTON`/`HTCLOSE` for the three drawn window controls (and Win11
snap-layout flyout via `HTMAXBUTTON`), maximized-state inset correction, and drawing the three
controls at the band's right edge in `BuildUIOverlay`. This phase only starts after tab 0 has
shipped, because the band then carries the application identity that the title bar used to.

## File touch list (Phases 1–3)

| File | Change |
|------|--------|
| `code-core/ApplicationTab.h` / `.cpp` | **New.** Everything from Decision 5. |
| `code-core/Main.cpp` | Startup sequence, thread launch loop 1..3, guards in `WndProc`, `PushSystemTodoToTab`, `RequestCloseEngineeringTab`, `GetActiveTabForUIAction`, `ProcessPendingUIActions`. |
| `code-core/UserInterface.cpp` | Tab-band tab-0 branch (icon, no `x`, no drag); sub-tab-band tab-0 branch; `BuildApplicationTabOverlay` call. |
| `code-core/विश्वकर्मा.h` | `kApplicationTabId`, `IsApplicationTab()` (or these live in `ApplicationTab.h` and are included where needed). |
| `code-core/SVGIconManifest.h` + `website/static/SVGIcons/` | Word-mark icon entry + SVG file. |
| `code-core/MemoryManagerCPU.h` / `MemoryManagerGPU-DirectX12.h` | Tiny read-only page counters for the Stats view (only if not already readable). |
| Project file (`Vishwakarma.vcxproj`) | Add the new .h/.cpp pair. |

## Open questions and changeable defaults

- **Startup active tab**: default engineering tab 1 during development; flips to tab 0 when the
  Launcher is real.
- **Ribbon on tab 0**: MVP keeps the full ribbon drawn (uniform layout; commands no-op via the
  `GetActiveTabForUIAction` guard). A contextual ribbon (or none) for the Application Tab is a
  later decision.
- **Data tree on tab 0**: hidden by default (`dataTreeView.isVisible = false`).
- **View 7 name**: "Common Geometry" per the naming note above — confirm or rename.
- **Extracting tab-0 views into their own windows** (e.g. Docs on a second monitor): refused in
  the MVP; the machinery (`subTabHostWindowSlots`) would allow it later since these views render
  purely from the UI overlay.
- **Recent-files persistence for the Launcher**: file format and location (likely next to the
  AccountManager identity data) — decide when the Launcher gets real content.
