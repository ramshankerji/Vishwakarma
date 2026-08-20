// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
#pragma once

#include <memory>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <wrl.h>

#include "RenderScene3D.h" // GeometryPageKind and the portable primitive-library ABI.

// Full definitions live in MemoryManagerGPU-DirectX12.h (forward-declared to avoid a cycle).
struct DX12ResourcesPerWindow;
struct DX12ResourcesPerTab;
struct GeometryPage;
struct SceneCullScratch;
struct CommandToCopyThread;
struct DATASETTAB; // विश्वकर्मा.h

// Scene3D background. SceneTopUIHeightPx is the reserved top-UI band height in pixels;
// ClearSceneSkyGradient fills the scene area below it with the vertical sky gradient, drawn as one
// quad through the pipeline InitSkyGradientResources creates (call it once, before render threads
// start). It draws into whatever render target the caller has bound.
int  SceneTopUIHeightPx(int monitorId, const DX12ResourcesPerWindow& winRes);
void InitSkyGradientResources(ID3D12Device* device);
void ClearSceneSkyGradient(ID3D12GraphicsCommandList* commandList, DX12ResourcesPerWindow& winRes,
    int monitorId);

// GPU draw-command compaction pipeline (graphics.md, 10M plan Step 7 - vertical slice). Creates the
// compute root signature, PSO and the shared 4-byte zero buffer on the gpu singleton. Call once,
// before render threads start, alongside InitSkyGradientResources.
void InitSceneCullResources(ID3D12Device* device);

// Runtime toggle between the GPU-compacted one-ExecuteIndirect-per-Viewport draw path (true) and
// the legacy per-page ExecuteIndirect-of-all-templates path (false). Defaults to TRUE: the compute
// path draws a whole view in one call with no IA binds, which the legacy path cannot match. The
// legacy path stays maintained as the A/B reference; a debug key flips between them.
extern bool gUseComputeCull;

/* Pins every instanced template to the LOD the CPU chose (kPrimitiveFixedLod) instead of selecting
one per frame from projected screen size. Debug only, default OFF. It exists because LOD popping is
a tuning question rather than a correctness one: with this on, the compute path draws exactly what
Step 1 drew, so a suspect frame can be A/B'd against a known-good tessellation without rebuilding.
The legacy path is unaffected either way - it executes the templates directly and so is always
pinned. */
extern bool gLodPinned;

// Per-monitor drawn-command telemetry (10M plan Step 7). Both are called by the render thread on
// its own SceneCullScratch, and only ever record or read - neither affects what is drawn.
// PublishVisibleCountReadback consumes the PREVIOUS frame's counts once its fence has passed, so
// call it after the command list is reset and before recording; FinalizeVisibleCountFence tags the
// counts recorded this frame, so call it right after the frame's fence has been signalled.
void PublishVisibleCountReadback(SceneCullScratch& cullScratch);
void FinalizeVisibleCountFence(SceneCullScratch& cullScratch, uint64_t frameFenceValue);

/* The vertex and index buffer views one page draws through - the page's own buffer for a Bespoke
page, the global primitive library for an InstancedGlobal one.

ONE definition on purpose. Before shared geometry the same view construction was written out
separately in the scene loop, in Selection3D's BindPageBuffers and in the print collector; adding a
second page kind would have made that three places to remember, and a missed one is geometry that
silently disappears from that path alone. All three now call these. */
D3D12_VERTEX_BUFFER_VIEW PageVertexBufferView(const GeometryPage& page);
D3D12_INDEX_BUFFER_VIEW  PageIndexBufferView(const GeometryPage& page);

/* Build and upload the global primitive library onto the gpu singleton (graphics.md, "Shared
geometry and the primitive libraries"). Call ONCE from InitD3DDeviceOnly, after the copy queue and
fence exist and before any thread starts: it stages through a one-off committed upload buffer and
blocks on the copy fence, which is affordable exactly once for ~123 KB and removes any ordering
question about a resource every tab reads. */
void InitPrimitiveLibrary(ID3D12Device* device);

/* A fresh geometry page (COMMON state) for the given container. A Bespoke page gets the 4 MB
double-ended vertex/index buffer; an InstancedGlobal page gets NO geometry buffer at all - only the
draw-template argument buffer - because its objects draw from the primitive library. That is what
takes a page from ~4.25 MB to ~256 KB, and it is also why adding one sphere to a scene of 100,000
clones 256 KB rather than 4.25 MB. */
std::unique_ptr<GeometryPage> CreateNewPage(uint64_t containerMemoryId,
    GeometryPageKind kind = GeometryPageKind::Bespoke);

// Commit more 64 KB tiles behind a tab's instance arena until it holds at least minimumCapacity
// records, and refresh the per-tab SRV to match (graphics.md, 10M plan Step 2). The arena's virtual
// address is unaffected, so there is nothing to copy, retire or republish. Copy thread only, plus
// InitD3DPerTab for the very first step, which runs before any thread can see the tab.
void GrowInstanceArena(DX12ResourcesPerTab& tabRes, uint32_t minimumCapacity);

// 3D-geometry half of the copy thread: applies one drained ADD/MODIFY/REMOVE batch to the
// per-tab GeometryPages (RCU clone -> mutate -> publish). Mirrors ProcessCad2DCopyBatch.
// The COPY-type allocator/list stay owned by GpuCopyThread and are passed in for recording.
void ProcessScene3DCopyBatch(const std::vector<CommandToCopyThread>& batch,
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>& commandAllocator,
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>& commandList);

// Copy-thread-only: full teardown of one closed tab's Scene3D geometry (active + retired pages,
// snapshots, and this thread's objectID->page bookkeeping). Called by GpuCopyThread once every
// monitor's render fence has passed the tab's release fence, and by its shutdown path after the
// render threads are joined. Never call from the UI thread.
void ReleaseTabGpuGeometry(DATASETTAB& tab);