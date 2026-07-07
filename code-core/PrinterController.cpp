// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

#include "PrinterController.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "MemoryManagerGPU-DirectX12.h"
#include "MemoryManagerGPU2D-DirectX12.h"
#include "UserInterface-DirectX12.h"
#include "विश्वकर्मा.h"

#include <commdlg.h> // PrintDlgW. Comdlg32.lib is already linked by the project.

extern शंकर gpu;
extern std::atomic<uint64_t> atlasFence;

namespace {

// Raster resolution of the offscreen print render target. printer.md: 300 or 600 DPI.
// The printer driver upscales from here to the device DPI via StretchDIBits.
constexpr int kPrintDPI = 300;
// Safety cap so an A0 style page can not exhaust VRAM. D3D12 texture limit is 16384 anyway.
constexpr uint32_t kMaxPrintDimensionPx = 8192;

HWND OwnerWindowHandle() {
    uint16_t* windowList = publishedWindowIndexes.load(std::memory_order_acquire);
    uint16_t windowCount = publishedWindowCount.load(std::memory_order_acquire);
    if (windowCount == 0) return nullptr;
    return allWindows[windowList[0]].hWnd;
}

DATASETTAB* ActiveTabForPrint() {
    uint16_t* windowList = publishedWindowIndexes.load(std::memory_order_acquire);
    uint16_t windowCount = publishedWindowCount.load(std::memory_order_acquire);
    for (uint16_t i = 0; i < windowCount; ++i) {
        int tabID = allWindows[windowList[i]].activeTabIndex;
        if (tabID >= 0 && tabID < MV_MAX_TABS) return &allTabs[tabID];
    }

    uint16_t* tabList = publishedTabIndexes.load(std::memory_order_acquire);
    uint16_t tabCount = publishedTabCount.load(std::memory_order_acquire);
    if (tabCount > 0) return &allTabs[tabList[0]];
    return nullptr;
}

// Local copies of the GPU resources of one Page2D container. Holding the ComPtrs keeps
// the underlying buffers alive even if the copy thread republishes/retires the snapshot
// while our print command list is still executing on our private queue.
struct Print2DPage {
    ComPtr<ID3D12Resource> lineBuffer;
    ComPtr<ID3D12Resource> lineIndirectBuffer;
    uint32_t lineCount = 0;
    ComPtr<ID3D12Resource> curveBuffer;
    ComPtr<ID3D12Resource> curveIndirectBuffer;
    uint32_t curveCount = 0;
    ComPtr<ID3D12Resource> textVertexBuffer;
    ComPtr<ID3D12Resource> textIndexBuffer;
    uint32_t textVertexCount = 0;
    uint32_t textIndexCount = 0;
};

// Same idea for the 3D geometry pages of one Scene3D container.
struct Print3DPage {
    ComPtr<ID3D12Resource> buffer;
    ComPtr<ID3D12Resource> indirectBuffer;
    uint32_t indirectCount = 0;
    uint32_t vertexHead = 0;
    uint32_t indexTail = 0;
    uint32_t pageSize = 0;
};

std::vector<Print2DPage> Collect2DPages(TabCad2DStorage& storage, uint64_t containerMemoryId) {
    std::vector<Print2DPage> result;
    Cad2DPageSnapshot* snapshot = storage.activeSnapshot.load(std::memory_order_acquire);
    if (!snapshot) return result;

    for (Cad2DPageGPU* page : snapshot->pages) {
        if (!page || page->containerMemoryId != containerMemoryId) continue;
        Print2DPage copy;
        copy.lineBuffer = page->lineBuffer;
        copy.lineIndirectBuffer = page->lineIndirectBuffer;
        copy.lineCount = page->lineCount;
        copy.curveBuffer = page->curveBuffer;
        copy.curveIndirectBuffer = page->curveIndirectBuffer;
        copy.curveCount = page->curveCount;
        copy.textVertexBuffer = page->textVertexBuffer;
        copy.textIndexBuffer = page->textIndexBuffer;
        copy.textVertexCount = page->textVertexCount;
        copy.textIndexCount = page->textIndexCount;
        result.push_back(std::move(copy));
    }
    return result;
}

std::vector<Print3DPage> Collect3DPages(TabGeometryStorage& storage, uint64_t containerMemoryId) {
    std::vector<Print3DPage> result;
    GeometryPageSnapshot* snapshot = storage.activeSnapshot.load(std::memory_order_acquire);
    if (!snapshot) return result;

    for (GeometryPage* page : snapshot->pages) {
        if (!page || !page->published.load(std::memory_order_acquire)) continue;
        if (page->containerMemoryId != containerMemoryId) continue;
        if (page->indirectCount == 0 || page->vertexHead == 0 || page->indexTail == page->pageSize) continue;
        Print3DPage copy;
        copy.buffer = page->buffer;
        copy.indirectBuffer = page->indirectBuffer;
        copy.indirectCount = page->indirectCount;
        copy.vertexHead = page->vertexHead;
        copy.indexTail = page->indexTail;
        copy.pageSize = page->pageSize;
        result.push_back(std::move(copy));
    }
    return result;
}

// Records draw calls for a Page2D drawing. Mirror of RenderCad2DPage, but with our own
// render target, viewport and view-constant buffer so the on-screen renderer is untouched.
void Record2DDraws(ID3D12GraphicsCommandList* cmd, DATASETTAB& tab,
    const std::vector<Print2DPage>& pages, ID3D12Resource* viewConstantBuffer,
    uint8_t* pViewConstantData, uint32_t widthPx, uint32_t heightPx) {
    TabCad2DStorage& storage = *tab.cad2d;

    Cad2DViewConstants constants{};
    constants.viewCenterCU = {
        static_cast<float>(storage.view.centerXCU.load(std::memory_order_acquire)),
        static_cast<float>(storage.view.centerYCU.load(std::memory_order_acquire))
    };

    // Print exactly the region currently visible on screen: scale the on-screen zoom so
    // the same horizontal CU extent fills the printed page width at print DPI.
    const float screenZoom =
        (std::max)(storage.view.zoomPixelsPerCU.load(std::memory_order_acquire),
            kCad2DZoomMinPixelsPerCU);
    int viewportWidth = 0, viewportHeight = 0, viewportTop = 0;
    if (GetVisibleSceneViewportForTab(tab, viewportWidth, viewportHeight, viewportTop) &&
        viewportWidth > 0) {
        constants.zoomPixelsPerCU = screenZoom * static_cast<float>(widthPx) /
            static_cast<float>(viewportWidth);
    } else {
        constants.zoomPixelsPerCU = screenZoom * static_cast<float>(kPrintDPI) / 96.0f;
    }
    constants.dpiY = static_cast<float>(kPrintDPI);
    constants.viewportSizePx = { static_cast<float>(widthPx), static_cast<float>(heightPx) };
    // Keep hairlines visible: 1 screen pixel at ~96 DPI equals ~3 pixels at 300 DPI.
    constants.minLineWeightPx = (std::max)(1.0f, static_cast<float>(kPrintDPI) / 96.0f);
    memcpy(pViewConstantData, &constants, sizeof(constants));

    if (storage.dx.lineRootSignature && storage.dx.linePSO && storage.dx.lineCommandSignature) {
        cmd->SetGraphicsRootSignature(storage.dx.lineRootSignature.Get());
        cmd->SetPipelineState(storage.dx.linePSO.Get());
        cmd->SetGraphicsRootConstantBufferView(0, viewConstantBuffer->GetGPUVirtualAddress());
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        for (const Print2DPage& page : pages) {
            if (page.lineCount == 0 || !page.lineBuffer || !page.lineIndirectBuffer) continue;
            cmd->SetGraphicsRootShaderResourceView(1, page.lineBuffer->GetGPUVirtualAddress());
            cmd->ExecuteIndirect(storage.dx.lineCommandSignature.Get(), 1,
                page.lineIndirectBuffer.Get(), 0, nullptr, 0);
        }
    }

    if (storage.dx.curveRootSignature && storage.dx.curvePSO && storage.dx.curveCommandSignature) {
        cmd->SetGraphicsRootSignature(storage.dx.curveRootSignature.Get());
        cmd->SetPipelineState(storage.dx.curvePSO.Get());
        cmd->SetGraphicsRootConstantBufferView(0, viewConstantBuffer->GetGPUVirtualAddress());
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        for (const Print2DPage& page : pages) {
            if (page.curveCount == 0 || !page.curveBuffer || !page.curveIndirectBuffer) continue;
            cmd->SetGraphicsRootShaderResourceView(1, page.curveBuffer->GetGPUVirtualAddress());
            cmd->ExecuteIndirect(storage.dx.curveCommandSignature.Get(), 1,
                page.curveIndirectBuffer.Get(), 0, nullptr, 0);
        }
    }

    // Text: same MSDF atlas as on screen, rendered as high resolution raster (printer.md).
    const uint64_t atlasFenceValue = atlasFence.load(std::memory_order_acquire);
    const bool textAtlasReady = atlasFenceValue != 0 && gpu.copyFence &&
        gpu.copyFence->GetCompletedValue() >= atlasFenceValue;
    if (!textAtlasReady || !storage.dx.textRootSignature || !storage.dx.textPSO) return;
    if (!gpu.uiResources.srvHeap || !gpu.uiResources.samplerHeap) return;

    cmd->SetGraphicsRootSignature(storage.dx.textRootSignature.Get());
    cmd->SetPipelineState(storage.dx.textPSO.Get());
    ID3D12DescriptorHeap* heaps[] = { gpu.uiResources.srvHeap.Get(), gpu.uiResources.samplerHeap.Get() };
    cmd->SetDescriptorHeaps(_countof(heaps), heaps);
    cmd->SetGraphicsRootConstantBufferView(0, viewConstantBuffer->GetGPUVirtualAddress());
    cmd->SetGraphicsRootDescriptorTable(1, gpu.uiResources.srvHeap->GetGPUDescriptorHandleForHeapStart());
    cmd->SetGraphicsRootDescriptorTable(2, gpu.uiResources.samplerHeap->GetGPUDescriptorHandleForHeapStart());

    for (const Print2DPage& page : pages) {
        if (page.textIndexCount == 0 || !page.textVertexBuffer || !page.textIndexBuffer) continue;

        D3D12_VERTEX_BUFFER_VIEW vbv{};
        vbv.BufferLocation = page.textVertexBuffer->GetGPUVirtualAddress();
        vbv.SizeInBytes = page.textVertexCount * sizeof(Cad2DTextVertex);
        vbv.StrideInBytes = sizeof(Cad2DTextVertex);

        D3D12_INDEX_BUFFER_VIEW ibv{};
        ibv.BufferLocation = page.textIndexBuffer->GetGPUVirtualAddress();
        ibv.SizeInBytes = page.textIndexCount * sizeof(uint32_t);
        ibv.Format = DXGI_FORMAT_R32_UINT;

        cmd->IASetVertexBuffers(0, 1, &vbv);
        cmd->IASetIndexBuffer(&ibv);
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->DrawIndexedInstanced(page.textIndexCount, 1, 0, 0, 0);
    }
}

// Records draw calls for an image of the 3D scene. Mirror of शंकर::PopulateCommandList,
// but with our own constant buffer so the per-window one is untouched.
void Record3DDraws(ID3D12GraphicsCommandList* cmd, DATASETTAB& tab,
    const std::vector<Print3DPage>& pages, ID3D12Resource* constantBuffer,
    uint8_t* pConstantData, uint32_t widthPx, uint32_t heightPx) {
    DX12ResourcesPerTab& tabRes = tab.dx;
    if (!tabRes.rootSignature || !tabRes.pipelineState || !tabRes.commandSignature) return;
    if (!tabRes.worldMatrixBuffer) return;

    const CameraState camera = tab.camera;
    XMVECTOR eyePosition = XMLoadFloat3(&camera.position);
    XMVECTOR focusPoint = XMLoadFloat3(&camera.target);
    XMVECTOR upDirection = XMLoadFloat3(&camera.up);
    DirectX::XMMATRIX viewMatrix = DirectX::XMMatrixLookAtLH(eyePosition, focusPoint, upDirection);
    const float aspectRatio = static_cast<float>(widthPx) / static_cast<float>(heightPx);
    XMMATRIX projectionMatrix = XMMatrixPerspectiveFovLH(
        camera.fov, aspectRatio, camera.nearZ, camera.farZ);

    ConstantBuffer constantBufferData;
    XMStoreFloat4x4(&constantBufferData.viewProj, XMMatrixTranspose(viewMatrix * projectionMatrix));
    memcpy(pConstantData, &constantBufferData, sizeof(constantBufferData));

    cmd->SetGraphicsRootSignature(tabRes.rootSignature.Get());
    cmd->SetPipelineState(tabRes.pipelineState.Get());
    cmd->SetGraphicsRootConstantBufferView(0, constantBuffer->GetGPUVirtualAddress());
    cmd->SetGraphicsRootShaderResourceView(1, tabRes.worldMatrixBuffer->GetGPUVirtualAddress());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    for (const Print3DPage& page : pages) {
        D3D12_VERTEX_BUFFER_VIEW vbv{};
        vbv.BufferLocation = page.buffer->GetGPUVirtualAddress();
        vbv.SizeInBytes = page.vertexHead;
        vbv.StrideInBytes = sizeof(Vertex);

        D3D12_INDEX_BUFFER_VIEW ibv{};
        ibv.BufferLocation = page.buffer->GetGPUVirtualAddress() + page.indexTail;
        ibv.SizeInBytes = page.pageSize - page.indexTail;
        ibv.Format = DXGI_FORMAT_R16_UINT;

        cmd->IASetVertexBuffers(0, 1, &vbv);
        cmd->IASetIndexBuffer(&ibv);
        cmd->ExecuteIndirect(tabRes.commandSignature.Get(), page.indirectCount,
            page.indirectBuffer.Get(), 0, nullptr, 0);
    }
}

// Renders the active container offscreen at print DPI and reads the pixels back.
// Returns tightly packed top-down BGRA rows (GDI DIB layout), empty on failure.
std::vector<uint8_t> RenderForPrint(DATASETTAB& tab, bool isPage2D, uint64_t containerMemoryId,
    uint32_t widthPx, uint32_t heightPx) {
    std::vector<uint8_t> bgraPixels;

    // Private queue/list/fence: printing must not disturb the per-monitor render queues.
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ThrowIfFailed(gpu.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)));

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> cmd;
    ThrowIfFailed(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&allocator)));
    ThrowIfFailed(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        allocator.Get(), nullptr, IID_PPV_ARGS(&cmd)));

    // Render target at print resolution. White paper background (printer.md color policy).
    ComPtr<ID3D12Resource> renderTexture;
    const D3D12_CLEAR_VALUE whiteClear{ .Format = gpu.rttFormat, .Color = { 1.0f, 1.0f, 1.0f, 1.0f } };
    auto texDesc = CD3DX12_RESOURCE_DESC::Tex2D(gpu.rttFormat, widthPx, heightPx,
        1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(gpu.device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE,
        &texDesc, D3D12_RESOURCE_STATE_RENDER_TARGET, &whiteClear, IID_PPV_ARGS(&renderTexture)));

    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = 1;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ThrowIfFailed(gpu.device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap)));
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(rtvHeap->GetCPUDescriptorHandleForHeapStart());
    gpu.device->CreateRenderTargetView(renderTexture.Get(), nullptr, rtvHandle);

    // Depth buffer, only needed for the 3D scene pipeline.
    ComPtr<ID3D12Resource> depthBuffer;
    ComPtr<ID3D12DescriptorHeap> dsvHeap;
    if (!isPage2D) {
        D3D12_CLEAR_VALUE depthClear = {};
        depthClear.Format = DXGI_FORMAT_D32_FLOAT;
        depthClear.DepthStencil.Depth = 1.0f;
        auto depthDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_D32_FLOAT, widthPx, heightPx,
            1, 0, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        ThrowIfFailed(gpu.device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE,
            &depthDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear, IID_PPV_ARGS(&depthBuffer)));

        D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
        dsvHeapDesc.NumDescriptors = 1;
        dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        ThrowIfFailed(gpu.device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&dsvHeap)));
        gpu.device->CreateDepthStencilView(depthBuffer.Get(), nullptr,
            dsvHeap->GetCPUDescriptorHandleForHeapStart());
    }

    // Private 256 byte constant buffer, persistently mapped for this one job.
    ComPtr<ID3D12Resource> constantBuffer;
    uint8_t* pConstantData = nullptr;
    CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
    auto cbDesc = CD3DX12_RESOURCE_DESC::Buffer(256);
    ThrowIfFailed(gpu.device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE,
        &cbDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&constantBuffer)));
    CD3DX12_RANGE readRange(0, 0);
    ThrowIfFailed(constantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&pConstantData)));

    // Readback buffer sized from the copyable footprint (row pitch is 256 byte aligned).
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 readbackSize = 0;
    gpu.device->GetCopyableFootprints(&texDesc, 0, 1, 0, &footprint, nullptr, nullptr, &readbackSize);

    ComPtr<ID3D12Resource> readbackBuffer;
    CD3DX12_HEAP_PROPERTIES readbackHeap(D3D12_HEAP_TYPE_READBACK);
    auto readbackDesc = CD3DX12_RESOURCE_DESC::Buffer(readbackSize);
    ThrowIfFailed(gpu.device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE,
        &readbackDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readbackBuffer)));

    // Record: clear white, draw content, copy to readback buffer.
    CD3DX12_VIEWPORT viewport(0.0f, 0.0f, static_cast<float>(widthPx), static_cast<float>(heightPx));
    CD3DX12_RECT scissor(0, 0, static_cast<LONG>(widthPx), static_cast<LONG>(heightPx));
    cmd->RSSetViewports(1, &viewport);
    cmd->RSSetScissorRects(1, &scissor);

    if (!isPage2D && dsvHeap) {
        CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle(dsvHeap->GetCPUDescriptorHandleForHeapStart());
        cmd->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);
        cmd->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    } else {
        cmd->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
    }
    cmd->ClearRenderTargetView(rtvHandle, whiteClear.Color, 0, nullptr);

    if (isPage2D) {
        std::vector<Print2DPage> pages = Collect2DPages(*tab.cad2d, containerMemoryId);
        Record2DDraws(cmd.Get(), tab, pages, constantBuffer.Get(), pConstantData, widthPx, heightPx);
    } else {
        std::vector<Print3DPage> pages = Collect3DPages(tab.geometry, containerMemoryId);
        Record3DDraws(cmd.Get(), tab, pages, constantBuffer.Get(), pConstantData, widthPx, heightPx);
    }

    auto toCopySource = CD3DX12_RESOURCE_BARRIER::Transition(renderTexture.Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->ResourceBarrier(1, &toCopySource);

    CD3DX12_TEXTURE_COPY_LOCATION copySource(renderTexture.Get(), 0);
    CD3DX12_TEXTURE_COPY_LOCATION copyDest(readbackBuffer.Get(), footprint);
    cmd->CopyTextureRegion(&copyDest, 0, 0, 0, &copySource, nullptr);

    ThrowIfFailed(cmd->Close());
    ID3D12CommandList* lists[] = { cmd.Get() };
    queue->ExecuteCommandLists(1, lists);

    // Synchronous wait. A print job is a rare user action; blocking briefly is fine.
    ComPtr<ID3D12Fence> fence;
    ThrowIfFailed(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    HANDLE fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!fenceEvent) return bgraPixels;
    ThrowIfFailed(queue->Signal(fence.Get(), 1));
    if (fence->GetCompletedValue() < 1) {
        ThrowIfFailed(fence->SetEventOnCompletion(1, fenceEvent));
        WaitForSingleObject(fenceEvent, INFINITE);
    }
    CloseHandle(fenceEvent);

    // Read back and convert RGBA (render target) to tightly packed BGRA (GDI DIB).
    uint8_t* mapped = nullptr;
    CD3DX12_RANGE readbackRange(0, static_cast<SIZE_T>(readbackSize));
    ThrowIfFailed(readbackBuffer->Map(0, &readbackRange, reinterpret_cast<void**>(&mapped)));

    bgraPixels.resize(static_cast<size_t>(widthPx) * heightPx * 4u);
    const uint32_t rowPitch = footprint.Footprint.RowPitch;
    for (uint32_t y = 0; y < heightPx; ++y) {
        const uint8_t* src = mapped + static_cast<size_t>(y) * rowPitch;
        uint8_t* dst = bgraPixels.data() + static_cast<size_t>(y) * widthPx * 4u;
        for (uint32_t x = 0; x < widthPx; ++x) {
            dst[x * 4 + 0] = src[x * 4 + 2]; // B
            dst[x * 4 + 1] = src[x * 4 + 1]; // G
            dst[x * 4 + 2] = src[x * 4 + 0]; // R
            dst[x * 4 + 3] = 0xFF;
        }
    }

    CD3DX12_RANGE emptyRange(0, 0);
    readbackBuffer->Unmap(0, &emptyRange);
    constantBuffer->Unmap(0, nullptr);
    return bgraPixels;
}

