// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

#include "Selection3D-DirectX12.h"
#include "MemoryManagerGPU-DirectX12.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

// Generated shader byte-code headers (see FxCompile entries in Vishwakarma.vcxproj).
#include "ShaderSceneVertex.h"           // g_sceneVertexShader   (reused by the highlight PSO)
#include "ShaderScenePickVertex.h"       // g_scenePickVertexShader
#include "ShaderScenePickPixel.h"        // g_scenePickPixelShader
#include "ShaderSceneHighlightPixel.h"   // g_sceneHighlightPixelShader
#include "ShaderCubeVertex.h"            // g_cubeVertexShader
#include "ShaderCubePixel.h"             // g_cubePixelShader

using namespace DirectX;

extern शंकर gpu; // Global VRAM manager (defined in विश्वकर्मा.cpp).

namespace {

// The scene vertex input layout, shared by the pick and highlight PSOs so they consume the exact
// same per-object geometry as the visible scene pipeline.
const D3D12_INPUT_ELEMENT_DESC kSceneInputLayout[] = {
    { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "NORMAL",   0, DXGI_FORMAT_R8G8B8A8_SNORM,     0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "COLOR",    0, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
};

struct CubeConstantsCPU {
    XMFLOAT4X4 mvp;
    XMFLOAT4 color;
};

struct CubeVertex { float px, py, pz, nx, ny, nz; };

// Unit cube centered on the origin, half-extent 0.5, one normal per face (24 verts, 36 indices).
const CubeVertex kCubeVerts[24] = {
    { 0.5f,-0.5f,-0.5f, 1,0,0}, { 0.5f, 0.5f,-0.5f, 1,0,0}, { 0.5f, 0.5f, 0.5f, 1,0,0}, { 0.5f,-0.5f, 0.5f, 1,0,0}, // +X
    {-0.5f,-0.5f,-0.5f,-1,0,0}, {-0.5f,-0.5f, 0.5f,-1,0,0}, {-0.5f, 0.5f, 0.5f,-1,0,0}, {-0.5f, 0.5f,-0.5f,-1,0,0}, // -X
    {-0.5f, 0.5f,-0.5f, 0,1,0}, {-0.5f, 0.5f, 0.5f, 0,1,0}, { 0.5f, 0.5f, 0.5f, 0,1,0}, { 0.5f, 0.5f,-0.5f, 0,1,0}, // +Y
    {-0.5f,-0.5f,-0.5f, 0,-1,0},{ 0.5f,-0.5f,-0.5f, 0,-1,0},{ 0.5f,-0.5f, 0.5f, 0,-1,0},{-0.5f,-0.5f, 0.5f, 0,-1,0}, // -Y
    {-0.5f,-0.5f, 0.5f, 0,0,1}, { 0.5f,-0.5f, 0.5f, 0,0,1}, { 0.5f, 0.5f, 0.5f, 0,0,1}, {-0.5f, 0.5f, 0.5f, 0,0,1}, // +Z
    {-0.5f,-0.5f,-0.5f, 0,0,-1},{-0.5f, 0.5f,-0.5f, 0,0,-1},{ 0.5f, 0.5f,-0.5f, 0,0,-1},{ 0.5f,-0.5f,-0.5f, 0,0,-1}, // -Z
};
const uint16_t kCubeIdx[36] = {
    0,1,2, 0,2,3,   4,5,6, 4,6,7,   8,9,10, 8,10,11,
    12,13,14, 12,14,15,   16,17,18, 16,18,19,   20,21,22, 20,22,23,
};

constexpr float kNavCubePixels = 26.0f;                 // Approx on-screen cube size in pixels.
const float kNavCubeColor[3] = { 1.0f, 0.55f, 0.10f };  // Warm orange, distinct from selection blue.

inline UINT AlignUp256(UINT v) { return (v + 255u) & ~255u; }

// Upload-heap buffer holding a copy of 'data'. Small static geometry lives directly in the upload
// heap (GPU-readable); cheap for a 24-vertex cube drawn a few times per second.
template <typename T>
ComPtr<ID3D12Resource> MakeUploadBufferWithData(const T* data, size_t count) {
    ComPtr<ID3D12Resource> buffer;
    const UINT64 sizeBytes = static_cast<UINT64>(count * sizeof(T));
    CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
    auto desc = CD3DX12_RESOURCE_DESC::Buffer(sizeBytes);
    ThrowIfFailed(gpu.device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buffer)));
    uint8_t* mapped = nullptr;
    CD3DX12_RANGE readRange(0, 0);
    ThrowIfFailed(buffer->Map(0, &readRange, reinterpret_cast<void**>(&mapped)));
    memcpy(mapped, data, sizeBytes);
    buffer->Unmap(0, nullptr);
    return buffer;
}

