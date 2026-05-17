// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

#include "UserInterface-DirectX12.h"
#include <algorithm>
#include <d3dcompiler.h>
#include <dxcapi.h>
#include "FontManager.h"
#include <MemoryManagerGPU-DirectX12.h>
#include "विश्वकर्मा.h"
#include "TextureSaver.h"
#include "UserInterfaceTranslationCompiled.h"
#include <array>
#include <cmath>
extern शंकर gpu;
extern std::atomic<uint16_t*> publishedTabIndexes;
extern std::atomic<uint16_t>  publishedTabCount;
extern void PrintHResult(int);
std::atomic<uint32_t> actionWriteIndex;
// ASCII Character set.
std::string charset = " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~";
std::atomic<uint64_t> atlasFence = 0;

struct UIAtlasRegion {
    float uvMinX = 0.0f;
    float uvMinY = 0.0f;
    float uvMaxX = 0.0f;
    float uvMaxY = 0.0f;
};

struct UIRoundedRectangleNineSlice {
    std::array<std::array<UIAtlasRegion, 3>, 3> regions{};
};

struct UIIconAtlasMetadata {
    UIRoundedRectangleNineSlice roundedRectangle{};
    std::array<char32_t, 4> dummyIconCodepoints{ U'\uE000', U'\uE001', U'\uE002', U'\uE003' };
};

static UIIconAtlasMetadata gIconAtlasMetadata{};

static UIAtlasRegion MakeAtlasRegion(int x, int y, int w, int h, int atlasW, int atlasH) {
    UIAtlasRegion region{};
    region.uvMinX = (float)x / (float)atlasW;
    region.uvMinY = (float)y / (float)atlasH;
    region.uvMaxX = (float)(x + w) / (float)atlasW;
    region.uvMaxY = (float)(y + h) / (float)atlasH;
    return region;
}

static void GenerateRoundedRectangleNineSlice(AtlasBitmap& atlas, int originX, int originY,
    int sourceSizePx, int sourceRadiusPx, UIRoundedRectangleNineSlice& outSlice) {
    const int atlasW = atlas.width;
    const int atlasH = atlas.height;

    for (int y = 0; y < sourceSizePx; ++y) {
        for (int x = 0; x < sourceSizePx; ++x) {
            const float px = (float)x + 0.5f;
            const float py = (float)y + 0.5f;
            const float nearestX = std::clamp(px, (float)sourceRadiusPx,
                (float)(sourceSizePx - sourceRadiusPx));
            const float nearestY = std::clamp(py, (float)sourceRadiusPx,
                (float)(sourceSizePx - sourceRadiusPx));
            const float dx = px - nearestX;
            const float dy = py - nearestY;
            const float distance = std::sqrt(dx * dx + dy * dy);
            const float coverage = std::clamp((float)sourceRadiusPx + 0.5f - distance, 0.0f, 1.0f);

            atlas.pixels[(originY + y) * atlasW + (originX + x)] = (uint8_t)std::round(coverage * 255.0f);
        }
    }

    const int middle = sourceSizePx - 2 * sourceRadiusPx;
    const int widths[3] = { sourceRadiusPx, middle, sourceRadiusPx };
    const int heights[3] = { sourceRadiusPx, middle, sourceRadiusPx };
    int yCursor = originY;
    for (int row = 0; row < 3; ++row) {
        int xCursor = originX;
        for (int col = 0; col < 3; ++col) {
            outSlice.regions[row][col] =
                MakeAtlasRegion(xCursor, yCursor, widths[col], heights[row], atlasW, atlasH);
            xCursor += widths[col];
        }
        yCursor += heights[row];
    }
}

static void FillRect(AtlasBitmap& atlas, int x, int y, int w, int h, uint8_t coverage) {
    for (int yy = y; yy < y + h; ++yy) {
        for (int xx = x; xx < x + w; ++xx) {
            atlas.pixels[yy * atlas.width + xx] = coverage;
        }
    }
}

static AtlasBitmap BuildIconAtlas() {
    constexpr int atlasW = 128;
    constexpr int atlasH = 128;
    AtlasBitmap atlas{};
    atlas.width = atlasW;
    atlas.height = atlasH;
    atlas.pixels.resize(atlasW * atlasH, 0);

    // The source rounded rectangle is split into 9 texture regions at draw time.
    // Destination corners are resized to ~2 mm in screen space by PushRoundedRectangle.
    GenerateRoundedRectangleNineSlice(atlas, 0, 0, 32, 8, gIconAtlasMetadata.roundedRectangle);

    constexpr int iconSize = 20;
    constexpr int iconY = 48;
    constexpr int iconXs[4] = { 0, 24, 48, 72 };
    iconGlyphLookup.clear();
    for (int i = 0; i < 4; ++i) {
        Glyph glyph{};
        glyph.uvMinX = (float)iconXs[i] / atlasW;
        glyph.uvMinY = (float)iconY / atlasH;
        glyph.uvMaxX = (float)(iconXs[i] + iconSize) / atlasW;
        glyph.uvMaxY = (float)(iconY + iconSize) / atlasH;
        glyph.width = iconSize;
        glyph.height = iconSize;
        glyph.advanceX = iconSize;
        iconGlyphLookup[gIconAtlasMetadata.dummyIconCodepoints[i]] = glyph;
    }

    // Dummy icon 0: plus
    FillRect(atlas, iconXs[0] + 8, iconY + 2, 4, 16, 255);
    FillRect(atlas, iconXs[0] + 2, iconY + 8, 16, 4, 255);

    // Dummy icon 1: folder-like block
    FillRect(atlas, iconXs[1] + 2, iconY + 6, 16, 11, 255);
    FillRect(atlas, iconXs[1] + 4, iconY + 3, 7, 4, 255);

    // Dummy icon 2: ring
    for (int y = 0; y < iconSize; ++y) {
        for (int x = 0; x < iconSize; ++x) {
            const float dx = (float)x + 0.5f - 10.0f;
            const float dy = (float)y + 0.5f - 10.0f;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d >= 5.0f && d <= 8.0f) {
                atlas.pixels[(iconY + y) * atlasW + (iconXs[2] + x)] = 255;
            }
        }
    }

    // Dummy icon 3: 2x2 grid
    FillRect(atlas, iconXs[3] + 2, iconY + 2, 7, 7, 255);
    FillRect(atlas, iconXs[3] + 11, iconY + 2, 7, 7, 255);
    FillRect(atlas, iconXs[3] + 2, iconY + 11, 7, 7, 255);
    FillRect(atlas, iconXs[3] + 11, iconY + 11, 7, 7, 255);

    return atlas;
}

