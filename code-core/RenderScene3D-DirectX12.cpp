// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

#include "RenderScene3D-DirectX12.h"
#include "MemoryManagerGPU-DirectX12.h"
#include "RenderPage2D-DirectX12.h"
#include "UserInterface-DirectX12.h"
#include "ShaderSceneVertex_16.h"
#include "ShaderScenePixel.h"
#include "ShaderSceneCull.h"
#include "ShaderSkyGradientVertex.h"
#include "ShaderSkyGradientPixel.h"
#include <algorithm>
#include <set> // Append targets are keyed by (container, page kind), which needs no hash.
#include <cfloat>
#include <cmath>
#include "विश्वकर्मा.h"
#include <iomanip>
#include <unordered_set>
#include <colors.h>

extern शंकर gpu;
extern std::atomic<uint64_t> atlasFence;

/* Defaults ON since the compaction path became one ExecuteIndirect per Viewport (10M plan Step 7).
Before that it was strictly a lateral move - per-page draws either way - so it stayed off. Now it
draws a whole view in one call with no IA binds and no per-page barriers, which the legacy path
cannot match, so it is the primary path and the 'k' debug key is for A/B comparison rather than for
bringing it up. The legacy path remains maintained; see the branch in RenderScene3D. */
bool gUseComputeCull = true;
bool gLodPinned = false;

namespace {

    // Root constants for the sky pipeline (b0). Layout must match the cbuffer in
    // ShaderSkyGradient*.hlsl: HLSL packs each float3 + trailing float into one 16-byte register.
    struct SkyGradientConstants {
        float topColor[3];
        float ndcTopY;
        float horizonColor[3];
        float padding0;
    };
    static_assert(sizeof(SkyGradientConstants) == 8 * sizeof(float),
        "SkyGradientConstants must stay 8 root constants wide");

    /* Root constants (b0) of the draw-command compaction pass. Layout must match the CullParams
    cbuffer in ShaderSceneCull.hlsl member for member.

    The first three are per Viewport; the eight view fields are per page, and carrying them here is
    exactly what lets one ExecuteIndirect draw templates from several pages (10M plan Step 7).

    The camera block is per VIEWPORT and drives per-frame LOD selection for instanced templates
    ("Shared geometry and the primitive libraries", Step 2). Nothing straddles a float4 register and
    the cbuffer is a clean 16 DWORDs - `cullPadding` is what rounds it there, as it did at 12. */
    struct SceneCullConstants {
        uint32_t templateCount;
        uint32_t subTabBit;
        uint32_t maxCommands;
        uint32_t vertexAddressLo;
        uint32_t vertexAddressHi;
        uint32_t vertexSizeInBytes;
        uint32_t vertexStrideInBytes;
        uint32_t indexAddressLo;
        uint32_t indexAddressHi;
        uint32_t indexSizeInBytes;
        uint32_t indexFormat;
        float    cameraX;   // Viewport's eye position, world space.
        float    cameraY;
        float    cameraZ;
        /* Focal length in PIXELS: sceneHeightPx / (2 tan(fovY/2)). This is what makes LOD a
        function of the object's SCREEN size rather than of raw distance - the same sphere at the
        same distance deserves different tessellation at 4K and at 1080p, and at 20 degrees FOV
        against 60. Zero or negative pins every instanced template to the level the CPU already
        chose, which is what the LOD debug key toggles. */
        float    focalPixels;
        uint32_t cullPadding;
    };
    constexpr uint32_t kSceneCullConstantCount = sizeof(SceneCullConstants) / sizeof(uint32_t);
    static_assert(kSceneCullConstantCount == 16,
        "SceneCullConstants must stay 16 root constants wide - the HLSL cbuffer declares 16.");
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

void InitSkyGradientResources(ID3D12Device* device) {
    if (!device || gpu.skyGradientPSO) return;

    // Everything the two shaders need fits in root constants, so there is no descriptor heap, no
    // constant buffer and no vertex buffer to bind before the draw.
    CD3DX12_ROOT_PARAMETER1 rootParam;
    rootParam.InitAsConstants(sizeof(SkyGradientConstants) / sizeof(float), 0, 0,
        D3D12_SHADER_VISIBILITY_ALL);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootDesc;
    rootDesc.Init_1_1(1, &rootParam, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3DX12SerializeVersionedRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1_1,
        &signature, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob)
            std::cerr << "Sky Gradient Root Signature Serialization Failed:\n"
            << (char*)errorBlob->GetBufferPointer() << std::endl;
        ThrowIfFailed(hr);
    }
    ThrowIfFailed(device->CreateRootSignature(0, signature->GetBufferPointer(),
        signature->GetBufferSize(), IID_PPV_ARGS(&gpu.skyGradientRootSignature)));
    gpu.skyGradientRootSignature->SetName(L"Sky Gradient");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = gpu.skyGradientRootSignature.Get();
    psoDesc.VS = CD3DX12_SHADER_BYTECODE(g_skyGradientVertexShader, sizeof(g_skyGradientVertexShader));
    psoDesc.PS = CD3DX12_SHADER_BYTECODE(g_skyGradientPixelShader, sizeof(g_skyGradientPixelShader));
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    // The sky is the background: no blending, and depth is neither tested nor written. DSVFormat
    // still has to match the depth buffer the compositor has bound while this draws.
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = gpu.rttFormat;
    psoDesc.SampleDesc.Count = 1;

    ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&gpu.skyGradientPSO)));
    std::wcout << L"Sky gradient pipeline initialized\n";
}

void ClearSceneSkyGradient(ID3D12GraphicsCommandList* commandList, DX12ResourcesPerWindow& winRes,
    int monitorId) {
    if (!commandList || winRes.WindowWidth <= 0 || winRes.WindowHeight <= 0) return;
    // The compositor already cleared the whole RTT to the sky-top colour, so bailing out here
    // degrades to a flat sky rather than an undefined background.
    if (!gpu.skyGradientPSO || !gpu.skyGradientRootSignature) return;

    const int topUI = SceneTopUIHeightPx(monitorId, winRes);
    if (winRes.WindowHeight - topUI <= 0) return;

    SkyGradientConstants constants{
        { kSceneSkyTopR, kSceneSkyTopG, kSceneSkyTopB },
        1.0f - 2.0f * static_cast<float>(topUI) / static_cast<float>(winRes.WindowHeight),
        { kSceneSkyHorizonR, kSceneSkyHorizonG, kSceneSkyHorizonB },
        0.0f };

    commandList->SetGraphicsRootSignature(gpu.skyGradientRootSignature.Get());
    commandList->SetPipelineState(gpu.skyGradientPSO.Get());
    commandList->SetGraphicsRoot32BitConstants(0, sizeof(constants) / sizeof(float), &constants, 0);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    commandList->DrawInstanced(4, 1, 0, 0);
}


void InitSceneCullResources(ID3D12Device* device) {
    if (!device || gpu.sceneCullPSO) return;

    // All root descriptors, no descriptor tables, and no shader-visible descriptor heap: the page's
    // buffer views travel in the root constants rather than in a GPU-side page directory the shader
    // would have to look them up in (10M plan Step 7).
    CD3DX12_ROOT_PARAMETER1 params[8] = {};
    // b0: 16 DWORDs - { templateCount, subTabBit, maxCommands }, the page's VBV and IBV, and the
    // Viewport's camera. Must equal the CullParams cbuffer size in ShaderSceneCull.hlsl exactly.
    params[0].InitAsConstants(kSceneCullConstantCount, 0);
    params[1].InitAsShaderResourceView(0);      // t0: Templates (the page's indirectBuffer)
    params[2].InitAsShaderResourceView(1);      // t1: VisibilityMask
    params[3].InitAsUnorderedAccessView(0);     // u0: VisibleOut (compacted commands)
    params[4].InitAsUnorderedAccessView(1);     // u1: VisibleCount (raw uint at byte 0)
    /* The last three exist for per-frame LOD selection ("Shared geometry and the primitive
    libraries", Step 2). Reaching an object's transform needs the SAME two dependent loads the
    scene vertex shader already performs - redirect table, then arena - because the arena is
    addressed by instanceSlot and not by gpuInstanceIndex.

    Binding these here also settles the open question in "Planned: draw buckets": routing by
    renderFlags can now read them rather than copying flag bits into the draw template, which is
    what preserves the property that an appearance change never touches geometry. */
    params[5].InitAsShaderResourceView(2);      // t2: InstanceSlotOf (redirect table)
    params[6].InitAsShaderResourceView(3);      // t3: Instances (the 64-byte arena records)
    params[7].InitAsShaderResourceView(4);      // t4: PrimitiveLibrary (shapeId,lod) draw ranges

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootDesc;
    rootDesc.Init_1_1(_countof(params), params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3DX12SerializeVersionedRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1_1,
        &signature, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob)
            std::cerr << "Scene Cull Root Signature Serialization Failed:\n"
            << (char*)errorBlob->GetBufferPointer() << std::endl;
        ThrowIfFailed(hr);
    }
    ThrowIfFailed(device->CreateRootSignature(0, signature->GetBufferPointer(),
        signature->GetBufferSize(), IID_PPV_ARGS(&gpu.sceneCullRootSignature)));
    gpu.sceneCullRootSignature->SetName(L"Scene Cull");

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = gpu.sceneCullRootSignature.Get();
    psoDesc.CS = CD3DX12_SHADER_BYTECODE(g_sceneCullShader, sizeof(g_sceneCullShader));
    ThrowIfFailed(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&gpu.sceneCullPSO)));

    // 4-byte zero source: each page resets its visible-command count with a CopyBufferRegion from
    // here before the dispatch, which keeps the reset root-descriptor-only (no ClearUAV, which would
    // need both a CPU and a shader-visible descriptor).
    CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
    auto zeroDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(uint32_t));
    ThrowIfFailed(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &zeroDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&gpu.cullZeroBuffer)));
    gpu.cullZeroBuffer->SetName(L"Cull Zero");
    uint8_t* mapped = nullptr;
    CD3DX12_RANGE readRange(0, 0);
    ThrowIfFailed(gpu.cullZeroBuffer->Map(0, &readRange, reinterpret_cast<void**>(&mapped)));
    const uint32_t zero = 0;
    memcpy(mapped, &zero, sizeof(zero));
    gpu.cullZeroBuffer->Unmap(0, nullptr);

    std::wcout << L"Scene cull compaction pipeline initialized\n";
}

