// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

#include "RenderScene3D-DirectX12.h"
#include "MemoryManagerGPU-DirectX12.h"
#include "RenderPage2D-DirectX12.h"
#include "UserInterface-DirectX12.h"
#include "ShaderSceneVertex.h"
#include "ShaderScenePixel.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include "विश्वकर्मा.h"
#include <iomanip>
#include <unordered_set>
#include <colors.h>

extern शंकर gpu;
extern std::atomic<uint64_t> atlasFence;

namespace {

    float LerpFloat(float a, float b, float t) {
        return a + (b - a) * t;
    }
} // namespace

int SceneTopUIHeightPx(int monitorId, const DX12ResourcesPerWindow& winRes) {
    if (winRes.contentOnly) return 0; // Extracted view windows render content edge to edge.
    int topUITotalHeightPx = 0;
    if (monitorId >= 0 && monitorId < gpu.currentMonitorCount) {
        const UITopRibbonLayout& layout = gpu.screens[monitorId].topRibbonLayout;
        if (layout.isValid && layout.topUITotalHeightPx > 0.0f) {
            topUITotalHeightPx = static_cast<int>(std::round(layout.topUITotalHeightPx));
        }
        else {
            float pixelsPerMMy = static_cast<float>(gpu.screens[monitorId].physicalDpiY) / 25.4f;
            topUITotalHeightPx = static_cast<int>(std::round((UI_TAB_BAR_HEIGHT_MM + UI_DIVIDER_GAP_PX +
                UI_ACTION_GROUP_LABEL_HEIGHT_MM + UI_DIVIDER_GAP_PX +
                UI_ACTION_GROUP_HEIGHT_MM + UI_DIVIDER_GAP_PX +
                UI_ACTION_GROUP_LABEL_HEIGHT_MM + UI_DIVIDER_GAP_PX +
                UI_INTERNAL_TAB_BAR_HEIGHT_MM) * pixelsPerMMy)) + 7;
        }
    }

    return std::clamp(topUITotalHeightPx, 0, winRes.WindowHeight);
}

void ClearSceneSkyGradient(ID3D12GraphicsCommandList* commandList, DX12ResourcesPerWindow& winRes,
    D3D12_CPU_DESCRIPTOR_HANDLE rttHandle, int monitorId) {
    if (!commandList || winRes.WindowWidth <= 0 || winRes.WindowHeight <= 0) return;

    const int topUI = SceneTopUIHeightPx(monitorId, winRes);
    const int sceneHeight = winRes.WindowHeight - topUI;
    if (sceneHeight <= 0) return;

    const int bandCount = (std::min)(kSceneSkyGradientBands, sceneHeight);
    for (int band = 0; band < bandCount; ++band) {
        const LONG y0 = static_cast<LONG>(topUI + (sceneHeight * band) / bandCount);
        const LONG y1 = static_cast<LONG>(topUI + (sceneHeight * (band + 1)) / bandCount);
        if (y1 <= y0) continue;

        const float t = (static_cast<float>(band) + 0.5f) / static_cast<float>(bandCount);
        const float smoothT = t * t * (3.0f - 2.0f * t);
        const float skyColor[] = {
            LerpFloat(kSceneSkyTopR, kSceneSkyHorizonR, smoothT),
            LerpFloat(kSceneSkyTopG, kSceneSkyHorizonG, smoothT),
            LerpFloat(kSceneSkyTopB, kSceneSkyHorizonB, smoothT),
            1.0f
        };
        const D3D12_RECT rect = { 0, y0, static_cast<LONG>(winRes.WindowWidth), y1 };
        commandList->ClearRenderTargetView(rttHandle, skyColor, 1, &rect);
    }
}


std::unique_ptr<GeometryPage> CreateNewPage(uint64_t containerMemoryId)
//Do not make this static function. It accesses global gpu singleton.
{
    auto page = std::make_unique<GeometryPage>();
    page->pageSize = 4 * 1024 * 1024;
    page->indexTail = page->pageSize;
    page->containerMemoryId = containerMemoryId;

    CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
    auto desc = CD3DX12_RESOURCE_DESC::Buffer(page->pageSize);
    // ThrowIfFailed: a silent failure here (VRAM exhaustion) returns a page with null buffers,
    // which crashes later at CopyResource with no indication of the real cause. The copy thread
    // catches this exception and drops the batch instead of aborting.
    ThrowIfFailed(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&page->buffer)));
    auto indirectDesc = CD3DX12_RESOURCE_DESC::Buffer(65536 * sizeof(IndirectCommand));
    ThrowIfFailed(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &indirectDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&page->indirectBuffer)));

    static std::atomic<uint64_t> totalPages = 0; //Telemetry helper / counter.
    //std::wcout << "New page allocated. New Page Counter: " << ++totalPages << std::endl;
    return page;
}