void CreatePickPSO(DX12ResourcesPerTab& tabRes) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.InputLayout = { kSceneInputLayout, _countof(kSceneInputLayout) };
    pso.pRootSignature = tabRes.rootSignature.Get(); // Same signature as the scene pipeline.
    pso.VS = CD3DX12_SHADER_BYTECODE(g_scenePickVertexShader, sizeof(g_scenePickVertexShader));
    pso.PS = CD3DX12_SHADER_BYTECODE(g_scenePickPixelShader, sizeof(g_scenePickPixelShader));
    pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT); // Back-cull, matches scene.
    pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    pso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT); // Depth LESS, write on.
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 2;
    pso.RTVFormats[0] = DXGI_FORMAT_R32_UINT;   // pick id
    pso.RTVFormats[1] = DXGI_FORMAT_R32_FLOAT;  // NDC depth
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = 1;
    ThrowIfFailed(gpu.device->CreateGraphicsPipelineState(&pso,
        IID_PPV_ARGS(&tabRes.selection3D.pickPSO)));
}

void CreateHighlightPSO(DX12ResourcesPerTab& tabRes) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.InputLayout = { kSceneInputLayout, _countof(kSceneInputLayout) };
    pso.pRootSignature = tabRes.rootSignature.Get();
    pso.VS = CD3DX12_SHADER_BYTECODE(g_sceneVertexShader, sizeof(g_sceneVertexShader));
    pso.PS = CD3DX12_SHADER_BYTECODE(g_sceneHighlightPixelShader, sizeof(g_sceneHighlightPixelShader));
    pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    pso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL; // Redraw at same depth.
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO; // Don't disturb depth.
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = gpu.rttFormat;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = 1;
    ThrowIfFailed(gpu.device->CreateGraphicsPipelineState(&pso,
        IID_PPV_ARGS(&tabRes.selection3D.highlightPSO)));
}

void CreateCubePipeline(DX12ResourcesPerTab& tabRes) {
    Selection3DResources& sel = tabRes.selection3D;

    CD3DX12_ROOT_PARAMETER1 rootParams[1] = {};
    rootParams[0].InitAsConstantBufferView(0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE,
        D3D12_SHADER_VISIBILITY_ALL);
    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootDesc;
    rootDesc.Init_1_1(_countof(rootParams), rootParams, 0, nullptr,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    D3D12_FEATURE_DATA_ROOT_SIGNATURE feat = {};
    feat.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
    if (FAILED(gpu.device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &feat, sizeof(feat)))) {
        feat.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
    }
    ComPtr<ID3DBlob> signature, error;
    ThrowIfFailed(D3DX12SerializeVersionedRootSignature(&rootDesc, feat.HighestVersion,
        &signature, &error));
    ThrowIfFailed(gpu.device->CreateRootSignature(0, signature->GetBufferPointer(),
        signature->GetBufferSize(), IID_PPV_ARGS(&sel.cubeRootSignature)));
    sel.cubeRootSignature->SetName(L"Selection3D Cube");

    const D3D12_INPUT_ELEMENT_DESC cubeLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.InputLayout = { cubeLayout, _countof(cubeLayout) };
    pso.pRootSignature = sel.cubeRootSignature.Get();
    pso.VS = CD3DX12_SHADER_BYTECODE(g_cubeVertexShader, sizeof(g_cubeVertexShader));
    pso.PS = CD3DX12_SHADER_BYTECODE(g_cubePixelShader, sizeof(g_cubePixelShader));
    pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    pso.BlendState.RenderTarget[0].BlendEnable = TRUE;
    pso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    pso.DepthStencilState.DepthEnable = FALSE;   // Overlay: always visible on top of the scene.
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = gpu.rttFormat;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT; // Matches the scene DSV that stays bound.
    pso.SampleDesc.Count = 1;
    ThrowIfFailed(gpu.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&sel.cubePSO)));

    sel.cubeVertexBuffer = MakeUploadBufferWithData(kCubeVerts, _countof(kCubeVerts));
    sel.cubeIndexBuffer = MakeUploadBufferWithData(kCubeIdx, _countof(kCubeIdx));
    sel.cubeVBV.BufferLocation = sel.cubeVertexBuffer->GetGPUVirtualAddress();
    sel.cubeVBV.SizeInBytes = sizeof(kCubeVerts);
    sel.cubeVBV.StrideInBytes = sizeof(CubeVertex);
    sel.cubeIBV.BufferLocation = sel.cubeIndexBuffer->GetGPUVirtualAddress();
    sel.cubeIBV.SizeInBytes = sizeof(kCubeIdx);
    sel.cubeIBV.Format = DXGI_FORMAT_R16_UINT;
    sel.cubeIndexCount = _countof(kCubeIdx);

    CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
    auto cbDesc = CD3DX12_RESOURCE_DESC::Buffer(256);
    ThrowIfFailed(gpu.device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&sel.cubeConstantBuffer)));
    CD3DX12_RANGE readRange(0, 0);
    ThrowIfFailed(sel.cubeConstantBuffer->Map(0, &readRange,
        reinterpret_cast<void**>(&sel.cubeConstantData)));
}

