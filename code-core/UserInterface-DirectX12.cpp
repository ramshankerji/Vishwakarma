// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

// DirectX12 half of the UI overlay module: atlas texture upload, PSO / root signature / sampler
// heap, per-window dynamic buffers and the actual draw call. The platform-agnostic half (atlas
// bitmap building, ribbon layout, tessellation, widget logic) lives in UserInterface.cpp.

#include "UserInterface-DirectX12.h"
#include <algorithm>
#include <d3dcompiler.h>
#include "ShaderUIVertex.h"
#include "ShaderUIPixel.h"
#include <MemoryManagerGPU-DirectX12.h>
#include "विश्वकर्मा.h"
#include "TextureSaver.h"
extern शंकर gpu;
extern void PrintHResult(int);
std::atomic<uint64_t> atlasFence = 0;

bool SubmitTextureUpload(const TextureUploadDesc& desc,
    ComPtr<ID3D12Resource>* outTex,  std::atomic<uint64_t>* fenceOut) {

    uint32_t index = gUploadQueue.writeIndex.fetch_add(1, std::memory_order_relaxed);
    UploadRequest& req = gUploadQueue.requests[index % MAX_UPLOAD_REQUESTS];

    req.type = UploadType::Texture2D;
    req.texture = desc;
    req.outResource = outTex;
    req.completionFence = fenceOut;

    return true;
}

bool UploadUIAtlasTexture(DX12ResourcesUI& uiRes, ID3D12Device* device, uint32_t atlasSlot,
    const AtlasBitmap& atlas) {
    if (!device || atlasSlot >= UI_MAX_ATLAS_TEXTURES || atlas.width <= 0 || atlas.height <= 0 ||
        atlas.pixels.empty() || (atlas.bytesPerPixel != 1 && atlas.bytesPerPixel != 4)) {
        return false;
    }

    TextureUploadDesc desc = {};
    desc.width = atlas.width;
    desc.height = atlas.height;
    desc.format = atlas.bytesPerPixel == 4 ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R8_UNORM;
    desc.pixels = atlas.pixels.data();
    desc.rowPitch = atlas.width * atlas.bytesPerPixel;

    std::atomic<uint64_t> uploadFence = 0;
    SubmitTextureUpload(desc, &uiRes.uiAtlasTextures[atlasSlot], &uploadFence);

    uint64_t atlasReadyFence = gpu.copyFenceValue.fetch_add(1, std::memory_order_relaxed);
    uploadFence.store(atlasReadyFence, std::memory_order_release);
    toCopyThreadCV.notify_one();

    if (gpu.copyFence->GetCompletedValue() < atlasReadyFence) {
        ThrowIfFailed(gpu.copyFence->SetEventOnCompletion(atlasReadyFence, gpu.copyFenceEvent));
        WaitForSingleObject(gpu.copyFenceEvent, INFINITE);
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = desc.format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

    CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(uiRes.srvHeap->GetCPUDescriptorHandleForHeapStart(),
        atlasSlot, device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV));
    device->CreateShaderResourceView(uiRes.uiAtlasTextures[atlasSlot].Get(), &srvDesc, srvHandle);
    return true;
}

