---
name: ribbon-command-recipe
description: "End-to-end recipe for adding a new top-ribbon button/command (8 files, 2 generators) + PowerShell screenshot/click verify recipe; done for ZOOM_MAX/ZOOM_FOCUS, SOFTWARE_UPDATE_CHECK"
metadata: 
  node_type: memory
  type: project
  originSessionId: b3dc00f1-4f26-424a-984e-39a4256db7e7
  modified: 2026-08-24T14:55:10.313Z
---

Adding a new ribbon button (done 2026-07-06 for ZOOM_MAX=2914702845 / ZOOM_FOCUS=3690481522, "View" subgroup at end of "Common" group):

1. `code-core/ListOfCommands.h` — new `Commands` enum entry, random unique 10-digit ID (1000000000..4294967295).
2. `code-miscellaneous/UserInterfaceTranslation.csv` — row `<cmdID>,<SHORT_NAME>,<English>,<comment>`; then run `python code-miscellaneous/UserInterfaceTranslationCompiler.py` to regenerate `UserInterfaceTranslationCompiled.{h,cpp}` (NOT a pre-build step; commit generated files).
3. `code-core/UserInterface.h` `AllUIControls[]` — struct order: action, nameStringID, UIIconForCommand(cmd), type(1=button), noOfVerticalSlots, verticalSlotNo, zIndex, showText, isEnabled, actionGroupIndex, actionSubGroupIndex. Layout stacks a column until the next control's verticalSlotNo==0. Subgroup labels render per contiguous *run*, so reusing a subgroup index (e.g. View=1) in two groups is fine.
4. Icon: hand-author `website/static/SVGIcons/icon_<cmdID>_<slug>.svg` (viewBox 0 0 64 64, stroke #333 width 4 style) + entry in `code-core/SVGIconManifest.h` (numerically sorted). Pre-build `svg_icon_embedder.py` embeds automatically.
5. Dispatch: `Main.cpp ProcessPendingUIActions()` maps Commands → `PushSystemTodoToTab(GetActiveTabForUIAction(), ACTION_TYPE::…)`; add ACTION_TYPE in `UserInputProcessing.h` (30001+ range); handle in the todo loop in `विश्वकर्मा.cpp` (~line 1470).

6. **Flip `isEnabled` to `true` in that `AllUIControls[]` row.** Since 2026-07-19 the flag means
   "has a handler in `ProcessPendingUIActions()`", not "user switched it off": every row whose
   command the dispatcher never matches was set `false` (~160 of ~200), so it renders dull gray
   (`kUIDisabledTextGray` in `colors.h`) and ignores hover/click. A newly wired command stays gray
   until you flip it — the struct comment in UserInterface.h now says so. The old `0xAA333333`
   gray-out overlay was deleted: Ram wanted text+icon dulled only, leaving the
   `StableRandomUIColour` placeholder squares alone.

Modal cursor modes (done for ZOOM_WINDOW=3213983172): atomic mode flag on DATASETTAB (render thread reads it to trail the command icon next to the cursor in UserInterface-DirectX12.cpp ~line 2112, per-mode blocks gated on activeInternalSubTabType); engineering thread handles input via a Handle*Input() called at the TOP of the userInputQueue loop (order = priority, first consumer wins); ESC cancels; mutual exclusion = Begin cancels other modes + self-heals by checking their flags each event.

Visual verify recipe (used for SOFTWARE_UPDATE_CHECK=3271869050 + its "Restart to Update" toast, 2026-07-17): launch `build\Debug\Vishwakarma.exe` via Start-Process, Sleep 14 s (DX12 init), click with user32 `SetCursorPos`+`mouse_event` (Add-Type MUST be in the same PowerShell call — shell state does not persist between tool calls), screenshot via System.Drawing `CopyFromScreen`, Read the PNG. Clicking a ribbon nav-band label (y≈68 @1080p) scrolls the ribbon to that group. Overlay-only states can be temp-forced (e.g. init `g_softwareUpdateStagedForRestart{true}` in SoftwareUpdate.cpp), screenshotted, then reverted. UI vertex colour packs (b<<16)|(g<<8)|r per StableRandomUIColour — symmetric greys like 0xFF333333 are channel-order-proof. Non-command UI strings live in the same CSV with small WordIDs (10xx block, e.g. RestartToUpdate=1057).

**Driving the 3D scene by script (learned the hard way, 2026-08-01 — these cost ~15 failed attempts):**
- `SendKeys` goes to the FOREGROUND window. After `Start-Process` the app is NOT foreground, so debug
  keys silently vanish. `ShowWindow`+`BringWindowToTop`+`SetForegroundWindow` alone was not enough
  either — a real click inside the app window is what reliably gives it keyboard focus.
- Turn OFF **Auto Random** (ribbon) and **camera auto-rotation** (`r` toggles) before any before/after
  comparison, or objects keep appearing and the camera orbits, and every screenshot diff is noise.
  Verify staticness by diffing two screenshots 3-4 s apart rather than assuming.
- **The data tree does NOT set the 3D selection.** `ApplyPickResult` (विश्वकर्मा.cpp) is the only
  writer of `selection.selectedObjectIds`, driven by the GPU pick from a *viewport* click — and the
  pick is an async fence-gated readback, so the result lands a frame or two later (wait ~3-4 s).
- Reading the debug heartbeat: the app owns its console via `AllocConsole`+`freopen("CONOUT$")`, so
  `-RedirectStandardOutput` captures nothing. Minimise the main window, `MoveWindow` the
  "Vishwakarma Debug Console" window, screenshot it. (Attaching via `AttachConsole`+`CONOUT$` +
  `GetConsoleScreenBufferInfo` failed here.)
- **Verifying a render-TRANSFORM change (2026-08-20).** The Properties Pane is NOT sufficient evidence:
  it composes placement over authored coords *itself*, so it reports a correct "+2 in Z" while the
  object renders in the wrong place at the wrong size. Two techniques that do work:
  (a) **Isolate the selected object by its highlight colour** — `ShaderSceneHighlightPixel.hlsl` uses
  `deepBlue (0.05, 0.15, 0.65)` scaled by `lerp(0.55, 1.20, t)`, so match on the RATIO (R 6..17,
  G ≈ 3R, B ≈ 13R) not on absolute values. Gives an exact centroid + bbox for the selection; a loose
  blue filter catches other scene objects and is worthless.
  (b) **Projective collinearity test** — move the object twice along one world axis; the three screen
  centroids MUST be collinear under any pinhole camera. Normalised `|cross| / (len1·len2)` was 0.0006
  when correct and ~0.7 when the matrix was wrong. Equal step lengths + constant screen size on top of
  that rules out a scale reset. This is what caught two defects a pane-reading called clean.
  Gotchas: opening the Properties Pane RE-PROJECTS the viewport (narrows it), so never diff across
  that event — take the baseline after it is open; and confirm the camera never moved by diffing a
  background band (0 differing samples) before trusting any delta.
- **Modal-state traps that cost ~20 attempts on 2026-08-21.** (i) **`Esc` closes the ACTIVE TAB** when
  the 3D view has focus — it killed two working tabs mid-measurement; to leave a create-cursor mode
  instead, click the same ribbon button again. (ii) A create tool **stays armed** after placing, so
  the next viewport click PLACES another object rather than selecting one — every "the pick returned
  the wrong type" symptom traced to this. (iii) **Auto Random and camera orbit are PER TAB**: a newly
  opened tab starts with both ON even though you just turned them off next door. (iv) `Zoom Focus`
  frames the SELECTION when there is one and the whole scene when there is not. (v) `Hide Unselected`
  lives in ribbon tab *3D Basic* → Helpers.
- **The collinearity test needs an UNOCCLUDED object, and the tell is the highlight pixel count.**
  Counts swinging 918→2546 across frames mean something moved in front of it and the centroid is
  biased — that read `|cross|/(l1·l2)` = 0.076 on a matrix that was actually correct. The cheapest
  way to get an isolated object is a **fresh app launch** (~20 objects, 16 blobs): quiet it, label
  connected non-sky components, and pick one with the most clear sky in the direction of travel.
  Clean result on a placed cuboid moved +2 Z twice: max 0.087 px off the best-fit line.
- Quantify visual results with a sampled pixel diff + bounding box instead of eyeballing: "444 changed
  samples in a 52x56 box" proves one object moved; a screenshot pair does not.

**The console-screenshot recipe MINIMISES the main window, so it silently breaks the next SendKeys
(2026-08-24).** Reading the console leaves the app minimised and the console foreground at the
screen origin - so the focus click that every SendKeys run starts with lands on the *console*, and
four debug keypresses went into it with no error anywhere. Always restore + SetForegroundWindow the
main window between a console capture and the next key. The tell is a `[cad2d][stress]` /
`[gpu][stress]` line that never appears while the script cheerfully reports "sent 'B' 4/4".

**Reading the app's Debug console (2026-08-23).** Debug builds `AllocConsole()` and reopen stdout to
CONOUT$ (Main.cpp AllocateConsoleWindow), so `Start-Process -RedirectStandardOutput` captures
NOTHING - the `[gpu][copy]`, `[cad2d][dbg]`, `[3d][warn]` diagnostics only exist in that window.
Two approaches failed: `FindWindowW($null, "Vishwakarma Debug Console")` returns 0 (the null class
marshals badly from PowerShell), and AttachConsole(pid) + CreateFileW("CONOUT$") fails with err=2
from the harness's PowerShell host even though AttachConsole itself succeeds. What works: EnumWindows
filtered by the app's pid, pick the window whose class is `ConsoleWindowClass`, **minimise the main
3D window first** (SW_MINIMIZE = 6 - otherwise CopyFromScreen grabs the app window sitting on top of
the console's rect), then screenshot the console by its rect. Only the visible tail is readable that
way, which is enough to confirm a diagnostic is absent while the app churns.

**Driving the 2D canvas by script (2026-09-02, verifying the live tool preview).** Three traps, each
of which looked like a broken feature:
- **`SetForegroundWindow` alone does not raise the app** when another window sits on its rect — the
  screenshot cheerfully captures whatever is on top. What works is `ShowWindow(SW_RESTORE)` then
  `AttachThreadInput(myThread, foregroundThread, true)` + `BringWindowToTop` + `SetForegroundWindow`.
  `SW_RESTORE` un-maximises, so follow with `ShowWindow(SW_MAXIMIZE)` if you want the big canvas.
- **A PowerShell helper named `Move` silently becomes `Move-Item`** (built-in alias) and "moves" the
  cursor coordinates into a file path. Name mouse helpers `MoveTo` / `WMove`.
- **Clicks are SNAPPED, selection clicks are not.** A point clicked to create geometry lands on the
  ambient grid, so the drawn line sits several pixels off the cursor, and the later selection click
  at the same coordinates misses the 6 px `tolCU` in `Cad2DHandleSelectionClick`. Screenshot first,
  find the stroke's actual pixel, click THAT. Confirm selection by sampling for the selection blue
  `13,38,166` (RGB of ABGR `0xFFA6260D`) rather than by eye — at 1 px it reads as black.
- **Sample pixels, do not eyeball thin strokes.** A 4-edge polygon preview looked like "2 amber
  edges + 2 grey ones" in a downscaled screenshot and cost a real bug hunt; every edge sampled
  exactly `255,128,26`. The scaling in the image viewer, not the renderer, was the liar.

Gotchas: engineering thread is sole writer of `storageObjects3D` → iterate without lock from that thread. 2D active check = `Cad2DIsActivePage2D(tab)`; 2D view state = `tab.cad2d->view` (centerXCU/centerYCU/zoomPixelsPerCU, clamp zoom 0.02..5000). 3D camera = `tab.camera` (position/target/up, fov 60°, wheel-zoom clamps distance 1.0..farZ-10). Viewport via `GetVisibleSceneViewportForTab`. See [[commandline-build]] for the msbuild invocation.

**Verifying generated MESH data without the app (2026-08-21).** Screen-reading cannot tell a winding
bug from a culling bug. Extract the builder function's text VERBATIM from its header with a small
Python slicer (brace-match from `inline void <Name>`), `#include` that into a ~60-line standalone
`.cpp` with a local `Vertex`/`PackNormal`, and assert the invariants numerically — max index < vertex
count, no degenerate triangles, and `dot(normalize(cross(v1-v0,v2-v0)), storedNormal)` on every
triangle. Verbatim extraction is the point: a hand-retyped copy drifts from what the app compiles.
Compiling it is where the time went, and none of it was where it looked:
**the Bash tool eats backslash escapes in heredocs and printf** (backslash-1, backslash-b,
backslash-v), so a Windows path written as "Visual Studio" + backslash + "18" silently arrived as
Visual Studio<0x01>8, and "...arma" + backslash + "build" as ...arma<0x08>uild - which reads exactly
like a broken toolchain. It hit both a printf and a quoted <<'PY' Python heredoc. **Write .bat and
.ps1 files with the Write tool**, never through the shell. Then: **vcvars64.bat cannot find
vswhere.exe on this machine**, so it sets the MSVC include but NOT the Windows SDK one - set INCLUDE
/ LIB / PATH explicitly instead (MSVC VC/Tools/MSVC/<ver> plus SDK "C:/Program Files (x86)/Windows
Kits/10" Include+Lib for 10.0.26100.0 ucrt, um, shared), and put cd /d "%~dp0" at the top so the
.bat can be invoked by absolute path from PowerShell. Add /I build/Debug/Intermediate for the
generated headers (SteelProfileCatalogEmbedded.generated.h).
A working harness is worth the setup: it proved the instanced RC member's cube corners coincide
bit-exactly with the bespoke extruder's vertices, which no screenshot could.

**Measuring 2D quantization optically, when no readback exists (2026-08-22).** To prove the ambient
grid was live there was nothing to read: the properties pane is 3D-only (see
[[properties-pane-plan]]) and saving needs a file dialog. What worked: screenshot the window, drive
N clicks whose x advances **1 px each time** (each pair draws one vertical line), screenshot again,
diff the two images along one horizontal row, then collapse adjacent changed pixels into stroke
centres. Snapping shows up as N clicks producing far fewer strokes at uniform spacing — 40 click
pixels -> 5 strokes at 10/11/9/11 px. The before/after diff is what makes it work in a scene already
full of demo content: it isolates exactly the strokes you just drew, so no empty canvas is needed.
Two things that corrupt the reading: **clicks delivered faster than ~200 ms apart get dropped**,
which halves the stroke count and fakes a doubled step (a slow run gave clean uniform spacing where a
fast one gave 4,8,4,7,34); and **reusing a y-band you already drew in**. Also note ribbon create
tools TOGGLE, so a stray toggle puts the click sequence out of phase by one and every later pair
straddles two lines — re-toggle off/on to reset before measuring, never assume the phase.