// (Re)create the pick render targets + readback buffers to match the scene viewport size.
void EnsurePickTargets(PickPassContext& ctx, UINT w, UINT h) {
    if (ctx.idTexture && ctx.width == w && ctx.height == h) return;

    ctx.idTexture.Reset(); ctx.depthColor.Reset(); ctx.depthStencil.Reset();
    ctx.rtvHeap.Reset(); ctx.dsvHeap.Reset(); ctx.readbackId.Reset(); ctx.readbackDepth.Reset();
    ctx.width = w; ctx.height = h;

    CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);

    auto idDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_UINT, w, h, 1, 1, 1, 0,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    D3D12_CLEAR_VALUE idClear{}; idClear.Format = DXGI_FORMAT_R32_UINT;
    ThrowIfFailed(gpu.device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &idDesc,
        D3D12_RESOURCE_STATE_COMMON, &idClear, IID_PPV_ARGS(&ctx.idTexture)));

    auto depthColorDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_FLOAT, w, h, 1, 1, 1, 0,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    D3D12_CLEAR_VALUE dcClear{}; dcClear.Format = DXGI_FORMAT_R32_FLOAT;
    dcClear.Color[0] = dcClear.Color[1] = dcClear.Color[2] = dcClear.Color[3] = 1.0f;
    ThrowIfFailed(gpu.device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE,
        &depthColorDesc, D3D12_RESOURCE_STATE_COMMON, &dcClear, IID_PPV_ARGS(&ctx.depthColor)));

    auto dsDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_D32_FLOAT, w, h, 1, 0, 1, 0,
        D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    D3D12_CLEAR_VALUE dsClear{}; dsClear.Format = DXGI_FORMAT_D32_FLOAT;
    dsClear.DepthStencil.Depth = 1.0f;
    ThrowIfFailed(gpu.device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &dsDesc,
        D3D12_RESOURCE_STATE_COMMON, &dsClear, IID_PPV_ARGS(&ctx.depthStencil)));

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = 2;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ThrowIfFailed(gpu.device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&ctx.rtvHeap)));
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtv(ctx.rtvHeap->GetCPUDescriptorHandleForHeapStart());
    gpu.device->CreateRenderTargetView(ctx.idTexture.Get(), nullptr, rtv);
    rtv.Offset(1, gpu.rtvDescriptorSize);
    gpu.device->CreateRenderTargetView(ctx.depthColor.Get(), nullptr, rtv);

    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    ThrowIfFailed(gpu.device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&ctx.dsvHeap)));
    gpu.device->CreateDepthStencilView(ctx.depthStencil.Get(), nullptr,
        ctx.dsvHeap->GetCPUDescriptorHandleForHeapStart());

    ctx.readbackRowPitch = AlignUp256(static_cast<UINT>(kPickBoxSize) * sizeof(uint32_t));
    const UINT64 rbSize = static_cast<UINT64>(ctx.readbackRowPitch) * kPickBoxSize;
    CD3DX12_HEAP_PROPERTIES readbackHeap(D3D12_HEAP_TYPE_READBACK);
    auto rbDesc = CD3DX12_RESOURCE_DESC::Buffer(rbSize);
    ThrowIfFailed(gpu.device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &rbDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&ctx.readbackId)));
    ThrowIfFailed(gpu.device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &rbDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&ctx.readbackDepth)));
}

