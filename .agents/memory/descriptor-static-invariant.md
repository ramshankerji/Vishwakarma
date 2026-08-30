---
name: descriptor-static-invariant
description: "D3D12 root-signature ranges with DESCRIPTOR_RANGE_FLAG_NONE are DESCRIPTORS_STATIC — every slot must hold a view, including reserved ones; plus the sky-gradient PSO that replaced the banded clears"
metadata: 
  node_type: memory
  type: project
  originSessionId: 56e34e0c-a483-4d90-ad52-cb87b1d1bba1
---

Vishwakarma declares its UI/text SRV ranges as the full `UI_MAX_ATLAS_TEXTURES` (10) with
`D3D12_DESCRIPTOR_RANGE_FLAG_NONE` (UserInterface-DirectX12.cpp `InitUIResources`, and the text root
signature in RenderPage2D-DirectX12.cpp `InitCad2DTabResources`). In root signature 1.1 that flag
means **DESCRIPTORS_STATIC**: every descriptor in the range must be initialized before the table is
bound, even slots no shader ever samples. Only slots 0 (English MSDF) and 1 (icon) hold real views,
so slots 2..9 produced `#646 INVALID_DESCRIPTOR_HANDLE` + `#1023 RESOURCE_DIMENSION_MISMATCH` every
frame until `FillSpareAtlasSlots` aliased them onto the English atlas (fixed 2026-07-19).

**How to apply:** whenever `UI_MAX_ATLAS_TEXTURES` grows, a new root signature declares an SRV range,
or a new heap is created for these tables, fill *all* slots. The per-monitor heaps
(`BuildMonitorIconAtlas`) fill from slot 2; the shared `uiResources.srvHeap` — bound only by the
print path, which draws text and no icons — fills from slot 1. A real atlas uploaded into an aliased
slot simply overwrites it, so there is nothing to unwind.

Same session: the Scene3D sky stopped being 48 banded `ClearRenderTargetView` rect calls (which
also spammed `#820 CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE`, since the RTT bakes only the
sky-*top* clear value) and became `ShaderSkyGradient{Vertex,Pixel}.hlsl` — a 4-vertex triangle-strip
quad, 8 root constants (float3 top + ndcTopY + float3 horizon + pad, matching HLSL register packing),
no CB/VB/descriptor heap. `InitSkyGradientResources` runs once in Main.cpp before
`RestartRenderThreads`; `gpu.skyGradientPSO` is then read-only, which is what makes it safe for the
per-monitor render threads. If the PSO is null `ClearSceneSkyGradient` returns and the compositor's
earlier full clear leaves a flat sky. Note a *rect* clear never gets the fast-clear path anyway, so
those 48 warnings were reporting an optimization that was never available. The one remaining #820 is
Page2D's grey background clear on a sky-blue-baked RTT.

Related: [[graphics-refactor-phases]], [[per-monitor-icon-atlas]], [[commandline-build]].
