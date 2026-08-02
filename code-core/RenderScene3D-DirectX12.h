// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
#pragma once

#include <memory>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <wrl.h>

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

// Per-monitor drawn-command telemetry (10M plan Step 7). Both are called by the render thread on
// its own SceneCullScratch, and only ever record or read - neither affects what is drawn.
// PublishVisibleCountReadback consumes the PREVIOUS frame's counts once its fence has passed, so
// call it after the command list is reset and before recording; FinalizeVisibleCountFence tags the
// counts recorded this frame, so call it right after the frame's fence has been signalled.
void PublishVisibleCountReadback(SceneCullScratch& cullScratch);
void FinalizeVisibleCountFence(SceneCullScratch& cullScratch, uint64_t frameFenceValue);

// A fresh 4 MB double-ended geometry page (COMMON state) for the given container. Foundation's
// GpuCopyThread and the Scene3D copy path both allocate through here.
std::unique_ptr<GeometryPage> CreateNewPage(uint64_t containerMemoryId);

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