// Sends the rendered bitmap to the printer DC, fitted and centered on the printable area.
bool SendBitmapToPrinter(HDC printerDC, const std::wstring& documentName,
    const std::vector<uint8_t>& bgraPixels, uint32_t widthPx, uint32_t heightPx) {
    const int pageWidth = GetDeviceCaps(printerDC, HORZRES);
    const int pageHeight = GetDeviceCaps(printerDC, VERTRES);
    if (pageWidth <= 0 || pageHeight <= 0) return false;

    // Fit the image into the printable page preserving aspect ratio, centered.
    int destWidth = pageWidth;
    int destHeight = static_cast<int>(static_cast<int64_t>(pageWidth) * heightPx / widthPx);
    if (destHeight > pageHeight) {
        destHeight = pageHeight;
        destWidth = static_cast<int>(static_cast<int64_t>(pageHeight) * widthPx / heightPx);
    }
    const int destX = (pageWidth - destWidth) / 2;
    const int destY = (pageHeight - destHeight) / 2;

    BITMAPINFO bitmapInfo = {};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = static_cast<LONG>(widthPx);
    bitmapInfo.bmiHeader.biHeight = -static_cast<LONG>(heightPx); // Negative: top-down rows.
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    DOCINFOW docInfo = {};
    docInfo.cbSize = sizeof(docInfo);
    docInfo.lpszDocName = documentName.c_str();

    if (StartDocW(printerDC, &docInfo) <= 0) return false;
    if (StartPage(printerDC) <= 0) {
        AbortDoc(printerDC);
        return false;
    }

    SetStretchBltMode(printerDC, HALFTONE);
    SetBrushOrgEx(printerDC, 0, 0, nullptr);
    const int copiedScanLines = StretchDIBits(printerDC,
        destX, destY, destWidth, destHeight,
        0, 0, static_cast<int>(widthPx), static_cast<int>(heightPx),
        bgraPixels.data(), &bitmapInfo, DIB_RGB_COLORS, SRCCOPY);

    if (copiedScanLines <= 0) {
        AbortDoc(printerDC);
        return false;
    }

    EndPage(printerDC);
    EndDoc(printerDC);
    return true;
}
} // namespace