// Bind whole-page vertex/index buffers exactly like RenderScene3D does, so per-object
// StartIndexLocation / BaseVertexLocation match the indirect-draw arithmetic.
void BindPageBuffers(ID3D12GraphicsCommandList* cmd, GeometryPage& page) {
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
}

bool PageIsRenderable(const GeometryPage& page, uint64_t activeContainerMemoryId) {
    return page.published.load(std::memory_order_acquire) && page.indirectCount != 0 &&
        page.vertexHead != 0 && page.indexTail != page.pageSize &&
        activeContainerMemoryId != 0 && page.containerMemoryId == activeContainerMemoryId;
}

// Resolve a completed pick result and publish it to SelectionState.
void ResolveAndPublish(PickPassContext& ctx, DX12ResourcesPerTab& tabRes, TabGeometryStorage& storage,
    SelectionState& selection, uint64_t activeContainerMemoryId) {
    // Scan the sampled box for the nearest (smallest depth) non-background pixel.
    uint32_t bestId = 0; float bestDepth = 2.0f; int bestCol = 0, bestRow = 0;
    {
        uint8_t* idPtr = nullptr; uint8_t* depthPtr = nullptr;
        CD3DX12_RANGE readAll(0, static_cast<SIZE_T>(ctx.readbackRowPitch) * ctx.boxH);
        ThrowIfFailed(ctx.readbackId->Map(0, &readAll, reinterpret_cast<void**>(&idPtr)));
        ThrowIfFailed(ctx.readbackDepth->Map(0, &readAll, reinterpret_cast<void**>(&depthPtr)));
        for (int r = 0; r < ctx.boxH; ++r) {
            for (int c = 0; c < ctx.boxW; ++c) {
                const uint32_t id = *reinterpret_cast<uint32_t*>(idPtr + r * ctx.readbackRowPitch + c * 4);
                const float d = *reinterpret_cast<float*>(depthPtr + r * ctx.readbackRowPitch + c * 4);
                if (id != 0 && d < bestDepth) { bestDepth = d; bestId = id; bestCol = c; bestRow = r; }
            }
        }
        CD3DX12_RANGE noWrite(0, 0);
        ctx.readbackId->Unmap(0, &noWrite);
        ctx.readbackDepth->Unmap(0, &noWrite);
    }

    const bool hit = (bestId != 0); // Geometry (not background) was under the cursor.
    uint64_t objectId = 0;
    XMVECTOR worldCG = XMVectorZero();
    XMVECTOR worldSurface = XMVectorZero();

    if (hit) {
        const uint32_t matrixIndex = bestId - 1;

        // Reconstruct the world-space surface point from the hit pixel + its depth.
        const float px = static_cast<float>(ctx.boxX + bestCol) + 0.5f;
        const float py = static_cast<float>(ctx.boxY + bestRow) + 0.5f;
        const float ndcX = px / static_cast<float>(ctx.sceneW) * 2.0f - 1.0f;
        const float ndcY = 1.0f - py / static_cast<float>(ctx.sceneH) * 2.0f;
        XMVECTOR clip = XMVectorSet(ndcX, ndcY, bestDepth, 1.0f);
        XMVECTOR world = XMVector4Transform(clip, ctx.invViewProj);
        const float w = XMVectorGetW(world);
        if (std::abs(w) > 1e-8f) worldSurface = XMVectorScale(world, 1.0f / w);
        worldCG = worldSurface; // Fallback if the object can't be resolved this frame.

        // Resolve object id + local AABB from the current snapshot; compute world AABB center.
        GeometryPageSnapshot* snapshot = storage.activeSnapshot.load(std::memory_order_acquire);
        if (snapshot) {
            for (GeometryPage* pagePtr : snapshot->pages) {
                if (!pagePtr) continue;
                GeometryPage& page = *pagePtr;
                if (!page.published.load(std::memory_order_acquire)) continue;
                if (activeContainerMemoryId != 0 && page.containerMemoryId != activeContainerMemoryId) continue;
                for (const GeometryPlacementRecordInPage& obj : page.objects) {
                    if (obj.isDeleted || obj.matrixIndex != matrixIndex) continue;
                    objectId = obj.objectID;
                    XMVECTOR localCenter = XMVectorSet(
                        (obj.minX + obj.maxX) * 0.5f, (obj.minY + obj.maxY) * 0.5f,
                        (obj.minZ + obj.maxZ) * 0.5f, 1.0f);
                    // The stored per-object matrix is transpose(world); XMVector3Transform mirrors
                    // the vertex shader's mul(pos, world), so this yields the world-space center.
                    // Mirrors, capacity first: the copy thread regrows the table, and the snapshot
                    // acquire-load above orders these reads (see DX12ResourcesPerTab).
                    const uint32_t matrixCapacity =
                        tabRes.matrixCapacityShared.load(std::memory_order_acquire);
                    UINT8* matrixData = tabRes.worldMatrixDataShared.load(std::memory_order_acquire);
                    if (matrixIndex < matrixCapacity && matrixData) {
                        XMMATRIX worldMat = XMLoadFloat4x4(reinterpret_cast<const XMFLOAT4X4*>(
                            matrixData + matrixIndex * sizeof(XMFLOAT4X4)));
                        worldCG = XMVector3Transform(localCenter, worldMat);
                    } else {
                        worldCG = localCenter;
                    }
                    break;
                }
                if (objectId != 0) break; // Found the picked object; stop scanning pages.
            }
        }
    }

    std::lock_guard<std::mutex> lock(selection.resultMutex);
    selection.resultHit = hit;
    selection.resultObjectId = objectId;
    XMStoreFloat3(&selection.resultCG, worldCG);
    XMStoreFloat3(&selection.resultSurface, worldSurface);
    selection.resultPurpose = ctx.purpose;
    selection.resultReady.store(true, std::memory_order_release);
}

} // namespace