static uint32_t StableRandomUIColour(uint32_t seed) {
    seed ^= seed >> 16;
    seed *= 0x7FEB352Du;
    seed ^= seed >> 15;
    seed *= 0x846CA68Bu;
    seed ^= seed >> 16;

    uint32_t r = 80u + ((seed >> 0) & 0x7Fu);
    uint32_t g = 80u + ((seed >> 8) & 0x7Fu);
    uint32_t b = 80u + ((seed >> 16) & 0x7Fu);
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

// Shader compilation helper
static void CompileShader( const char* code, LPCWSTR entry, LPCWSTR target, bool debugShaders,
    ComPtr<IDxcBlob>& outBlob) {

    ComPtr<IDxcUtils> utils;
    ComPtr<IDxcCompiler3> compiler;
    ThrowIfFailed(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils)));
    ThrowIfFailed(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)));

    DxcBuffer sourceBuffer = {};
    sourceBuffer.Ptr = code;
    sourceBuffer.Size = strlen(code);
    sourceBuffer.Encoding = DXC_CP_UTF8;

    std::vector<LPCWSTR> arguments = { L"-E", entry, L"-T", target };
    if (debugShaders) {
        arguments.push_back(DXC_ARG_DEBUG);
        arguments.push_back(DXC_ARG_SKIP_OPTIMIZATIONS);
    }

    ComPtr<IDxcResult> result;
    ThrowIfFailed(compiler->Compile(&sourceBuffer, arguments.data(),
        static_cast<UINT32>(arguments.size()), nullptr, IID_PPV_ARGS(&result)));

    HRESULT hr = S_OK;
    ThrowIfFailed(result->GetStatus(&hr));
    if (FAILED(hr)) {
        ComPtr<IDxcBlobUtf8> errorBlob;
        result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errorBlob), nullptr);
        if (errorBlob && errorBlob->GetStringLength() > 0) {
            std::cerr << errorBlob->GetStringPointer() << std::endl;
        }
        ThrowIfFailed(hr);
    }

    ThrowIfFailed(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&outBlob), nullptr));
}

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
        atlas.pixels.empty()) {
        return false;
    }

    TextureUploadDesc desc = {};
    desc.width = atlas.width;
    desc.height = atlas.height;
    desc.format = DXGI_FORMAT_R8_UNORM;
    desc.pixels = atlas.pixels.data();
    desc.rowPitch = atlas.width;

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
    srvDesc.Format = DXGI_FORMAT_R8_UNORM;
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

    // Create the actual sampler (Point sampling is perfect for UI atlas)
    D3D12_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;   // sharp UI text
    // TODO: Above works best only if there is NO scaling. Right now we are doing scaling.
	// Hence text is slightly blurry. 
    // Latter we generate separate font for different monitors based on their DPI and do 1:1 sampling.
    samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    samplerDesc.MinLOD = 0;
    samplerDesc.MaxLOD = D3D12_FLOAT32_MAX;

    device->CreateSampler(&samplerDesc,
        uiRes.samplerHeap->GetCPUDescriptorHandleForHeapStart());

    // Shaders

#if defined(_DEBUG)
    bool compileDebugShaders = true;
#else
    bool compileDebugShaders = false;
#endif

    static const char* vsCode = R"(
cbuffer OrthoConstantBuffer : register(b0) {
    float4x4 ortho;
};

struct VSInput {
    float2 position : POSITION;
    float2 uv       : TEXCOORD0;
    uint   color    : COLOR0;
    uint   atlasIndex : TEXCOORD1;
};

struct PSInput {
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
    uint   color    : COLOR0;
    nointerpolation uint atlasIndex : TEXCOORD1;
};

PSInput VSMain(VSInput input) {
    PSInput output;
    float4 pos = float4(input.position,0,1);
    output.position = mul(pos,ortho);
    output.uv = input.uv;
    output.color = input.color;
    output.atlasIndex = input.atlasIndex;
    return output;
}
)";

    static const char* psCode = R"(
struct PSInput {
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
    uint   color    : COLOR0;
    nointerpolation uint atlasIndex : TEXCOORD1;
};