void InitUIResources( DX12ResourcesUI& uiRes, ID3D12Device* device) {
    if (!InitFontSystem()) { // FONT system initialization (CPU-side)
        std::cerr << "Font system initialization failed failed\n";
        return;
    }

    // Root signature
    CD3DX12_DESCRIPTOR_RANGE1 ranges[2]; // Descriptor ranges

    // Range 0: SRV (t0)  → from srvHeap // 1: 1 Texture, 0: register t0 = atlas
    ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UI_MAX_ATLAS_TEXTURES, 0, 0,
        D3D12_DESCRIPTOR_RANGE_FLAG_NONE);
    // Range 1: SAMPLER (s0) → from samplerHeap // 1: 1 Sampler, 0: register s0 = sampler
    ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE);

    CD3DX12_ROOT_PARAMETER1 rootParams[3];
    rootParams[0].InitAsConstantBufferView(0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE,
        D3D12_SHADER_VISIBILITY_VERTEX);// b0 - Ortho constant buffer (vertex shader)

    // Root Parameter 1: Descriptor Table containing only the SRV
    rootParams[1].InitAsDescriptorTable(1, &ranges[0], D3D12_SHADER_VISIBILITY_PIXEL);

    // Root Parameter 2: Descriptor Table containing only the SAMPLER
    rootParams[2].InitAsDescriptorTable(1, &ranges[1],
        D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootDesc;
    rootDesc.Init_1_1(_countof(rootParams), rootParams, 0, nullptr,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> errorBlob;

    HRESULT hr = D3DX12SerializeVersionedRootSignature(&rootDesc,
        D3D_ROOT_SIGNATURE_VERSION_1_1, &signature, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob)
            std::cerr << "Root Signature Serialization Failed:\n"
            << (char*)errorBlob->GetBufferPointer() << std::endl;
        ThrowIfFailed(hr); // will print the real error
    }

    ThrowIfFailed(device->CreateRootSignature(0, signature->GetBufferPointer(),
        signature->GetBufferSize(), IID_PPV_ARGS(&uiRes.uiRootSignature)));
    
    // Create SRV descriptor heap (1 texture)
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = UI_MAX_ATLAS_TEXTURES;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(device->CreateDescriptorHeap( &heapDesc, IID_PPV_ARGS(&uiRes.srvHeap) ));

    // Create SAMPLER descriptor heap (shader-visible)
    D3D12_DESCRIPTOR_HEAP_DESC samplerHeapDesc = {};
    samplerHeapDesc.NumDescriptors = 1;
    samplerHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    samplerHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(device->CreateDescriptorHeap(&samplerHeapDesc,
        IID_PPV_ARGS(&uiRes.samplerHeap)));

    // Create the actual sampler.
    D3D12_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    samplerDesc.MinLOD = 0;
    samplerDesc.MaxLOD = D3D12_FLOAT32_MAX;

    device->CreateSampler(&samplerDesc,
        uiRes.samplerHeap->GetCPUDescriptorHandleForHeapStart());

    // Shaders are compiled to DXIL during the build and embedded into the executable.
    // Input layout

    D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0 },
        { "TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0 },
        { "COLOR",0,DXGI_FORMAT_R32_UINT,0,16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0 },
        { "TEXCOORD",1,DXGI_FORMAT_R32_UINT,0,20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0 }
    };

    // PSO
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { layout,_countof(layout) };
    psoDesc.pRootSignature = uiRes.uiRootSignature.Get();
    psoDesc.VS = CD3DX12_SHADER_BYTECODE(g_uiVertexShader, sizeof(g_uiVertexShader));
    psoDesc.PS = CD3DX12_SHADER_BYTECODE(g_uiPixelShader, sizeof(g_uiPixelShader));
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
    psoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;

    ThrowIfFailed( device->CreateGraphicsPipelineState( &psoDesc, IID_PPV_ARGS(&uiRes.uiPSO)));

    // The dynamic vertex/index/ortho buffers are per window (DX12ResourcesPerWindow), created
    // lazily by RenderUIOverlay (see EnsureWindowUIBuffers).
    std::wcout << L"UI Resources Initialized (Phase 4A)\n";
    
    AtlasBitmap englishAtlas = BuildMSDFFontAtlas();
    AtlasBitmap iconAtlas = BuildIconAtlas();
    
    TextureUploadDesc desc = {};
    desc.width = englishAtlas.width;
    desc.height = englishAtlas.height;
    desc.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.pixels = englishAtlas.pixels.data();
    desc.rowPitch = englishAtlas.width * englishAtlas.bytesPerPixel;

    int bytesPerPixel = iconAtlas.bytesPerPixel;

    SubmitTextureUpload(desc, &uiRes.uiAtlasTextures[UI_ENGLISH_ATLAS_SLOT], &atlasFence);// Enqueue the upload through upload queue
    // RESERVED FENCE VALUE FOR THIS UPLOAD (this is the key change)
    // The copy thread MUST eventually signal exactly this value.
    uint64_t atlasReadyFence = gpu.copyFenceValue.fetch_add(1, std::memory_order_relaxed);
    // Tell everyone (including the render thread) what fence value to wait for
    atlasFence.store(atlasReadyFence, std::memory_order_release);
	toCopyThreadCV.notify_one(); // Wakeup CPU thread to process the newly uploaded texture.
    // CPU-blocking wait until Copy Queue has processed this upload
    if (gpu.copyFence->GetCompletedValue() < atlasReadyFence) {
        ThrowIfFailed(gpu.copyFence->SetEventOnCompletion(atlasReadyFence, gpu.copyFenceEvent));
        WaitForSingleObject(gpu.copyFenceEvent, INFINITE);   // CPU blocks here
    }

    // Now the texture is in DEFAULT heap → safe to create SRV
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    
    device->CreateShaderResourceView(uiRes.uiAtlasTextures[UI_ENGLISH_ATLAS_SLOT].Get(), &srvDesc,
        uiRes.srvHeap->GetCPUDescriptorHandleForHeapStart());

    SaveToBmp("icon_atlas_debug.bmp", iconAtlas.pixels.data(),
        iconAtlas.width, iconAtlas.height, bytesPerPixel);
    UploadUIAtlasTexture(uiRes, device, UI_ICON_ATLAS_SLOT, iconAtlas);

    std::wcout << L"Mandatory UI atlases uploaded: English slot " << UI_ENGLISH_ATLAS_SLOT
        << L", icon slot " << UI_ICON_ATLAS_SLOT << L"\n";
}

