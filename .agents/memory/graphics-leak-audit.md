---
name: graphics-leak-audit
description: "Graphics engine GPU/CPU memory lifetime — the retire-pruner invariant that governs all deferred frees (and the twice-seen minimised-window crash that looks like it breaking), the three reserved-tile per-tab buffers (arena/redirect/visibility mask) and the root-signature binding contract they share, plus the leaks still open"
metadata:
  node_type: memory
  type: project
  originSessionId: df468650-2faf-4a4f-9c21-59d782cc1c7f
  modified: 2026-08-01T14:10:27.171Z
---

Audit done 2026-07-18 after Ram's crash (Debug, 2+ monitors, abort() a few minutes after STD/DXF
import). Root cause and the batch-1/2/3 fixes are committed; what follows is the durable part.

**The invariant everything else hangs off.** GpuCopyThread's prune loop
(MemoryManagerGPU-DirectX12.cpp) computes `safeRetireFence = min(GetCompletedValue)` over monitors
with `renderFenceValue >= 2`. Any deferred free — retired pages/snapshots, retired matrix-table
buffers, tab GPU teardown — is gated on it. So **a monitor that has ever rendered must keep
signalling its fence forever**, or the min freezes and every retire queue grows without bound until
OOM throws inside the copy thread (a `std::thread`, so the throw is uncaught → terminate/abort).
That was the crash. The fix is the idle branch in RenderCompositor-DirectX12.cpp signalling
renderFence every 16 ms tick; the `[gpu][warn] retire-backlog >128` sentinel is the early warning.
Cad2D makes it worse under DXF import because PublishCad2DPages retires *all* pages per batch —
retention goes quadratic while the pruner is stuck.

**This invariant is very likely still broken FOR A MINIMISED WINDOW — open, 2026-08-25.**
Vishwakarma.exe has now died twice with the SAME signature: exception `0x0000087d` raised from
KERNELBASE.dll (a RaiseException, not an access violation), Report Ids
`6a2e9748-ffbb-485b-835e-3fdff0b7d613` (2026-08-24) and
`db626d65-2513-4643-8248-e8316ceaa18c` (2026-08-25, build 0.0.0.279). What the two share is not the
edit that preceded them: both times the main window had been MINIMISED and left that way for roughly
a minute with a large 2D drawing loaded. It is NOT a Page2D-paging regression — the first sighting
predates step 2e by a day — and it is not every minimise, since the same session minimised a dozen
times without incident. The shape matches this section exactly: minimised monitor stops presenting →
min-monitor-fence freezes → retire queues grow → OOM inside the copy thread → uncaught throw. The
number to read is `retireBacklog(live/peak)` in the `[gpu][copy]` heartbeat, **while minimised**,
which no capture so far has done (every console read so far restores the window first). If it is
this, the idle-signal branch below does not cover the minimised case.

Corollaries when adding anything deferred: fence-tag it, prune it in the same loop, and make sure
nothing can stop signalling. Tab close now rides this path via `DATASETTAB.gpuReleaseState`
(1=requested → 2=fence-tagged → released), driven from the copy thread, not the engineering thread.

**Update 2026-07-29 (10M plan Steps 0-1 shipped):** the sweep is no longer once-per-copy-iteration.
It is now the function `PruneRetiredGpuResources()` with **two** callers — the copy-thread loop AND
`ProcessScene3DCopyBatch` after every published chunk. That second caller is mandatory, not an
optimisation: per-chunk publish retires the append-target page on every chunk, and with reclaim only
at batch end a bulk import hit E_OUTOFMEMORY partway through (reproduced deliberately by shrinking
the chunk budget). Because reclaim is still gated on min-monitor-fence, a copy thread outrunning the
monitors can still accumulate, so the chunk loop takes bounded short waits above a ~64-page backlog
cap. Matrix slots freed by REMOVE/MODIFY now ride this same sweep (`pendingFreeMatrixSlots`).
So: **anything deferred added from here must be pruned inside that function, not at a call site.**

**Instance arena — replaced the world-matrix table (2026-07-30, 10M plan Steps 2-3).** The doubling
`GrowMatrixTable`, the mapped UPLOAD buffer, `TabGeometryStorage.retiredBuffers` and the three atomic
mirrors (`worldMatrixVAShared`/`DataShared`/`matrixCapacityShared`) are **all gone** — do not go
looking for them. Now: one reserved (tiled) DEVICE-LOCAL buffer per tab, 640 MB of GPU VA reserved
up front (`MV_MAX_INSTANCES_PER_TAB`), 64 KB tiles committed by `GrowInstanceArena` via
`UpdateTileMappings` **on the copy queue** (verified working, AMD Tier 3), one backing heap per
doubling step. The VA is fixed for the tab's lifetime, so the four readers — RenderScene3D,
ServicePick, RecordSelectionOverlays, PrinterController Record3DDraws — just bind
`tabRes.instanceArenaVA`. Still check all four when touching it.