Texture2D atlases[10] : register(t0);
SamplerState samp : register(s0);

float SampleCoverage(uint atlasIndex, float2 uv) {
    // Shader Model 5.0 does not permit dynamic resource-array indexing.
    // Keep the descriptor array future-ready while sampling through static cases.
    switch (atlasIndex) {
    case 0: return atlases[0].Sample(samp, uv).r;
    case 1: return atlases[1].Sample(samp, uv).r;
    case 2: return atlases[2].Sample(samp, uv).r;
    case 3: return atlases[3].Sample(samp, uv).r;
    case 4: return atlases[4].Sample(samp, uv).r;
    case 5: return atlases[5].Sample(samp, uv).r;
    case 6: return atlases[6].Sample(samp, uv).r;
    case 7: return atlases[7].Sample(samp, uv).r;
    case 8: return atlases[8].Sample(samp, uv).r;
    case 9: return atlases[9].Sample(samp, uv).r;
    default: return atlases[0].Sample(samp, uv).r;
    }
}

float4 PSMain(PSInput input) : SV_TARGET {
    float r = ((input.color >> 0) & 0xFF) / 255.0;
    float g = ((input.color >> 8) & 0xFF) / 255.0;
    float b = ((input.color >> 16) & 0xFF) / 255.0;
    float a = ((input.color >> 24) & 0xFF) / 255.0;
    float4 baseColor = float4(r, g, b, a);

    if (input.uv.x == 0.0 && input.uv.y == 0.0) {
        return baseColor;
    }

    float coverage = SampleCoverage(input.atlasIndex, input.uv);
    return float4(baseColor.rgb, baseColor.a * coverage);
}
)";

    ComPtr<IDxcBlob> vsBlob;
    ComPtr<IDxcBlob> psBlob;
    CompileShader(vsCode, L"VSMain", L"vs_6_0", compileDebugShaders, vsBlob);
    CompileShader(psCode, L"PSMain", L"ps_6_0", compileDebugShaders, psBlob);
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
    psoDesc.VS = CD3DX12_SHADER_BYTECODE(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize());
    psoDesc.PS = CD3DX12_SHADER_BYTECODE(psBlob->GetBufferPointer(), psBlob->GetBufferSize());
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
    
    // Vertex buffer
    auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

    auto vbDesc = CD3DX12_RESOURCE_DESC::Buffer( uiRes.maxVertices * sizeof(UIVertex));
    ThrowIfFailed( device->CreateCommittedResource( &uploadHeap, D3D12_HEAP_FLAG_NONE, &vbDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uiRes.uiVertexBuffer)));

    auto ibDesc = CD3DX12_RESOURCE_DESC::Buffer( uiRes.maxIndices * sizeof(uint16_t));
    ThrowIfFailed( device->CreateCommittedResource( &uploadHeap, D3D12_HEAP_FLAG_NONE, &ibDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uiRes.uiIndexBuffer)));

    auto cbDesc = CD3DX12_RESOURCE_DESC::Buffer(256);
    ThrowIfFailed( device->CreateCommittedResource( &uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uiRes.uiOrthoConstantBuffer)));

    CD3DX12_RANGE readRange(0, 0);
    uiRes.uiVertexBuffer->Map( 0, &readRange, reinterpret_cast<void**>(&uiRes.pVertexDataBegin));
    uiRes.uiIndexBuffer->Map(  0, &readRange, reinterpret_cast<void**>(&uiRes.pIndexDataBegin));
    uiRes.uiOrthoConstantBuffer->Map( 0, &readRange, reinterpret_cast<void**>(&uiRes.pOrthoDataBegin));
    std::wcout << L"UI Resources Initialized (Phase 4A)\n";
    
    AtlasBitmap englishAtlas = BuildFontAtlas();
    AtlasBitmap iconAtlas = BuildIconAtlas();
    
    TextureUploadDesc desc = {};
    desc.width = englishAtlas.width;
    desc.height = englishAtlas.height;
    desc.format = DXGI_FORMAT_R8_UNORM;
    desc.pixels = englishAtlas.pixels.data();
    desc.rowPitch = englishAtlas.width;

    int bytesPerPixel = 1; // Use 4 if DXGI_FORMAT_R8G8B8A8_UNORM, use 1 if DXGI_FORMAT_R8_UNORM
    bool isSaved = SaveToBmp("font_atlas_debug.bmp", englishAtlas.pixels.data(),
        englishAtlas.width, englishAtlas.height, bytesPerPixel);
    if (isSaved) std::cout << "SUCCESS: Debug atlas saved to working directory." << std::endl;
    else std::cerr << "FAIL: Could not save debug atlas. Pointer might be invalid!" << std::endl;

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
    srvDesc.Format = DXGI_FORMAT_R8_UNORM;
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
    if (uiRes.uiVertexBuffer) uiRes.uiVertexBuffer->Unmap(0, nullptr);
    if (uiRes.uiIndexBuffer) uiRes.uiIndexBuffer->Unmap(0, nullptr);
    if (uiRes.uiOrthoConstantBuffer) uiRes.uiOrthoConstantBuffer->Unmap(0, nullptr);

    uiRes = {};
}