// Cleanup
void CleanupUIResources(DX12ResourcesUI& uiRes) {
    uiRes = {};
}


// Creates this window's dynamic UI buffers on first use. Per window by necessity: one monitor
// command list records all its windows before executing, so a shared upload buffer would be
// overwritten by the last-recorded window and every window would present that window's UI.
static void EnsureWindowUIBuffers(DX12ResourcesPerWindow& winRes, ID3D12GraphicsCommandList* cmd,
    const DX12ResourcesUI& uiRes) {
    if (winRes.uiVertexBuffer) return;

    ComPtr<ID3D12Device> device;
    if (FAILED(cmd->GetDevice(IID_PPV_ARGS(&device)))) return;

    auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

    auto vbDesc = CD3DX12_RESOURCE_DESC::Buffer(uiRes.maxVertices * sizeof(UIVertex));
    ThrowIfFailed(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &vbDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&winRes.uiVertexBuffer)));

    auto ibDesc = CD3DX12_RESOURCE_DESC::Buffer(uiRes.maxIndices * sizeof(uint16_t));
    ThrowIfFailed(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &ibDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&winRes.uiIndexBuffer)));

    auto cbDesc = CD3DX12_RESOURCE_DESC::Buffer(256);
    ThrowIfFailed(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&winRes.uiOrthoConstantBuffer)));

    CD3DX12_RANGE readRange(0, 0);
    winRes.uiVertexBuffer->Map(0, &readRange, reinterpret_cast<void**>(&winRes.pUIVertexDataBegin));
    winRes.uiIndexBuffer->Map(0, &readRange, reinterpret_cast<void**>(&winRes.pUIIndexDataBegin));
    winRes.uiOrthoConstantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&winRes.pUIOrthoDataBegin));
}

