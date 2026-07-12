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
    gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&page->buffer));
    auto indirectDesc = CD3DX12_RESOURCE_DESC::Buffer(65536 * sizeof(IndirectCommand));
    gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &indirectDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&page->indirectBuffer));

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
    commandList->SetGraphicsRootShaderResourceView(1, tabRes.worldMatrixBuffer->GetGPUVirtualAddress());

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