/* Consume the counts recorded by an EARLIER frame, once that frame's fence has passed (10M plan
Step 7). A one-frame delay is inherent: the count is produced on the GPU timeline, so reading it any
sooner would either stall the render thread or read a value the GPU has not written yet.

Reported as a live gauge - what the last completed frame actually drew on this monitor - rather than
a running total, because that is the number the plan's "reduce each SubTab to what its camera needs"
is measured by. A count above the cap means the shader dropped commands and the view was visibly
incomplete, which is counted separately. */
void PublishVisibleCountReadback(SceneCullScratch& cullScratch) {
    cullScratch.readbackSlotsRecorded = 0; // Starting a new frame's recording.
    if (cullScratch.readbackFence == 0 || !cullScratch.countReadback) return;
    if (gpu.renderFenceValue.load(std::memory_order_acquire) == 0) return;

    // The fence to test is the monitor's own: this scratch is written only by this render thread.
    // A frame that never got as far as signalling leaves readbackFence at 0 and is skipped above.
    uint64_t completed = 0;
    for (int i = 0; i < gpu.currentMonitorCount; ++i) {
        if (!gpu.screens[i].renderFence) continue;
        // Any monitor's completed value works as a lower bound here; the counts are diagnostics and
        // reading them one frame late is harmless. Using the global fence keeps this free of a
        // monitorId parameter the caller would otherwise have to thread through.
        completed = (std::max)(completed, gpu.screens[i].renderFence->GetCompletedValue());
    }
    if (completed == UINT64_MAX) return;          // Device lost; nothing to read.
    if (completed < cullScratch.readbackFence) return; // Not retired yet - try again next frame.

    const uint32_t slots = (std::min)(cullScratch.readbackSlotsPending,
        SceneCullScratch::kCountReadbackSlots);
    uint32_t* counts = nullptr;
    CD3DX12_RANGE readRange(0, slots * sizeof(uint32_t));
    if (FAILED(cullScratch.countReadback->Map(0, &readRange, reinterpret_cast<void**>(&counts)))) {
        cullScratch.readbackFence = 0;
        return;
    }
    uint64_t drawn = 0;
    uint64_t overflows = 0;
    for (uint32_t i = 0; i < slots; ++i) {
        // The shader lets the count run past the cap on purpose so the overflow is visible here;
        // the draw itself was clamped by MaxCommandCount, so only the excess was lost.
        if (counts[i] > SceneCullScratch::kMaxCommands) ++overflows;
        drawn += (std::min)(counts[i], SceneCullScratch::kMaxCommands);
    }
    CD3DX12_RANGE noWrite(0, 0);
    cullScratch.countReadback->Unmap(0, &noWrite);

    gRenderStats.drawnCommands.store(drawn, std::memory_order_relaxed);
    if (overflows) gRenderStats.commandOverflows.fetch_add(overflows, std::memory_order_relaxed);
    cullScratch.readbackFence = 0;
}

// Tag the counts recorded during this frame with the fence they must wait on. Mirrors the pick
// pass's FinalizePickFence, and for the same reason: the fence value is not known until after the
// frame has been submitted and signalled.
void FinalizeVisibleCountFence(SceneCullScratch& cullScratch, uint64_t frameFenceValue) {
    if (cullScratch.readbackSlotsRecorded == 0) return;
    cullScratch.readbackSlotsPending = cullScratch.readbackSlotsRecorded;
    cullScratch.readbackFence = frameFenceValue;
}

/* (Re)allocate a page's ExecuteIndirect argument buffer to hold at least `commands`, and record the
capacity. Replacing an existing buffer here is safe ONLY because every caller is on an unpublished
page - a fresh page, or an RCU clone before PublishPages - so no render thread can hold the old one.
Never call this on a page that is already published.

Rounds up to a power of two so a page that keeps growing reallocates O(log n) times, not per append. */
static void AllocateIndirectBuffer(GeometryPage& page, uint32_t commands) {
    uint32_t capacity = kIndirectInitialBytes / sizeof(IndirectCommand);
    while (capacity < commands) capacity *= 2;
    if (page.indirectBuffer && page.indirectCapacity >= capacity) return;
    if (page.indirectBuffer) gCopyStats.indirectGrowths.fetch_add(1, std::memory_order_relaxed);

    CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
    auto indirectDesc = CD3DX12_RESOURCE_DESC::Buffer(
        static_cast<uint64_t>(capacity) * sizeof(IndirectCommand));
    ThrowIfFailed(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &indirectDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&page.indirectBuffer)));
    page.indirectCapacity = capacity;
}

std::unique_ptr<GeometryPage> CreateNewPage(uint64_t containerMemoryId, GeometryPageKind kind)
//Do not make this static function. It accesses global gpu singleton.
{
    auto page = std::make_unique<GeometryPage>();
    page->kind = kind;
    page->containerMemoryId = containerMemoryId;

    /* An INSTANCED page has no geometry buffer: its objects draw from gpu.primitiveLibrary, so the
    only GPU resource it needs is the draw-template argument buffer allocated below. pageSize,
    vertexHead and indexTail all stay 0, which is what makes `holeBytes += vertexSize + indexSize`
    and the compaction arithmetic trivially correct for it - an instanced object occupies no bytes,
    and its placement record says so literally. ~256 KB against ~4.25 MB. */
    if (kind == GeometryPageKind::Bespoke) {
        page->pageSize = 4 * 1024 * 1024;
        page->indexTail = page->pageSize;

        CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
        auto desc = CD3DX12_RESOURCE_DESC::Buffer(page->pageSize);
        // ThrowIfFailed: a silent failure here (VRAM exhaustion) returns a page with null buffers,
        // which crashes later at CopyResource with no indication of the real cause. The copy thread
        // catches this exception and drops the batch instead of aborting.
        ThrowIfFailed(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&page->buffer)));
    }
    AllocateIndirectBuffer(*page, kIndirectInitialBytes / sizeof(IndirectCommand));

    static std::atomic<uint64_t> totalPages = 0; //Telemetry helper / counter.
    //std::wcout << "New page allocated. New Page Counter: " << ++totalPages << std::endl;
    return page;
}

D3D12_VERTEX_BUFFER_VIEW PageVertexBufferView(const GeometryPage& page) {
    // An instanced page owns no buffer and names the global primitive library instead. That single
    // substitution is the whole of the draw-side change for shared geometry - command format, PSO,
    // vertex shader and compute shader are all untouched, and a Viewport is still ONE
    // ExecuteIndirect however its pages are mixed, because both kinds write into the same output
    // buffer under the same counter.
    if (page.kind == GeometryPageKind::InstancedGlobal) return gpu.primitiveLibrary.VertexView();
    D3D12_VERTEX_BUFFER_VIEW vbv{};
    vbv.BufferLocation = page.buffer->GetGPUVirtualAddress();
    vbv.SizeInBytes = page.vertexHead;
    vbv.StrideInBytes = sizeof(Vertex);
    return vbv;
}

D3D12_INDEX_BUFFER_VIEW PageIndexBufferView(const GeometryPage& page) {
    if (page.kind == GeometryPageKind::InstancedGlobal) return gpu.primitiveLibrary.IndexView();
    D3D12_INDEX_BUFFER_VIEW ibv{};
    // Bind at the PAGE BASE covering the whole page, so StartIndexLocation is absolute
    // (indexByteOffset / 2) and stable across appends - indexTail moves down on every append, which
    // would silently invalidate an indexTail-relative start index (graphics.md, 10M plan Step 7,
    // constraint 1). The low-byte overlap with the vertex region is harmless: no draw references
    // indices down there.
    ibv.BufferLocation = page.buffer->GetGPUVirtualAddress();
    ibv.SizeInBytes = page.pageSize;
    ibv.Format = DXGI_FORMAT_R16_UINT;
    return ibv;
}

void InitPrimitiveLibrary(ID3D12Device* device) {
    PrimitiveLibrary& library = gpu.primitiveLibrary;
    if (library.IsReady()) return;

    std::vector<Vertex> vertices;
    std::vector<uint16_t> indices;
    BuildPrimitiveLibraryMesh(vertices, indices, library.table);

    library.vertexBytes = static_cast<uint32_t>(vertices.size() * sizeof(Vertex));
    library.indexBytes = static_cast<uint32_t>(indices.size() * sizeof(uint16_t));
    // The index region starts on a 256-byte boundary so both views are comfortably aligned; the
    // gap is a handful of bytes against ~123 KB.
    library.indexByteOffset = GeometryPage::AlignUp(library.vertexBytes, 256);
    /* The (shapeId, lod) draw-range table rides in the SAME buffer, after the indices, so the whole
    library is one resource and one upload. It is bound as its own root SRV at its own offset, which
    is why it needs no descriptor - a root SRV is just an address. */
    constexpr uint32_t kDrawRangeCount = kPrimitiveShapeCount * kPrimitiveLodCount;
    const uint32_t drawRangeOffset =
        GeometryPage::AlignUp(library.indexByteOffset + library.indexBytes, 256);
    const uint32_t drawRangeBytes = kDrawRangeCount * sizeof(PrimitiveLibraryDrawRange);
    const uint64_t totalBytes = static_cast<uint64_t>(drawRangeOffset) + drawRangeBytes;

    CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
    auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(totalBytes);
    ThrowIfFailed(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&library.buffer)));
    library.buffer->SetName(L"PrimitiveLibrary");
    library.va = library.buffer->GetGPUVirtualAddress();
    library.drawRangeBuffer = library.buffer;   // Same resource, later offset.
    library.drawRangeVA = library.va + drawRangeOffset;

    /* A one-off committed upload plus its own allocator and command list, rather than the shared
    upload ring: this runs before the copy thread exists, so the ring's fence bookkeeping has no
    owner yet, and paying for a throwaway 123 KB staging buffer once at startup is cheaper than the
    ordering argument the alternative would need. */
    CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
    ComPtr<ID3D12Resource> staging;
    ThrowIfFailed(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&staging)));
    uint8_t* mapped = nullptr;
    CD3DX12_RANGE readRange(0, 0);
    ThrowIfFailed(staging->Map(0, &readRange, reinterpret_cast<void**>(&mapped)));
    memcpy(mapped, vertices.data(), library.vertexBytes);
    memcpy(mapped + library.indexByteOffset, indices.data(), library.indexBytes);
    PrimitiveLibraryDrawRange drawRanges[kDrawRangeCount] = {};
    library.table.ToDrawRanges(drawRanges);
    memcpy(mapped + drawRangeOffset, drawRanges, drawRangeBytes);
    staging->Unmap(0, nullptr);

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ThrowIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY,
        IID_PPV_ARGS(&allocator)));
    ThrowIfFailed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, allocator.Get(),
        nullptr, IID_PPV_ARGS(&commandList)));
    commandList->CopyBufferRegion(library.buffer.Get(), 0, staging.Get(), 0, totalBytes);
    ThrowIfFailed(commandList->Close());
    ID3D12CommandList* lists[] = { commandList.Get() };
    gpu.copyCommandQueue->ExecuteCommandLists(1, lists);

    // Block until it lands. The library is immutable and every tab reads it, so finishing here
    // means no later path ever has to wonder whether it is resident.
    const uint64_t fenceValue = gpu.copyFenceValue.fetch_add(1);
    ThrowIfFailed(gpu.copyCommandQueue->Signal(gpu.copyFence.Get(), fenceValue));
    if (gpu.copyFence->GetCompletedValue() < fenceValue) {
        ThrowIfFailed(gpu.copyFence->SetEventOnCompletion(fenceValue, gpu.copyFenceEvent));
        WaitForSingleObject(gpu.copyFenceEvent, INFINITE);
    }

    std::wcout << L"Primitive library: " << vertices.size() << L" vertices, " << indices.size()
               << L" indices, " << (totalBytes / 1024) << L" KB." << std::endl;
}