void PrintActiveTab() {
    DATASETTAB* tab = ActiveTabForPrint();
    if (!tab) return;

    // Determine what is currently visible: a Page2D drawing or a 3D scene.
    uint64_t containerMemoryId = 0;
    VishwakarmaStorage::ObjectType containerType = VishwakarmaStorage::ObjectType::Unknown;
    if (tab->storageObjectsMutex) {
        std::lock_guard<std::mutex> lock(*tab->storageObjectsMutex);
        containerMemoryId = tab->activeInternalSubTabMemoryId;
        for (const InternalSubTab& subTab : tab->openInternalSubTabs) {
            if (subTab.containerMemoryId == containerMemoryId) {
                containerType = subTab.containerType;
                break;
            }
        }
    }
    if (containerMemoryId == 0) {
        MessageBoxW(OwnerWindowHandle(), L"Open a 2D Page or a 3D Scene to print.",
            L"Print", MB_OK | MB_ICONINFORMATION);
        return;
    }
    const bool isPage2D = containerType == VishwakarmaStorage::ObjectType::Page2D && tab->cad2d;

    // Standard Windows print dialog. The user picks the printer; the installed driver
    // and the print spooler handle everything device specific (printer.md).
    PRINTDLGW printDialog = {};
    printDialog.lStructSize = sizeof(printDialog);
    printDialog.hwndOwner = OwnerWindowHandle();
    printDialog.Flags = PD_RETURNDC | PD_NOPAGENUMS | PD_NOSELECTION | PD_USEDEVMODECOPIESANDCOLLATE;
    if (!PrintDlgW(&printDialog)) {
        if (printDialog.hDevMode) GlobalFree(printDialog.hDevMode);
        if (printDialog.hDevNames) GlobalFree(printDialog.hDevNames);
        return; // User cancelled.
    }
    HDC printerDC = printDialog.hDC;

    // Offscreen render size: printable area re-expressed at kPrintDPI, capped for VRAM.
    const int printerDpiX = (std::max)(GetDeviceCaps(printerDC, LOGPIXELSX), 1);
    const int printerDpiY = (std::max)(GetDeviceCaps(printerDC, LOGPIXELSY), 1);
    uint32_t renderWidth = static_cast<uint32_t>(
        (std::max)(MulDiv(GetDeviceCaps(printerDC, HORZRES), kPrintDPI, printerDpiX), 32));
    uint32_t renderHeight = static_cast<uint32_t>(
        (std::max)(MulDiv(GetDeviceCaps(printerDC, VERTRES), kPrintDPI, printerDpiY), 32));
    const uint32_t largestDimension = (std::max)(renderWidth, renderHeight);
    if (largestDimension > kMaxPrintDimensionPx) {
        renderWidth = static_cast<uint32_t>(
            static_cast<uint64_t>(renderWidth) * kMaxPrintDimensionPx / largestDimension);
        renderHeight = static_cast<uint32_t>(
            static_cast<uint64_t>(renderHeight) * kMaxPrintDimensionPx / largestDimension);
    }

    bool printed = false;
    std::wstring failureReason = L"Rendering the drawing for print failed.";
    try {
        std::vector<uint8_t> bgraPixels =
            RenderForPrint(*tab, isPage2D, containerMemoryId, renderWidth, renderHeight);
        if (!bgraPixels.empty()) {
            const std::wstring documentName = tab->fileName.empty()
                ? L"Vishwakarma Drawing" : L"Vishwakarma - " + tab->fileName;
            failureReason = L"The printer rejected the print job.";
            printed = SendBitmapToPrinter(printerDC, documentName, bgraPixels,
                renderWidth, renderHeight);
        }
    } catch (const HrException& e) {
        std::cerr << "Print rendering failed. HRESULT: 0x" << std::hex << e.Error() << std::dec << std::endl;
    }

    DeleteDC(printerDC);
    if (printDialog.hDevMode) GlobalFree(printDialog.hDevMode);
    if (printDialog.hDevNames) GlobalFree(printDialog.hDevNames);

    if (!printed) {
        MessageBoxW(OwnerWindowHandle(), failureReason.c_str(), L"Print failed",
            MB_OK | MB_ICONERROR);
    }
}
