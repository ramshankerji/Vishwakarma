---
title: "Tab System and the Application Tab"
weight: 100113
---
This page documents **Tab 0 — the un-closable Application Tab** — as it works today, and the
Chrome-style tab strip it enables. To be referred by AI for coding as well as humans.

The application starts with two tabs. **Tab 0** has no file name — its tab button shows the logo
and the Devanagari application title **विश्वकर्मा**. It cannot be closed, cannot be extracted into
another window, and has **no engineering thread**: everything inside it is owned and serviced by
the main UI thread. It hosts 8 fixed, un-closable views (sub-tabs): Launcher, Profile, Settings,
Support, Peer Chat, Documentation, Common Geometry, and Stats. Its memory group (group 0) is
reserved as the future home of the immutable instancing geometry masters. **Tab 1** is the single
engineering tab the application opens with; every further tab comes from the `+` button.

The OS title bar is gone: the window is frameless, nearly identical to the Google Chrome tab
system — `[विश्वकर्मा][Tab][Tab][+] ................ [─][□][✕]`. See
[Chrome-style frameless window](#chrome-style-frameless-window) below.

## Design decisions

1. **Tab 0 reuses `DATASETTAB` slot `allTabs[0]`.** No parallel structure. Every reader (tab band,
   sub-tab band, compositor, data tree) works on the same arrays and published lists.
   `IsApplicationTab(tabID)` is simply `tabID == kApplicationTabId`.
2. **No engineering thread, UI thread is the single writer.** Tab 0's `DATASETTAB` fields are
   written only by the main UI thread (startup init + `ProcessPendingUIActions`). Render threads
   read exactly as they do for other tabs (acquire loads of published lists). The 8 view slots are
   filled and published **once at startup, before render threads launch**, and never change
   afterward — the published sub-tab list of tab 0 is immutable, so there is nothing to race on.
   Only `activeInternalSubTabMemoryId` changes at runtime.
3. **Tab 0's queues are never drained — so nothing may ever push to them.** `userInputQueue` and
   `todoCPUQueue` exist (the constructor makes them) but have no consumer. The guards in the
   inventory below stop every push site; otherwise memory grows unbounded. This is the single most
   important correctness rule of this feature.
4. **Views are identified by reserved synthetic container IDs.** Real container memory IDs come
   from `GetNewTempID()` which counts up from 1, so low constants would collide. The top range is
   reserved instead: `constexpr uint64_t kAppViewContainerIdBase = 0xFFFFFFFFFFFFFF00ULL`, and
   `AppViewKind` 1..8 maps to `kAppViewContainerIdBase + kind`. `InternalSubTab` is **not**
   modified; `containerType` stays `ObjectType::Unknown` and the helpers
   `IsAppViewContainerId(uint64_t)` / `AppViewKindForContainerId(uint64_t)` derive the kind. The
   storage schema is untouched — these IDs are never persisted.
5. **The file pair `ApplicationTab.h` / `ApplicationTab.cpp`** hosts everything: the `AppViewKind`
   enum, synthetic ID helpers, `InitializeApplicationTab()`, `ActivateApplicationTabView()`,
   `BuildApplicationTabOverlay()` and the per-view content. Bifurcate into per-view files only when
   a view outgrows a screenful of code.
6. **Engineering tabs start at slot 1; memory group 0 is reserved.** `tabNo` doubles as the CPU
   memory group number (`new (tab->tabNo)` placement allocation), so tab 0 keeping group 0 reserves
   that group for the future immutable instancing masters.
7. **The Devanagari title renders as an SVG word-mark icon, not shaped text.** विश्वकर्मा needs
   complex shaping (conjunct श्व, rakar र्मा) that the codepoint→glyph MSDF atlas cannot do, and the
   tab-label path filters to ASCII anyway. The word is committed once as vector paths — see the
   word-mark section below. Full Devanagari text shaping is explicitly out of scope.
8. **Un-closable, and un-movable by hand.** No `x` on the tab button, close requests refused at the
   `RequestCloseEngineeringTab` level (defence in depth), no drag-out extraction, no drag-merge into
   another window. Closing the last *engineering* tab is legal — the hosting window falls back to the
   Application Tab, Chrome-style. Tab 0 does move in exactly one involuntary case: when the window
   hosting it is closed while another tab-host window survives, it **migrates** there rather than
   dying (see "Closing a tab-host window" below), so `hostWindowSlot` is no longer pinned to 0 and a
   secondary window can end up showing tab 0.
9. **Views are un-closable and un-extractable.** The sub-tab band draws no `x` for tab 0's views
   and ignores drag-out. View activation for tab 0 is handled directly on the UI thread inside
   `ProcessPendingUIActions`, since there is no engineering thread to service `kActivateUIAction`.
   The machinery (`subTabHostWindowSlots`) would allow extraction later — e.g. Docs on a second
   monitor — since these views render purely from the UI overlay.
10. **The full ribbon stays drawn on tab 0.** Uniform layout; engineering commands no-op through
    the `GetActiveTabForUIAction` guard. A contextual ribbon for the Application Tab is a later
    decision, not a gap.

## The 8 application views

| # | `AppViewKind` | Band title | Panel heading | Content |
|---|---------------|------------|---------------|---------|
| 1 | `Launcher` | Launcher | Launcher | Stub. Eventually: recent local files + online servers; click opens a new engineering tab |
| 2 | `Profile` | Profile | Profile | Stub. Eventually: login details, badges, achievements (AccountManager identity) |
| 3 | `Settings` | Settings | Settings | Stub. Eventually: application-level settings |
| 4 | `Support` | Support | Support | Stub. Eventually: direct chat with support team (server: mv-server.ramshanker.in) |
| 5 | `PeerChat` | Peer Chat | Peer Chat | Stub. Eventually: IPMessenger-style LAN chat for the local office |
| 6 | `Documentation` | Docs | Documentation | Stub. Eventually: local copy of the online documentation (this website) |
| 7 | `CommonGeometry` | Geometry | Common Geometry | Stub. Eventually: catalogue of the frequently-used / instanced master geometry |
| 8 | `Stats` | Stats | Stats | **Live numbers** — see below |

One `AppViewDescriptor` table in `ApplicationTab.cpp` carries all three strings per view: the short
band title (the buttons are narrow), the fuller panel heading, and the placeholder line. Heading
and body are left-aligned at a one-row margin rather than centred; these panels grow into real
content, and left alignment is where that content will start.

Stats was worth building first because it is nearly free and it proves the whole UI-thread-owned
view pattern end to end. It reads five numbers, all lock-free from the render thread:
`publishedTabCount`, `publishedWindowCount`, the `राम::liveChunkCount` atomic (incremented in
`getNewChunkForTab`, decremented in `notifyTabClosed`, both already under the global mutex), that
count × 4 MB, and the sum of `geometry.activeSnapshot->pages.size()` over the published tabs — the
same RCU snapshot the render threads already walk, so the GPU side needed no new counter at all.

## Startup sequence (`Main.cpp` `wWinMain`)

1. The per-tab init loop runs for slots 0..1: slot 0 gets `InitializeApplicationTab(allTabs[0])`,
   slot 1 gets `tabID = tabNo = 1` and `fileName = L"Untitled 0"`. `gpu.InitD3DPerTab` and
   `InitCad2DTabResources` run for tab 0 as well — empty but valid GPU state keeps every downstream
   path (matrix tables, compositor, copy-thread retirement) untouched.
2. `InitializeApplicationTab` fills `tabID = tabNo = 0`, `fileName = L"विश्वकर्मा"` (used by
   window-title contexts; the band draws the word-mark icon instead), `dataTreeView.isVisible =
   false`, then opens the 8 view slots — slot `k-1` gets `containerType = Unknown`,
   `containerMemoryId = kAppViewContainerIdBase + k`, the ASCII band title, state `SUBTAB_OPEN` —
   and publishes the 8-entry list straight into `subTabIndexesA`. `activeInternalSubTabMemoryId`
   starts at Launcher. No lock needed: render threads do not exist yet. Nothing ever re-publishes
   tab 0's list, so the double buffer never swaps and buffer A stays published for the whole run.
3. Publish tab list `{0, 1}`, count 2. `nextTabSlot = 2`.
4. One engineering thread is launched, for slot 1. Tab 0 gets no thread.
5. `mainWindow.activeTabIndex = 1`: the application opens on the engineering tab, since the user's
   work is what they came for and tab 0 is one click away in the band.

## Guard inventory

Every place that knows about the Application Tab. Each is one small `IsApplicationTab(...)` check;
together they make "no thread + un-closable" airtight.

| Where | Guard |
|-------|-------|
| `WndProc` (`Main.cpp`, all `tab->userInputQueue->push` sites) | The routed tab pointer is dropped when it is tab 0, right after `GetActiveTabFromHwnd` — one line covering every push site below it (queue has no consumer, Decision 3). The per-window `uiInput` snapshot path is untouched, so all overlay UI (buttons, text fields) keeps working. `WM_CAPTURECHANGED` fans out over the published tab list and skips tab 0 separately. |
| `PushSystemTodoToTab()` | Early-return for tab 0 (same reason). Single choke point for all ribbon/system todos, including those raised from the compositor. |
| `FileInputThread()` (`Input_UI_Network_File.h`) | Its startup bulk load and the two auto-import dev hooks push straight into the `todoCPUQueue` of `tabList[0]`, bypassing `PushSystemTodoToTab` — and `tabList[0]` *is* tab 0. They resolve "first tab" through a local `FirstEngineeringTab()` instead. This one was found by the debug sentinel below, not by inspection. |
| `RequestCloseEngineeringTab()` | Refuses `tabID == kApplicationTabId`. This replaced the old `tabCount <= 1` guard: closing the last engineering tab is legal now, and the existing replacement logic lands the main window on tab 0 (always in the published list, hosted by window 0). |
| Tab band (`UserInterface.cpp`) | Tab 0: draw the logo + word-mark instead of a label; skip the `x` button and its hit test; never set `pressedTabId` (no drag-out). |
| `ExtractTabToNewWindow` | Refuses tab 0 (no *hand* drag-out). Tab 0 still migrates involuntarily when its host window is closed — see "Closing a tab-host window". Drag-merge needs no guard: it only moves tabs hosted by a secondary window. |
| Sub-tab band (`UserInterface.cpp`) | When the active tab is 0: skip the `x` draw and hit-test and the drag-out candidate (`pressedSubTabSlot` stays -1); the title takes the freed width. Activation click still emits `kActivateUIAction`. |
| `ExtractViewToNewWindow` | Refuses tab 0. Needed only once the views were published: before that the slot lookup failed on its own, afterwards it would succeed and build a real view window. |
| `ProcessPendingUIActions` | `kActivateUIAction` for tab 0 calls `ApplicationTab::ActivateApplicationTabView(p2)` on the UI thread. `kCloseUIAction` needs no branch — it routes through the guarded `PushSystemTodoToTab`. |
| `GetActiveTabForUIAction()` | Never returns tab 0, in any of its three resolution paths — `nullptr` when the action originated there. All engineering ribbon commands (save, create sphere, import…) then no-op on the Application Tab through their existing null checks. Commands that do not need a tab (e.g. `PROJECT_OPEN` → `OpenStorageFileInNewTab`) keep working. |
| Shutdown / `CleanupReleasedTabs` | No change needed — tab 0 never sets `closeRequested`, owns no engineering thread to join, and never enters the GPU-release handshake. |

`ApplicationTab::DebugVerifyQueuesEmpty()` is the standing regression net for Decision 3: a
Debug-only drain of both of tab 0's queues at the end of `ProcessPendingUIActions`, printing
`[apptab][bug]` with the action type of anything that slipped through. It caught the
`FileInputThread` leak on its first run. Keep it. To see its output, launch with stdout and stderr
redirected to files — `AllocateConsoleWindow` keeps the inherited handles when stdout is a pipe or
a disk file.

Not guards, but consequences worth knowing: tab 0 has no camera orbit, no random geometry
generation, no pick servicing (no pick requests are ever queued for it), and its data tree is
hidden. The compositor's Scene3D branch runs against empty geometry and draws only the sky gradient
behind the view panel — harmless, and it is fully covered by the opaque panel anyway.

## Rendering the application views

`BuildUIOverlay` (render thread, immediate-mode — same model as the ribbon and the properties
pane) calls `ApplicationTab::BuildApplicationTabOverlay(...)` when the window's active tab is tab 0.
It runs after the bands and before the right-side overlay, so the icon bar and panes still land on
top of it. It:

1. Draws one opaque full-content panel (from `topUITotalHeightPx` down) in the UI background
   colour — this is why the compositor needs no changes.
2. Switches on `AppViewKindForContainerId(...)` and draws the active view: heading + placeholder
   line, or live numbers for Stats.
3. Can take the same `UIInput` snapshot as the rest of the overlay when a view needs it, so future
   views (Launcher file list, Settings toggles) get clicks and text input for free, and commit
   their effects via `PushUIAction` handled on the UI thread — never via tab queues.

So the panel could live in `ApplicationTab.cpp` rather than inside `BuildUIOverlay`, the three
existing widget primitives `PushWidgetRect` / `PushWidgetText` / `WidgetTextBaselineY` moved out of
their anonymous namespace into `UserInterface.h`, alongside `extern UIColors uiActiveColors`. That
is the whole drawing API a view needs; no primitive was duplicated.

`ActivateApplicationTabView()` validates the incoming id against the published list rather than the
id range, and takes `storageObjectsMutex` — the same lock `ActivateInternalSubTab` takes on the
engineering thread — so the compositor's locked read of `activeInternalSubTabMemoryId` cannot race
the UI thread's write.

Threading recap: render thread draws and hit-tests; durable state changes travel through
`PushUIAction` → `ProcessPendingUIActions` (UI thread), which is already the pattern for every band
button.

## Devanagari tab title (word-mark details)

Tab 0's button is `[logo][विश्वकर्मा]`, centred as one group. The word-mark is
`icon_3950482947_VishwakaramText.svg`: a single `<path>` (text→path in Inkscape — no `<text>`
element, the embedded rasterizer handles paths only), viewBox `0 0 56.72 15.43` ≈ 3.68:1,
registered with one line in `SVGIconManifest.h`.

That aspect ratio forced a real change to the icon pipeline, and the reasoning is worth keeping:

- **The "square viewBox with a horizontal middle strip" idea does not work at the real cell size.**
  Cells are `iconSizePx` = `UI_ICON_SIZE_MM` (4 mm) at the floored layout DPI — **20 px** on an
  ordinary monitor. A square viewBox would give the whole word 20 px of width (~5 px tall), then
  upscale it ~3.7× to fill the band. Unreadable.
- **Wide icons get a horizontal run of atlas cells instead.** Two changes, both no-ops for the 201
  square icons: `RenderEmbeddedSVGIcons` rasterizes with `renderToBitmap(-1, pixelSize)` so width
  follows the SVG's own aspect — passing *both* dimensions makes lunasvg scale x and y
  independently, which squashes rather than letterboxes; and `BuildIconAtlas` reserves
  `ceil(width / cellSize)` adjacent cells via `TryReserveIconCellRun`, never splitting a run across
  rows. The glyph's UVs span the run, so `Glyph::width / Glyph::height` is the aspect callers size
  their quad by. The word-mark lands at 74 × 20 px — 1:1 with how it is drawn.
- The band draws it at `0.75 × iconSizePx` tall with the width derived from that glyph aspect, in
  `tabBackgroundText`, so it downsamples slightly rather than upscaling.

## Sub-tab close button

The `x` on *engineering* sub-tab buttons was invisible for a long time: the glyph is wider than the
fixed `closeSize * 0.55` clip width that was passed to `pushTextClipped`, and that function drops a
glyph whole rather than truncating it, so nothing was drawn even though the hit test worked. It is
now centred on its measured width (`MeasureUIStringWidth`) and drawn in `kUIDisabledTextGray` at
rest, white on the hover pill. Dull at rest is deliberate — a row of view buttons should not read
as clutter. **Size such clips from `MeasureUIStringWidth`, never from a guessed fraction.**

## Chrome-style frameless window

The tab-host window has no OS title bar. Its client area is extended over the caption and the tab
band carries the whole caption strip: `[विश्वकर्मा][tabs…][+] …… [─][□][✕]`. `WINDOW_KIND_VIEW`
windows (extracted sub-tabs) have no band and keep the normal OS frame — one `windowKind` branch
guards every case below. No window-style change was needed: the window keeps `WS_OVERLAPPEDWINDOW`
(so `WS_THICKFRAME` / `WS_CAPTION` still give snap, resize and the minimise animation); only the
*visual* frame is removed.

`ApplyFramelessFrame(HWND)` (`Main.cpp`) runs once per tab-host window, right after it is published
and before it is shown: it calls `DwmExtendFrameIntoClientArea` with a 1 px top margin (so the OS
keeps drawing the drop shadow) and forces one `WM_NCCALCSIZE` via `SetWindowPos(SWP_FRAMECHANGED)`.
The main window gets it before `ShowWindow(SW_MAXIMIZE)`, so no framed window ever flashes.

**Reclaiming the caption — `WM_NCCALCSIZE`.** For a frameless window it keeps the left/right/bottom
resize borders (insets from `GetSystemMetricsForDpi` at the window's current DPI). When maximised,
Windows oversizes the window by the frame thickness, so the top is pushed back down by that inset —
otherwise the top of the band is clipped off-screen (visible on the very first run, which opens
maximised). When restored, the top keeps a **1 px** non-client sliver (`rc.top += 1`) so the OS still
draws its thin border line at the very top — the crisp Chrome-style top edge — with the band starting
just below it.

**Hit-testing — `WM_NCHITTEST`.** The render thread publishes three px coordinates per window —
`frameTabBarBottomPx`, `frameControlsLeftPx`, `frameCaptionDragLeftPx`, atomics on `SingleUIWindow`,
the same publish-through-atomic pattern as `rightOverlayWidthPx`. Inside the tab-bar strip **`WndProc`
owns the hit test** — the min/max/close block at the right returns `HTMINBUTTON` / `HTMAXBUTTON` /
`HTCLOSE`, the empty strip between the `+` button and the controls returns `HTCAPTION`, the reclaimed
top edge is synthesised as `HTTOP` / `HTTOPLEFT` / `HTLEFT` (restored only, non-control area) so the
top can still be resized, and the rest (tabs, `+`) stays `HTCLIENT`. **Controls take priority over the
resize edges** — critical on a restored window, whose top-right corner is otherwise an OS resize
border: deferring to `DefWindowProc` first there turned the buttons into resize handles. Below the
strip, `WndProc` falls through to `DefWindowProc`, which keeps the side/bottom resize borders. The
button size follows the tab-bar height (`round(frameTabBarBottomPx * 1.5)`), the same formula the
render thread lays them out with, so the hit regions match the drawn buttons regardless of any
transient window-width lag. Returning `HTMAXBUTTON` still earns the Windows 11 snap-layout flyout on
hover from `DefWindowProc` for free.

**Stale-layout fallback.** A `WM_NCHITTEST` can arrive before the first frame publishes the geometry
(startup, monitor topology change); then `frameTabBarBottomPx` is 0 and the handler falls back to a
DPI-derived tab-bar height with the whole top strip draggable. Without this the window could be
undraggable and unclosable — the one bug here that makes the app unusable rather than merely ugly.

**The window controls.** `BuildUIOverlay` draws three buttons at the right of the tab bar and
publishes the coordinates above. They are white line-art SVGs (ids 10–13 in `SVGIconManifest.h`) so
the shader (`atlas.rgb * vertex colour`) tints them: grey at rest, white on hover, over a red pill on
the close button's hover. The middle button shows a maximise square when windowed and a restore glyph
when maximised (`SingleUIWindow::isMaximized`, set in `WM_SIZE`). Because the buttons are non-client,
their hover never arrives as `WM_MOUSEMOVE`; `WndProc` feeds the cursor position from `WM_NCMOUSEMOVE`
into the per-window `uiInput` (and parks it off-screen on `WM_NCMOUSELEAVE`) so the render thread
lights the hovered button. The **clicks are handled in `WM_NCLBUTTONDOWN`** — `HTMINBUTTON` →
`SW_MINIMIZE`, `HTMAXBUTTON` → maximise/restore, `HTCLOSE` → `WM_CLOSE` — rather than left to
`DefWindowProc`, which on a frameless window runs close and maximise but silently drops minimise.
Doing it ourselves is deterministic for all three and does not disturb the hover-driven snap flyout.

**Per-monitor DPI.** All insets come from `GetSystemMetricsForDpi` at the window's current DPI and
are recomputed on every `WM_NCCALCSIZE`, so a window dragged between a 1080p and a 4K panel rescales
itself. `WM_DPICHANGED` now also applies the OS-suggested window rect from `lParam` before restarting
the render threads (it previously ignored it).

Dragging by `HTCAPTION` still fires `WM_ENTERSIZEMOVE` / `WM_EXITSIZEMOVE`, so `TryMergeWindowOnDrop`
and tab drag-out are untouched. The published rects live in the new atomics rather than the
long-unused `tabBandRect` / `viewBandRect` / `contentRect` fields, which stay dead for now.

The extra window churn this feature adds (a `SWP_FRAMECHANGED` per window, caption-reclaim resizes,
windows dragged across monitors) surfaced a latent compositor bug: `InitD3DPerWindow` created each
window's render-texture in `RENDER_TARGET`, but every frame's opening barrier and the resize path
expect `PIXEL_SHADER_RESOURCE`. The main window's startup maximise-resize hid it (that recreates the
RTT as `PIXEL_SHADER_RESOURCE` before render threads run); a freshly extracted tab, or the
monitor-mismatch recreation partway through the frame loop, rendered straight after creation and
tripped `RESOURCE_BARRIER_BEFORE_AFTER_MISMATCH`. The RTT is now created in `PIXEL_SHADER_RESOURCE`,
matching the barrier and the resize path.

## Closing a tab-host window

Closing a tab-host window (its `[✕]`, or the OS close) **closes every tab hosted in it** — the tabs
are destroyed, not migrated back to the main window as they were before. `WndProc`'s `WM_CLOSE`
branch drives it:

- **Extracted `WINDOW_KIND_VIEW` windows** (a sub-tab pulled into its own ribbon-less window): closing
  the window **closes the sub-tab it hosts** — `HandleSecondaryWindowClose` pushes
  `CLOSE_INTERNAL_SUB_TAB` for that container (Scene3D / Page2D), so it no longer reappears inline in
  the parent tab. Dragging the view window back onto the parent's band (`TryMergeWindowOnDrop`) still
  returns it inline — only the close button destroys the sub-tab.
- **A tab-host window** first looks for another surviving tab-host window. If one exists the
  application lives on: every engineering tab hosted here is closed through
  `CloseAllHostedEngineeringTabs` (same `CLOSE_TAB` teardown a single `[x]` uses, minus the per-tab
  window bookkeeping), and the window is torn down with `TeardownWindowSlot`. If this window hosts the
  Application Tab, tab 0 is **migrated** to the survivor first (its `hostWindowSlot` retargeted) so it
  is never destroyed; the survivor is brought to the foreground.
- **If there is no other tab-host window**, this is the last one — it necessarily hosts tab 0 — so the
  **whole application shuts down** (pause + join render threads, clean up, `PostQuitMessage`).

Two lifecycle assumptions had to go, because the main window (slot 0) can now be the one closed:
`TeardownWindowSlot` generalises the old `CloseSecondaryWindow` to any slot (nulling `hWnd` before
`DestroyWindow`, so the `WM_DESTROY` quit-guard on `allWindows[0].hWnd` does not fire and the app
keeps running); and the quit is posted **explicitly** from the last-window branch, since the last
tab-host window is no longer guaranteed to be slot 0. `CreateEngineeringTab`'s host fallback now
resolves to whichever window currently hosts tab 0 rather than hard-coding slot 0.

## Files

| File | Role |
|------|------|
| `code-core/ApplicationTab.h` / `.cpp` | Everything from Decision 5: `kApplicationTabId`, `IsApplicationTab()`, `AppViewKind`, synthetic ID helpers, `InitializeApplicationTab()`, `ActivateApplicationTabView()`, `BuildApplicationTabOverlay()`, `DebugVerifyQueuesEmpty()`. |
| `code-core/Main.cpp` | Startup sequence, engineering thread launch, guards in `WndProc`, `PushSystemTodoToTab`, `RequestCloseEngineeringTab`, `GetActiveTabForUIAction`, `ProcessPendingUIActions`. |
| `code-core/UserInterface.cpp` / `.h` | Tab-band tab-0 branch (logo + word-mark, no `x`, no drag); sub-tab-band tab-0 branch; wide-icon atlas runs; `BuildApplicationTabOverlay` call; the three exported widget primitives. |
| `code-core/Input_UI_Network_File.h` | `FirstEngineeringTab()` for the bulk load and the auto-import dev hooks. |
| `code-core/SVGIconRenderer.cpp` | Aspect-preserving rasterization. |
| `code-core/SVGIconManifest.h` + `website/static/SVGIcons/` | Word-mark icon entry + SVG file. |
| `code-core/MemoryManagerCPU.h` | `राम::liveChunkCount` for the Stats view. |
| `code-core/RenderCompositor-DirectX12.cpp` | Tab-0 refusals in `ExtractTabToNewWindow` / `ExtractViewToNewWindow`. |

## Future work

### Instancing / Common Geometry masters
Tab 0's memory group 0 hosts the frequently-used geometry elements at all instancing levels.
Created **once at startup on the main thread before the copy/render threads consume them**, then
immutable — the same create-before-publish reasoning that makes the view slots race-free, applied
to geometry pages. The CommonGeometry view becomes their visual catalogue. Engineering tabs will
reference these masters by ID for instanced draws; the graphics-pipeline side (instance buffers,
per-instance transforms) is its own future design document section in `graphics.md`.

### Recent-files persistence for the Launcher
TODO. File format and location — likely next to the AccountManager identity data — to be decided
when the Launcher gets real content.
