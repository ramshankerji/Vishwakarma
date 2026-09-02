# Memory Index

## How Ram works
- [Working style](user-working-style.md) — fixed slot arrays over std::vector, minimal scope, mapping data in files he owns, deletions are his, clean submodules, warning-free builds
- [Dev machine monitors](dev-machine-monitors.md) — 4K DISPLAY2 sits ABOVE the primary, so negative window Y is normal; screenshot the VirtualScreen, not PrimaryScreen

## Build & release
- [Command-line build](commandline-build.md) — build Vishwakarma.vcxproj with BuildProjectReferences=false; ExternalIncludePath + per-file metadata keep it warning-free; Release blocked locally
- [Devanagari source editing](devanagari-source-editing.md) — scripted regex edits double-encode UTF-8 silently (perl needs -CSD or ASCII-only replacements); iconv does NOT catch it; ALSO the LF line-ending policy (.gitattributes since 2026-08-22) and its .bat/UTF-16-.rc exceptions
- [Release signing setup](release-signing-setup.md) — PFX not in git (CI secret), Ed25519 manifest key shares the PFX password, GenerateRelease.ps1 is the whole pipeline

## Graphics engine
- [Graphics memory lifetime](graphics-leak-audit.md) — the safeRetireFence invariant governing every deferred free (a silent monitor freezes it → OOM → abort), incl. the twice-seen crash after MINIMISING with a big 2D drawing; the reserved-tile instance arena + registry; leaks still open
- [Graphics file layout](graphics-refactor-phases.md) — post-reorg seam map (group table lives in website graphics.md); NOMINMAX gotcha; what was deliberately left undone
- [Per-monitor icon atlas](per-monitor-icon-atlas.md) — icons are per-monitor RGBA atlases at DPI-floored cell size (UI_MIN_LAYOUT_DPI 127); rebuild+drain rides RestartRenderThreads; MSDF text -size 48 -pxrange 6
- [Static descriptor invariant](descriptor-static-invariant.md) — SRV ranges with FLAG_NONE need every slot filled (reserved ones included); sky gradient is now a PSO, not 48 rect clears
- [Multi-window sub-tabs](multi-window-subtabs.md) — 128-slot subTabs[] + delayed GPU release; per-view cameras and Page2D pan/zoom via inputViewSubTabSlot; UI + 2D-view constant buffers must be per-WINDOW
- [Application Tab](application-tab-plan.md) — tabs.md: un-closable tab 0, no engineering thread, queues that must never be pushed to, wide-icon atlas runs, live Stats; ALL SHIPPED, opens with 2 tabs. Chrome-style frameless window also SHIPPED (4 new SingleUIWindow atomics, NOT the old RECT fields; WM_NCCALCSIZE/NCHITTEST, control SVGs ids 10-13)
- [Theme design](theme-design.md) — theme.md: System/Light/Dark + 2 bg overrides, palettes in colors.h, RTT baked-clear forces RestartRenderThreads ride, shader black↔white remap, luminance-keyed; DESIGNED ONLY
- [Hot-drag placement design](hot-drag-placement-design.md) — placement SHIPPED (design now in graphics.md); why composed-placement beat bake-back, the parked local-space refactor (point-shaped vs axis-shaped split), rotation untested
- [Step 7 per-Viewport draw](step7-per-viewport-draw.md) — one ExecuteIndirect per Viewport SHIPPED (default ON); per-command VBV/IBV as root constants made SceneEpoch + descriptor heap unnecessary; 56-byte command layout traps, scratch is per MONITOR not per Viewport
- [16-byte vertex](vertex-16-byte-format.md) — vertex is pos+normal only; colour is per-object packedColor, shaders split _16/_24 by stride, registry colour shadow stops drags going black, per-face colour knowingly dropped

