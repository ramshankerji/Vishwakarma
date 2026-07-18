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
struct GeometryPage;
struct CommandToCopyThread;
struct DATASETTAB; // विश्वकर्मा.h

// Scene3D background clear. SceneTopUIHeightPx is the reserved top-UI band height in pixels;
// ClearSceneSkyGradient fills the scene area below it with the vertical sky gradient.
int  SceneTopUIHeightPx(int monitorId, const DX12ResourcesPerWindow& winRes);
void ClearSceneSkyGradient(ID3D12GraphicsCommandList* commandList, DX12ResourcesPerWindow& winRes,
    D3D12_CPU_DESCRIPTOR_HANDLE rttHandle, int monitorId);

// A fresh 4 MB double-ended geometry page (COMMON state) for the given container. Foundation's
// GpuCopyThread and the Scene3D copy path both allocate through here.
std::unique_ptr<GeometryPage> CreateNewPage(uint64_t containerMemoryId);

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