void शंकर::RenderScene3D(ID3D12GraphicsCommandList* commandList,
    DX12ResourcesPerWindow& winRes, const DX12ResourcesPerTab& tabRes, TabGeometryStorage& storage,
    int monitorId, uint64_t activeContainerMemoryId) {
    //int i = 0; // Latter to be iterated over number of screens.
    // Update constant buffer with transformation matrices

    // Create view matrix (camera looking at scene from distance)
    XMVECTOR eyePosition = XMLoadFloat3(&tabRes.camera.position);
    XMVECTOR focusPoint = XMLoadFloat3(&tabRes.camera.target);
    XMVECTOR upDirection = XMLoadFloat3(&tabRes.camera.up);
    DirectX::XMMATRIX viewMatrix = DirectX::XMMatrixLookAtLH(eyePosition, focusPoint, upDirection);

    // Create projection matrix
    if (winRes.WindowHeight == 0) return; //Prevent divide by 0. If minimized window: WindowHeight = 0.

    // Compute top UI height in pixels based on monitor physical DPI (DPI -> pixels per mm)
    const int topUITotalHeightPx = SceneTopUIHeightPx(monitorId, winRes);

    // Adjust viewport/scissor to exclude the top UI area so 3D scene starts below it.
    int sceneHeight = winRes.WindowHeight - topUITotalHeightPx;
    if (sceneHeight <= 0) return;
    float aspectRatio = static_cast<float>(winRes.WindowWidth) / static_cast<float>(sceneHeight);

    XMMATRIX projectionMatrix = XMMatrixPerspectiveFovLH(
        tabRes.camera.fov, aspectRatio, tabRes.camera.nearZ, tabRes.camera.farZ);
    XMMATRIX viewProj = viewMatrix * projectionMatrix;

    // Create world matrix with rotation. Now the camera rotates, not the world !
    // Update constant buffer
    ConstantBuffer constantBufferData;
    XMStoreFloat4x4(&constantBufferData.viewProj, XMMatrixTranspose(viewProj));
    memcpy(winRes.cbvDataBegin, &constantBufferData, sizeof(constantBufferData));

    // Root Signature: The maximum size of a root signature is 64 DWORDs. 1 DWORD = 4 bytes, so that's 256 bytes total.
    // Root constants: 1 DWORD, i.e. 32-bit values. Root descriptors(64 - bit GPU virtual addresses) cost 2 DWORDs each.
    // https://learn.microsoft.com/en-us/windows/win32/direct3d12/root-signature-limits
    commandList->SetGraphicsRootSignature(tabRes.rootSignature.Get());
    commandList->SetPipelineState(tabRes.pipelineState.Get());
    // Set root descriptor table. No longer used.
    // Bind directly using GPU Virtual Addresses!
    commandList->SetGraphicsRootConstantBufferView(0, winRes.constantBuffer->GetGPUVirtualAddress());
    // Matrix-table VA (t0) is bound below, AFTER the snapshot acquire-load: the copy thread can
    // regrow the table, and the snapshot publish order guarantees a snapshot referencing the new
    // capacity is only visible together with the new buffer address (see DX12ResourcesPerTab).

    // Create named variables (l‑values)
    // Viewport starts at y = topUITotalHeightPx and has reduced height
    CD3DX12_VIEWPORT viewport(0.0f, static_cast<float>(topUITotalHeightPx),
        static_cast<float>(winRes.WindowWidth),
        static_cast<float>(sceneHeight)
    );

    CD3DX12_RECT scissorRect(0, topUITotalHeightPx, winRes.WindowWidth, winRes.WindowHeight);

    // Now you can take their addresses and call the methods
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissorRect);

    /* Transition from D3D12_RESOURCE_STATE_PRESENT to D3D12_RESOURCE_STATE_RENDER_TARGET
    Already done by parent function i.e. Render Thread.*/

    // Record commands
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(winRes.rtvHeap->GetCPUDescriptorHandleForHeapStart(),
        winRes.frameIndex, gpu.rtvDescriptorSize);
    CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle(winRes.dsvHeap->GetCPUDescriptorHandleForHeapStart());
    //commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle); //Removed. Already done by GpuRenderThread.

    // Clear render target and depth stencil
    const float clearColor[] = { kSceneSkyTopR, kSceneSkyTopG, kSceneSkyTopB, 1.0f }; // Example color, adjust as needed
    //commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr); //Removed. Already done by GpuRenderThread.
    commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // PAGE-BASED RENDERING (Solid Opaque Only)
    GeometryPageSnapshot* snapshot = storage.activeSnapshot.load(std::memory_order_acquire);
    if (snapshot) {
        commandList->SetGraphicsRootShaderResourceView(1,
            tabRes.worldMatrixVAShared.load(std::memory_order_acquire));
        for (GeometryPage* pagePtr : snapshot->pages) {
            GeometryPage& page = *pagePtr;
            if (!page.published.load(std::memory_order_acquire)) continue;
            if (page.indirectCount == 0) continue; //Some safety checks.
            if (page.vertexHead == 0) continue;
            if (page.indexTail == page.pageSize) continue;

            D3D12_VERTEX_BUFFER_VIEW vbv{};
            vbv.BufferLocation = page.buffer->GetGPUVirtualAddress();
            vbv.SizeInBytes = page.vertexHead;
            vbv.StrideInBytes = sizeof(Vertex);

            D3D12_INDEX_BUFFER_VIEW ibv{};
            ibv.BufferLocation = page.buffer->GetGPUVirtualAddress() + page.indexTail;
            ibv.SizeInBytes = page.pageSize - page.indexTail;
            ibv.Format = DXGI_FORMAT_R16_UINT;

            commandList->IASetVertexBuffers(0, 1, &vbv);
            commandList->IASetIndexBuffer(&ibv);

            const uint32_t argumentCount =
                activeContainerMemoryId != 0 && page.containerMemoryId == activeContainerMemoryId
                ? page.indirectCount
                : 0;
            commandList->ExecuteIndirect(tabRes.commandSignature.Get(),
                argumentCount, page.indirectBuffer.Get(), 0, nullptr, 0);
        }
    } // End of if (snapshot)
    // TODO: Add support for transparent pages with proper sorting and blending states.
    // TODO: Similarly for all varients of geometry, like wireframe, hugoObjects etc. all unique PSO.

    /* Transition from D3D12_RESOURCE_STATE_RENDER_TARGET to D3D12_RESOURCE_STATE_RENDER_PRESENT
    taken care by parent function. i.e. Render Thread */
    //commandList->Close();// No longer required here. It will be done by render thread.

    //Following mutex will release automatically when this function returns, i.e. when lock goes out of scope.
    //std::lock_guard<std::mutex> lock(tabRes.objectsOnGPUMutex);
}