// PushRect
void PushRect( UIDrawContext& ctx, float x, float y, float w, float h,
    uint32_t color, DX12ResourcesUI& uiRes) {
    if (ctx.vertexCount + 4 > uiRes.maxVertices) return;
    if (ctx.indexCount + 6 > uiRes.maxIndices) return;

    uint16_t base = ctx.vertexCount;

    ctx.vertexPtr[0] = { x,y,0,0,color, UI_ENGLISH_ATLAS_SLOT };
    ctx.vertexPtr[1] = { x + w,y,0,0,color, UI_ENGLISH_ATLAS_SLOT };
    ctx.vertexPtr[2] = { x + w,y + h,0,0,color, UI_ENGLISH_ATLAS_SLOT };
    ctx.vertexPtr[3] = { x,y + h,0,0,color, UI_ENGLISH_ATLAS_SLOT };

    ctx.indexPtr[0] = base + 0;
    ctx.indexPtr[1] = base + 1;
    ctx.indexPtr[2] = base + 2;
    ctx.indexPtr[3] = base + 0;
    ctx.indexPtr[4] = base + 2;
    ctx.indexPtr[5] = base + 3;
}

static void PushTexturedQuad(UIDrawContext& ctx, float x, float y, float w, float h,
    const UIAtlasRegion& region, uint32_t atlasIndex, uint32_t color, DX12ResourcesUI& uiRes) {
    if (ctx.vertexCount + 4 > uiRes.maxVertices) return;
    if (ctx.indexCount + 6 > uiRes.maxIndices) return;

    uint16_t base = ctx.vertexCount;
    ctx.vertexPtr[0] = { x,     y,     region.uvMinX, region.uvMinY, color, atlasIndex };
    ctx.vertexPtr[1] = { x + w, y,     region.uvMaxX, region.uvMinY, color, atlasIndex };
    ctx.vertexPtr[2] = { x + w, y + h, region.uvMaxX, region.uvMaxY, color, atlasIndex };
    ctx.vertexPtr[3] = { x,     y + h, region.uvMinX, region.uvMaxY, color, atlasIndex };

    ctx.indexPtr[0] = base + 0;
    ctx.indexPtr[1] = base + 1;
    ctx.indexPtr[2] = base + 2;
    ctx.indexPtr[3] = base + 0;
    ctx.indexPtr[4] = base + 2;
    ctx.indexPtr[5] = base + 3;

    ctx.vertexPtr += 4;
    ctx.indexPtr += 6;
    ctx.vertexCount += 4;
    ctx.indexCount += 6;
}

void PushRoundedRectangle(UIDrawContext& ctx, float x, float y, float w, float h, float radiusPx,
    uint32_t color, DX12ResourcesUI& uiRes) {
    if (w <= 0.0f || h <= 0.0f) return;
    if (ctx.vertexCount + 36 > uiRes.maxVertices) return;
    if (ctx.indexCount + 54 > uiRes.maxIndices) return;

    const float clampedRadius = std::max(1.0f, std::min(radiusPx, 0.5f * std::min(w, h)));
    const float xCuts[4] = { x, x + clampedRadius, x + w - clampedRadius, x + w };
    const float yCuts[4] = { y, y + clampedRadius, y + h - clampedRadius, y + h };

    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            PushTexturedQuad(ctx, xCuts[col], yCuts[row],
                xCuts[col + 1] - xCuts[col], yCuts[row + 1] - yCuts[row],
                gIconAtlasMetadata.roundedRectangle.regions[row][col],
                UI_ICON_ATLAS_SLOT, color, uiRes);
        }
    }
}

static void PushIcon(UIDrawContext& ctx, float x, float y, float w, float h, char32_t iconCodepoint,
    uint32_t color, DX12ResourcesUI& uiRes) {
    auto iconIt = iconGlyphLookup.find(iconCodepoint);
    if (iconIt == iconGlyphLookup.end()) return;
    const Glyph& glyph = iconIt->second;
    UIAtlasRegion icon{ glyph.uvMinX, glyph.uvMinY, glyph.uvMaxX, glyph.uvMaxY };
    PushTexturedQuad(ctx, x, y, w, h, icon, UI_ICON_ATLAS_SLOT, color, uiRes);
}

// Returns true if clicked this frame
bool PushInteractiveRect(UIDrawContext& ctx, float x, float y, float w, float h, uint32_t baseColor,
    uint32_t id, const UIInput& input, DX12ResourcesUI& uiRes, bool enabled = true) {
    uint32_t color = baseColor;

    bool hovered = enabled && (input.mouseX >= x && input.mouseX < x + w &&
        input.mouseY >= y && input.mouseY < y + h);

    if (hovered) color = 0xFF555555; // hover tint (TODO: theme-aware)
    if (hovered && input.leftButtonDown) color = 0xFF333333; // pressed tint
    if (!enabled) color = 0xFF1E1E1E; // If disabled, force a darker/grayer base color
    
    PushRect(ctx, x, y, w, h, color, uiRes);

    if (!enabled) return false;// Disabled controls do NOT respond to clicks
    if (hovered && input.leftButtonPressedThisFrame) {
        return true;
    }
    return false;
}

