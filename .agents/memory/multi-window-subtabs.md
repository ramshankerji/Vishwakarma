---
name: multi-window-subtabs
description: Tab/view extraction into dedicated windows + fixed 128-slot sub-tab registry in DATASETTAB — architecture and key invariants
metadata: 
  node_type: memory
  type: project
  originSessionId: 182b95d9-eb41-4e5d-8350-93084201e003
---

Implemented 2026-07-07 (multi-window tab/view extraction). Key architecture decisions:

- `MV_MAX_SUBTABS = 128` (ConstantsApplication.h). `DATASETTAB` owns `subTabs[128]` fixed slots +
  double-buffered `subTabIndexesA/B` published via atomics (mirrors allTabs pattern; user explicitly
  wants no std::vector here). Old `openInternalSubTabs` vector is gone.
- Delayed release: slot lifecycle SUBTAB_FREE → OPEN → PENDING_GPU_RELEASE → FREE. Close records
  `gpu.renderFenceValue`; `CleanupReleasedSubTabs` (engineering loop) frees slot once every
  monitor's render fence passed min(recorded, submitted).
- Tab hosting: `DATASETTAB::hostWindowSlot` (atomic int16, default 0 = main window). Tab band in
  RenderUIOverlay filters the global tab list by hostWindowSlot == its own window slot. Extraction /
  merge = one atomic store retargeting the slot.
- `SingleUIWindow::windowKind`: WINDOW_KIND_TABHOST (full ribbon) vs WINDOW_KIND_VIEW (content only,
  `dx.contentOnly=true` makes SceneTopUIHeightPx/TopUIHeightPx return 0). View windows carry
  viewParentTabIndex (mirrored into activeTabIndex so WndProc input routing works) + viewSubTabSlot.
- Extraction gestures: drag tab button > 2.5 band heights downward → kExtractTabUIAction; same for
  sub-tabs → InternalSubTabs::kExtractUIAction. Render thread only pushes UI actions; only the UI
  thread creates/destroys HWNDs (CreateSecondaryWindow / CloseSecondaryWindow in Main.cpp).
- Extracted-view rule (2026-07-08, generalized from Page2D to ALL types): a sub-tab lives in
  exactly 1 view. While extracted it never renders inline (render loop nulls id for the host
  window); band button stays (accent color), clicking it focuses the window; extraction pushes
  INTERNAL_SUB_TAB_EXTRACTED → HandleSubTabExtracted hands inline-active to next non-extracted slot.
- Per-view cameras (2026-07-08): CameraState lives in InternalSubTab (per slot); DATASETTAB::camera
  is only the no-sub-tab fallback. Input routing: WndProc RouteSceneInputToWindowView writes
  `DATASETTAB::inputViewSubTabSlot` atomic (view window → its slot; tab-host content-area click →
  -1; ribbon clicks leave it unchanged so ribbon tools act on last-worked view). Engineering
  resolves via InputViewSlot/InputViewContainerId/ActiveSceneCamera (विश्वकर्मा.cpp);
  Cad2DIsActivePage2D + Cad2DFindTargetPage2DMemoryId are keyed on input view too.
  GetVisibleSceneViewportForTab + IsOverRightOverlay resolve the input view's window. Render loop
  computes renderSlot per window, sets tabRes.camera from slot camera, and gates ServicePick +
  RecordSelectionOverlays to `renderSlot == InputViewSlot(tab)` (per-tab pick/overlay CBs would
  clobber otherwise). UpdateCameraOrbit made stateless (angle from atan2); auto-rotate loops all
  open Scene3D slots.
- UI overlay buffers are PER WINDOW (DX12ResourcesPerWindow::uiVertexBuffer/uiIndexBuffer/
  uiOrthoConstantBuffer, lazily created by EnsureWindowUIBuffers in RenderUIOverlay, freed in
  CleanupWindowResources). Root cause of "both tab-host windows show the extracted tab": one
  monitor command list records ALL its windows then executes once, so the old shared
  gpu.uiResources upload buffers made every window draw the last-recorded window's UI. The 3D
  scene constant buffer was ALREADY per-window (winRes.constantBuffer), which is why 3D per-view
  cameras worked once tabRes.camera was set per window.
- Per-view Page2D pan/zoom (2026-07-08): Cad2DViewState is now `views[MV_MAX_SUBTABS]` in
  TabCad2DStorage (was a single per-tab `view`), indexed by sub-tab slot. Input path resolves via
  Cad2DInputView(tab)=views[InputViewSlot]; RenderCad2DPage takes a viewSlot param (renderSlot);
  Record2DDraws takes containerMemoryId→slot; UI text caret uses FindPublishedSubTabSlot(active).
  The 2D view CONSTANT BUFFER also had to move per-window: DX12ResourcesPerWindow::
  cad2dViewConstantBuffer (lazy EnsureWindowCad2DViewBuffer), removed from DX12Resources2DPerTab —
  same shared-command-list clobber reason as UI buffers. Cad2DViewState::Reset() + camera.Initialize()
  called in OpenInternalSubTab's fresh-slot path so a recycled slot starts at default view.
- Merge: WM_EXITSIZEMOVE → TryMergeWindowOnDrop hit-tests cursor against other tab-host windows'
  FULL top-UI band (topUITotalHeightPx via GetTopRibbonHeightPxForWindow — was only the thin tab
  bar, nearly impossible to hit). View windows may drop ONLY on the window hosting their parent
  tab; on merge the view is re-activated inline (ACTIVATE_INTERNAL_SUB_TAB) + routing reset
  (ResetInputViewRouting), same on window-close return.
- Command routing across windows: `g_uiActionSourceTabIndex` (set on any click in RenderUIOverlay)
  consulted first by GetActiveTabForUIAction.
- CloseSecondaryWindow: unpublish → 50ms sleep (render thread mid-frame) → fence drain via
  `gpu.renderFenceValue.fetch_add(1)` Signal + wait → CleanupWindowResources → DestroyWindow.
  WM_DESTROY posts quit only for allWindows[0].

Page2D pan/zoom is now fully per-view (see above) — the earlier per-tab caveat is resolved.
Build: [[commandline-build]].
