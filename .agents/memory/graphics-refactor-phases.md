---
name: graphics-refactor-phases
description: "Rendering-code file layout after the 2026-07-13 reorg — where each seam lives and what was deliberately left undone"
metadata:
  node_type: memory
  type: project
  originSessionId: c4aae8ac-4598-4dac-b019-2f1914c4fa99
---

The rendering refactor is fully implemented (phases 1/2/5 = commit 22883d6; 3a/3b/3c/4 on
2026-07-13). `refactor.md` was deleted afterwards; its Goal section now lives in
`website/content/software/graphics.md` under "Rendering architecture (control flow)" — **read that
for the group table**, this memory only holds what isn't written down there.

Layout: Scene3D / Page2D / Compositor / UI overlay / GPU foundation, each split platform-agnostic vs
`-DirectX12`, same function names per platform, no virtual dispatch.

Seams worth knowing:
- `RenderCompositor.h/.cpp`: `ResolveWindowViewTarget(window, tab) -> WindowViewTarget{...}`;
  GpuRenderThread consumes it and writes `tabRes.camera` each frame.
- `UserInterface.cpp` holds the portable UI half — atlas building, `PrecomputeTopRibbonLayout`, all
  `Push*`, and `BuildUIOverlay(...)` (the whole widget body). The DX12 wrapper only binds PSO/buffers,
  calls BuildUIOverlay, draws.
- `ProcessScene3DCopyBatch(...)` in RenderScene3D-DirectX12.cpp is the old per-tab RCU loop;
  `objectLocation` is a function-local static (single copy thread). GpuCopyThread now only drains
  queues, dispatches, prunes.
- Portable ABI structs: 2D records + `Cad2DViewConstants` (`Cad2DFloat2`) + `Cad2DViewState` →
  RenderPage2D.h. `IndirectCommand` (size-asserted mirror of D3D12_DRAW_INDEXED_ARGUMENTS),
  `GeometryPlacementRecordInPage`, `CommandToCopyThread` + queues → RenderScene3D.h.

**Gotcha:** the new UserInterface.cpp TU needs `#define NOMINMAX` before includes — windows.h
arrives via विश्वकर्मा.h without it, breaking `std::min/max` including inside fast_float.

**Deliberately NOT done** (Ram chose minimal scope — don't reopen unasked, see
[[user-working-style]]): TabCad2DStorage → Page2DGpuResources restructure; moving
GeometryPage/TabGeometryStorage out of MemoryManagerGPU-DirectX12.h; विश्वकर्मा.h still includes
RenderPage2D-DirectX12.h directly; डेटा.h's direct d3d12.h includes (invariant-5 violation). Dead
code left as-is: `batchLocationOverride`, `int counter`, `g_gpuCommandQueue`/ThreadSafeQueueGPU.

If doing another verbatim-move refactor: line-range slicing script with seam asserts (scratchpad
`split_ui.ps1` / `split_copybatch.ps1` pattern); beware PowerShell `R` aliases Invoke-History; files
are UTF-8 no BOM, LF. Related: [[commandline-build]], [[graphics-leak-audit]].