void शंकर::RenderScene3D(ID3D12GraphicsCommandList* commandList,
    DX12ResourcesPerWindow& winRes, const DX12ResourcesPerTab& tabRes, TabGeometryStorage& storage,
    const CameraState& camera, int monitorId, const SubTabContainerSet& containers,
    uint32_t subTabBit, SceneCullScratch& cullScratch) {
    // Update constant buffer with transformation matrices

    // Create view matrix (camera looking at scene from distance)
    XMVECTOR eyePosition = XMLoadFloat3(&camera.position);
    XMVECTOR focusPoint = XMLoadFloat3(&camera.target);
    XMVECTOR upDirection = XMLoadFloat3(&camera.up);
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
        camera.fov, aspectRatio, camera.nearZ, camera.farZ);
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
    // Instance arena (t0) is bound below. Its VA is fixed for the tab's lifetime since Step 2 -
    // growth commits tiles behind the same address - so there is no ordering constraint left here.

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
    if (snapshot && tabRes.instanceArena.va != 0) {
        commandList->SetGraphicsRootShaderResourceView(1, tabRes.instanceArena.va);
        commandList->SetGraphicsRootShaderResourceView(3, tabRes.instanceSlotOf.va);
        // Per-object hide/show (10M plan Step 5). The bit identifies which SubTab is being drawn;
        // it is set once for the whole view, unlike b1 which ExecuteIndirect rewrites per command.
        commandList->SetGraphicsRootShaderResourceView(4, tabRes.visibilityMask.va);
        commandList->SetGraphicsRoot32BitConstant(5, subTabBit, 0);

        SceneCullScratch& s = cullScratch;
        auto barrier = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES& cur,
            D3D12_RESOURCE_STATES next) {
                if (cur == next) return;
                auto b = CD3DX12_RESOURCE_BARRIER::Transition(r, cur, next);
                commandList->ResourceBarrier(1, &b);
                cur = next;
            };

        /* Walk only the containers this SubTab shows, via the snapshot's directory (10M plan
        Step 6, items 3-4). This replaces "visit every page, bind its buffers, then discover the
        argument count is 0": the container test now happens ABOVE the two IA binds instead of
        after them, and pages belonging to other containers are never touched at all. */
        auto ForEachPage = [&](auto&& visit) {
            for (uint8_t c = 0; c < containers.count; ++c) {
                auto containerPages = snapshot->pagesByContainer.find(containers.ids[c]);
                if (containerPages == snapshot->pagesByContainer.end()) continue;
                for (GeometryPage* pagePtr : containerPages->second) {
                    if (!PageIsRenderable(*pagePtr)) continue;
                    visit(*pagePtr);
                }
            }
            };

        // One page's buffer views, shared by both draw paths and by the pick, highlight and print
        // paths so they can never drift apart: the legacy path binds them on the IA, the compute
        // path copies them into every command it emits.
        auto PageVertexView = [](const GeometryPage& page) { return PageVertexBufferView(page); };
        auto PageIndexView = [](const GeometryPage& page) { return PageIndexBufferView(page); };

        if (!gUseComputeCull) {
            /* LEGACY PATH: one ExecuteIndirect per page over that page's full template list, with
            its buffers bound on the IA first. Hidden objects are still drawn here and collapse to a
            degenerate primitive in the vertex shader - the interim Step 5 cost the compute path
            below removes. Kept as a second maintained path: it is the A/B reference the compute
            path is checked against, and the fallback if per-command buffer views ever misbehave on
            some driver. Any change to what gets drawn must be made in BOTH branches. */
            ForEachPage([&](GeometryPage& page) {
                const D3D12_VERTEX_BUFFER_VIEW vbv = PageVertexView(page);
                const D3D12_INDEX_BUFFER_VIEW ibv = PageIndexView(page);
                commandList->IASetVertexBuffers(0, 1, &vbv);
                commandList->IASetIndexBuffer(&ibv);
                commandList->ExecuteIndirect(tabRes.commandSignature.Get(),
                    page.indirectCount, page.indirectBuffer.Get(), 0, nullptr, 0);
                });
            return;
        }

        /* GPU DRAW-COMMAND COMPACTION - ONE ExecuteIndirect FOR THE WHOLE VIEWPORT
        (graphics.md, 10M plan Step 7).

        A compute pass per page copies only the templates whose VisibilityMask bit for this SubTab
        is set, all into ONE per-monitor output buffer sharing ONE count. Hidden and filtered
        objects are dropped before they become draw commands rather than vertex-shaded into
        degenerates, and - because each emitted command carries its own page's vertex/index views -
        every page's survivors can be drawn by a single call with no IA binds at all.

        Three things this deliberately does NOT do, each of which the per-page form used to:
          - no barrier between the page dispatches. They touch the count only through
            InterlockedAdd, so they are order-independent and free to overlap; command order within
            the buffer therefore varies, which is immaterial for depth-tested opaque draws.
          - no count reset per page. One reset for the whole Viewport, before the first dispatch.
          - no per-page IASetVertexBuffers / IASetIndexBuffer.

        The compute root signature and its arguments are independent of the graphics root signature
        + arguments already bound above, so only the shared pipeline-state slot has to be restored
        before the draw. Viewport/scissor are RS state and are untouched by the dispatch. */

        // Reset the Viewport's visible-command count to 0 (root-descriptor-only clear - ClearUAV
        // would need both a CPU and a shader-visible descriptor, which this path does without).
        barrier(s.visibleCount.Get(), s.countState, D3D12_RESOURCE_STATE_COPY_DEST);
        commandList->CopyBufferRegion(s.visibleCount.Get(), 0, gpu.cullZeroBuffer.Get(), 0,
            sizeof(uint32_t));
        barrier(s.visibleCount.Get(), s.countState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        barrier(s.visibleIndirect.Get(), s.visibleState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        // Page-invariant compute bindings, hoisted out of the loop. Only the templates SRV and the
        // root constants change per page below.
        commandList->SetComputeRootSignature(gpu.sceneCullRootSignature.Get());
        commandList->SetPipelineState(gpu.sceneCullPSO.Get());
        commandList->SetComputeRootShaderResourceView(2, tabRes.visibilityMask.va);   // t1 mask
        // Per-frame LOD needs the object's transform, reached the same two-load way the scene vertex
        // shader reaches it, plus the library's (shapeId, lod) table. All three addresses are fixed
        // for the tab's / process's lifetime, so they bind once per Viewport and never per page.
        commandList->SetComputeRootShaderResourceView(5, tabRes.instanceSlotOf.va);   // t2 redirect
        commandList->SetComputeRootShaderResourceView(6, tabRes.instanceArena.va);    // t3 arena
        commandList->SetComputeRootShaderResourceView(7,
            gpu.primitiveLibrary.drawRangeVA);                                        // t4 LOD table
        commandList->SetComputeRootUnorderedAccessView(3,
            s.visibleIndirect->GetGPUVirtualAddress());                               // u0 output
        commandList->SetComputeRootUnorderedAccessView(4,
            s.visibleCount->GetGPUVirtualAddress());                                  // u1 count

        /* Per-Viewport LOD inputs. focalPixels converts a world radius into a screen radius:
        projectedDiameterPx = 2 * radius * focalPixels / distance. `sceneHeight` is the scene area
        below the top UI band, i.e. the pixels this Viewport actually rasterises into, which is why
        resolution and FOV both fold in here rather than being re-derived in the shader.

        gLodPinned makes it non-positive, which the shader reads as "keep the level the CPU already
        chose" - that is the whole implementation of the LOD debug toggle. */
        const float focalPixels = gLodPinned ? -1.0f
            : static_cast<float>(sceneHeight) / (2.0f * tanf(camera.fov * 0.5f));

        uint32_t totalTemplates = 0; // Upper bound on surviving commands, for MaxCommandCount.
        ForEachPage([&](GeometryPage& page) {
            const D3D12_VERTEX_BUFFER_VIEW vbv = PageVertexView(page);
            const D3D12_INDEX_BUFFER_VIEW ibv = PageIndexView(page);

            SceneCullConstants constants{};
            constants.cameraX = camera.position.x;
            constants.cameraY = camera.position.y;
            constants.cameraZ = camera.position.z;
            constants.focalPixels = focalPixels;
            constants.templateCount = page.indirectCount;
            constants.subTabBit = subTabBit;
            constants.maxCommands = SceneCullScratch::kMaxCommands;
            constants.vertexAddressLo = static_cast<uint32_t>(vbv.BufferLocation);
            constants.vertexAddressHi = static_cast<uint32_t>(vbv.BufferLocation >> 32);
            constants.vertexSizeInBytes = vbv.SizeInBytes;
            constants.vertexStrideInBytes = vbv.StrideInBytes;
            constants.indexAddressLo = static_cast<uint32_t>(ibv.BufferLocation);
            constants.indexAddressHi = static_cast<uint32_t>(ibv.BufferLocation >> 32);
            constants.indexSizeInBytes = ibv.SizeInBytes;
            constants.indexFormat = static_cast<uint32_t>(ibv.Format);

            commandList->SetComputeRoot32BitConstants(0, kSceneCullConstantCount, &constants, 0);
            commandList->SetComputeRootShaderResourceView(1,
                page.indirectBuffer->GetGPUVirtualAddress());                         // t0 templates
            commandList->Dispatch((page.indirectCount + 63) / 64, 1, 1);

            totalTemplates += page.indirectCount;
            });

        // Nothing dispatched means nothing to draw. Leaving the buffers in UNORDERED_ACCESS is
        // fine: the next Viewport (or the next frame's COMMON decay) transitions them again.
        if (totalTemplates == 0) return;

        /* Snapshot this Viewport's surviving-command count for telemetry (10M plan Step 7). It has
        to happen HERE, between the dispatches and the draw, because the count is reset by the next
        Viewport. The detour through COPY_SOURCE replaces the single UAV -> INDIRECT_ARGUMENT
        transition below with two barriers on a 4-byte buffer, which is why it is affordable to
        leave on in release builds - and a counter that only exists in debug catches nothing. */
        if (s.countReadback &&
            s.readbackSlotsRecorded < SceneCullScratch::kCountReadbackSlots) {
            barrier(s.visibleCount.Get(), s.countState, D3D12_RESOURCE_STATE_COPY_SOURCE);
            commandList->CopyBufferRegion(s.countReadback.Get(),
                s.readbackSlotsRecorded * sizeof(uint32_t), s.visibleCount.Get(), 0,
                sizeof(uint32_t));
            ++s.readbackSlotsRecorded;
        }

        // Hand the compacted commands + count to the draw as indirect arguments.
        barrier(s.visibleIndirect.Get(), s.visibleState, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        barrier(s.visibleCount.Get(), s.countState, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);

        // Restore graphics PSO (root signature + its arguments were never disturbed). ONE call for
        // the whole Viewport. MaxCommandCount caps the draw independently of the GPU-written count,
        // so a count the shader deliberately let overflow past kMaxCommands cannot read past the
        // buffer - ExecuteIndirect executes min(MaxCommandCount, count) commands.
        commandList->SetPipelineState(tabRes.pipelineState.Get());
        commandList->ExecuteIndirect(tabRes.visibleCommandSignature.Get(),
            (std::min)(totalTemplates, SceneCullScratch::kMaxCommands),
            s.visibleIndirect.Get(), 0, s.visibleCount.Get(), 0);
    } // End of if (snapshot)
    // TODO: Add support for transparent pages with proper sorting and blending states.
    // TODO: Similarly for all varients of geometry, like wireframe, hugoObjects etc. all unique PSO.

    /* Transition from D3D12_RESOURCE_STATE_RENDER_TARGET to D3D12_RESOURCE_STATE_RENDER_PRESENT
    taken care by parent function. i.e. Render Thread */
    //commandList->Close();// No longer required here. It will be done by render thread.

    //Following mutex will release automatically when this function returns, i.e. when lock goes out of scope.
    //std::lock_guard<std::mutex> lock(tabRes.objectsOnGPUMutex);
}

// Copy-thread-only: full teardown of one closed tab's Scene3D geometry. GpuCopyThread calls this
// once every monitor's render fence has passed the tab's release fence (no submitted frame can
// still reference these pages), and again from its shutdown path after render threads joined.
void ReleaseTabGpuGeometry(DATASETTAB& tab) {
    TabGeometryStorage& storage = tab.geometry;

    // The identity registry is per tab now (10M plan Step 3), so purging it is a clear() instead
    // of the scan over one global map that the shared objectLocation table used to need. It must
    // happen here, not in CleanupTabResources: every page pointer it holds dies below, and the
    // shutdown path calls this function without calling CleanupTabResources at all.
    tab.dx.registry.Clear();
    tab.dx.instanceCount = 0;
    tab.dx.freeInstanceIndexes.clear();
    tab.dx.pendingFreeInstanceIndexes.clear();
    tab.dx.hiddenInstanceMasks.clear();

    GeometryPageSnapshot* snapshot =
        storage.activeSnapshot.exchange(nullptr, std::memory_order_acq_rel);
    delete snapshot;
    for (auto& retired : storage.retiredSnapshots) delete retired.snapshot;
    storage.retiredSnapshots.clear();
    storage.retiredPages.clear(); // unique_ptr<GeometryPage> releases the VRAM buffers.
    storage.activePages.clear();
}

// Refresh the per-tab srvHeap descriptor so it covers exactly the arena's committed range: on
// Tier 1 a read of an unmapped tile is undefined, so the view must never advertise more than is
// backed. Unused by today's root-SRV draw paths; it is what the Step 7 cull dispatch will bind.
static void RefreshInstanceArenaSrv(DX12ResourcesPerTab& tabRes) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srvView = {};
    srvView.Format = DXGI_FORMAT_UNKNOWN;
    srvView.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvView.Buffer.FirstElement = 0;
    srvView.Buffer.NumElements = tabRes.instanceArena.capacity;
    srvView.Buffer.StructureByteStride = kInstanceRecordBytes;
    gpu.device->CreateShaderResourceView(tabRes.instanceArena.resource.Get(), &srvView,
        tabRes.srvHeap->GetCPUDescriptorHandleForHeapStart());
}