Consequences that bite: the arena is not CPU-mappable, so records stage through the upload ring
(64 B + one `CopyBufferRegion` each); and the pick resolve's world-AABB centre now comes from a
CPU shadow inside the per-tab `InstanceRegistry` (reserve/commit VirtualMemory array, stable base
because RENDER threads read it). Convention trap: the arena stores `transpose(world)` because HLSL
packs StructuredBuffer matrices column-major — so the CPU-side centre must use the **untransposed**
matrix. The old pick code had this backwards; harmless only because every generator leaves
`geo.worldMatrix` identity and bakes positions into vertices.

**Step 4 shipped too (same day).** Second tiled buffer `instanceSlotOf[gpuInstanceIndex] -> slot`;
arena is now indexed by SLOT, not by index. Every ADD/MODIFY allocates a fresh slot, writes the
record, then flips 4 bytes — record + flip share one ring allocation and one command list, relying
on copy-queue in-order execution. Root signature gained **t1**; bind it in all four draw paths.
Record layout changed to 48-byte affine + 16-byte payload: `transformA/B/C` = first three rows of
`transpose(world)` = columns of W; point = `dot(float4(p,1), A/B/C)`, normal = `dot(n, A/B/C.xyz)`.
The struct is DUPLICATED in ShaderSceneVertex.hlsl and ShaderScenePickVertex.hlsl (no .hlsli
mechanism in the build) — change both plus `InstanceRecord` in RenderScene3D.h together.

Trap that bit during Step 4: a transform-only chunk publishes nothing, so the vacated-identity
handover must NOT sit after the "nothing to publish → continue" early exit (leaks a slot per move),
but must still run *after* `PublishPages` on the publishing path (fence must be ≥ the one retired
pages were tagged with). It is a lambda called from both exits.

**Transform-only fast path has no producer.** Keyed off `IsTransformOnlyEdit` (MODIFY with a world
matrix but empty vertex/index vectors). Nothing emits it because every generator bakes world
positions into vertices and leaves `GeometryData::worldMatrix` identity. That is now the ONLY thing
blocking Phase 5 hot-drag.

**Step 5 — visibility mask (2026-07-30).** THIRD tiled buffer `visibilityMask[gpuInstanceIndex]`,
8 bytes = a 64-bit SubTab membership word, `uint2` in shaders. Grown in lockstep with
`instanceSlotOf` inside `AllocateInstanceIndex` — both are ROOT descriptors, which have no bounds
check, so the mask must never lag. Root signature gained **t2** (mask SRV) and **b2** (the SubTab
bit). b2 is a SEPARATE root-constant range from b1 on purpose: b1 is what the command signature
rewrites per command, so a render-thread value sharing it would be clobbered by ExecuteIndirect.
Bind t2 + b2 in all four draw paths — the same four as t0/t1. The bit is the sub-tab SLOT;
`SubTabVisibilityBit()` maps slots >= 64 to `kNoSubTabBit`, which the shader reads as "show all".

Four things that are easy to get wrong here, all now commented in place:
- The default (all-ones) must be written on EVERY ADD. A freshly committed D3D12 tile has UNDEFINED
  contents — not zero — and a recycled index would otherwise inherit its predecessor's hides.
- Mask commands must stay out of BOTH the dedup map (it keys on `id` alone, so ADD+hide of one
  object in a batch collapses and the geometry never uploads) and Pass 1's `affectedPages` scan
  (else an 8-byte write clones a 4 MB page).
- The hidden-object shadow `DX12ResourcesPerTab::hiddenInstanceMasks` (copy-thread map holding ONLY
  non-default masks) is what replaces graphics.md's "one compute dispatch over the mask array" —
  compute needs a shader-visible descriptor heap that does not exist until Step 7.
- Clearing a retired sub-tab's bit had to go in `CleanupReleasedSubTabs`, NOT `RetireSubTabSlot`:
  the latter runs under `storageObjectsMutex`, and enqueueing there nests it inside
  `toCopyThreadMutex`, against the never-nested discipline — and render threads take
  storageObjectsMutex every frame in ResolveWindowViewTarget.