void InitSelection3DResources(DX12ResourcesPerTab& tabRes) {
    if (tabRes.selection3D.initialized) return;
    if (!tabRes.rootSignature) return; // Scene root signature must exist first.
    CreatePickPSO(tabRes);
    CreateHighlightPSO(tabRes);
    CreateCubePipeline(tabRes);
    tabRes.selection3D.initialized = true;
}

void CleanupSelection3DResources(DX12ResourcesPerTab& tabRes) {
    Selection3DResources& sel = tabRes.selection3D;
    if (sel.cubeConstantBuffer && sel.cubeConstantData) {
        sel.cubeConstantBuffer->Unmap(0, nullptr);
        sel.cubeConstantData = nullptr;
    }
    sel.pickPSO.Reset();
    sel.highlightPSO.Reset();
    sel.cubePSO.Reset();
    sel.cubeRootSignature.Reset();
    sel.cubeVertexBuffer.Reset();
    sel.cubeIndexBuffer.Reset();
    sel.cubeConstantBuffer.Reset();
    sel.initialized = false;
}

void CleanupPickPassContext(PickPassContext& ctx) {
    ctx.idTexture.Reset(); ctx.depthColor.Reset(); ctx.depthStencil.Reset();
    ctx.rtvHeap.Reset(); ctx.dsvHeap.Reset(); ctx.readbackId.Reset(); ctx.readbackDepth.Reset();
    ctx.width = 0; ctx.height = 0; ctx.state = PickPassContext::State::Idle;
}