void GrowInstanceArena(DX12ResourcesPerTab& tabRes, uint32_t minimumCapacity) {
    if (tabRes.instanceArena.Grow(minimumCapacity)) RefreshInstanceArenaSrv(tabRes);
}

/* Copy-thread-only: the next free gpuInstanceIndex - the object's stable renderer IDENTITY.
Reuse comes only from freeInstanceIndexes, which the safeRetireFence sweep fills; an index inside
its zombie interval is never handed out (10M plan Step 3). */
static uint32_t AllocateInstanceIndex(DX12ResourcesPerTab& tabRes) {
    if (!tabRes.freeInstanceIndexes.empty()) {
        const uint32_t reused = tabRes.freeInstanceIndexes.back();
        tabRes.freeInstanceIndexes.pop_back();
        return reused;
    }
    // Both index-addressed buffers grow together. They are bound as ROOT descriptors, which carry
    // no bounds check whatsoever, so a shader read past the committed tiles is undefined rather
    // than clamped - the mask must never lag the redirect table (10M plan Step 5).
    if (tabRes.instanceCount >= tabRes.instanceSlotOf.capacity) {
        tabRes.instanceSlotOf.Grow(tabRes.instanceCount + 1);
    }
    if (tabRes.instanceCount >= tabRes.visibilityMask.capacity) {
        tabRes.visibilityMask.Grow(tabRes.instanceCount + 1);
    }
    const uint32_t index = tabRes.instanceCount++;
    tabRes.registry.Commit(tabRes.instanceCount);
    return index;
}

/* Append targets are chosen per (container, page kind): the two kinds never share a page, because a
page carries ONE vertex/index buffer view for all of its templates. Ordered rather than hashed so a
std::pair needs no hash specialisation; there are a handful of these per chunk. */
using AppendTargetKey = std::pair<uint64_t, GeometryPageKind>;

// Which page kind a command's geometry belongs in. An object naming a library shape brings no bytes
// and goes in a template-only page; everything else owns its vertices.
static GeometryPageKind PageKindFor(const CommandToCopyThread& command) {
    return IsInstancedGeometry(command) ? GeometryPageKind::InstancedGlobal
                                        : GeometryPageKind::Bespoke;
}

/* Copy-thread-only: the next free instanceSlot - a physical LOCATION in the arena, not an identity.
Every transform edit burns one, so this churns far faster than the index allocator; slots come back
through the same fence-gated sweep (10M plan Step 4). */
static uint32_t AllocateInstanceSlot(DX12ResourcesPerTab& tabRes) {
    if (!tabRes.freeInstanceSlots.empty()) {
        const uint32_t reused = tabRes.freeInstanceSlots.back();
        tabRes.freeInstanceSlots.pop_back();
        return reused;
    }
    if (tabRes.instanceSlotCount >= tabRes.instanceArena.capacity) {
        GrowInstanceArena(tabRes, tabRes.instanceSlotCount + 1);
    }
    return tabRes.instanceSlotCount++;
}