Producers are the HIDE_SELECTED / HIDE_UNSELECTED / HIDE_RESET ribbon rows, which already existed
unwired. Session-only; nothing persisted.

**Move fix + compaction + Step 6 (2026-07-31).** Three things worth keeping:

1. **TWO scans decide what gets cloned, not one.** `ProcessScene3DCopyBatch` Pass 1 (affectedPages)
   *and* the `containersNeedingAppend` loop that picks append candidates. The second gates on
   `cmd.geometry.has_value()`, which is TRUE for a transform-only edit — carrying the world matrix
   is what the otherwise-empty GeometryData is *for* — so fixing only Pass 1 still force-cloned one
   append page per container. Anything else that rides MODIFY but touches no bytes must opt out of
   **both**. Measured after: 27 moves, `clones`/`cloneMB` flat, scene visibly translated.
2. **Compaction can never fire in the batch that punches the holes**, because holes land on the
   CLONE in Pass 3 while the decision reads the OLD page in Pass 2. And it competes with the
   empty-page GC, which usually wins: a page whose objects ALL leave drains to objectCount 0 and is
   dropped, so compaction only ever sees pages that keep survivors. A test that re-meshes every
   object measures nothing — re-mesh alternating halves. Also removed the clone's
   `CopyResource` of the old argument buffer (rebuilt unconditionally moments later): 5.5 → 4.0 MB
   per clone, ~27% off all clone traffic.
3. **Step 6 shipped.** `GeometryPageSnapshot::pagesByContainer` is now the ONLY way render paths
   reach pages; `PageIsRenderable` lost its container argument, and the count-0 ExecuteIndirect
   trick is gone. Scene/pick/highlight/print share `ForEachSubTabPage`. Camera + Page2D pan/zoom
   moved out of InternalSubTab / TabCad2DStorage into `DATASETTAB::viewports[]`
   (`Viewport{subTabSlot, camera, page2DView}`, 1:1 with sub-tab slots today). `RenderPage2D` takes
   a `const Cad2DViewState&` instead of a slot. Debug keys: `g` bulk import, `m` transform-only
   moves, `n` alternating-half re-mesh (drives compaction).

**Step 7 vertical slice (2026-07-31) — BUILT (full Debug 0/0) + RUNTIME-VERIFIED by Ram via hotkey
`k` (ran correctly). Default stays OFF (`gUseComputeCull=false`); `k` toggles it.** GPU
draw-command compaction. Ram chose **per-page EI, GPU-compacted** (NOT one-EI-per-viewport) and a
**vertical slice** (regenerate per frame, keep legacy path as fallback) — so the compute pass is
**all ROOT DESCRIPTORS, no shader-visible descriptor heap** (that heap + SceneEpoch + cull caching +
retiring per-page indirectBuffers are all deliberately deferred). Pieces:
- **Absolute StartIndexLocation now live** (Step 7 constraint 1, done first, provable byte-equivalence):
  IBV binds at page BASE over whole pageSize, `StartIndexLocation = indexByteOffset/2`. Touched ALL
  FOUR draw paths — `RebuildIndirectBuffer` + `RenderScene3D` IBV, `Selection3D` `BindPageBuffers` +
  inline highlight startIndex, `PrinterController` IBV. Removed now-orphaned `Print3DPage::indexTail`.
- `ShaderSceneCull.hlsl` (cs_6_0, `g_sceneCullShader`, FxCompile added to vcxproj): 1 thread/template,
  tests VisibilityMask bit, InterlockedAdd-appends survivors to VisibleOut. Mirrors 24-byte
  IndirectCommand — keep in lockstep with the two scene shaders + RenderScene3D.h.
- `InitSceneCullResources` on gpu (siblings `sceneCullRootSignature`/`sceneCullPSO`/`cullZeroBuffer`),
  called in Main.cpp after InitSkyGradientResources. `SceneCullScratch` per render thread
  (visibleIndirect 1.5MB + visibleCount 4B), states reset to COMMON each frame after commandList Reset
  (buffers decay to COMMON at ECL). Compute root sig: b0 consts(2) + t0/t1 SRV + u0/u1 UAV.
- Per page (when `gUseComputeCull`): clear count via CopyBufferRegion from cullZeroBuffer, dispatch,
  barrier UAV->INDIRECT_ARGUMENT, restore graphics PSO (graphics root sig+args untouched by compute),
  EI from scratch with GPU count. **Toggle defaults OFF**; debug key `k` flips it (sibling of g/m/n).