// Copy-thread-private bookkeeping: maps each objectID to the VRAM page (raw ptr) that currently
// owns it. Persists across batches: exactly one copy thread exists for the process lifetime.
// File scope (not function-static) so ReleaseTabGpuGeometry can purge a closed tab's entries.
struct ObjectLocation {
    GeometryPage* page;   // page where object currently resides
    uint32_t slot;        // index in page->objects
};
static std::unordered_map<uint64_t, ObjectLocation> objectLocation;

// Copy-thread-only: full teardown of one closed tab's Scene3D geometry. GpuCopyThread calls this
// once every monitor's render fence has passed the tab's release fence (no submitted frame can
// still reference these pages), and again from its shutdown path after render threads joined.
void ReleaseTabGpuGeometry(DATASETTAB& tab) {
    TabGeometryStorage& storage = tab.geometry;

    // Purge this tab's entries from the location map: the pages they point to are destroyed
    // below, and tab slots are never reused within a session, so the ids never come back.
    std::unordered_set<const GeometryPage*> ownedPages;
    for (const auto& page : storage.activePages) ownedPages.insert(page.get());
    for (const auto& retired : storage.retiredPages) ownedPages.insert(retired.page.get());
    for (auto it = objectLocation.begin(); it != objectLocation.end();) {
        if (ownedPages.count(it->second.page)) it = objectLocation.erase(it);
        else ++it;
    }

    GeometryPageSnapshot* snapshot =
        storage.activeSnapshot.exchange(nullptr, std::memory_order_acq_rel);
    delete snapshot;
    for (auto& retired : storage.retiredSnapshots) delete retired.snapshot;
    storage.retiredSnapshots.clear();
    storage.retiredPages.clear(); // unique_ptr<GeometryPage> releases the VRAM buffers.
    storage.retiredBuffers.clear(); // Outgrown matrix buffers.
    storage.activePages.clear();
}

// Copy-thread-only: double the per-tab world-matrix table when it is full. The old buffer goes
// onto storage.retiredBuffers (still mapped: in-flight frames may bind its VA and the pick
// resolve may read its pointer) and is freed by the safeRetireFence sweep. The render-thread
// mirrors are republished LAST, so readers holding stale values still hit a live buffer, and any
// snapshot referencing the new capacity is published after the new values (see the mirror
// comments in DX12ResourcesPerTab). Throws HrException on allocation failure; GpuCopyThread's
// batch try/catch drops the batch and the old table stays valid.
static void GrowMatrixTable(DX12ResourcesPerTab& tabRes, TabGeometryStorage& storage) {
    const uint32_t oldCapacity = tabRes.matrixCapacity;
    const uint32_t newCapacity = oldCapacity * 2;

    ComPtr<ID3D12Resource> newBuffer;
    auto matrixDesc = CD3DX12_RESOURCE_DESC::Buffer(
        static_cast<uint64_t>(newCapacity) * sizeof(DirectX::XMFLOAT4X4));
    CD3DX12_HEAP_PROPERTIES uploadProps(D3D12_HEAP_TYPE_UPLOAD);
    ThrowIfFailed(gpu.device->CreateCommittedResource(&uploadProps, D3D12_HEAP_FLAG_NONE,
        &matrixDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&newBuffer)));

    UINT8* newData = nullptr;
    CD3DX12_RANGE readRange(0, 0);
    ThrowIfFailed(newBuffer->Map(0, &readRange, reinterpret_cast<void**>(&newData)));
    // Both buffers are persistently mapped upload heaps: a plain CPU memcpy carries every
    // existing matrix over. No GPU copy, no fence needed for the data itself.
    memcpy(newData, tabRes.pWorldMatrixDataBegin, oldCapacity * sizeof(DirectX::XMFLOAT4X4));

    // Old buffer: keep alive (and mapped) until every monitor's fence passes this value.
    storage.retiredBuffers.push_back({ tabRes.worldMatrixBuffer,
        gpu.renderFenceValue.load(std::memory_order_acquire) });

    // Copy-thread-private fields.
    tabRes.worldMatrixBuffer = newBuffer;
    tabRes.pWorldMatrixDataBegin = newData;
    tabRes.matrixCapacity = newCapacity;

    // Keep the per-tab srvHeap descriptor (created by InitD3DPerTab, not used by the current
    // root-SRV draw paths) pointing at the live buffer instead of dangling on the retired one.
    D3D12_SHADER_RESOURCE_VIEW_DESC srvView = {};
    srvView.Format = DXGI_FORMAT_UNKNOWN;
    srvView.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvView.Buffer.FirstElement = 0;
    srvView.Buffer.NumElements = newCapacity;
    srvView.Buffer.StructureByteStride = sizeof(DirectX::XMFLOAT4X4);
    gpu.device->CreateShaderResourceView(tabRes.worldMatrixBuffer.Get(), &srvView,
        tabRes.srvHeap->GetCPUDescriptorHandleForHeapStart());

    // Publish to render threads last: pointer, then VA, then capacity (capacity gates indices).
    tabRes.worldMatrixDataShared.store(newData, std::memory_order_release);
    tabRes.worldMatrixVAShared.store(newBuffer->GetGPUVirtualAddress(), std::memory_order_release);
    tabRes.matrixCapacityShared.store(newCapacity, std::memory_order_release);

    std::wcout << L"World-matrix table grown: " << oldCapacity << L" -> " << newCapacity
        << L" slots (" << (newCapacity * sizeof(DirectX::XMFLOAT4X4) / 1024) << L" KB)." << std::endl;
}