void RecordSelectionOverlays(ID3D12GraphicsCommandList* commandList, DX12ResourcesPerWindow& winRes,
    DX12ResourcesPerTab& tabRes, TabGeometryStorage& storage, SelectionState& selection,
    const XMMATRIX& viewProj, int /*topUIHeightPx*/, int /*sceneWidth*/, int sceneHeight,
    uint64_t activeContainerMemoryId) {
    if (!commandList || !tabRes.selection3D.initialized || sceneHeight <= 0) return;

    // --- Highlight the selected objects in deep blue -------------------------------------------
    std::vector<uint64_t> selectedCopy;
    {
        std::lock_guard<std::mutex> lock(selection.selectedMutex);
        selectedCopy = selection.selectedObjectIds;
    }
    if (!selectedCopy.empty() && activeContainerMemoryId != 0) {
        std::unordered_set<uint64_t> selectedSet(selectedCopy.begin(), selectedCopy.end());
        GeometryPageSnapshot* snapshot = storage.activeSnapshot.load(std::memory_order_acquire);
        if (snapshot) {
            commandList->SetGraphicsRootSignature(tabRes.rootSignature.Get());
            commandList->SetPipelineState(tabRes.selection3D.highlightPSO.Get());
            commandList->SetGraphicsRootConstantBufferView(0, winRes.constantBuffer->GetGPUVirtualAddress());
            // Mirror, not the ComPtr: the copy thread regrows the table; the snapshot loaded
            // above orders this read (see DX12ResourcesPerTab mirror comments).
            commandList->SetGraphicsRootShaderResourceView(1,
                tabRes.worldMatrixVAShared.load(std::memory_order_acquire));
            commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            for (GeometryPage* pagePtr : snapshot->pages) {
                if (!pagePtr) continue;
                GeometryPage& page = *pagePtr;
                if (!PageIsRenderable(page, activeContainerMemoryId)) continue;

                bool boundBuffers = false;
                for (const GeometryPlacementRecordInPage& obj : page.objects) {
                    if (obj.isDeleted || selectedSet.find(obj.objectID) == selectedSet.end()) continue;
                    if (!boundBuffers) { BindPageBuffers(commandList, page); boundBuffers = true; }
                    const UINT startIndex =
                        (obj.indexByteOffset - page.indexTail) / static_cast<UINT>(sizeof(uint16_t));
                    const INT baseVertex =
                        static_cast<INT>(obj.vertexByteOffset / sizeof(Vertex));
                    commandList->SetGraphicsRoot32BitConstant(2, obj.matrixIndex, 0);
                    commandList->DrawIndexedInstanced(obj.indexCount, 1, startIndex, baseVertex, 0);
                }
            }
        }
    }

    // --- Rotation-center cube overlay ----------------------------------------------------------
    const uint64_t now = GetTickCount64();
    const uint64_t lastNav = selection.lastNavInteractionMs.load(std::memory_order_acquire);
    if (lastNav == 0 || now < lastNav) return;
    const uint64_t elapsed = now - lastNav;
    if (elapsed >= kNavCubeVisibleMs) return;
    const float alpha = std::clamp(
        static_cast<float>(kNavCubeVisibleMs - elapsed) / 250.0f, 0.0f, 1.0f);

    XMVECTOR eye = XMLoadFloat3(&tabRes.camera.position);
    XMVECTOR target = XMLoadFloat3(&tabRes.camera.target);
    const float distance = XMVectorGetX(XMVector3Length(XMVectorSubtract(eye, target)));
    const float tanHalfFov = std::tan(tabRes.camera.fov * 0.5f);
    // Scale the unit cube so it keeps a roughly constant on-screen size regardless of zoom.
    const float scale = kNavCubePixels * 2.0f * tanHalfFov * (std::max)(distance, 1e-3f) /
        static_cast<float>(sceneHeight);

    XMMATRIX model = XMMatrixScaling(scale, scale, scale) *
        XMMatrixTranslationFromVector(target);
    XMMATRIX mvp = model * viewProj;

    CubeConstantsCPU cb{};
    XMStoreFloat4x4(&cb.mvp, XMMatrixTranspose(mvp));
    cb.color = XMFLOAT4(kNavCubeColor[0], kNavCubeColor[1], kNavCubeColor[2], alpha);
    memcpy(tabRes.selection3D.cubeConstantData, &cb, sizeof(cb));

    commandList->SetGraphicsRootSignature(tabRes.selection3D.cubeRootSignature.Get());
    commandList->SetPipelineState(tabRes.selection3D.cubePSO.Get());
    commandList->SetGraphicsRootConstantBufferView(0,
        tabRes.selection3D.cubeConstantBuffer->GetGPUVirtualAddress());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->IASetVertexBuffers(0, 1, &tabRes.selection3D.cubeVBV);
    commandList->IASetIndexBuffer(&tabRes.selection3D.cubeIBV);
    commandList->DrawIndexedInstanced(tabRes.selection3D.cubeIndexCount, 1, 0, 0, 0);
}