void PushText(UIDrawContext& ctx, float x, float y, const char* text, uint32_t color, DX12ResourcesUI& uiRes)
{
    float cursorX = x;
    uint32_t glyphCount = 0;

    for (const char* p = text; *p; ++p)
    {
        // Bounds Checking (Crucial for text strings)
        if (ctx.vertexCount + (glyphCount + 1) * 4 > uiRes.maxVertices) return;
        if (ctx.indexCount + (glyphCount + 1) * 6 > uiRes.maxIndices) return;

        char c = *p;
        if (glyphLookup.find(c) == glyphLookup.end()) continue;

        const Glyph& g = glyphLookup[c];
        if (g.width <= 0 || g.height <= 0) {
            cursorX += g.advanceX;
            continue;
        }

        float xpos = cursorX + g.bearingX;
        float ypos = y - g.bearingY;
        float w = (float)g.width;
        float h = (float)g.height;
        uint32_t vertexOffset = glyphCount * 4;
        uint32_t indexOffset = glyphCount * 6;
        
        // Add 4 vertices. Write relative to the current pointer (0, 1, 2, 3)
        uint32_t vidx = ctx.vertexCount + vertexOffset;
        ctx.vertexPtr[vertexOffset + 0] = { xpos,     ypos,     g.uvMinX, g.uvMinY, color, UI_ENGLISH_ATLAS_SLOT };
        ctx.vertexPtr[vertexOffset + 1] = { xpos + w, ypos,     g.uvMaxX, g.uvMinY, color, UI_ENGLISH_ATLAS_SLOT };
        ctx.vertexPtr[vertexOffset + 2] = { xpos + w, ypos + h, g.uvMaxX, g.uvMaxY, color, UI_ENGLISH_ATLAS_SLOT };
        ctx.vertexPtr[vertexOffset + 3] = { xpos,     ypos + h, g.uvMinX, g.uvMaxY, color, UI_ENGLISH_ATLAS_SLOT };

        // Add 6 indices. Write indices relative to the current index pointer
        ctx.indexPtr[indexOffset + 0] = vidx + 0;
        ctx.indexPtr[indexOffset + 1] = vidx + 1;
        ctx.indexPtr[indexOffset + 2] = vidx + 2;
        ctx.indexPtr[indexOffset + 3] = vidx + 0;
        ctx.indexPtr[indexOffset + 4] = vidx + 2;
        ctx.indexPtr[indexOffset + 5] = vidx + 3;

        glyphCount++;
        cursorX += g.advanceX;
    }

    ctx.vertexPtr += glyphCount * 4;
    ctx.indexPtr += glyphCount * 6;
    ctx.vertexCount += glyphCount * 4;
    ctx.indexCount += glyphCount * 6;
}

// Convenience for tabs (fixed width for now)
bool PushTab(UIDrawContext& ctx, float x, float w, float h, uint16_t tabID, bool isActive,
    const UIInput& input, DX12ResourcesUI& uiRes) {
    
    uint32_t color = isActive ? COLOR_UI_TAB_ACTIVE : COLOR_UI_TAB_INACTIVE;
    uint32_t actionID = 0x10000000u | tabID;   // high bit = tab family

    if (PushInteractiveRect(ctx, x, 0, w, h, color, actionID, input, uiRes)) {
        // Optional immediate feedback (UI thread)
        // window.activeTabIndex = tabID;   // you can still do it here if you want instant visual
        return true;
    }
    return false;
}

