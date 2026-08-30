---
name: application-tab-plan
description: "Tab 0 \"Application Tab\" (website/content/software/tabs.md) — un-closable, no engineering thread, 8 views, stub panels + live Stats; PHASES 1-3 ALL SHIPPED 2026-07-23"
metadata:
  node_type: memory
  type: project
  originSessionId: 65c569f8-e44c-448f-b931-e1c0b616b49b
  modified: 2026-07-23T14:22:55.776Z
---

Design doc `website/content/software/tabs.md` (weight 100113) — rewritten from plan to
description-of-current-behaviour; the phase/DONE structure and the "open questions" section are gone.
**All three phases shipped 2026-07-23** in `code-core/ApplicationTab.{h,cpp}` (`namespace
ApplicationTab`): tab 0 at slot 0, no thread for tab 0, 8 views published at startup, UI-thread
activation, per-view stub panels and a live Stats view. Remaining work is *future* only — instancing
masters in memory group 0, Chrome-style frameless window (full survey now in the doc's Future Work).

**Startup finalized 2026-07-23 (Ram's calls):** the app now opens with **exactly two tabs** — tab 0
+ one engineering tab named `L"Untitled 0"` at slot 1. `nextTabSlot = 2`, published count 2, one
engineering thread launched (`for` loop gone, single `std::thread(विश्वकर्मा, 1)`),
`mainWindow.activeTabIndex = 1`. The old 3-tab `initialTabNames[]` array is deleted; further tabs
come from `+` only. Ribbon stays full on tab 0; data tree hidden on tab 0; view 7 = "Common
Geometry"; tab-0 views stay non-extractable. Verified live: Stats shows "Open tabs: 2", `+` still
creates engineering tabs, closing the last engineering tab leaves the app healthy on tab 0 alone.

Phase 3 shape: `BuildApplicationTabOverlay` draws one opaque panel over the whole content area
(which is why the compositor needs no changes) then the active view, from a single
`AppViewDescriptor` table holding band title + panel heading + placeholder line. To put it in
ApplicationTab.cpp instead of BuildUIOverlay, `PushWidgetRect` / `PushWidgetText` /
`WidgetTextBaselineY` moved out of their anon namespace into UserInterface.h, plus
`extern UIColors uiActiveColors`. Stats reads `publishedTabCount`, `publishedWindowCount`, the new
`राम::liveChunkCount` atomic (inc in `getNewChunkForTab`, dec in `notifyTabClosed`), ×4 MB, and the
sum of `geometry.activeSnapshot->pages.size()` — the RCU snapshot render threads already walk, so
the GPU side needed no new counter.

**The queue rule dominates the design:** tab 0 has no engineering thread, so its
`userInputQueue`/`todoCPUQueue` are never drained — anything pushed grows memory forever. The
`WndProc` guard is one line (`if (tab && IsApplicationTab(tab->tabID)) tab = nullptr;` right after
`GetActiveTabFromHwnd`), covering all ~12 push sites while leaving the per-window `uiInput` snapshot
path — hence all overlay UI — working on tab 0.

**How to apply:** any new code resolving "the first tab" from `publishedTabIndexes[0]` now gets the
Application Tab and must skip it. `FileInputThread()` in `Input_UI_Network_File.h` did exactly that
in three places, pushing straight into `todoCPUQueue` past `PushSystemTodoToTab`; fixed with a local
`FirstEngineeringTab()`. Found only by `ApplicationTab::DebugVerifyQueuesEmpty()` — a Debug-only
drain of both queues at the end of `ProcessPendingUIActions` printing `[apptab][bug] <actionType>`.
Keep it; it is the regression net. To see its output, launch with stdout+stderr redirected to files
(`AllocateConsoleWindow` keeps inherited handles when stdout is a pipe/disk).

Phase 2 specifics: slots are filled in startup order (slot k-1 = kind k) and published directly
into `subTabIndexesA` — nothing re-publishes tab 0's list, so the double buffer never swaps.
`ActivateApplicationTabView()` validates against the published list and takes `storageObjectsMutex`
(same lock `ActivateInternalSubTab` uses) so the compositor's locked read cannot race it.
`ExtractViewToNewWindow` needed a tab-0 guard that Phase 1 did not: once the slots are published,
the lookup succeeds and would build a real view window.

**Wide-icon atlas support** (built for the word-mark, useful for any future wide icon): icon atlas
cells are `iconSizePx` = 4 mm at floored DPI = **20 px**, so the doc's original "square viewBox with
a horizontal middle strip" idea is unreadable — rejected. Instead `RenderEmbeddedSVGIcons` now
rasterizes with `renderToBitmap(-1, pixelSize)` (passing BOTH dimensions makes lunasvg scale x and y
independently — it squashes, never letterboxes), and `BuildIconAtlas` reserves
`ceil(width/cellSize)` adjacent cells through `TryReserveIconCellRun`, never splitting a run across
rows. Glyph UVs span the run, so `Glyph::width/height` is the aspect to size the quad by. No-op for
the 201 square icons. Ram authored `icon_3950482947_VishwakaramText.svg` (text→path, 3.68:1); tab 0
draws `[logo][word-mark]` centred, word-mark at 0.75×iconSizePx tall. See
[[ribbon-command-recipe]] for the icon pipeline, [[multi-window-subtabs]], [[commandline-build]].

`pushTextClipped` silently drops a glyph whose ink exceeds the maxWidth it is given — that is why
the engineering sub-tab `x` was invisible for months (fixed width `closeSize * 0.55`). Size such
clips from `MeasureUIStringWidth`, never a guessed fraction. Ram's stated preference: small
affordances like a close `x` stay **dull** (`kUIDisabledTextGray`) at rest and only brighten on
hover, to keep the UI feeling uncluttered.

**Chrome-style frameless window SHIPPED 2026-07-23** (now a present-tense section in tabs.md, out of
Future Work). `ApplyFramelessFrame(HWND)` in Main.cpp (DwmExtendFrameIntoClientArea 1px top +
SWP_FRAMECHANGED) runs per tab-host window before show; WM_NCCALCSIZE reclaims the caption (keeps
side/bottom resize borders, adds the frame inset back to `rc.top` only when `IsZoomed`); WM_NCHITTEST
returns HTMINBUTTON/HTMAXBUTTON/HTCLOSE/HTCAPTION/HTCLIENT and synthesises HTTOP for the top few px
when restored. Scope stayed **per window** (WINDOW_KIND_VIEW keeps the OS frame) as locked. **Deviation
from the locked decision:** I did NOT reuse `tabBandRect`/`viewBandRect`/`contentRect` — those stay
dead. Published instead through four NEW scalar atomics on SingleUIWindow (`frameTabBarBottomPx`,
`frameControlsLeftPx`, `frameCaptionDragLeftPx`, `isMaximized`), lock-free like `rightOverlayWidthPx`,
because atomic<RECT> isn't lock-free and scalar atomics matched the existing precedent. The 3 window
controls are white line-art SVGs ids 10-13 (SVGIconManifest.h), tinted grey→white, red pill on
close-hover; middle button swaps maximise↔restore on `isMaximized` (set in WM_SIZE). Buttons are
non-client, so hover is bridged via WM_NCMOUSEMOVE→uiInput (WM_NCMOUSELEAVE parks it off-screen).
WM_DPICHANGED now also applies the OS-suggested rect from lParam (was ignored). The `,,,,`-typo twin
WM_NCCALCSIZE stubs were removed. Build warning-free.

**Round-2 fixes after Ram's manual testing (2026-07-23):** (1) Controls dead on *restored/extracted*
windows — WM_NCHITTEST deferred to DefWindowProc first, so the top-right corner returned HTTOPRIGHT
(resize) over the buttons; only the maximised main window (no resize borders) worked. Fix: the band
strip now hit-tests **controls-first**, defers to DefWindowProc only *below* the strip. (2) Minimise
still dead even after (1): DefWindowProc runs close/maximise from HTMINBUTTON/HTMAXBUTTON/HTCLOSE but
silently drops minimise on a frameless window — added an explicit **WM_NCLBUTTONDOWN** handler
(HTMINBUTTON→SW_MINIMIZE, HTMAXBUTTON→maximise/restore, HTCLOSE→WM_CLOSE). (3) Button hit width now
`round(frameTabBarBottomPx*1.5)` (same formula the render draws with) instead of
`(GetClientRect.right - controlsLeft)/3`, so hits match drawn buttons even if window.dx.WindowWidth
lags GetClientRect. (4) Icons were ~30% of bar height → iconExtent 0.40→0.60 and the 4 SVGs redrawn
to fill their viewBox. (5) Chrome-style 1px top line: WM_NCCALCSIZE now does `rc.top += 1` when
restored (OS draws its border there). All verified live via SendMessage(WM_NCHITTEST) probing +
mouse_event: min/max/restore/close all work on a restored window. **Gotcha for future live testing:**
after minimising the render window, Process.MainWindowHandle switches to the "Vishwakarma Debug
Console" window — enumerate top-level windows by PID and exclude that title to get the render HWND.
This machine's primary runs at a DPI where the tab bar is ~27px (not the 20px floor), so control
buttons are ~40px wide — compute click coords, don't guess.

**Window-close semantics rewritten (2026-07-23, Ram's spec):** closing a tab-host window now
**closes all its tabs** (destroys them) instead of migrating them back to the main window. WM_CLOSE:
VIEW windows → HandleSecondaryWindowClose now **closes the hosted sub-tab** (pushes
CLOSE_INTERNAL_SUB_TAB for its Scene3D/Page2D container) instead of returning it inline — Ram's
follow-up; verified live (extract Scene3D → close its ribbon-less window → Scene3D gone from the band,
not reappearing). Its dead tab-host else-branch (retarget to slot 0) was removed. Drag-merge back
(TryMergeWindowOnDrop) still returns a view inline — only the close button destroys the sub-tab.
tab-host windows → if another tab-host
window survives, close all engineering tabs here via new `CloseAllHostedEngineeringTabs(slot)` (bulk
CLOSE_TAB, no per-tab window bookkeeping) + `TeardownWindowSlot(slot)`, and if this window hosts tab 0
**migrate tab 0** to the survivor (retarget hostWindowSlot, focus survivor); if NO other tab-host
window, it's the last one (hosts tab 0) → full app shutdown. **Broke two old invariants:** (a) slot 0
is no longer permanent — `CloseSecondaryWindow` refactored to `TeardownWindowSlot` (works for any
slot, nulls hWnd before DestroyWindow so the WM_DESTROY `==allWindows[0].hWnd` guard doesn't fire);
CloseSecondaryWindow is now a thin slot!=0 wrapper. (b) quit is `PostQuitMessage(0)` **explicit** in
the last-window branch (last window may not be slot 0 after migration); WM_DESTROY guard kept as
belt-and-suspenders. Also `CreateEngineeringTab` host fallback now resolves to tab 0's current host,
not hardcoded slot 0. All 3 scenarios verified live via SendMessage(WM_CLOSE) + drag-extract: A close
secondary→tab destroyed+app alive; B close main→tab0 migrates to survivor (word-mark appears in its
band)+app alive; C close last→app quits (~3-4s cleanup, poll don't assume). Decision 8 in tabs.md
updated (tab0 hostWindowSlot no longer pinned to 0).

**Bonus D3D12 fix (RESOURCE_BARRIER_BEFORE_AFTER_MISMATCH):** the frameless window churn exposed it.
`InitD3DPerWindow` (RenderCompositor-DirectX12.cpp ~line 584) created renderTextures in
RENDER_TARGET, but the per-frame opening barrier and the resize path (~line 743) expect
PIXEL_SHADER_RESOURCE. Startup maximise-resize hid it for the main window; extracted tabs and the
monitor-mismatch recreation (lines 180-188, which InitD3DPerWindow then hits the barrier in the same
loop iteration) tripped it. Fixed by creating the RTT in PIXEL_SHADER_RESOURCE. See [[graphics-leak-audit]].

**Verification gotchas on this machine:** screenshot the whole virtual desktop, not the primary
monitor — see [[dev-machine-monitors]]. `SetForegroundWindow` from a background PowerShell needs a
synthetic ALT tap first — which also pops the window's *system menu*, so send ESC before clicking or
the first click is swallowed. Focus does not survive between tool calls: put the focus grab and the
whole click sequence in ONE call and re-check `GetForegroundWindow` before each click. Tab-band X
coords: app-tab button ~80; with only tab 0 present `+` is ~185; with an engineering tab too, the
engineering tab is ~230 and `+` shifts right to ~335.