void ServicePick(ID3D12GraphicsCommandList* commandList, DX12ResourcesPerWindow& winRes,
    DX12ResourcesPerTab& tabRes, TabGeometryStorage& storage, SelectionState& selection,
    PickPassContext& ctx, int monitorId, const XMMATRIX& viewProj,
    int topUIHeightPx, int sceneWidth, int sceneHeight, uint64_t activeContainerMemoryId) {
    if (!commandList || !tabRes.selection3D.initialized) return;

    // 1) Publish a completed pick once its frame's fence has passed on the GPU.
    if (ctx.state == PickPassContext::State::InFlight) {
        const uint64_t completed = monitorId >= 0 && monitorId < MV_MAX_MONITORS &&
            gpu.screens[monitorId].renderFence
            ? gpu.screens[monitorId].renderFence->GetCompletedValue() : ctx.pendingFence;
        if (completed >= ctx.pendingFence) {
            ResolveAndPublish(ctx, tabRes, storage, selection, activeContainerMemoryId);
            ctx.state = PickPassContext::State::Idle;
        }
    }

    // 2) Start a new pick if one is requested and we're idle.
    if (ctx.state != PickPassContext::State::Idle) return;
    if (!selection.pickRequested.load(std::memory_order_acquire)) return;

    const int px = selection.pickX.load(std::memory_order_relaxed);
    const int py = selection.pickY.load(std::memory_order_relaxed);
    const uint32_t purpose = selection.pickPurpose.load(std::memory_order_relaxed);
    selection.pickRequested.store(false, std::memory_order_release);

    if (sceneWidth <= 0 || sceneHeight <= 0 || activeContainerMemoryId == 0) return;
    const int localX = px;
    const int localY = py - topUIHeightPx;
    if (localX < 0 || localX >= sceneWidth || localY < 0 || localY >= sceneHeight) return;

    EnsurePickTargets(ctx, static_cast<UINT>(sceneWidth), static_cast<UINT>(sceneHeight));

    // Sample an NxN box centered on the cursor, clamped to the viewport.
    const int half = kPickBoxSize / 2;
    ctx.boxX = std::clamp(localX - half, 0, sceneWidth - 1);
    ctx.boxY = std::clamp(localY - half, 0, sceneHeight - 1);
    ctx.boxW = (std::min)(kPickBoxSize, sceneWidth - ctx.boxX);
    ctx.boxH = (std::min)(kPickBoxSize, sceneHeight - ctx.boxY);
    ctx.sceneW = sceneWidth;
    ctx.sceneH = sceneHeight;
    ctx.purpose = purpose;
    ctx.invViewProj = XMMatrixInverse(nullptr, viewProj);

    // Transition targets: COMMON -> render/depth.
    CD3DX12_RESOURCE_BARRIER toRender[3] = {
        CD3DX12_RESOURCE_BARRIER::Transition(ctx.idTexture.Get(),
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET),
        CD3DX12_RESOURCE_BARRIER::Transition(ctx.depthColor.Get(),
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET),
        CD3DX12_RESOURCE_BARRIER::Transition(ctx.depthStencil.Get(),
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_DEPTH_WRITE),
    };
    commandList->ResourceBarrier(3, toRender);

    CD3DX12_CPU_DESCRIPTOR_HANDLE rtv0(ctx.rtvHeap->GetCPUDescriptorHandleForHeapStart());
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtv1(rtv0, 1, gpu.rtvDescriptorSize);
    CD3DX12_CPU_DESCRIPTOR_HANDLE dsv(ctx.dsvHeap->GetCPUDescriptorHandleForHeapStart());
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2] = { rtv0, rtv1 };
    commandList->OMSetRenderTargets(2, rtvs, FALSE, &dsv);

    const float idClearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    const float depthClearColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    commandList->ClearRenderTargetView(rtv0, idClearColor, 0, nullptr);
    commandList->ClearRenderTargetView(rtv1, depthClearColor, 0, nullptr);
    commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    CD3DX12_VIEWPORT viewport(0.0f, 0.0f, static_cast<float>(sceneWidth), static_cast<float>(sceneHeight));
    CD3DX12_RECT scissor(ctx.boxX, ctx.boxY, ctx.boxX + ctx.boxW, ctx.boxY + ctx.boxH);
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);

    commandList->SetGraphicsRootSignature(tabRes.rootSignature.Get());
    commandList->SetPipelineState(tabRes.selection3D.pickPSO.Get());
    commandList->SetGraphicsRootConstantBufferView(0, winRes.constantBuffer->GetGPUVirtualAddress());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    GeometryPageSnapshot* snapshot = storage.activeSnapshot.load(std::memory_order_acquire);
    // Matrix-table VA bound AFTER the snapshot acquire-load: the copy thread regrows the table,
    // and the snapshot publish order guarantees address/capacity consistency (mirror comments).
    commandList->SetGraphicsRootShaderResourceView(1,
        tabRes.worldMatrixVAShared.load(std::memory_order_acquire));
    if (snapshot) {
        for (GeometryPage* pagePtr : snapshot->pages) {
            if (!pagePtr) continue;
            GeometryPage& page = *pagePtr;
            if (!PageIsRenderable(page, activeContainerMemoryId)) continue;
            BindPageBuffers(commandList, page);
            commandList->ExecuteIndirect(tabRes.commandSignature.Get(), page.indirectCount,
                page.indirectBuffer.Get(), 0, nullptr, 0);
        }
    }

    // Transition color targets to copy source, copy the box to the readback buffers.
    CD3DX12_RESOURCE_BARRIER toCopy[2] = {
        CD3DX12_RESOURCE_BARRIER::Transition(ctx.idTexture.Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
        CD3DX12_RESOURCE_BARRIER::Transition(ctx.depthColor.Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
    };
    commandList->ResourceBarrier(2, toCopy);

    D3D12_BOX srcBox = { static_cast<UINT>(ctx.boxX), static_cast<UINT>(ctx.boxY), 0,
        static_cast<UINT>(ctx.boxX + ctx.boxW), static_cast<UINT>(ctx.boxY + ctx.boxH), 1 };

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT idFp = {};
    idFp.Footprint.Format = DXGI_FORMAT_R32_UINT;
    idFp.Footprint.Width = static_cast<UINT>(ctx.boxW);
    idFp.Footprint.Height = static_cast<UINT>(ctx.boxH);
    idFp.Footprint.Depth = 1;
    idFp.Footprint.RowPitch = ctx.readbackRowPitch;
    CD3DX12_TEXTURE_COPY_LOCATION idDst(ctx.readbackId.Get(), idFp);
    CD3DX12_TEXTURE_COPY_LOCATION idSrc(ctx.idTexture.Get(), 0);
    commandList->CopyTextureRegion(&idDst, 0, 0, 0, &idSrc, &srcBox);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT depthFp = idFp;
    depthFp.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
    CD3DX12_TEXTURE_COPY_LOCATION depthDst(ctx.readbackDepth.Get(), depthFp);
    CD3DX12_TEXTURE_COPY_LOCATION depthSrc(ctx.depthColor.Get(), 0);
    commandList->CopyTextureRegion(&depthDst, 0, 0, 0, &depthSrc, &srcBox);

    // Return everything to COMMON for the next pick.
    CD3DX12_RESOURCE_BARRIER toCommon[3] = {
        CD3DX12_RESOURCE_BARRIER::Transition(ctx.idTexture.Get(),
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
        CD3DX12_RESOURCE_BARRIER::Transition(ctx.depthColor.Get(),
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
        CD3DX12_RESOURCE_BARRIER::Transition(ctx.depthStencil.Get(),
            D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COMMON),
    };
    commandList->ResourceBarrier(3, toCommon);

    ctx.state = PickPassContext::State::Recorded;
}

void FinalizePickFence(PickPassContext& ctx, uint64_t signaledFenceValue) {
    if (ctx.state == PickPassContext::State::Recorded) {
        ctx.pendingFence = signaledFenceValue;
        ctx.state = PickPassContext::State::InFlight;
    }
}