// This function renders the list of tabs, all top menu buttons (with dropdowns if required),
// side favorite / frequent buttons bars, right side property window, bottom status bar.
// This is also responsible for all relevant DirectX12 configurations required for rendering User Interface.
void RenderUIOverlay(SingleUIWindow& window, ID3D12GraphicsCommandList* cmd, DX12ResourcesUI& uiRes,
    UITopRibbonLayout& topRibbonLayout, float monitorDPIX, float monitorDPIY, const UIInput& input,
    uint64_t activeInternalSubTabMemoryId) {

    if (!cmd) return; //Defensive check.

    EnsureWindowUIBuffers(window.dx, cmd, uiRes);
    if (!window.dx.pUIVertexDataBegin || !window.dx.pUIIndexDataBegin ||
        !window.dx.pUIOrthoDataBegin) {
        return;
    }


    cmd->SetPipelineState(uiRes.uiPSO.Get());
    cmd->SetGraphicsRootSignature(uiRes.uiRootSignature.Get());

    // Bind descriptor heap
    ID3D12DescriptorHeap* heaps[] = { uiRes.srvHeap.Get(), uiRes.samplerHeap.Get() };
    cmd->SetDescriptorHeaps(_countof(heaps), heaps);

    // Bind the descriptor table (which contains t0 + s0)    
    // Root Parameter 1 = SRV table// must match rootParams[1]
    cmd->SetGraphicsRootDescriptorTable(1, uiRes.srvHeap->GetGPUDescriptorHandleForHeapStart());
    // Root Parameter 2 = Sampler table
    cmd->SetGraphicsRootDescriptorTable(2, uiRes.samplerHeap->GetGPUDescriptorHandleForHeapStart());
    // Bind this window's ortho constant buffer (still root parameter 0)
    cmd->SetGraphicsRootConstantBufferView(0, window.dx.uiOrthoConstantBuffer->GetGPUVirtualAddress());

    float W = (float)window.dx.WindowWidth;
    float H = (float)window.dx.WindowHeight;
    float* ortho = (float*)window.dx.pUIOrthoDataBegin;

    ortho[0] = 2 / W; ortho[1] = 0;   ortho[2] = 0; ortho[3] = -1;
    ortho[4] = 0;   ortho[5] = -2 / H; ortho[6] = 0; ortho[7] = 1;
    ortho[8] = 0;   ortho[9] = 0;   ortho[10] = 1; ortho[11] = 0;
    ortho[12] = 0;  ortho[13] = 0;  ortho[14] = 0; ortho[15] = 1;

    UIDrawContext ctx;
    ctx.vertexPtr = reinterpret_cast<UIVertex*>(window.dx.pUIVertexDataBegin);
    ctx.indexPtr = reinterpret_cast<uint16_t*>(window.dx.pUIIndexDataBegin);
    ctx.vertexCount = 0;
    ctx.indexCount = 0;

    // Portable widget/hit-test half (UserInterface.cpp): fills ctx and emits UIActions.
    BuildUIOverlay(window, ctx, uiRes, topRibbonLayout, monitorDPIX, monitorDPIY, input,
        activeInternalSubTabMemoryId);

    // DRAW ALL UI GEOMETRY
    if (ctx.indexCount == 0) return;

    D3D12_VERTEX_BUFFER_VIEW vbv{};
    vbv.BufferLocation = window.dx.uiVertexBuffer->GetGPUVirtualAddress();
    vbv.SizeInBytes = ctx.vertexCount * sizeof(UIVertex);
    vbv.StrideInBytes = sizeof(UIVertex);

    D3D12_INDEX_BUFFER_VIEW ibv{};
    ibv.BufferLocation = window.dx.uiIndexBuffer->GetGPUVirtualAddress();
    ibv.SizeInBytes = ctx.indexCount * sizeof(uint16_t);
    ibv.Format = DXGI_FORMAT_R16_UINT;

    cmd->IASetVertexBuffers(0, 1, &vbv);
    cmd->IASetIndexBuffer(&ibv);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawIndexedInstanced(ctx.indexCount, 1, 0, 0, 0);
}