// 3D-geometry half of the copy thread: applies one drained batch of ADD/MODIFY/REMOVE commands
// to the per-tab GeometryPages (RCU clone -> mutate -> publish), then atomically publishes the
// new page snapshot. Mirrors ProcessCad2DCopyBatch (RenderPage2D-DirectX12.cpp). The COPY-type
// commandAllocator/commandList stay owned by GpuCopyThread and are passed in for recording.
//
// The batch is split into CHUNKS whose staging fits the global upload ring, and each chunk is a
// SINGLE command-list recording - clone, geometry uploads and argument rebuilds together -
// followed by one execute, one fence wait and its own publish (graphics.md, 10M plan Step 0).
// The copy queue executes strictly in order, so a page clone completes before the
// CopyBufferRegions that write into it; the three record/execute/CPU-wait cycles this function
// used to perform per tab were unnecessary. Publishing per chunk also lets a bulk import appear
// progressively instead of freezing until the whole batch lands, and the CPU wait at the end of
// each chunk is not a stall to optimise away - it is the back-pressure that keeps staging bounded.
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
                        // so dropping it frees its ~4.25 MB now instead of parking a dead page in
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
                // Container -> page directory, built once here so no render thread ever has to
                // scan for it (10M plan Step 6, item 4). Pages never mix containers, so each page
                // lands in exactly one bucket.
                newSnapshot->pagesByContainer[pagePtr->containerMemoryId].push_back(pagePtr.get());
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

        for (uint16_t tabID : sortedTabs) { //Process 1 tab at a time.
            bool tabTouched = false;

            DATASETTAB& targetTab = allTabs[tabID];
            TabGeometryStorage& storage = targetTab.geometry;
            DX12ResourcesPerTab& tabRes = targetTab.dx;

            std::unordered_set<GeometryPage*> affectedPages;
            std::unordered_map<GeometryPage*, std::unique_ptr<GeometryPage>> clonedPages;
            std::vector<std::unique_ptr<GeometryPage>> newPages;
            std::set<AppendTargetKey> containersNeedingAppend;

            // Pre-Pass: Deduplicate commands for this tab. If the same object is modified twice,
            // or added then modified, etc., we need only FINAL state in this batch to persist.
            std::unordered_map<uint64_t, size_t> latestCommandIndex;
            for (size_t i = 0; i < batch.size(); ++i) {
                // Visibility commands are excluded deliberately. This map keys on `id` alone, so an
                // ADD and a hide of the same object in one batch would collapse into whichever came
                // last - and if that were the hide, the geometry would silently never be uploaded
                // (10M plan Step 5). CLEAR_SUBTAB_HIDES carries id 0 and would collide with every
                // other id-0 command besides.
                if (IsVisibilityCommand(batch[i])) continue;
                if (batch[i].tabID == tabID) { latestCommandIndex[batch[i].id] = i; }
            }

            // Pointers, not copies: every command carries a GeometryData with two heap vectors, so
            // copying the batch here would deep-copy every object's geometry a second time.
            std::vector<const CommandToCopyThread*> deduplicatedBatch;
            deduplicatedBatch.reserve(batch.size());
            for (size_t i = 0; i < batch.size(); ++i) {
                if (batch[i].tabID != tabID) continue;
                // Every mask command survives, in order: each names ONE SubTab bit rather than a
                // whole membership word, so collapsing two of them would drop a bit change.
                if (IsVisibilityCommand(batch[i])) { deduplicatedBatch.push_back(&batch[i]); continue; }
                // Only keep the command if it is the absolute latest operation for this ID
                if (latestCommandIndex[batch[i].id] == i) { deduplicatedBatch.push_back(&batch[i]); }
            }

            // Update the tabTouched flag based on our deduplicated list
            if (deduplicatedBatch.empty()) continue; // No command for this tab. Skip this tab.
            tabTouched = true;

            // A chunk's staging must fit the ring. Reserve headroom for the per-page argument
            // rebuilds recorded after the geometry: a densely packed 4 MB page rebuilds in
            // ~155 KB, and a 64 MB chunk of the smallest objects spans only ~16 pages.
            constexpr uint64_t kArgumentStagingReserve = 8ull * 1024 * 1024;
            const uint64_t chunkBudget = GpuUploadRing::kCapacity - kArgumentStagingReserve;

        // Chunk loop. Deliberately left at the tab loop's indent so this change reads as a logic
        // diff rather than a 400-line re-indent; its body is everything down to "End of chunk loop".
        for (size_t chunkStart = 0; chunkStart < deduplicatedBatch.size(); ) {
            size_t chunkEnd = chunkStart;
            for (uint64_t chunkBytes = 0; chunkEnd < deduplicatedBatch.size(); ++chunkEnd) {
                const uint64_t cost = EstimateStagingBytes(*deduplicatedBatch[chunkEnd]);
                // Always take at least one command however large it is: the oversize staging
                // fallback covers a payload bigger than the whole ring, so it cannot stall here.
                if (chunkEnd > chunkStart && chunkBytes + cost > chunkBudget) break;
                chunkBytes += cost;
            }

            // Per-chunk page bookkeeping. A chunk publishes on its own, so nothing carries over
            // except the registry (which now points at the pages this chunk published).
            affectedPages.clear();
            clonedPages.clear();
            newPages.clear();

            // Pass 1: Identify affected pages. We will clone these pages,
            //apply modifications to the clones, and then publish atomically.
            for (size_t ci = chunkStart; ci < chunkEnd; ++ci) {
                const CommandToCopyThread& cmd = *deduplicatedBatch[ci];
                if (cmd.type == CommandToCopyThreadType::ADD) continue; // handled later
                // A mask write never reads or relocates geometry, so pulling its object's page in
                // here would clone 4 MB to change 8 bytes - the exact cost this step exists to
                // avoid (10M plan Step 5).
                if (IsVisibilityCommand(cmd)) continue;
                /* Same reasoning for a MOVE (10M plan Step 4): it writes one fresh instance record
                and flips one redirect entry, and touches nothing inside the geometry page - not the
                bytes, not the placement records, not the indirect arguments. Cloning the page would
                copy ~4.25 MB only to publish a byte-identical replacement and retire the original.
                This line is what makes Step 4's stated criterion - "moving N scattered objects
                clones zero geometry pages" - actually true. */
                if (IsTransformOnlyEdit(cmd)) continue;
                const uint32_t existing = tabRes.registry.Find(cmd.id);
                if (existing != kInvalidInstanceIndex) {
                    affectedPages.insert(tabRes.registry[existing].page);
                }
            }

            // Find the best append target for each (container, page kind) receiving geometry.
            // Pages never mix containers, and never mix kinds.
            containersNeedingAppend.clear();
            for (size_t ci = chunkStart; ci < chunkEnd; ++ci) {
                const CommandToCopyThread& cmd = *deduplicatedBatch[ci];
                if (!cmd.geometry.has_value()) continue;
                /* The second half of the move fix, and the easier one to miss: a transform-only
                edit DOES carry a GeometryData - that is how it smuggles the world matrix - so
                has_value() above is true for it. It appends no bytes, though, so letting it name a
                container here force-clones that container's append-target page a few lines down,
                and a chunk of pure moves would still clone one page per container. */
                if (IsTransformOnlyEdit(cmd)) continue;
                uint64_t containerMemoryId = cmd.containerMemoryId;
                const uint32_t existing = tabRes.registry.Find(cmd.id);
                if (containerMemoryId == 0 && existing != kInvalidInstanceIndex) {
                    containerMemoryId = tabRes.registry[existing].page->containerMemoryId;
                }
                containersNeedingAppend.insert({ containerMemoryId, PageKindFor(cmd) });
            }

            /* Append targets are per (container, KIND), not per container. The two kinds live in
            separate pages, so a container receiving both a bespoke and an instanced object this
            chunk needs one append target of each - keying on the container alone would route an
            instanced object into a bespoke page's clone and silently draw it from the wrong
            buffer. std::map rather than unordered_map only because a pair needs no hash written. */
            std::map<AppendTargetKey, GeometryPage*> bestAppendCandidates;
            std::map<AppendTargetKey, size_t> maxHoleByTarget;
            for (const auto& pagePtr : storage.activePages) {
                GeometryPage* p = pagePtr.get();
                if (!p->published.load(std::memory_order_acquire)) continue; //Just for extra safety.
                const AppendTargetKey key{ p->containerMemoryId, p->kind };
                if (containersNeedingAppend.find(key) == containersNeedingAppend.end()) continue;

                // "Largest middle gap" is a vertex/index-region idea. An instanced page has no such
                // region, so what decides there is remaining TEMPLATE capacity.
                const size_t hole = p->kind == GeometryPageKind::InstancedGlobal
                    ? (p->indirectCapacity > p->objects.size()
                        ? p->indirectCapacity - p->objects.size() : 0)
                    : static_cast<size_t>(p->indexTail - p->vertexHead);
                size_t& maxHole = maxHoleByTarget[key];
                if (hole > maxHole) {
                    maxHole = hole;
                    bestAppendCandidates[key] = p;
                }
            }
            // Force-clone each append candidate so Pass 3 never mutates a published page.
            for (const auto& [key, candidate] : bestAppendCandidates) {
                affectedPages.insert(candidate);
            }

            commandAllocator->Reset(); // Prepare command allocator for more work !
            commandList->Reset(commandAllocator.Get(), nullptr); // Opens command list for closing.

            /* Pass 2: Clone Affected Pages (RCU copy).

            The clone's ExecuteIndirect argument buffer is deliberately NOT copied. Every cloned and
            every new page goes through RebuildIndirectBuffer unconditionally below, so copying the
            old one first moved 1.5 MB that was immediately overwritten - and it contradicted the
            standing rule that argument buffers are regenerated per clone, never patched. Only the
            first indirectCount commands are ever read, so the rest of the fresh buffer staying
            uninitialised is fine. */
            bool compactedThisChunk = false; // At most one page compacts per chunk - see below.
            for (GeometryPage* oldPage : affectedPages) {
                auto clonedPage = CreateNewPage(oldPage->containerMemoryId, oldPage->kind);

                /* An INSTANCED page has no geometry buffer to copy - only its CPU placement records
                and the draw templates rebuilt from them below. So the RCU clone of a page backing
                100,000 spheres moves ZERO bytes on the GPU, which is what makes adding one sphere
                to such a scene cost ~256 KB instead of 4.25 MB. Compaction has nothing to do here
                either: an instanced object occupies no bytes, so a page of them accrues no holes. */
                if (oldPage->kind == GeometryPageKind::InstancedGlobal) {
                    clonedPage->objects = oldPage->objects;
                    clonedPage->objectCount = oldPage->objectCount;
                    clonedPage->version = oldPage->version + 1;
                    clonedPages[oldPage] = std::move(clonedPage);
                    continue;
                }

                /* PAGE COMPACTION (graphics.md, "Defragmentation logic"). A page whose holes have
                crossed the threshold is not copied wholesale: its clone is filled by copying each
                LIVE object's vertex and index ranges into tightly packed offsets, and the deleted
                records are dropped. Byte contents never change - indices are object-relative and
                resolved per draw through BaseVertexLocation / StartIndexLocation - so only the CPU
                offsets need remapping, and the argument rebuild every clone performs anyway picks
                those up for free. No freeze, no resource-state gymnastics: it rides the RCU clone
                that was already going to happen.

                One page per chunk bounds the extra copy volume, and compaction is strictly cheaper
                than the CopyResource it replaces (live bytes <= page size), so this never makes a
                chunk slower - it just trades one big copy for a burst of small ones. */
                const bool compact = !compactedThisChunk &&
                    oldPage->holeBytes > oldPage->pageSize / 4; // ~25% threshold.

                if (compact) {
                    uint32_t vertexHead = 0;
                    uint32_t indexTail = clonedPage->pageSize;
                    clonedPage->objects.reserve(oldPage->objects.size());
                    for (const GeometryPlacementRecordInPage& record : oldPage->objects) {
                        if (record.isDeleted) continue; // Dropping these IS the compaction.
                        GeometryPlacementRecordInPage packed = record;
                        // Same placement rules as a fresh append: whole vertices, 4-byte indices.
                        // Survivors keep their relative order, so a packed offset can never exceed
                        // the original one and the page cannot overflow.
                        packed.vertexByteOffset = GeometryPage::VertexAlign(vertexHead);
                        packed.indexByteOffset =
                            GeometryPage::AlignDown(indexTail - record.indexSize, 4);
                        commandList->CopyBufferRegion(clonedPage->buffer.Get(),
                            packed.vertexByteOffset, oldPage->buffer.Get(),
                            record.vertexByteOffset, record.vertexSize);
                        commandList->CopyBufferRegion(clonedPage->buffer.Get(),
                            packed.indexByteOffset, oldPage->buffer.Get(),
                            record.indexByteOffset, record.indexSize);
                        vertexHead = packed.vertexByteOffset + packed.vertexSize;
                        indexTail = packed.indexByteOffset;
                        clonedPage->objects.push_back(packed);
                    }
                    clonedPage->vertexHead = vertexHead;
                    clonedPage->indexTail = indexTail;
                    clonedPage->holeBytes = 0; // Every hole is gone by construction.
                    compactedThisChunk = true;
                    gCopyStats.pagesCompacted.fetch_add(1, std::memory_order_relaxed);
                    gCopyStats.clonedBytes.fetch_add(
                        static_cast<uint64_t>(vertexHead) + (clonedPage->pageSize - indexTail),
                        std::memory_order_relaxed);
                } else {
                    commandList->CopyResource(clonedPage->buffer.Get(), oldPage->buffer.Get());
                    clonedPage->objects = oldPage->objects; //CPU side metadata copy.
                    clonedPage->vertexHead = oldPage->vertexHead;
                    clonedPage->indexTail = oldPage->indexTail;
                    clonedPage->holeBytes = oldPage->holeBytes;
                    gCopyStats.clonedBytes.fetch_add(oldPage->pageSize, std::memory_order_relaxed);
                }

                clonedPage->objectCount = oldPage->objectCount;
                clonedPage->version = oldPage->version + 1;
                clonedPages[oldPage] = std::move(clonedPage);
            }
            gCopyStats.pagesCloned.fetch_add(affectedPages.size(), std::memory_order_relaxed);

            // Re-point the registry at the clones. Every live record already carries its stable
            // gpuInstanceIndex, so this is a direct array write per object - the identity lookup
            // the old objectLocation map needed here is gone.
            for (auto& [oldRaw, clone] : clonedPages) {
                for (uint32_t i = 0; i < clone->objects.size(); ++i) {
                    auto& obj = clone->objects[i];
                    if (obj.isDeleted) continue;

                    InstanceRegistryEntry& entry = tabRes.registry[obj.gpuInstanceIndex];
                    if (entry.page == oldRaw) {
                        entry.page = clone.get();
                        entry.pageSlot = i;
                    }
                }
            }

            // Route each container's appends at its cloned page. Purely CPU bookkeeping: the clone
            // copies recorded above stay in the same command list as the uploads that follow, and
            // the copy queue runs them in order, so there is nothing to wait for here.
            std::map<AppendTargetKey, GeometryPage*> addTargetPages;
            for (const auto& [key, candidate] : bestAppendCandidates) {
                auto cloneIt = clonedPages.find(candidate);
                addTargetPages[key] = cloneIt != clonedPages.end()
                    ? cloneIt->second.get()
                    : candidate;
            }

            //std::wcout << "activePages: " << storage.activePages.size() << 
            //    ", clonedPages:" << clonedPages.size() << std::endl;

            // Pass 3 — Apply every command in the chunk to the (already-cloned) pages.

            // One-off committed staging for payloads the ring can never hold (a jumbo mesh larger
            // than the whole ring). Kept alive until after ExecuteCommandLists + fence-wait.
            std::vector<ComPtr<ID3D12Resource>> oversizeStaging;

            // All staging for this chunk: the global upload ring, with the oversize fallback so a
            // huge object can never wait for space that will never exist (10M plan Step 0).
            auto AcquireStaging = [&](uint64_t bytes, uint8_t*& outCpu,
                ID3D12Resource*& outResource, uint64_t& outOffset) {
                if (gpu.uploadRing.Allocate(bytes, outCpu, outOffset)) {
                    outResource = gpu.uploadRing.buffer.Get();
                    gCopyStats.ringBytes.fetch_add(bytes, std::memory_order_relaxed);
                    return;
                }
                ComPtr<ID3D12Resource> fallback;
                CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
                auto uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(bytes);
                ThrowIfFailed(gpu.device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE,
                    &uploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&fallback)));
                CD3DX12_RANGE readRange(0, 0);
                ThrowIfFailed(fallback->Map(0, &readRange, reinterpret_cast<void**>(&outCpu)));
                outResource = fallback.Get();
                outOffset = 0;
                oversizeStaging.push_back(std::move(fallback));
                gCopyStats.oversizeStaging.fetch_add(1, std::memory_order_relaxed);
            };

            // Common lambda: write vertex+index data into a page Used by both ADD (to last/new page) and MODIFY-grow paths.
            // Records CopyBufferRegion into the open commandList.Returns the filled-in placement record; caller appends it.
            auto RecordGeometryUpload = [&](GeometryPage* dstPage, const GeometryData& geo,
                uint32_t gpuInstanceIndex) -> GeometryPlacementRecordInPage {
                const uint32_t vertexBytes = static_cast<uint32_t>(geo.vertices.size() * sizeof(Vertex));
                const uint32_t indexBytes = static_cast<uint32_t>(geo.indices.size() * sizeof(uint16_t));

                // Vertex offsets must be a whole number of vertices, NOT merely 16-byte aligned:
                // RebuildIndirectBuffer below divides this by sizeof(Vertex) to get
                // BaseVertexLocation (graphics.md, live defect 1).
                const uint32_t vOffset = GeometryPage::VertexAlign(dstPage->vertexHead);
                const uint32_t iOffset = GeometryPage::AlignDown(dstPage->indexTail - indexBytes, 4);

                // Staging: vertex + index packed into one contiguous ring region.
                uint8_t* mapped = nullptr;
                ID3D12Resource* stagingResource = nullptr;
                uint64_t stagingOffset = 0;
                AcquireStaging(static_cast<uint64_t>(vertexBytes) + indexBytes, mapped,
                    stagingResource, stagingOffset);
                memcpy(mapped, geo.vertices.data(), vertexBytes);
                memcpy(mapped + vertexBytes, geo.indices.data(), indexBytes);

                // Record GPU copies (no Execute yet — one submit at the end of the chunk).
                commandList->CopyBufferRegion(dstPage->buffer.Get(), vOffset,
                    stagingResource, stagingOffset, vertexBytes);
                commandList->CopyBufferRegion(dstPage->buffer.Get(), iOffset,
                    stagingResource, stagingOffset + vertexBytes, indexBytes);

                // Build and return the placement record (caller updates page state)
                GeometryPlacementRecordInPage rec{};
                rec.objectID = geo.id;
                rec.vertexByteOffset = vOffset;
                rec.vertexSize = vertexBytes;
                rec.indexByteOffset = iOffset;
                rec.indexSize = indexBytes;
                rec.indexCount = static_cast<uint32_t>(geo.indices.size());
                rec.gpuInstanceIndex = gpuInstanceIndex;

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

            /* The instanced counterpart of RecordGeometryUpload, and note what it does NOT do: it
            stages nothing, copies nothing and touches no page bytes. An object drawing from the
            global primitive library contributes zero vertex and zero index bytes anywhere, so all
            that is recorded is WHICH mesh to draw.

            The four byte-offset/size fields stay at literal zero, which is what makes the page's
            hole accounting (`holeBytes += vertexSize + indexSize`) and its compaction arithmetic
            correct with no special case: an instanced object genuinely occupies nothing.

            The AABB comes from the library entry, in CANONICAL space, and it is load-bearing rather
            than decorative. WriteInstanceRecord derives the registry's world-centre shadow from it,
            and that shadow is what the pick resolve and zoom-to-fit read; leaving it zero would put
            every instanced object's reported centre at the origin. */
            auto MakeInstancedRecord = [&](const GeometryData& geo, uint32_t gpuInstanceIndex)
                -> GeometryPlacementRecordInPage {
                const uint32_t lod = kPrimitiveFixedLod; // Step 2 replaces this with per-frame LOD.
                const PrimitiveLibraryEntry& entry =
                    gpu.primitiveLibrary.table.At(geo.libraryShapeId, lod);

                GeometryPlacementRecordInPage rec{};
                rec.objectID = geo.id;
                rec.gpuInstanceIndex = gpuInstanceIndex;
                rec.libraryShapeId = geo.libraryShapeId;
                rec.libraryLod = static_cast<uint8_t>(lod);
                rec.indexCount = entry.indexCount;
                rec.minX = entry.minX; rec.minY = entry.minY; rec.minZ = entry.minZ;
                rec.maxX = entry.maxX; rec.maxY = entry.maxY; rec.maxZ = entry.maxZ;
                return rec;
                };

            /* THE WRITE MODEL (graphics.md, 10M plan Step 4). Allocate a FRESH arena slot, write
            the 64-byte record where no published frame can reach it, then flip the object's
            4-byte redirect entry to point at it. A concurrent reader sees the old slot or the new
            one - both hold valid transforms - so the worst case is one frame of staleness on one
            monitor. Overwriting the record in place instead (what Step 3 shipped) could be
            observed half-written: a garbage matrix, not a stale one.

            Both copies go in ONE ring allocation and ONE command list. The copy queue executes
            strictly in order, so the record lands before the redirect that publishes it - the same
            ordering guarantee the page clone relies on.

            Returns the new slot; the caller records it for fence-gated release of the old one.

            The world-centre shadow is refreshed here too. It is what the pick resolve reads now
            that the arena is device-local, and it must follow every transform edit, not just the
            ones that touch geometry. `packedColor` is shadowed for the mirror-image reason: this
            writes a WHOLE fresh record every time, so the caller must supply the appearance bytes
            even when it is only moving the object. A transform-only edit has no GeometryData to
            take them from, so it passes the shadow straight back in - see the REMOVE-free MODIFY
            path below and InstanceRegistryEntry::packedColor.

            CONVENTION: transformA/B/C are the first three rows of transpose(world) - see
            InstanceRecord in RenderScene3D.h. The 4th row of the transpose is always (0,0,0,1),
            which is what makes 48 bytes enough. The CPU-side centre uses the ORIGINAL matrix,
            mirroring the shader's row-vector `pos * world`, not the transposed copy. */
            auto WriteInstanceRecord = [&](uint32_t gpuInstanceIndex, const XMFLOAT4X4& worldMatrix,
                const GeometryPlacementRecordInPage& placement, uint32_t packedColor) -> uint32_t {
                const uint32_t newSlot = AllocateInstanceSlot(tabRes);

                uint8_t* mapped = nullptr;
                ID3D12Resource* stagingResource = nullptr;
                uint64_t stagingOffset = 0;
                AcquireStaging(kInstanceRecordBytes + kInstanceSlotBytes, mapped,
                    stagingResource, stagingOffset);

                const XMMATRIX world = XMLoadFloat4x4(&worldMatrix);
                XMFLOAT4X4 transposed;
                XMStoreFloat4x4(&transposed, XMMatrixTranspose(world));
                InstanceRecord record{};
                // The record's 48-byte affine block IS the leading 3 rows of the transpose.
                memcpy(record.transformA, &transposed, 3 * 4 * sizeof(float));
                // The only one of the four appearance fields with a producer today: the object's
                // single surface color, which the 16-byte vertex no longer carries.
                record.packedColor = packedColor;
                memcpy(mapped, &record, kInstanceRecordBytes);
                memcpy(mapped + kInstanceRecordBytes, &newSlot, kInstanceSlotBytes);

                commandList->CopyBufferRegion(tabRes.instanceArena.resource.Get(),
                    static_cast<uint64_t>(newSlot) * kInstanceRecordBytes,
                    stagingResource, stagingOffset, kInstanceRecordBytes);
                // The flip. One naturally-aligned 4-byte write whose old and new values are both
                // valid slots, so a reader is never torn - only old-or-new.
                commandList->CopyBufferRegion(tabRes.instanceSlotOf.resource.Get(),
                    static_cast<uint64_t>(gpuInstanceIndex) * kInstanceSlotBytes,
                    stagingResource, stagingOffset + kInstanceRecordBytes, kInstanceSlotBytes);

                const XMVECTOR localCenter = XMVectorSet(
                    (placement.minX + placement.maxX) * 0.5f,
                    (placement.minY + placement.maxY) * 0.5f,
                    (placement.minZ + placement.maxZ) * 0.5f, 1.0f);
                const XMVECTOR worldCenter = XMVector3Transform(localCenter, world);
                InstanceRegistryEntry& entry = tabRes.registry[gpuInstanceIndex];
                entry.worldCenterX = XMVectorGetX(worldCenter);
                entry.worldCenterY = XMVectorGetY(worldCenter);
                entry.worldCenterZ = XMVectorGetZ(worldCenter);
                entry.packedColor = packedColor;
                return newSlot;
                };

            /* THE MASK WRITE (10M plan Step 5). Eight staged bytes and one CopyBufferRegion, and
            that is the entire cost of a hide or a show: no page cloned, no argument buffer rebuilt,
            no snapshot published, no arena slot burned. The destination is naturally aligned and a
            reader only ever tests one bit inside one 32-bit half, so a mask observed part-way
            through the write still reads old-or-new for that bit - invariant 2.

            The CPU shadow is kept in lockstep so the next edit knows the current word without
            reading back from device-local memory, and so retiring a sub-tab slot can restore its
            bit on exactly the hidden objects instead of sweeping the whole index space. An entry
            that returns to the all-visible default is ERASED, which is what keeps the shadow
            proportional to what is actually hidden rather than to the scene. */
            auto WriteVisibilityMask = [&](uint32_t index, uint64_t mask) {
                uint8_t* mapped = nullptr;
                ID3D12Resource* stagingResource = nullptr;
                uint64_t stagingOffset = 0;
                AcquireStaging(kVisibilityMaskBytes, mapped, stagingResource, stagingOffset);
                memcpy(mapped, &mask, kVisibilityMaskBytes);
                commandList->CopyBufferRegion(tabRes.visibilityMask.resource.Get(),
                    static_cast<uint64_t>(index) * kVisibilityMaskBytes,
                    stagingResource, stagingOffset, kVisibilityMaskBytes);

                if (mask == kVisibleInAllSubTabs) tabRes.hiddenInstanceMasks.erase(index);
                else tabRes.hiddenInstanceMasks[index] = mask;
                gCopyStats.maskWrites.fetch_add(1, std::memory_order_relaxed);
                };

            // Current membership word: the shadow when the object is hidden somewhere, else the
            // all-visible default. Absent from the shadow IS the default - see WriteVisibilityMask.
            auto CurrentVisibilityMask = [&](uint32_t index) -> uint64_t {
                auto it = tabRes.hiddenInstanceMasks.find(index);
                return it == tabRes.hiddenInstanceMasks.end() ? kVisibleInAllSubTabs : it->second;
                };

            uint32_t gpuInstanceIndex; // Stable renderer identity of the object being processed.
            std::map<AppendTargetKey, GeometryPage*> newestPagesByTarget;
            /* Indices vacated by REMOVE and slots vacated by REMOVE / MODIFY in this chunk. They do
            NOT go back on their free lists here: a later edit in this very chunk would take one and
            overwrite live data while render threads still draw the pre-publish snapshot, or still
            hold the pre-flip redirect value (10M plan Steps 1, 3 and 4). */
            std::vector<uint32_t> releasedInstanceIndexes;
            std::vector<uint32_t> releasedInstanceSlots;

            // Keyed by (container, kind): a container taking both an instanced and a bespoke object
            // in one chunk needs one append target of each, since a page carries a single pair of
            // buffer views for every template in it.
            auto AcquireAppendPage = [&](uint64_t containerMemoryId, GeometryPageKind kind,
                uint32_t incomingVertexBytes, uint32_t incomingIndexBytes) -> GeometryPage* {
                const AppendTargetKey key{ containerMemoryId, kind };
                GeometryPage*& targetPage = addTargetPages[key];
                if (targetPage && !targetPage->IsFull(incomingVertexBytes, incomingIndexBytes)) {
                    return targetPage;
                }

                GeometryPage*& newestPage = newestPagesByTarget[key];
                if (!newestPage || newestPage->IsFull(incomingVertexBytes, incomingIndexBytes)) {
                    newPages.push_back(CreateNewPage(containerMemoryId, kind));
                    newestPage = newPages.back().get();
                }
                targetPage = newestPage;
                return targetPage;
            };

            for (size_t ci = chunkStart; ci < chunkEnd; ++ci) { // Iterate over this chunk
                const CommandToCopyThread& cmd = *deduplicatedBatch[ci];
                if (cmd.tabID != tabID) continue;
                // Find the targe tab. Our static array of tabs is thread-safe for reading.

                uint32_t vertexBytes = 0, indexBytes = 0, newVertexBytes = 0, newIndexBytes = 0;

                decltype(clonedPages)::iterator cloneIt;

                GeometryPage* workPage = nullptr;
                GeometryPage* oldPage = nullptr;
                GeometryPage* modifyTargetPage = nullptr;
                const GeometryData* geo = nullptr;
                GeometryPlacementRecordInPage* oldRec = nullptr;
                GeometryPlacementRecordInPage rec;
                uint32_t slotIndex = 0;
                uint64_t targetContainerMemoryId = cmd.containerMemoryId;
                gpuInstanceIndex = kInvalidInstanceIndex;

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
                    if (tabRes.registry.Find(cmd.id) != kInvalidInstanceIndex) {goto handle_modify;}

                    geo = &(cmd.geometry.value());
                    vertexBytes = static_cast<uint32_t>(geo->vertices.size() * sizeof(Vertex));
                    indexBytes = static_cast<uint32_t>(geo->indices.size() * sizeof(uint16_t));
                    /* An INSTANCED object is empty BY DEFINITION - it names a library shape and
                    brings no bytes - so this guard has to let it through. Without the exception it
                    is the first thing that would reject shared geometry, and it would do so with a
                    console warning and nothing drawn rather than a failure anyone would notice. */
                    if (!IsInstancedGeometry(cmd) && (vertexBytes == 0 || indexBytes == 0)) {
                        std::wcout << "Warning: Skipping upload of empty geometry ID " << cmd.id << std::endl;
                        break; // Exit this case, process next command
                    }

                    // The object's renderer identity for its whole GPU lifetime (10M plan Step 3).
                    gpuInstanceIndex = AllocateInstanceIndex(tabRes); // Commits arena tiles if full.

                    GeometryPage* addTargetPage = AcquireAppendPage(targetContainerMemoryId,
                        PageKindFor(cmd), vertexBytes, indexBytes);

                    // Record the geometry upload into commandList - or, for an instanced object,
                    // record only WHICH library mesh to draw. Nothing is staged or copied there.
                    rec = IsInstancedGeometry(cmd)
                        ? MakeInstancedRecord(*geo, gpuInstanceIndex)
                        : RecordGeometryUpload(addTargetPage, *geo, gpuInstanceIndex);

                    // Update page CPU state
                    addTargetPage->objects.push_back(rec);
                    if (addTargetPage->kind == GeometryPageKind::Bespoke) {
                        addTargetPage->vertexHead = rec.vertexByteOffset + rec.vertexSize;
                        addTargetPage->indexTail = rec.indexByteOffset;
                    } // An instanced page has no vertex or index region to advance.
                    addTargetPage->objectCount++;

                    // Publish identity -> location in the copy thread's private registry.
                    slotIndex = static_cast<uint32_t>(addTargetPage->objects.size() - 1);
                    tabRes.registry.indexOfMemoryId[cmd.id] = gpuInstanceIndex;
                    tabRes.registry[gpuInstanceIndex].memoryID = cmd.id;
                    tabRes.registry[gpuInstanceIndex].page = addTargetPage;
                    tabRes.registry[gpuInstanceIndex].pageSlot = slotIndex;
                    // Record + redirect flip last: the registry entry it updates must already
                    // carry the placement this new object's world centre is derived from.
                    tabRes.registry[gpuInstanceIndex].instanceSlot =
                        WriteInstanceRecord(gpuInstanceIndex, geo->worldMatrix, rec,
                            PackColorRGBA8(geo->color));
                    /* A new object is visible in every SubTab. This write is MANDATORY, not an
                    optimisation to skip: a freshly committed D3D12 tile has undefined contents, so
                    an unwritten mask is not "all zeroes" but garbage - and a recycled index would
                    otherwise inherit the hides of the object that used to own it. */
                    WriteVisibilityMask(gpuInstanceIndex, kVisibleInAllSubTabs);

                    //std::wcout << "Added New object ID: " << cmd.id << std::endl;
                    break;
                }

                case CommandToCopyThreadType::MODIFY:
                handle_modify: // This is GOTO jump from ADD thread, if the ID already existed.
                    // TODO: Latter we will improve to use existing pages if it fits in place.
                    if (!cmd.geometry.has_value()) break;

                    gpuInstanceIndex = tabRes.registry.Find(cmd.id);
                    if (gpuInstanceIndex == kInvalidInstanceIndex) { goto handle_add; }// treat as ADD

                    geo = &(cmd.geometry.value());

                    /* TRANSFORM-ONLY EDIT - the path this whole design exists for (10M plan
                    Step 4). A new instance record plus a 4-byte redirect flip, and that is the
                    entire cost: no page is cloned, no argument buffer is rebuilt, no snapshot is
                    published. Moving 1000 objects scattered over 1000 pages costs ~68 KB of writes
                    instead of ~4 GB of page cloning.

                    Detected by the payload carrying a world matrix but no vertices or indices;
                    see IsTransformOnlyEdit in RenderScene3D.h. */
                    if (IsTransformOnlyEdit(cmd)) {
                        InstanceRegistryEntry& moved = tabRes.registry[gpuInstanceIndex];
                        if (!moved.page) break; // Not on the GPU yet; nothing to redirect.
                        releasedInstanceSlots.push_back(moved.instanceSlot);
                        /* The shadow, NOT geo->color: a transform-only payload carries no geometry
                        and therefore no color, so packing its default light-grey here would repaint
                        the object on every drag. */
                        moved.instanceSlot = WriteInstanceRecord(gpuInstanceIndex,
                            geo->worldMatrix, moved.page->objects[moved.pageSlot],
                            moved.packedColor);
                        gCopyStats.transformOnlyEdits.fetch_add(1, std::memory_order_relaxed);
                        break;
                    }

                    newVertexBytes = static_cast<uint32_t>(geo->vertices.size() * sizeof(Vertex));
                    newIndexBytes = static_cast<uint32_t>(geo->indices.size() * sizeof(uint16_t));

                    // Instanced again: empty is the normal case, so the guard must not reject it.
                    if (!IsInstancedGeometry(cmd) && (newVertexBytes == 0 || newIndexBytes == 0)) break;

                    // Object exists — work on its owning cloned page
                    oldPage = tabRes.registry[gpuInstanceIndex].page;
                    slotIndex = tabRes.registry[gpuInstanceIndex].pageSlot;
                    if (targetContainerMemoryId == 0) {
                        targetContainerMemoryId = oldPage->containerMemoryId;
                    }

                    // Resolve which mutable page we are working with: It will be in clonedPages (was in affectedPages
                    // from Pass 1), because Pass 1 already included pages from the existing registry.

                    cloneIt = clonedPages.find(oldPage);
                    if (cloneIt != clonedPages.end()) workPage = cloneIt->second.get();
                    else workPage = oldPage;   // page created this batch

                    oldRec = &(workPage->objects[slotIndex]);
                    oldRec->isDeleted = true;

                    workPage->holeBytes += oldRec->vertexSize + oldRec->indexSize;
                    workPage->objectCount--;

                    modifyTargetPage = AcquireAppendPage(targetContainerMemoryId,
                        PageKindFor(cmd), newVertexBytes, newIndexBytes);

                    /* GEOMETRY CHANGED - relocate into the cloned page as before, PLUS a new
                    instance slot. gpuInstanceIndex is unchanged: identity survives a modify, and
                    only the two locations (page and arena slot) move. Writing a fresh slot rather
                    than overwriting the old record in place is what keeps an in-flight frame from
                    reading a half-written matrix.

                    An INSTANCED edit lands here too, and deliberately so: its template names a
                    (shape, LOD) that this edit may have changed, so the page's argument buffer has
                    to be rebuilt. It is not a transform-only move even though its payload is empty
                    - that is exactly the confusion libraryShapeId exists to prevent. The clone it
                    forces is ~256 KB, not 4 MB, because an instanced page carries no geometry. */
                    rec = IsInstancedGeometry(cmd)
                        ? MakeInstancedRecord(*geo, gpuInstanceIndex)
                        : RecordGeometryUpload(modifyTargetPage, *geo, gpuInstanceIndex);
                    modifyTargetPage->objects.push_back(rec);
                    if (modifyTargetPage->kind == GeometryPageKind::Bespoke) {
                        modifyTargetPage->vertexHead = rec.vertexByteOffset + rec.vertexSize;
                        modifyTargetPage->indexTail = rec.indexByteOffset;
                    }
                    modifyTargetPage->objectCount++;

                    tabRes.registry[gpuInstanceIndex].page = modifyTargetPage;
                    tabRes.registry[gpuInstanceIndex].pageSlot =
                        static_cast<uint32_t>(modifyTargetPage->objects.size() - 1);
                    releasedInstanceSlots.push_back(tabRes.registry[gpuInstanceIndex].instanceSlot);
                    tabRes.registry[gpuInstanceIndex].instanceSlot =
                        WriteInstanceRecord(gpuInstanceIndex, geo->worldMatrix, rec,
                            PackColorRGBA8(geo->color));
                    break;

                case CommandToCopyThreadType::REMOVE:

                    gpuInstanceIndex = tabRes.registry.Find(cmd.id);
                    if (gpuInstanceIndex == kInvalidInstanceIndex) break; // not on GPU, nothing to do

                    oldPage = tabRes.registry[gpuInstanceIndex].page;
                    slotIndex = tabRes.registry[gpuInstanceIndex].pageSlot;

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
                    // Start the zombie interval for BOTH: the pre-publish snapshot still carries
                    // this object's indirect command, so neither its identity nor the arena slot
                    // that command reaches through may be reissued until every monitor is done.
                    releasedInstanceIndexes.push_back(gpuInstanceIndex);
                    releasedInstanceSlots.push_back(tabRes.registry[gpuInstanceIndex].instanceSlot);
                    tabRes.registry.indexOfMemoryId.erase(cmd.id);
                    tabRes.registry[gpuInstanceIndex] = InstanceRegistryEntry{}; // memoryID = 0.
                    // Drop any hide state with the identity. No GPU write is needed - the index is
                    // in its zombie interval and the next ADD to claim it writes the default mask.
                    tabRes.hiddenInstanceMasks.erase(gpuInstanceIndex);
                    break;

                case CommandToCopyThreadType::SET_VISIBILITY:
                    /* Per-object hide / show. One aligned mask write, nothing else - this is the
                    "hide any subset costs one atomic mask write per object" budget from the 10M
                    plan's workload table, and it holds whether the scene has 10 objects or 10M. */
                    gpuInstanceIndex = tabRes.registry.Find(cmd.id);
                    if (gpuInstanceIndex == kInvalidInstanceIndex) break; // Not on the GPU yet.
                    {
                        const uint64_t current = CurrentVisibilityMask(gpuInstanceIndex);
                        const uint64_t updated = cmd.visibilityVisible
                            ? (current | cmd.visibilityBits)
                            : (current & ~cmd.visibilityBits);
                        if (updated != current) WriteVisibilityMask(gpuInstanceIndex, updated);
                    }
                    break;

                case CommandToCopyThreadType::CLEAR_SUBTAB_HIDES:
                    /* A sub-tab slot has been retired and its bit is about to be reused. Force that
                    bit back ON everywhere, so hides authored for the old view do not silently
                    apply to whatever opens in the slot next. The doc's answer is a compute dispatch
                    over the whole mask array; the shadow lets us touch only the hidden objects
                    instead, which needs no shader-visible descriptor heap (a Step 7 prerequisite).
                    Collected first because WriteVisibilityMask mutates the map being read. */
                    {
                        std::vector<uint32_t> toRestore;
                        for (const auto& [index, mask] : tabRes.hiddenInstanceMasks) {
                            if ((mask & cmd.visibilityBits) != cmd.visibilityBits) {
                                toRestore.push_back(index);
                            }
                        }
                        /* Bounded per chunk: each entry stages 8 bytes and this single command is
                        charged only 8 in EstimateStagingBytes, so an unbounded fan-out could
                        overrun the ring and fall back to a committed buffer PER ENTRY. Whatever
                        does not fit is re-queued, and the loop terminates because every pass
                        strictly reduces the number of objects still hiding this bit. */
                        constexpr size_t kMaxRestoresPerChunk = 65536; // 512 KB of staging.
                        const size_t restoreCount = (std::min)(toRestore.size(), kMaxRestoresPerChunk);
                        for (size_t r = 0; r < restoreCount; ++r) {
                            WriteVisibilityMask(toRestore[r],
                                CurrentVisibilityMask(toRestore[r]) | cmd.visibilityBits);
                        }
                        if (restoreCount < toRestore.size()) {
                            CommandToCopyThread remainder;
                            remainder.type = CommandToCopyThreadType::CLEAR_SUBTAB_HIDES;
                            remainder.tabID = cmd.tabID;
                            remainder.visibilityBits = cmd.visibilityBits;
                            std::lock_guard<std::mutex> lock(toCopyThreadMutex);
                            commandToCopyThreadQueue.push(std::move(remainder));
                        }
                    }
                    break;

                default: break;
                } // End of switch (cmd.type)// Process Command
            } // end for (batch)

            // No execute here: the geometry uploads stay in the same recording as the clones above
            // and the argument rebuilds below, and go to the GPU as one submit at the end.

            // Rebuild Indirect Buffers (outside the per-command loop) Runs once per modified or new page,
            // after all commands are applied. Only live objects (isDeleted == false) are emitted.

            // Helper that rebuilds a single page's indirect buffer
            auto RebuildIndirectBuffer = [&](GeometryPage* page) {
                std::vector<IndirectCommand> commands;
                commands.reserve(page->objects.size());

                for (const auto& obj : page->objects) {
                    if (obj.isDeleted) continue; // skip soft-deleted slots

                    IndirectCommand ic{};
                    ic.gpuInstanceIndex = obj.gpuInstanceIndex;
                    ic.drawArguments.InstanceCount = 1;
                    /* Absolute within whatever view this page draws through: page-base-relative for
                    a bespoke page, library-region-relative for an instanced one. Stable for the
                    object's stay in the page regardless of later appends moving indexTail
                    (graphics.md, 10M plan Step 7, constraint 1). One shared resolver rather than
                    the division written out here - see ResolveObjectDrawRange. */
                    ResolveObjectDrawRange(obj, gpu.primitiveLibrary.table,
                        ic.drawArguments.IndexCountPerInstance,
                        ic.drawArguments.StartIndexLocation,
                        ic.drawArguments.BaseVertexLocation);
                    /* Which library shape this template draws, as `shapeId + 1` (0 = bespoke). The
                    cull pass reads it to decide whether to override the three offsets above with a
                    per-frame LOD; the legacy, pick and print paths execute the template as-is and
                    therefore keep drawing the CPU-chosen level. See TemplateShapeMarker. */
                    ic.drawArguments.StartInstanceLocation =
                        TemplateShapeMarker(obj.libraryShapeId);

                    commands.push_back(ic);
                }

                page->indirectCount = static_cast<uint32_t>(commands.size());
                if (commands.empty()) return; // nothing to upload

                /* Grow the argument buffer if this page turned out to hold more objects than the
                256 KB starting reservation covers - a page filled with very small objects can. This
                is the only growth point, and it is safe here and nowhere else: every page reaching
                RebuildIndirectBuffer is a clone or a brand-new page, neither of which is published,
                so replacing the buffer cannot be observed by a render thread. The rebuild below
                rewrites the whole buffer anyway, so a fresh one loses nothing. */
                AllocateIndirectBuffer(*page, page->indirectCount);

                const uint64_t commandBytes = commands.size() * sizeof(IndirectCommand);
                uint8_t* mapped = nullptr;
                ID3D12Resource* stagingResource = nullptr;
                uint64_t stagingOffset = 0;
                AcquireStaging(commandBytes, mapped, stagingResource, stagingOffset);
                memcpy(mapped, commands.data(), commandBytes);

                commandList->CopyBufferRegion(page->indirectBuffer.Get(), 0,
                    stagingResource, stagingOffset, commandBytes);
                };

            // Rebuild for every cloned (modified) page
            for (auto& [oldRaw, clonedPage] : clonedPages) RebuildIndirectBuffer(clonedPage.get());
            // Rebuild for every brand-new page
            for (auto& page : newPages) RebuildIndirectBuffer(page.get());

            // ONE submit for the whole chunk: clones, geometry uploads and argument rebuilds.
            // Always closed and executed, even when the chunk turned out to touch no page, so the
            // allocator is never Reset with the command list still recording.
            ThrowIfFailed(commandList->Close());
            ID3D12CommandList* lists[] = { commandList.Get() };
            gpu.copyCommandQueue->ExecuteCommandLists(1, lists);
            fenceValue = gpu.copyFenceValue.fetch_add(1);
            gpu.copyCommandQueue->Signal(gpu.copyFence.Get(), fenceValue);
            gpu.uploadRing.TagSubmission(fenceValue); // Ring space returns when this fence passes.
            if (gpu.copyFence->GetCompletedValue() < fenceValue) {
                gpu.copyFence->SetEventOnCompletion(fenceValue, gpu.copyFenceEvent);
                WaitForSingleObject(gpu.copyFenceEvent, INFINITE);
            }

            oversizeStaging.clear(); // Fallback staging buffers safe to free
            gCopyStats.chunks.fetch_add(1, std::memory_order_relaxed);
            gCopyStats.commands.fetch_add(chunkEnd - chunkStart, std::memory_order_relaxed);

            /* Start the zombie interval for everything this chunk vacated. Must be called at the
            chunk's publish point - AFTER PublishPages when there is one - because the fence read
            here has to be at or beyond the value PublishPages tags the retired pages with. Read it
            any earlier and a frame submitted in between still draws the pre-publish snapshot,
            which names a released INDEX, while that index is already back on the free list.

            Slots have the same shape one level down: an in-flight frame may still hold the
            pre-flip value of a redirect entry pointing at a released SLOT.

            Called from BOTH exits below, because a chunk of pure transform-only edits clones no
            page and publishes nothing, yet still vacates one slot per edit (10M plan Step 4). */
            auto RetireReleasedIdentities = [&]() {
                if (releasedInstanceIndexes.empty() && releasedInstanceSlots.empty()) return;
                const uint64_t retireFence = gpu.renderFenceValue.load(std::memory_order_acquire);
                for (uint32_t released : releasedInstanceIndexes) {
                    tabRes.pendingFreeInstanceIndexes.push_back({ released, retireFence });
                }
                for (uint32_t released : releasedInstanceSlots) {
                    if (released == kInvalidInstanceSlot) continue; // Never had a record.
                    tabRes.pendingFreeInstanceSlots.push_back({ released, retireFence });
                }
                releasedInstanceIndexes.clear();
                releasedInstanceSlots.clear();
                };

            if (clonedPages.empty() && newPages.empty()) { // Nothing to publish for this chunk.
                // The redirect flip WAS this chunk's publish; it completed with the copy fence above.
                RetireReleasedIdentities();
                PruneRetiredGpuResources(); // Sweep anyway: the releases above need draining.
                chunkStart = chunkEnd;
                continue;
            }

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
            RetireReleasedIdentities(); // Fence read after the publish - see the lambda.

            // Reclaim between chunks. Each chunk retires the page it appended to, so without this
            // a many-chunk batch accumulates ~4.25 MB per chunk with nothing freeing it until the
            // whole batch returns - which exhausts VRAM outright on a bulk import. When monitors
            // cannot keep up, give them a few short breathers rather than racing ahead: this
            // throttles the copy thread instead of letting retained pages grow without bound.
            constexpr size_t kRetireBacklogCap = 64;      // ~270 MB of pages held back.
            constexpr int kMaxBacklogWaits = 8;           // Bounded: a frozen monitor must not hang us.
            for (int attempt = 0; attempt <= kMaxBacklogWaits; ++attempt) {
                const size_t backlog = PruneRetiredGpuResources();
                if (backlog == SIZE_MAX || backlog <= kRetireBacklogCap) break;
                if (attempt == kMaxBacklogWaits) break; // Proceed anyway; the counter records it.
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
            }

            // End of Pass 3 + Publish
            chunkStart = chunkEnd;
        } // End of chunk loop
        } // End of per-tab loop
}