## Modelling & UI
- [LINE_MEMBER object](line-member-object.md) — ObjectType 27 end-to-end: 9 hot families + parametric RECT/CIRC/OCT/HEX, schema v2, STD import carry-over; IsGeometry3DObjectType range + (std::max) gotchas
- [Steel section catalog](steel-section-catalog.md) — Catalog/profiles_*.csv (512 rows) embedded at compile time via steel_profile_embedder.py; dims are DRAFT, proof-check pending; ID rules in id.md
- [Properties pane](properties-pane-plan.md) — accessor-fn descriptor tables (not offsetof), single float↔text format, MODIFY commit path; point fields now show WORLD coords (do not revert to raw); GeometryForObject linkage + M_PI include traps; serves 2D+3D via void*/double accessors; 2D read-only
- [Ribbon command recipe](ribbon-command-recipe.md) — 8-file pipeline for a new ribbon button (Commands ID, CSV+compiler, AllUIControls, SVG+manifest, dispatch) + PowerShell screenshot/click verify recipe, and how to drive the 3D scene by script (focus, Auto Random, pick is async); optical 2D quantization measurement
- [.yyy round-trip harness](yyy-roundtrip-harness.md) — validations/yyy_roundtrip: **GREEN 5/5 through the whole META_DATA migration**; headless load/save via env vars + status file; Scene3D-race and asset-parent gotchas
- [.yyy file inspection](yyy-file-inspection.md) — project files are SQLite + per-type protobuf blobs; assert persistence directly, but decode keys as varints or field 20 lies to you
- [Object model unification](object-model-unification.md) — id.md is the spec (**renumbered 2026-08-26, §11 deleted**); steps 0-2 shipped and **step 3's IDENTITY half done — all nine 2D types on META_DATA**; arena residency next; the reusable migration technique; settled decisions
- [Snapping ambient grid](snapping-ambient-grid.md) — stage 1 SHIPPED 08-22, then stages 2-4/6-8 + Snap2D/Snap3D ribbon groups 08-31 (doc §19 is the authoritative status); the ribbon's first LATCHED controls; `validations/snap/` runs on Linux and caught a real defect
- [Page2D live preview](page2d-live-preview.md) — armed 2D tools draw what the next click would commit (amber); preview is BUILT by the click path and CONVERTED by the copy path so it cannot drift; transform preview capped at 100k records; 2D text is not click-selectable
- [Page2D transforms](page2d-transforms.md) — Copy/Offset/Mirror/Rotate/Move via Cad2DTransformKind + BEGIN_TRANSFORM2D; copy-thread Add* = upsert; arc CCW mirror-swap, polygon param-angle math
- [Asset2D system](asset2d-system.md) — hidden masters at container 0, parentObjectId linkage, FK row order + two-phase load gotchas; DXF blocks never explode, insert scale/rotation baked at instantiation
- [Undo/redo design](undo-redo-design.md) — DESIGNED ONLY, nothing built; no delete op exists anywhere; Encode*/Decode* trapped in an anon namespace; soft-delete means create-undo needs no payload

## Extensions, server, writing
- [Login design](login-design.md) — DESIGNED ONLY (login.md doc): account = immutable Ed25519 keypair + salt, disable-only credential slots, no merger ever, pepper-vs-salt hash split
- [Extension system](extension-system-mvp.md) — protobuf-over-pipes IPC, STD + DXF importers, PocketPy vendored; deploy path and setup-bundle path must BOTH be wired; AppContainer next; store↔login design in extensions.md §6 (PLANNED)
- [CPython static worker](cpython-static-worker.md) — VishwakarmaExtension.exe frozen CPython 3.13.14; BuildCPython.ps1 pipeline, freeze gotchas, nt-scrub via Py_mod_exec slot
- [DXF import diagnostics](dxf-import-diagnostics.md) — %TEMP% marker forensics, standalone worker drive script, auto-import env hooks, Page2D float32 coordinate limits
- [Telemetry system](telemetry-system.md) — AccountManager Ed25519 chain + ImprovementData SQLite + Django ./server, live on a Pi behind Cloudflare; canonical host mv-server.ramshanker.in
- [User edits design](useredits-design.md) — DESIGNED ONLY (useredits.md): wiki [edit] links, Hugo RawMd output, Django /api/edits → bot PRs; single-branch premise pushed back to per-proposal branches
- [Support chat design](support-chat-design.md) — DESIGNED ONLY (support.md): anonymous per-installation chat, /api/support/sync upload+poll endpoint, SupportRequests.h/.cpp thread, Basic-auth inbox (no django admin)
- [Technical paper 2026](technical-paper-2026.md) — EIL FY2026-27 entry delivered 2026-07-15 in build/; measured LOC/commit stats; why the 2024 entry lost
- [Security hardening roadmap](security-hardening-roadmap.md) — DESIGN ONLY (security.md): P0 signing keys in git → P1 worker sandbox → P2 parser fuzzing + Valgrind server (Linux portable-core only, Win = MSVC ASan)