- Deferred from plan: per-frame GPU-count heartbeat readback (needs fence-gated readback like pick).
- **Ram must runtime-verify:** press `g` (bulk import), Hide Selected, press `k`, confirm identical
  image + no D3D12 debug-layer errors. Plan file: `~/.claude/plans/humming-chasing-nest.md`.

**Compose multiple Scene3Ds into one Viewport (2026-07-31) — BUILT (0/0), RUNTIME PENDING.**
Architecture decision settled here: composing whole Scene3Ds is a **container-SET** op
(`SubTabContainerSet`, already iterated by RenderScene3D / pagesByContainer / ForEachSubTabPage since
Step 6), **NOT** a 64-bit-mask op; the mask stays per-OBJECT hide. Ram first wanted flat/shared pages
+ mask-only filtering (option 2), reasoning that per-container pages duplicate geometry when one
Scene3D shows in 2 SubTabs — **that is a MISCONCEPTION**: pages are stored once per container and
referenced by ID from each SubTab's set (and repeated *placements* reuse geometry via the instance
arena). So **per-container pages kept.** Why not flat: flat forces every active Viewport to cull ALL
tab objects each frame (cost ~ active-subtabs × total objects, no spatial accel yet) and makes
container close/delete/evict O(objects)+defrag; container-set is the interim coarse reject, and
flat+mask-only is the Step 9 (spatial culling) END-STATE, not now. The tiny-container page waste
(5.5MB/page × many small Scene3Ds) is real but fixed SEPARATELY by small-start/growing pages +
retiring per-page arg buffers — NOT by going flat.
Feature (home+composable model): drag a Scene3D from the data tree, drop on the inline scene →
`ADD_CONTAINER_TO_SUBTAB`; a top-centre chip strip (home chip no `x`, composed chips get `x`) →
`REMOVE_CONTAINER_FROM_SUBTAB`. New UIActions `kAdd/RemoveContainerToViewUIAction`; engineering
handlers `Add/RemoveContainerToActiveSubTab` (Scene3D-only, home never removable). `SingleUIWindow`
gained `draggedContainerId` drag state (6px promote threshold, drop valid below ribbon & outside
tree). `SubTabContainerSet::Remove` added. New geometry still parents to the home
(`activeScene3DMemoryId`). Same-window only; cross-window drop onto extracted windows deferred.
Double-click-open + band-drag extraction already worked, untouched.

**Still open:**
- Phase 4's other residual: Page2D record uploads (`ProcessCad2DCopyBatch`, which still creates its
  own allocator + command list per batch) and texture uploads have NOT been migrated onto the
  global upload ring.
- Step 6 item 2 left out the Viewport's render-target rectangle and update-scheduling fields; the
  rect is still derived per frame by the compositor and scheduling is Step 10.
- No page compaction/GC — MODIFY leaves holeBytes forever; empty pages stay in activePages (~5.5 MB
  each). Compaction deferred with user agreement. Note the Pass-2 clone is a whole-resource
  `CopyResource`, NOT compacting (corrected Ram's assumption on this).
- Large-pool per-tab reclamation in the CPU allocator — no owner tracking. Currently harmless
  (zero large allocs from META_DATA).
- `SubmitTextureUpload` ring: `writeIndex` incremented before slot fields are written → copy-thread
  race. Init-time mostly, but real.

**Environment gotcha that cost a day:** a hung Vishwakarma.exe holding the D3D12 device makes the
next launch fail in `CreateSwapChainForHwnd` with DXGI "Device interface cannot be NULL" — the real
cause is `CreateCommandQueue` failing silently upstream. Reboot clears it. The zombie itself came
from a WM_DPICHANGED/DISPLAYCHANGE arriving before GpuCopyThread starts: the handler calls
RestartRenderThreads → BuildMonitorIconAtlas → `WaitForSingleObject(INFINITE)` on a fence nobody
will signal. Hence the `gpu.isGPUEngineInitialized` gate on those two messages. A `_com_error` pair
at startup is COM noise from the SoftwareUpdate thread's CoInitializeEx, not ours.

Standalone allocator testing works: scratchpad `test_allocator.cpp` with manual INCLUDE/LIB env —
`vcvars64.bat` winsdk resolution is BROKEN on this machine (SDK at
`C:\Program Files (x86)\Windows Kits\10`, VS18 winsdk.bat can't find it; MSVC at
`D:\Program Files\...\VC\Tools\MSVC\14.51.36231`).

Related: [[per-monitor-icon-atlas]], [[multi-window-subtabs]], [[dxf-import-diagnostics]],
[[graphics-refactor-phases]].