// Copy-thread-only: next free matrix slot, doubling the table when it is full.
static uint32_t AllocateMatrixSlot(DX12ResourcesPerTab& tabRes, TabGeometryStorage& storage) {
    if (!tabRes.freeMatrixSlots.empty()) {
        const uint32_t slot = tabRes.freeMatrixSlots.back();
        tabRes.freeMatrixSlots.pop_back();
        return slot;
    }
    if (tabRes.matrixCount >= tabRes.matrixCapacity) GrowMatrixTable(tabRes, storage);
    return tabRes.matrixCount++;
}

// 3D-geometry half of the copy thread: applies one drained batch of ADD/MODIFY/REMOVE commands
// to the per-tab GeometryPages (RCU clone -> mutate -> publish), then atomically publishes the
// new page snapshot. Mirrors ProcessCad2DCopyBatch (RenderPage2D-DirectX12.cpp). The COPY-type
// commandAllocator/commandList stay owned by GpuCopyThread and are passed in for recording.
void ProcessScene3DCopyBatch(const std::vector<CommandToCopyThread>& batch,
    ComPtr<ID3D12CommandAllocator>& commandAllocator,
    ComPtr<ID3D12GraphicsCommandList>& commandList) {
    auto PublishPages = [&](TabGeometryStorage& storage, const std::vector<GeometryPage*>& oldPagesToReplace,
        std::vector<std::unique_ptr<GeometryPage>> replacementPages,
        std::vector<std::unique_ptr<GeometryPage>> newPagesToAppend)
        {
            if (oldPagesToReplace.size() != replacementPages.size()) {// Ensure vectors are matched for replacement
                std::cerr << "RCU Error: Mismatch between old pages and replacement pages count." << std::endl;
                return;
            }

            // Mark all new/replacement pages as published before exposing them
            for (auto& page : replacementPages) page->published.store(true, std::memory_order_release);
            for (auto& page : newPagesToAppend) page->published.store(true, std::memory_order_release);

            // Tag with current render frame
            uint64_t currentRenderFence = gpu.renderFenceValue.load(std::memory_order_acquire);

            // Update the writer's authoritative list (activePages). Replace old pages
            for (size_t i = 0; i < oldPagesToReplace.size(); ++i) {
                GeometryPage* targetOldPage = oldPagesToReplace[i];

                // Find the unique_ptr in activePages that matches this raw pointer
                auto it = std::find_if(storage.activePages.begin(), storage.activePages.end(),
                    [targetOldPage](const std::unique_ptr<GeometryPage>& p) { return p.get() == targetOldPage; });

                if (it != storage.activePages.end()) { // Move the old page into the retirement queue
                    storage.retiredPages.push_back({ std::move(*it), currentRenderFence });
                    if (replacementPages[i]->objectCount == 0) {
                        // Empty-page GC: every object in this clone was deleted or relocated.
                        // It was never published (snapshot is built below from activePages) and
                        // its only GPU work, the Pass-2 clone copy, has already been fence-waited,
                        // so dropping it frees its ~5.5 MB now instead of parking a dead page in
                        // activePages forever (pages have no compaction yet).
                        storage.activePages.erase(it);
                    } else {
                        *it = std::move(replacementPages[i]);// Slot the replacement page into the exact same position
                    }
                }
            }
            for (auto& newPage : newPagesToAppend) { // Append new pages
                storage.activePages.push_back(std::move(newPage));
            }

            // Build the new RCU Snapshot
            GeometryPageSnapshot* newSnapshot = new GeometryPageSnapshot();
            newSnapshot->pages.reserve(storage.activePages.size());
            for (const auto& pagePtr : storage.activePages) {
                newSnapshot->pages.push_back(pagePtr.get()); // Read-only pointers for the Render thread
            }
            // Atomically Publish the new snapshot.  exchange() swaps the pointer and returns the old one.
            GeometryPageSnapshot* oldSnapshot = storage.activeSnapshot.exchange(newSnapshot, std::memory_order_acq_rel);
            // Retire the old snapshot (the struct itself) so it can be deleted later
            if (oldSnapshot) storage.retiredSnapshots.push_back({ oldSnapshot, currentRenderFence });

            /* Future Notes: DO NOT try to implement versioned page arrays technique.
            Since our page size is 4MB, and currently none of state of art graphics card has exceed 128 GB Memory,
            our worst case is still ~32000 Pages, in real world we expect it to be less than 1000.
            Hence no need to add additional complexity. RCU is already complex enough !
            TODO: Add page count to telemetry.*/
        };

    uint64_t fenceValue = 0;

        uint16_t* tabList = publishedTabIndexes.load(std::memory_order_acquire);
        uint16_t tabCount = publishedTabCount.load(std::memory_order_acquire);

        std::vector<uint16_t> sortedTabs(tabList, tabList + tabCount);
        std::sort(sortedTabs.begin(), sortedTabs.end()); //Sort such that tab 0 is always processed 1st. Don't remove.

        std::unordered_map<uint64_t, ObjectLocation> batchLocationOverride;

        for (uint16_t tabID : sortedTabs) { //Process 1 tab at a time.
            bool tabTouched = false;

            DATASETTAB& targetTab = allTabs[tabID];
            TabGeometryStorage& storage = targetTab.geometry;
            DX12ResourcesPerTab& tabRes = targetTab.dx;

            std::unordered_set<GeometryPage*> affectedPages;
            std::unordered_map<GeometryPage*, std::unique_ptr<GeometryPage>> clonedPages;
            std::vector<std::unique_ptr<GeometryPage>> newPages;

            // Pre-Pass: Deduplicate commands for this tab. If the same object is modified twice,
            // or added then modified, etc., we need only FINAL state in this batch to persist.
            std::unordered_map<uint64_t, size_t> latestCommandIndex;
            for (size_t i = 0; i < batch.size(); ++i) {
                if (batch[i].tabID == tabID) { latestCommandIndex[batch[i].id] = i; }
            }

            std::vector<CommandToCopyThread> deduplicatedBatch;
            deduplicatedBatch.reserve(batch.size());
            for (size_t i = 0; i < batch.size(); ++i) {
                if (batch[i].tabID != tabID) continue;
                // Only keep the command if it is the absolute latest operation for this ID
                if (latestCommandIndex[batch[i].id] == i) { deduplicatedBatch.push_back(batch[i]); }
            }

            // Update the tabTouched flag based on our deduplicated list
            if (deduplicatedBatch.empty()) continue; // No command for this tab. Skip this tab.
            tabTouched = true;

            // Pass 1: Identify affected pages. We will clone these pages,
            //apply modifications to the clones, and then publish atomically.
            for (auto& cmd : deduplicatedBatch) {
                if (cmd.type == CommandToCopyThreadType::ADD) continue; // handled later
                auto it = objectLocation.find(cmd.id);
                if (it != objectLocation.end()) affectedPages.insert(it->second.page);
            }

            // Find the page with the largest contiguous middle gap for each container receiving geometry.
            // Pages never mix containers, so inactive pages can be hidden with ExecuteIndirect count 0.
            std::unordered_set<uint64_t> containersNeedingAppend;
            for (const auto& cmd : deduplicatedBatch) {
                if (!cmd.geometry.has_value()) continue;
                uint64_t containerMemoryId = cmd.containerMemoryId;
                auto existingIt = objectLocation.find(cmd.id);
                if (containerMemoryId == 0 && existingIt != objectLocation.end()) {
                    containerMemoryId = existingIt->second.page->containerMemoryId;
                }
                containersNeedingAppend.insert(containerMemoryId);
            }

            std::unordered_map<uint64_t, GeometryPage*> bestAppendCandidates;
            std::unordered_map<uint64_t, size_t> maxHoleByContainer;
            for (const auto& pagePtr : storage.activePages) {
                GeometryPage* p = pagePtr.get();
                if (!p->published.load(std::memory_order_acquire)) continue; //Just for extra safety.
                if (containersNeedingAppend.find(p->containerMemoryId) == containersNeedingAppend.end()) continue;

                size_t hole = p->indexTail - p->vertexHead;  // middle contiguous free space
                size_t& maxHole = maxHoleByContainer[p->containerMemoryId];
                if (hole > maxHole) {
                    maxHole = hole;
                    bestAppendCandidates[p->containerMemoryId] = p;
                }
            }
            // Force-clone each append candidate so Pass 3 never mutates a published page.
            for (const auto& [containerMemoryId, candidate] : bestAppendCandidates) {
                affectedPages.insert(candidate);
            }

            commandAllocator->Reset(); // Prepare command allocator for more work !
            commandList->Reset(commandAllocator.Get(), nullptr); // Opens command list for closing.

            //Pass 2: Clone Affected Pages (RCU copy)
            for (GeometryPage* oldPage : affectedPages) {
                auto clonedPage = CreateNewPage(oldPage->containerMemoryId);

                // Following 2 commands are for copying the GPU VRAM data.
                commandList->CopyResource(clonedPage->buffer.Get(), oldPage->buffer.Get());
                // TODO: Copy with defragmentation using metadata.
                commandList->CopyResource(clonedPage->indirectBuffer.Get(), oldPage->indirectBuffer.Get());

                clonedPage->objects = oldPage->objects; //These commands are CPU side metadata copy.
                clonedPage->vertexHead = oldPage->vertexHead;
                clonedPage->indexTail = oldPage->indexTail;
                clonedPage->objectCount = oldPage->objectCount;
                clonedPage->version = oldPage->version + 1;
                clonedPage->holeBytes = oldPage->holeBytes;

                clonedPages[oldPage] = std::move(clonedPage);
            }

            for (auto& [oldRaw, clone] : clonedPages) {
                for (uint32_t i = 0; i < clone->objects.size(); ++i) {
                    auto& obj = clone->objects[i];
                    if (obj.isDeleted) continue;

                    auto it = objectLocation.find(obj.objectID);
                    if (it != objectLocation.end() && it->second.page == oldRaw) {
                        it->second.page = clone.get();
                        it->second.slot = i;
                    }
                }
            }

            std::unordered_map<uint64_t, GeometryPage*> addTargetPages;
            if (!affectedPages.empty()) { // If no pages cloned, we don't need these GPU operations.
                ThrowIfFailed(commandList->Close());
                ID3D12CommandList* lists[] = { commandList.Get() };
                gpu.copyCommandQueue->ExecuteCommandLists(1, lists);
                fenceValue = gpu.copyFenceValue.fetch_add(1);
                gpu.copyCommandQueue->Signal(gpu.copyFence.Get(), fenceValue);

                if (gpu.copyFence->GetCompletedValue() < fenceValue) {
                    // TODO: Can we delay this wait until just before we need to access the cloned pages ? 
                    // This would allow some CPU-GPU parallelism.
                    gpu.copyFence->SetEventOnCompletion(fenceValue, gpu.copyFenceEvent);
                    WaitForSingleObject(gpu.copyFenceEvent, INFINITE);
                }

                for (const auto& [containerMemoryId, candidate] : bestAppendCandidates) {
                    auto cloneIt = clonedPages.find(candidate);
                    addTargetPages[containerMemoryId] = cloneIt != clonedPages.end()
                        ? cloneIt->second.get()
                        : candidate;
                }

                // Re-open the command list for Pass-3 GPU work (geometry uploads for ADD/MODIFY-grow cases).
                commandAllocator->Reset();
                commandList->Reset(commandAllocator.Get(), nullptr);
            }

            //std::wcout << "activePages: " << storage.activePages.size() << 
            //    ", clonedPages:" << clonedPages.size() << std::endl;

            // Pass 3 — Apply every command in the batch to the (already-cloned) pages.
            
            // Staging uploads produced during this pass. Kept alive until after ExecuteCommandLists + fence-wait.
            std::vector<ComPtr<ID3D12Resource>> pass3Uploads;

            // Common lambda: write vertex+index data into a page Used by both ADD (to last/new page) and MODIFY-grow paths.
            // Records CopyBufferRegion into the open commandList.Returns the filled-in placement record; caller appends it.
            auto RecordGeometryUpload = [&](GeometryPage* dstPage, const GeometryData& geo, uint32_t matrixIndex)
                -> GeometryPlacementRecordInPage {
                const uint32_t vertexBytes = static_cast<uint32_t>(geo.vertices.size() * sizeof(Vertex));
                const uint32_t indexBytes = static_cast<uint32_t>(geo.indices.size() * sizeof(uint16_t));

                const uint32_t vOffset = GeometryPage::AlignUp(dstPage->vertexHead, 16);
                const uint32_t iOffset = GeometryPage::AlignDown(dstPage->indexTail - indexBytes, 4);

                // CPU-side staging upload buffer (vertex + index packed)
                ComPtr<ID3D12Resource> upload;
                CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
                auto uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(vertexBytes + indexBytes);

                ThrowIfFailed(gpu.device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)));

                uint8_t* mapped = nullptr;
                CD3DX12_RANGE readRange(0, 0);
                upload->Map(0, &readRange, reinterpret_cast<void**>(&mapped));
                memcpy(mapped, geo.vertices.data(), vertexBytes);
                memcpy(mapped + vertexBytes, geo.indices.data(), indexBytes);
                upload->Unmap(0, nullptr);

                // Record GPU copies (no Execute yet — batched at end of Pass 3)
                commandList->CopyBufferRegion(dstPage->buffer.Get(), vOffset, upload.Get(), 0, vertexBytes);
                commandList->CopyBufferRegion(dstPage->buffer.Get(), iOffset, upload.Get(), vertexBytes, indexBytes);
                pass3Uploads.push_back(std::move(upload)); // Keep upload buffer alive until the fence fires

                // Build and return the placement record (caller updates page state)
                GeometryPlacementRecordInPage rec{};
                rec.objectID = geo.id;
                rec.vertexByteOffset = vOffset;
                rec.vertexSize = vertexBytes;
                rec.indexByteOffset = iOffset;
                rec.indexSize = indexBytes;
                rec.indexCount = static_cast<uint32_t>(geo.indices.size());
                rec.matrixIndex = matrixIndex;

                // Local-space AABB. Consumed by GPU picking / selection re-centering (Selection3D).
                if (!geo.vertices.empty()) {
                    float minX = FLT_MAX, minY = FLT_MAX, minZ = FLT_MAX;
                    float maxX = -FLT_MAX, maxY = -FLT_MAX, maxZ = -FLT_MAX;
                    for (const Vertex& v : geo.vertices) {
                        minX = (std::min)(minX, v.position.x); maxX = (std::max)(maxX, v.position.x);
                        minY = (std::min)(minY, v.position.y); maxY = (std::max)(maxY, v.position.y);
                        minZ = (std::min)(minZ, v.position.z); maxZ = (std::max)(maxZ, v.position.z);
                    }
                    rec.minX = minX; rec.minY = minY; rec.minZ = minZ;
                    rec.maxX = maxX; rec.maxY = maxY; rec.maxZ = maxZ;
                }
                return rec;
                };

            uint32_t matrixIndex; // Allocate a matrix slot (new object)
            XMMATRIX worldMat;
            std::unordered_map<uint64_t, GeometryPage*> newestPagesByContainer;

            auto AcquireAppendPage = [&](uint64_t containerMemoryId,
                uint32_t incomingVertexBytes, uint32_t incomingIndexBytes) -> GeometryPage* {
                GeometryPage*& targetPage = addTargetPages[containerMemoryId];
                if (targetPage && !targetPage->IsFull(incomingVertexBytes, incomingIndexBytes)) {
                    return targetPage;
                }

                GeometryPage*& newestPage = newestPagesByContainer[containerMemoryId];
                if (!newestPage || newestPage->IsFull(incomingVertexBytes, incomingIndexBytes)) {
                    newPages.push_back(CreateNewPage(containerMemoryId));
                    newestPage = newPages.back().get();
                }
                targetPage = newestPage;
                return targetPage;
            };

            for (auto& cmd : deduplicatedBatch) { // Iterate over batch
                if (cmd.tabID != tabID) continue;
                // Find the targe tab. Our static array of tabs is thread-safe for reading.

                uint32_t vertexBytes = 0, indexBytes = 0, newVertexBytes = 0, newIndexBytes = 0;

                decltype(objectLocation)::iterator locIt;
                decltype(clonedPages)::iterator cloneIt;

                GeometryPage* workPage = nullptr;
                GeometryPage* oldPage = nullptr;
                GeometryPage* modifyTargetPage = nullptr;
                const GeometryData* geo = nullptr;
                GeometryPlacementRecordInPage* oldRec = nullptr;
                GeometryPlacementRecordInPage rec;
                uint32_t slotIndex = 0;
                uint64_t targetContainerMemoryId = cmd.containerMemoryId;
                matrixIndex = 0;

                // We mandatorily check if ID still exist even if the command is ADD as a safety measure.
                // This is also necessary when REMOVE + ADD command is received in same batch and deduped.
                // TODO: Future, add a fast path ADDINITIAL in future for quick initial loading at startup.
                switch (cmd.type)// Process Command
                {
                case CommandToCopyThreadType::ADD:
                handle_add:
                {
                    //std::wcout << "Adding New object ID: " << cmd.id << std::endl;
                    if (!cmd.geometry.has_value()) break;
                    if (objectLocation.find(cmd.id) != objectLocation.end()) {goto handle_modify;}

                    geo = &(cmd.geometry.value());
                    vertexBytes = static_cast<uint32_t>(geo->vertices.size() * sizeof(Vertex));
                    indexBytes = static_cast<uint32_t>(geo->indices.size() * sizeof(uint16_t));
                    if (vertexBytes == 0 || indexBytes == 0) {
                        std::wcout << "Warning: Skipping upload of empty geometry ID " << cmd.id << std::endl;
                        break; // Exit this case, process next command
                    }

                    matrixIndex = AllocateMatrixSlot(tabRes, storage); // Doubles the table when full.

                    // Copy transposed world matrix to upload buffer
                    worldMat = XMLoadFloat4x4(&geo->worldMatrix);
                    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(
                        tabRes.pWorldMatrixDataBegin + matrixIndex * sizeof(XMFLOAT4X4)),
                        XMMatrixTranspose(worldMat));

                    GeometryPage* addTargetPage =
                        AcquireAppendPage(targetContainerMemoryId, vertexBytes, indexBytes);

                    // Record the geometry upload into commandList
                    rec = RecordGeometryUpload(addTargetPage, *geo, matrixIndex);

                    // Update page CPU state
                    addTargetPage->objects.push_back(rec);
                    addTargetPage->vertexHead = rec.vertexByteOffset + rec.vertexSize;
                    addTargetPage->indexTail = rec.indexByteOffset;
                    addTargetPage->objectCount++;

                    // Update the copy-thread's private location map
                    slotIndex = static_cast<uint32_t>(addTargetPage->objects.size() - 1);
                    objectLocation[cmd.id] = { addTargetPage, slotIndex };

                    //std::wcout << "Added New object ID: " << cmd.id << std::endl;
                    break;
                }

                case CommandToCopyThreadType::MODIFY:
                handle_modify: // This is GOTO jump from ADD thread, if the ID already existed.
                    // TODO: Latter we will improve to use existing pages if it fits in place.
                    if (!cmd.geometry.has_value()) break;

                    locIt = objectLocation.find(cmd.id);
                    if (locIt == objectLocation.end()) { goto handle_add; }// treat as ADD

                    geo = &(cmd.geometry.value());

                    newVertexBytes = static_cast<uint32_t>(geo->vertices.size() * sizeof(Vertex));
                    newIndexBytes = static_cast<uint32_t>(geo->indices.size() * sizeof(uint16_t));

                    if (newVertexBytes == 0 || newIndexBytes == 0) break;

                    // Object exists — work on its owning cloned page
                    oldPage = locIt->second.page;
                    slotIndex = locIt->second.slot;
                    if (targetContainerMemoryId == 0) {
                        targetContainerMemoryId = oldPage->containerMemoryId;
                    }

                    // Resolve which mutable page we are working with: It will be in clonedPages (was in affectedPages 
                    // from Pass 1), because Pass 1 already included pages from existing objectLocation.

                    cloneIt = clonedPages.find(oldPage);
                    if (cloneIt != clonedPages.end()) workPage = cloneIt->second.get();
                    else workPage = oldPage;   // page created this batch

                    oldRec = &(workPage->objects[slotIndex]);
                    oldRec->isDeleted = true;

                    workPage->holeBytes += oldRec->vertexSize + oldRec->indexSize;
                    workPage->objectCount--;

                    modifyTargetPage =
                        AcquireAppendPage(targetContainerMemoryId, newVertexBytes, newIndexBytes);

                    // Allocate a fresh matrix slot for the relocated geometry
                    matrixIndex = AllocateMatrixSlot(tabRes, storage); // Doubles the table when full.
                    // Free the old matrix slot
                    worldMat = XMLoadFloat4x4(&geo->worldMatrix);
                    tabRes.freeMatrixSlots.push_back(oldRec->matrixIndex);
                    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(tabRes.pWorldMatrixDataBegin +
                        matrixIndex * sizeof(XMFLOAT4X4)), XMMatrixTranspose(worldMat));

                    rec = RecordGeometryUpload(modifyTargetPage, *geo, matrixIndex);
                    modifyTargetPage->objects.push_back(rec);
                    modifyTargetPage->vertexHead = rec.vertexByteOffset + rec.vertexSize;
                    modifyTargetPage->indexTail = rec.indexByteOffset;
                    modifyTargetPage->objectCount++;

                    objectLocation[cmd.id] = {
                        modifyTargetPage, static_cast<uint32_t>(modifyTargetPage->objects.size() - 1) };
                    break;

                case CommandToCopyThreadType::REMOVE:
                
                    locIt = objectLocation.find(cmd.id);
                    if (locIt == objectLocation.end()) break; // not on GPU, nothing to do

                    oldPage = locIt->second.page;
                    slotIndex = locIt->second.slot;

                    // Resolve mutable clone
                    cloneIt = clonedPages.find(oldPage);
                    if (cloneIt != clonedPages.end()) workPage = cloneIt->second.get();
                    else workPage = oldPage;   // page created this batch

                    // Must be a pointer into the page (like the MODIFY path above): a by-value copy
                    // here would mark only the copy deleted and the object would keep drawing.
                    oldRec = &(workPage->objects[slotIndex]);

                    // Soft-delete: mark the slot; IndirectBuffer rebuild will skip it
                    oldRec->isDeleted = true;
                    workPage->holeBytes += oldRec->vertexSize + oldRec->indexSize;
                    workPage->objectCount--;
                    tabRes.freeMatrixSlots.push_back(oldRec->matrixIndex); // Free the matrix slot for reuse
                    objectLocation.erase(locIt);// Remove from local bookkeeping
                    break;
                

                default: break;
                } // End of switch (cmd.type)// Process Command
            } // end for (batch)

            // Single GPU Execute for all Pass-3 geometry uploads
            if (!pass3Uploads.empty()) {
                ThrowIfFailed(commandList->Close());
                {
                    ID3D12CommandList* lists[] = { commandList.Get() };
                    gpu.copyCommandQueue->ExecuteCommandLists(1, lists);
                }
                fenceValue = gpu.copyFenceValue.fetch_add(1);
                gpu.copyCommandQueue->Signal(gpu.copyFence.Get(), fenceValue);
                if (gpu.copyFence->GetCompletedValue() < fenceValue) {
                    gpu.copyFence->SetEventOnCompletion(fenceValue, gpu.copyFenceEvent);
                    WaitForSingleObject(gpu.copyFenceEvent, INFINITE);
                }
                pass3Uploads.clear();// Upload staging buffers are now safe to release

                commandAllocator->Reset(); // Prepare for next stage.
                commandList->Reset(commandAllocator.Get(), nullptr);
            }

            // Rebuild Indirect Buffers (outside the per-command loop) Runs once per modified or new page, 
            // after all commands are applied. Only live objects (isDeleted == false) are emitted.
            // We rebuild into CPU staging, then do one more command record
            // + execute (or we can reuse the same allocator/list with a Reset).
            
            std::vector<ComPtr<ID3D12Resource>> indirectUploads;

            // Helper that rebuilds a single page's indirect buffer
            auto RebuildIndirectBuffer = [&](GeometryPage* page) {
                std::vector<IndirectCommand> commands;
                commands.reserve(page->objects.size());

                for (const auto& obj : page->objects) {
                    if (obj.isDeleted) continue; // skip soft-deleted slots

                    IndirectCommand ic{};
                    ic.matrixIndex = obj.matrixIndex;
                    ic.drawArguments.IndexCountPerInstance = obj.indexCount;
                    ic.drawArguments.InstanceCount = 1;
                    ic.drawArguments.StartIndexLocation = (obj.indexByteOffset - page->indexTail) / sizeof(uint16_t);
                    ic.drawArguments.BaseVertexLocation = obj.vertexByteOffset / sizeof(Vertex);
                    ic.drawArguments.StartInstanceLocation = 0;

                    commands.push_back(ic);
                }

                page->indirectCount = static_cast<uint32_t>(commands.size());
                if (commands.empty()) return; // nothing to upload

                ComPtr<ID3D12Resource> indirectUpload;
                CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
                auto iuDesc = CD3DX12_RESOURCE_DESC::Buffer(commands.size() * sizeof(IndirectCommand));

                ThrowIfFailed(gpu.device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &iuDesc,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&indirectUpload)));

                uint8_t* mapped = nullptr;
                CD3DX12_RANGE readRange(0, 0);
                indirectUpload->Map(0, &readRange, reinterpret_cast<void**>(&mapped));
                memcpy(mapped, commands.data(), commands.size() * sizeof(IndirectCommand));
                indirectUpload->Unmap(0, nullptr);

                commandList->CopyBufferRegion(page->indirectBuffer.Get(), 0, indirectUpload.Get(), 0,
                    commands.size() * sizeof(IndirectCommand));

                indirectUploads.push_back(std::move(indirectUpload));
                };

            // Rebuild for every cloned (modified) page
            for (auto& [oldRaw, clonedPage] : clonedPages) RebuildIndirectBuffer(clonedPage.get());
            // Rebuild for every brand-new page
            for (auto& page : newPages) RebuildIndirectBuffer(page.get());

            if (clonedPages.empty() && newPages.empty()) continue; //Skip everything if no commands?

            // Sync Copy queue.
            ThrowIfFailed(commandList->Close());
            ID3D12CommandList* lists[] = { commandList.Get() };
            gpu.copyCommandQueue->ExecuteCommandLists(1, lists);
            fenceValue = gpu.copyFenceValue.fetch_add(1);
            gpu.copyCommandQueue->Signal(gpu.copyFence.Get(), fenceValue);
            if (gpu.copyFence->GetCompletedValue() < fenceValue) {
                gpu.copyFence->SetEventOnCompletion(fenceValue, gpu.copyFenceEvent);
                WaitForSingleObject(gpu.copyFenceEvent, INFINITE);
            }

            indirectUploads.clear(); // staging buffers safe to free

            // Final RCU Publish (Single Atomic Operation). Gather per-tab publish work. Since commands can span multiple
            // tabs, group replacements and appends by their TabGeometryStorage.
            // Because clonedPages and newPages can belong to different tabs we need to route them to the correct storage.
            // The clonedPages map already contains the raw oldPage ptr which we can match
            // back to its owning storage via the batch's tabID.
            std::vector<GeometryPage*> oldPages;
            std::vector<std::unique_ptr<GeometryPage>> replacements;
            for (auto& [oldRaw, clone] : clonedPages)
            {
                oldPages.push_back(oldRaw);
                replacements.push_back(std::move(clone));
            }

            PublishPages(storage, oldPages, std::move(replacements), std::move(newPages));
            // End of Pass 3 + Publish 
        }
}
