---
name: step7-per-viewport-draw
description: "10M plan Step 7: one ExecuteIndirect per Viewport shipped via per-command VBV/IBV in root constants — why the SceneEpoch page directory was NOT needed, and what is still deferred"
metadata: 
  node_type: memory
  type: project
  originSessionId: d3bae128-d2ef-4be1-af53-4453a3842df0
  modified: 2026-08-02T04:13:27.120Z
---

Shipped 2026-08-02. Design lives in `website/content/software/graphics.md` Step 7 — **read that first**;
this holds only what is not written down there.

**The insight the design missed.** graphics.md offered two ways to get one `ExecuteIndirect` per
Viewport: per-command buffer views, *or* a `SceneEpoch` page directory the compute shader reads VBV/IBV
from. It reads as though the directory (and therefore the shader-visible descriptor heap + global
allocator) were prerequisites. They are not — **the dispatch is already per page**, so the CPU can hand
each page's views to its own dispatch as root constants. That one observation deleted SceneEpoch, the
descriptor heap, the global allocator and the cull cache from the critical path, and took the step from
a multi-week rewrite to an afternoon.

**Layout traps, all load-bearing:**
- Command-signature argument order is `{VBV, IBV, CONSTANT b1, DRAW_INDEXED}` — views FIRST. D3D12 packs
  arguments tightly, and both `D3D12_GPU_VIRTUAL_ADDRESS` fields must be 8-byte aligned. Leading with
  the 4-byte root constant puts the first address at offset 4 and every later one on an odd boundary.
  56-byte stride keeps the alignment for every command. Draw must be last (D3D12 requires it anyway).
- The HLSL `VisibleCommand` is **14 plain scalar uints**, not uint2/uint4 — no alignment padding to
  reason about, so the structured-buffer stride is unambiguously 56 and matches `ByteStride`. GPU
  addresses ride as explicit lo/hi uint pairs, so no 64-bit shader int support is needed.
- `CullParams` is 12 **scalar** uints (one is pure padding). Scalars never straddle a float4 register, so
  the cbuffer is exactly what `InitAsConstants` declares.
- Two command signatures coexist: 24-byte for the legacy draw path + GPU pick + print, 56-byte for the
  compacted per-Viewport draw. Both use the same root signature.

**Why the cull output is per MONITOR, not per Viewport** (the design says per Viewport): two main windows
on two monitors both resolve the same tab's active sub-tab in `ResolveWindowViewTarget`, so a
per-Viewport buffer would have two owning render threads — against invariant 4. Any future cull cache
must be keyed per monitor *per* Viewport. Several Viewports on one monitor share the scratch safely only
because they are recorded in order into one command list.

**No barriers between page dispatches** — they touch the count only via `InterlockedAdd`, so they are
order-independent and may overlap. Command order in the buffer therefore varies frame to frame, which is
fine for depth-tested opaque draws. Do not "fix" this by adding UAV barriers; the barrier removal is a
real part of the win.

**Overflow is safe by construction:** shader drops a slot past `kMaxCommands`, and `ExecuteIndirect`
independently runs `min(MaxCommandCount, countBuffer)`. The count is left over-counted on purpose so the
read-back can report it.

Still deferred: SceneEpoch + revision-keyed cull cache; retiring the per-page indirect buffers (the
1.5 MB fixed reservation per 4 MB page — the largest piece left, and it drags the pick and print paths
with it); collapsing the per-page dispatches.

Verification recipe that actually proved it (see [[ribbon-command-recipe]] for the scripting mechanics):
`k` A/B'd the two paths to **0 differing pixels of 131,520 samples**; Hide Unselected drove
`[gpu][cull] drawn` 182 → 1 with `clones`/`cloneMB` flat; composing a second container gave `drawn=188`
across two pages in one call. Turn OFF camera rotation (`r`) *and* Auto Random first, and prove
quiescence with a two-screenshot diff before trusting any A/B.

Related: [[graphics-refactor-phases]], [[multi-window-subtabs]], [[commandline-build]],
[[descriptor-static-invariant]], [[graphics-leak-audit]].