// This function renders the list of tabs, all top menu buttons (with dropdowns if required),
// side favorite / frequent buttons bars, right side property window, bottom status bar.
// This is also responsible for all relevant DirectX12 configurations required for rendering User Interface.
void RenderUIOverlay(SingleUIWindow& window, ID3D12GraphicsCommandList* cmd, DX12ResourcesUI& uiRes,
    float monitorDPIX, float monitorDPIY, const UIInput& input) {
    
    if (!cmd) return; //Defensive check.

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
    // Bind ortho constant buffer (still root parameter 0)
    cmd->SetGraphicsRootConstantBufferView(0, uiRes.uiOrthoConstantBuffer->GetGPUVirtualAddress());

    float W = (float)window.dx.WindowWidth;
    float H = (float)window.dx.WindowHeight;
    float* ortho = (float*)uiRes.pOrthoDataBegin;

    ortho[0] = 2 / W; ortho[1] = 0;   ortho[2] = 0; ortho[3] = -1;
    ortho[4] = 0;   ortho[5] = -2 / H; ortho[6] = 0; ortho[7] = 1;
    ortho[8] = 0;   ortho[9] = 0;   ortho[10] = 1; ortho[11] = 0;
    ortho[12] = 0;  ortho[13] = 0;  ortho[14] = 0; ortho[15] = 1;

    cmd->SetGraphicsRootConstantBufferView( 0, uiRes.uiOrthoConstantBuffer->GetGPUVirtualAddress());

    UIDrawContext ctx;
    ctx.vertexPtr = reinterpret_cast<UIVertex*>(uiRes.pVertexDataBegin);
    ctx.indexPtr = reinterpret_cast<uint16_t*>(uiRes.pIndexDataBegin);
    ctx.vertexCount = 0;
    ctx.indexCount = 0;
    float pixelsPerMMx = monitorDPIX / 25.4f;
    float pixelsPerMMy = monitorDPIY / 25.4f;
    float buttonWidthPx = std::round(UI_BUTTON_WIDTH_MM * pixelsPerMMx);
    float iconSizePx = std::round(UI_ICON_SIZE_MM * pixelsPerMMy);
    float textHeightPx = std::round(UI_TEXT_HEIGHT_MM * pixelsPerMMy);
    float buttonHeightPx = std::max(std::round(UI_BUTTON_HEIGHT_MM * pixelsPerMMy),
        std::max(iconSizePx, textHeightPx) + 4.0f);
	float iconReservedWidthPx = iconSizePx + 4.0f; // 2 pixels padding on each side of the icon.
    float textStartOffsetPx = iconReservedWidthPx + 4.0f;
    float textEndInsetPx = 6.0f;
    float tabBarHeightPx = std::round(UI_TAB_BAR_HEIGHT_MM * pixelsPerMMy);
    float roundedCornerRadiusPx = std::max(1.0f, std::round(UI_BUTTON_CORNER_RADIUS_MM * pixelsPerMMy));

    auto canPushRect = [&]() {
        return ctx.vertexCount + 4 <= uiRes.maxVertices &&
            ctx.indexCount + 6 <= uiRes.maxIndices;
    };

    auto advanceRect = [&]() {
        ctx.vertexPtr += 4;
        ctx.indexPtr += 6;
        ctx.vertexCount += 4;
        ctx.indexCount += 6;
    };

    auto pushRect = [&](float x, float y, float w, float h, uint32_t color) {
        bool pushed = canPushRect();
        PushRect(ctx, x, y, w, h, color, uiRes);
        if (pushed) advanceRect();
    };

    auto pushRoundedRect = [&](float x, float y, float w, float h, uint32_t color) {
        PushRoundedRectangle(ctx, x, y, w, h, roundedCornerRadiusPx, color, uiRes);
    };

    auto textScaleForHeight = [&](float targetHeight) {
        auto glyphIt = glyphLookup.find(U'M');
        if (glyphIt == glyphLookup.end() || glyphIt->second.height <= 0) return 1.0f;

        return targetHeight / (float)glyphIt->second.height;
    };

    const float uiTextScale = textScaleForHeight(textHeightPx);

    auto measureTextWidth = [&](const char32_t* text, float scale) { //Must support all Unicode latter.
        if (!text) return 0.0f;

        float cursorX = 0.0f;
        float maxRight = 0.0f;

        for (const char32_t* p = text; *p; ++p) {

            auto glyphIt = glyphLookup.find(*p);
            if (glyphIt == glyphLookup.end()) continue;

            const Glyph& g = glyphIt->second;
            float glyphLeft = cursorX + (float)g.bearingX * scale;
            float glyphRight = glyphLeft + (float)g.width * scale;
            if (glyphRight > maxRight) maxRight = glyphRight;
            cursorX += (float)g.advanceX * scale;
        }

        return std::max(cursorX, maxRight);
    };

    auto pushTextClipped = [&](float x, float y, const char32_t* text, float maxWidth, uint32_t color,
        float scale) {
        if (!text || maxWidth <= 0.0f) return;

        float cursorX = x;
        float textRight = x + maxWidth;

        for (const char32_t* p = text; *p; ++p) {
            if (*p > 0x7F) continue;

            auto glyphIt = glyphLookup.find(*p);
            if (glyphIt == glyphLookup.end()) continue;

            const Glyph& g = glyphIt->second;
            if (g.width <= 0 || g.height <= 0) {
                cursorX += (float)g.advanceX * scale;
                continue;
            }

            // It is always better to be aligned to pixels for better text clarity.
            float xpos = std::floor(cursorX + (float)g.bearingX * scale + 0.5f);
            float ypos = std::floor(y - (float)g.bearingY * scale + 0.5f);
            float glyphWidth = (float)g.width * scale;
            float glyphHeight = (float)g.height * scale;
            float glyphRight = xpos + glyphWidth;

            if (glyphRight > textRight) break;
            if (ctx.vertexCount + 4 > uiRes.maxVertices) return;
            if (ctx.indexCount + 6 > uiRes.maxIndices) return;

            uint16_t base = ctx.vertexCount;
            ctx.vertexPtr[0] = { xpos,                  ypos,                   g.uvMinX, g.uvMinY, color, UI_ENGLISH_ATLAS_SLOT };
            ctx.vertexPtr[1] = { xpos + glyphWidth,     ypos,                   g.uvMaxX, g.uvMinY, color, UI_ENGLISH_ATLAS_SLOT };
            ctx.vertexPtr[2] = { xpos + glyphWidth,     ypos + glyphHeight,     g.uvMaxX, g.uvMaxY, color, UI_ENGLISH_ATLAS_SLOT };
            ctx.vertexPtr[3] = { xpos,                  ypos + glyphHeight,     g.uvMinX, g.uvMaxY, color, UI_ENGLISH_ATLAS_SLOT };

            ctx.indexPtr[0] = base + 0;
            ctx.indexPtr[1] = base + 1;
            ctx.indexPtr[2] = base + 2;
            ctx.indexPtr[3] = base + 0;
            ctx.indexPtr[4] = base + 2;
            ctx.indexPtr[5] = base + 3;

            ctx.vertexPtr += 4;
            ctx.indexPtr += 6;
            ctx.vertexCount += 4;
            ctx.indexCount += 6;
            cursorX += (float)g.advanceX * scale;
        }
    };

    auto textBaselineY = [&](float y, float h, float scale) {
        auto glyphIt = glyphLookup.find(U'M');
        if (glyphIt == glyphLookup.end()) return y + h * 0.7f;

        const Glyph& g = glyphIt->second;
        return y + h * 0.5f + (float)g.bearingY * scale - (float)g.height * scale * 0.5f;
    };

    auto pushTab = [&](float x, float w, float h, uint16_t tabID, bool isActive) {
        bool pushed = canPushRect();
        bool clicked = PushTab(ctx, x, w, h, tabID, isActive, input, uiRes);
        if (pushed) advanceRect();
        return clicked;
    };

    // ENGINEERING / PROJECT TABs
    float tabWidth = 120.0f;
    float currentX = 0.0f;

    uint16_t tabCount = publishedTabCount.load(std::memory_order_acquire);
    uint16_t* tabList = publishedTabIndexes.load(std::memory_order_acquire);

    for (uint16_t i = 0; i < tabCount; i++) {
        uint16_t tabID = tabList[i];
        bool active = (window.activeTabIndex == tabID);
        if (pushTab(currentX, tabWidth, tabBarHeightPx, tabID, active)) {
            window.activeTabIndex = tabID; // instant visual feedback
        }
        currentX += tabWidth;
    }

    // TOP BUTTONS (ACTION GROUP BAR)
    currentX = 5.0f; // reset X for action groups

    const float buttonBaseHeight = buttonHeightPx;
    const float buttonGap = UI_BUTTON_GAP_MM * pixelsPerMMx;
    const float groupGap = 96.0f;

    float actionGroupLabelY = (UI_TAB_BAR_HEIGHT_MM + UI_DIVIDER_GAP_PX)* pixelsPerMMy;
	float groupLabelHeight = UI_ACTION_GROUP_LABEL_HEIGHT_MM * pixelsPerMMy;
    float topActionGroupY = (UI_TAB_BAR_HEIGHT_MM + UI_DIVIDER_GAP_PX +
        UI_ACTION_GROUP_LABEL_HEIGHT_MM) * pixelsPerMMy;
    float actionSubGroupLabelY = (UI_TAB_BAR_HEIGHT_MM + UI_DIVIDER_GAP_PX +
        UI_ACTION_GROUP_LABEL_HEIGHT_MM + UI_DIVIDER_GAP_PX +
        UI_TOP_ACTION_GROUP_HEIGHT_MM + UI_DIVIDER_GAP_PX) * pixelsPerMMy;

    auto localizedString = [](uint32_t stringID) {
        const char32_t* text = UITranslations::GetUILocalizedString(stringID, UILanguage::English);
        return text ? text : U"";
    };

    auto localizedControlLabel = [&](const UIControlDefinition& ctrl) {
        const char32_t* label = localizedString((uint32_t)ctrl.action);
        if (*label != U'\0') return label;

        if (ctrl.action == UIAction::INVALID || ctrl.type == 0 || ctrl.type == 3) {
            return localizedString(ctrl.nameStringID);
        }

        return U"";
    };

    float verticalSlotMaxSize = 0.0f;
    float currentActionGroupStartX = 0.0f;
    float actionSubGroupWidth = 0.0f;
    uint32_t currentActionGroupIndex = 100000; // To large value to trigger change at i=0
    uint32_t currentSubGroupID = 0;
    
    for (size_t i = 0; i < TotalUIControls; ++i) {
        const auto& ctrl = AllUIControls[i];

        if (ctrl.actionGroupIndex != currentActionGroupIndex) { // Action Group change
            const UIActionGroupNames& group = topUIActionGroupNames[ctrl.actionGroupIndex];
            const char32_t* label = localizedString(group.labelStringID);

            pushRect(currentActionGroupStartX, actionGroupLabelY, groupGap, groupLabelHeight, 0xFF2D2D30);
            pushTextClipped(currentActionGroupStartX + 4.0f, textBaselineY(actionGroupLabelY, groupLabelHeight, uiTextScale),
                label, groupGap - 8.0f, COLOR_UI_TEXT, uiTextScale);

            currentActionGroupIndex = ctrl.actionGroupIndex;
            currentActionGroupStartX += groupGap;
            pushRect(currentActionGroupStartX + 3, actionGroupLabelY, 4, 16, 0xFF555555); // group separator
            currentActionGroupStartX += 10.0f; // +3+4+3
        }

        // Button geometry
        float btnWidth = buttonWidthPx;
        float btnHeight = buttonHeightPx;
        float btnY = topActionGroupY;
        const char32_t* label = localizedControlLabel(ctrl);
        
        if (ctrl.showText && *label != U'\0') {
            float contentWidth = textStartOffsetPx + measureTextWidth(label, uiTextScale) + textEndInsetPx;
            btnWidth = std::max(btnWidth, contentWidth);
        }

        if (ctrl.noOfVerticalSlots > 1) {btnY += ctrl.verticalSlotNo * buttonBaseHeight;}
        uint32_t baseColor = StableRandomUIColour((uint32_t)ctrl.action ^ ((uint32_t)i * 0x9E3779B9u));// Render
        uint32_t iconColor = StableRandomUIColour(((uint32_t)ctrl.action << 1) ^ 0xA511E9B3u ^ (uint32_t)i);

        if (ctrl.type == 1 || ctrl.type == 2) {                     // Button or Dropdown trigger
            bool hovered = ctrl.isEnabled && (input.mouseX >= currentX && input.mouseX < currentX + btnWidth &&
                input.mouseY >= btnY && input.mouseY < btnY + btnHeight);
            uint32_t drawColor = hovered && input.leftButtonDown ? 0xFF333333 : baseColor;
            if (hovered && !input.leftButtonDown) drawColor = 0xFF555555;
            pushRoundedRect(currentX, btnY, btnWidth, btnHeight, drawColor);

            bool clicked = hovered && input.leftButtonPressedThisFrame;

            if (clicked && ctrl.isEnabled) {
                PushUIAction((uint32_t)ctrl.action);
                if (ctrl.zIndex == 1) { // Dropdown trigger
                    window.activeDropdownAction = ctrl.action;
                }
            }

            if (!ctrl.isEnabled) { // Gray-out overlay for disabled controls
                pushRoundedRect(currentX, btnY, btnWidth, btnHeight, 0xAA333333);
            }
        }
        else if (ctrl.type == 3) {
            // Future textbox
            pushRoundedRect(currentX, btnY, btnWidth, btnHeight, 0xFF1E1E1E);
        }
        else {
            // Plain label
            pushRoundedRect(currentX, btnY, btnWidth, btnHeight, 0xFF2D2D30);
        }

        float iconX = currentX + (iconReservedWidthPx - iconSizePx) * 0.5f;
        float iconY = btnY + (btnHeight - iconSizePx) * 0.5f;
        const uint32_t randomIconIndex =
            ((uint32_t)ctrl.action ^ (uint32_t)i) % (uint32_t)gIconAtlasMetadata.dummyIconCodepoints.size();
        PushIcon(ctx, iconX, iconY, iconSizePx, iconSizePx,
            gIconAtlasMetadata.dummyIconCodepoints[randomIconIndex], iconColor, uiRes);

        if (ctrl.showText) {
            float textX = currentX + textStartOffsetPx;
            float textWidth = btnWidth - textStartOffsetPx - textEndInsetPx;
            pushTextClipped(textX, textBaselineY(btnY, btnHeight, uiTextScale),
                label, textWidth, 0xFFFFFFFF, uiTextScale);
        }

        if (btnWidth > verticalSlotMaxSize) verticalSlotMaxSize = btnWidth;
        if (i < (TotalUIControls-1)) { //Except last one.
            if (AllUIControls[i + 1].verticalSlotNo == 0) {
                currentX += verticalSlotMaxSize + buttonGap;
                actionSubGroupWidth += verticalSlotMaxSize + buttonGap;
                verticalSlotMaxSize = 0;
            }
        }

        if (ctrl.actionSubGroupIndex != currentSubGroupID) { // Sub-Group change
            //Means current button belongs to different action sub group.
            //Hence write the action sub group name of previous action sub group.
            const UIActionGroupNames& group = topUIActionSubGroupNames[currentSubGroupID];
            const char32_t* label = localizedString(group.labelStringID);

            pushRect(currentX - actionSubGroupWidth + 1.0f,
                actionSubGroupLabelY, actionSubGroupWidth - 4.0f, groupLabelHeight, 0xFF2D2D30);
            pushTextClipped(currentX - actionSubGroupWidth / 2 - measureTextWidth(label, uiTextScale),
                textBaselineY(actionSubGroupLabelY, groupLabelHeight, uiTextScale),
                label, actionSubGroupWidth - 8.0f, COLOR_UI_TEXT, uiTextScale);

            // Draw thin 1px vertical separator line between sub-groups
            if (actionSubGroupWidth > 0.0f) {  // Don't draw before first group
                // Place it exactly in the middle of the 'buttonGap' between the two columns
                float lineX = std::floor(currentX - (buttonGap / 2.0f));
                // Span the height of the action group buttons (assumes 3 rows max per your UI_TOP_ACTION_GROUP_HEIGHT_MM)
                float lineHeight = UI_TOP_ACTION_GROUP_HEIGHT_MM * pixelsPerMMy;
                // Draw 1px wide line. Using 0xFF555555 to match your Action Group separator color
                pushRect(lineX, topActionGroupY, 1.0f, lineHeight, 0xFF555555);
            }

            currentSubGroupID = ctrl.actionSubGroupIndex;
            actionSubGroupWidth = 0; //Reset
        }
    }

    currentX += 30.0f;// Final padding

    // ACTIVE DROPDOWN (placeholder)
    if (window.activeDropdownAction != UIAction::INVALID) {
        float dropX = 400.0f;   // TODO: track real button X for proper positioning
        float dropY = topActionGroupY + 80.0f;
        pushRect(dropX, dropY, 160, 220, 0xFF1E1E1E);
        window.activeDropdownAction = UIAction::INVALID;   // immediate-mode auto-close
    }

    // DRAW ALL UI GEOMETRY
    if (ctx.indexCount == 0) return;

    D3D12_VERTEX_BUFFER_VIEW vbv{};
    vbv.BufferLocation = uiRes.uiVertexBuffer->GetGPUVirtualAddress();
    vbv.SizeInBytes = ctx.vertexCount * sizeof(UIVertex);
    vbv.StrideInBytes = sizeof(UIVertex);

    D3D12_INDEX_BUFFER_VIEW ibv{};
    ibv.BufferLocation = uiRes.uiIndexBuffer->GetGPUVirtualAddress();
    ibv.SizeInBytes = ctx.indexCount * sizeof(uint16_t);
    ibv.Format = DXGI_FORMAT_R16_UINT;

    cmd->IASetVertexBuffers(0, 1, &vbv);
    cmd->IASetIndexBuffer(&ibv);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawIndexedInstanced(ctx.indexCount, 1, 0, 0, 0);
}
