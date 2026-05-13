// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

#include "UserInterface-DirectX12.h"
#include <algorithm>
#include <d3dcompiler.h>
#include "FontManager.h"
#include <MemoryManagerGPU-DirectX12.h>
#include "विश्वकर्मा.h"
#include "TextureSaver.h"
#include "UserInterfaceTranslationCompiled.h"
extern शंकर gpu;
extern std::atomic<uint16_t*> publishedTabIndexes;
extern std::atomic<uint16_t>  publishedTabCount;
extern void PrintHResult(int);
std::atomic<uint32_t> actionWriteIndex;
std::string charset = " ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789&(),./-";
std::atomic<uint64_t> atlasFence = 0;

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
static void CompileShader( const char* code, const char* entry, const char* target, UINT flags,
    ComPtr<ID3DBlob>& outBlob) {

    ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3DCompile( code, strlen(code), nullptr, nullptr, nullptr, entry, target, flags, 0,
        &outBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) { std::cerr << (char*)errorBlob->GetBufferPointer() << std::endl; }
        ThrowIfFailed(hr);
    }
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

void InitUIResources( DX12ResourcesUI& uiRes, ID3D12Device* device) {
    if (!InitFontSystem()) { // FONT system initialization (CPU-side)
        std::cerr << "Font system initialization failed failed\n";
        return;
    }

    // Root signature
    CD3DX12_DESCRIPTOR_RANGE1 ranges[2]; // Descriptor ranges

    // Range 0: SRV (t0)  → from srvHeap // 1: 1 Texture, 0: register t0 = atlas
    ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE);
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
    heapDesc.NumDescriptors = 1;
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
    UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    UINT compileFlags = 0;
#endif

    static const char* vsCode = R"(
cbuffer OrthoConstantBuffer : register(b0) {
    float4x4 ortho;
};

struct VSInput {
    float2 position : POSITION;
    float2 uv       : TEXCOORD0;
    uint   color    : COLOR0;
};

struct PSInput {
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
    uint   color    : COLOR0;
};

PSInput VSMain(VSInput input) {
    PSInput output;
    float4 pos = float4(input.position,0,1);
    output.position = mul(pos,ortho);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}
)";

    static const char* psCode = R"(
struct PSInput {
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
    uint   color    : COLOR0;
};

Texture2D atlas : register(t0);
SamplerState samp : register(s0);

float4 PSMain(PSInput input) : SV_TARGET {
    float4 tex = atlas.Sample(samp, input.uv);
    float r = ((input.color >> 0) & 0xFF) / 255.0;
    float g = ((input.color >> 8) & 0xFF) / 255.0;
    float b = ((input.color >> 16) & 0xFF) / 255.0;
    float a = ((input.color >> 24) & 0xFF) / 255.0;
    return float4(r, g, b, a) * tex;
}
)";

    ComPtr<ID3DBlob> vsBlob;
    ComPtr<ID3DBlob> psBlob;
    CompileShader(vsCode, "VSMain", "vs_5_0", compileFlags, vsBlob);
    CompileShader(psCode, "PSMain", "ps_5_0", compileFlags, psBlob);
    // Input layout

    D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0 },
        { "TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0 },
        { "COLOR",0,DXGI_FORMAT_R32_UINT,0,16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0 }
    };

    // PSO
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { layout,_countof(layout) };
    psoDesc.pRootSignature = uiRes.uiRootSignature.Get();
    psoDesc.VS = CD3DX12_SHADER_BYTECODE(vsBlob.Get());
    psoDesc.PS = CD3DX12_SHADER_BYTECODE(psBlob.Get());
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
    
    AtlasBitmap atlas = BuildFontAtlas();// BUILD ATLAS ON CPU
    
    TextureUploadDesc desc = {};
    desc.width = atlas.width;
    desc.height = atlas.height;
    desc.format = DXGI_FORMAT_R8_UNORM;
    desc.pixels = atlas.pixels.data();
    desc.rowPitch = atlas.width;

    int bytesPerPixel = 1; // Use 4 if DXGI_FORMAT_R8G8B8A8_UNORM, use 1 if DXGI_FORMAT_R8_UNORM
    bool isSaved = SaveToBmp("font_atlas_debug.bmp", atlas.pixels.data(),
        atlas.width, atlas.height, bytesPerPixel);
    if (isSaved) std::cout << "SUCCESS: Debug atlas saved to working directory." << std::endl;
    else std::cerr << "FAIL: Could not save debug atlas. Pointer might be invalid!" << std::endl;

    SubmitTextureUpload(desc, &uiRes.uiAtlasTexture, &atlasFence);// Enqueue the upload through upload queue
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
    
    device->CreateShaderResourceView(uiRes.uiAtlasTexture.Get(), &srvDesc,
        uiRes.srvHeap->GetCPUDescriptorHandleForHeapStart());
    
    std::wcout << L"UI Atlas uploaded and SRV created (fence = " << atlasReadyFence << L")\n";
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

    ctx.vertexPtr[0] = { x,y,0,0,color };
    ctx.vertexPtr[1] = { x + w,y,0,0,color };
    ctx.vertexPtr[2] = { x + w,y + h,0,0,color };
    ctx.vertexPtr[3] = { x,y + h,0,0,color };

    ctx.indexPtr[0] = base + 0;
    ctx.indexPtr[1] = base + 1;
    ctx.indexPtr[2] = base + 2;
    ctx.indexPtr[3] = base + 0;
    ctx.indexPtr[4] = base + 2;
    ctx.indexPtr[5] = base + 3;
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
        ctx.vertexPtr[vertexOffset + 0] = { xpos,     ypos,     g.uvMinX, g.uvMinY, color };
        ctx.vertexPtr[vertexOffset + 1] = { xpos + w, ypos,     g.uvMaxX, g.uvMinY, color };
        ctx.vertexPtr[vertexOffset + 2] = { xpos + w, ypos + h, g.uvMaxX, g.uvMaxY, color };
        ctx.vertexPtr[vertexOffset + 3] = { xpos,     ypos + h, g.uvMinX, g.uvMaxY, color };

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
    float monitorDPI, const UIInput& input) {
    
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
    float pixelsPerMM = monitorDPI / 25.4f;
    float buttonWidthPx = UI_BUTTON_WIDTH_MM * pixelsPerMM;
    float buttonHeightPx = UI_BUTTON_HEIGHT_MM * pixelsPerMM;
    float iconReservedWidthPx = (UI_ICON_SIZE_MM + 2) * pixelsPerMM;
    float iconSizePx = UI_ICON_SIZE_MM * pixelsPerMM;
    float textStartOffsetPx = iconReservedWidthPx + 5.0f * pixelsPerMM;
    float textEndInsetPx = 10.0f * pixelsPerMM;
    float tabBarHeight = UI_TAB_BAR_HEIGHT_MM * pixelsPerMM;

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

    auto pushTextClipped = [&](float x, float y, const char32_t* text, float maxWidth, uint32_t color) {
        if (!text || maxWidth <= 0.0f) return;

        float cursorX = x;
        float textRight = x + maxWidth;

        for (const char32_t* p = text; *p; ++p) {
            if (*p > 0x7F) continue;

            auto glyphIt = glyphLookup.find(*p);
            if (glyphIt == glyphLookup.end()) continue;

            const Glyph& g = glyphIt->second;
            if (g.width <= 0 || g.height <= 0) {
                cursorX += g.advanceX;
                continue;
            }

            float xpos = cursorX + g.bearingX;
            float ypos = y - g.bearingY;
            float glyphRight = xpos + (float)g.width;

            if (glyphRight > textRight) break;
            if (ctx.vertexCount + 4 > uiRes.maxVertices) return;
            if (ctx.indexCount + 6 > uiRes.maxIndices) return;

            uint16_t base = ctx.vertexCount;
            ctx.vertexPtr[0] = { xpos,                  ypos,                   g.uvMinX, g.uvMinY, color };
            ctx.vertexPtr[1] = { xpos + (float)g.width, ypos,                   g.uvMaxX, g.uvMinY, color };
            ctx.vertexPtr[2] = { xpos + (float)g.width, ypos + (float)g.height, g.uvMaxX, g.uvMaxY, color };
            ctx.vertexPtr[3] = { xpos,                  ypos + (float)g.height, g.uvMinX, g.uvMaxY, color };

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
            cursorX += g.advanceX;
        }
    };

    auto textBaselineY = [&](float y, float h) {
        auto glyphIt = glyphLookup.find(U'M');
        if (glyphIt == glyphLookup.end()) return y + h * 0.7f;

        const Glyph& g = glyphIt->second;
        return y + h * 0.5f + (float)g.bearingY - (float)g.height * 0.5f;
    };

    auto pushTab = [&](float x, float w, float h, uint16_t tabID, bool isActive) {
        bool pushed = canPushRect();
        bool clicked = PushTab(ctx, x, w, h, tabID, isActive, input, uiRes);
        if (pushed) advanceRect();
        return clicked;
    };
    float topActionGroupY = tabBarHeight + UI_DIVIDER_WIDTH_PX;   // ← only one declaration

    // ENGINEERING / PROJECT TABs
    float tabWidth = 120.0f;
    float currentX = 0.0f;
    float currentActionGroupX = 0.0f;
    float currentActionSubGroupX = 0.0f;

    uint16_t tabCount = publishedTabCount.load(std::memory_order_acquire);
    uint16_t* tabList = publishedTabIndexes.load(std::memory_order_acquire);

    for (uint16_t i = 0; i < tabCount; i++) {
        uint16_t tabID = tabList[i];
        bool active = (window.activeTabIndex == tabID);
        if (pushTab(currentX, tabWidth, tabBarHeight, tabID, active)) {
            window.activeTabIndex = tabID; // instant visual feedback
        }
        currentX += tabWidth;
    }

    // TOP BUTTONS (ACTION GROUP BAR)
    currentX = 20.0f; // reset X for action groups

    const float buttonBaseHeight = buttonHeightPx;
    const float buttonGap = UI_BUTTON_GAP_MM * pixelsPerMM;
    const float subGroupGap = 18.0f;
    const float groupGap = 28.0f;

    uint32_t currentActionGroupIndex = 0xFFFFFFFF;
    uint32_t currentSubGroupID = 0xFFFFFFFF;

    float groupLabelY = topActionGroupY;
    float subGroupLabelY = topActionGroupY + 96.0f;
    const float groupLabelHeight = 16.0f;
    float currentActionGroupStartX = currentX;

    auto localizedString = [](uint32_t stringID) {
        const char32_t* text = UITranslations::GetUILocalizedString(stringID, UILanguage::English);
        return text ? text : U"";
    };

    auto drawActionGroupLabel = [&](uint32_t groupIndex, float startX, float endX) {
        if (groupIndex == 0xFFFFFFFF || groupIndex >= TotalTopUIActionGroups) return;

        const UIActionGroupNames& group = topUIActionGroupNames[groupIndex];
        if (!group.isEnabled) return;

        const char32_t* label = localizedString(group.labelStringID);
        if (*label == U'\0') return;

        float labelWidth = std::max(32.0f, endX - startX);
        pushRect(startX, groupLabelY, labelWidth, groupLabelHeight, 0xFF2D2D30);
        pushTextClipped(startX + 4.0f, textBaselineY(groupLabelY, groupLabelHeight),
            label, labelWidth - 8.0f, COLOR_UI_TEXT);
    };

    auto localizedControlLabel = [&](const UIControlDefinition& ctrl) {
        const char32_t* label = localizedString((uint32_t)ctrl.action);
        if (*label != U'\0') return label;

        if (ctrl.action == UIAction::INVALID || ctrl.type == 0 || ctrl.type == 3) {
            return localizedString(ctrl.nameStringID);
        }

        return U"";
    };

    for (size_t i = 0; i < TotalUIControls; ++i) {
        const auto& ctrl = AllUIControls[i];

        if (ctrl.actionGroupIndex != currentActionGroupIndex) { // Action Group change
            if (currentActionGroupIndex != 0xFFFFFFFF) {
                drawActionGroupLabel(currentActionGroupIndex, currentActionGroupStartX,
                    currentActionGroupX - buttonGap);
                currentActionGroupX += groupGap;
            }
            currentActionGroupIndex = ctrl.actionGroupIndex;
            currentSubGroupID = 0xFFFFFFFF;

            pushRect(currentActionGroupX, groupLabelY, 6, 48, 0xFF555555); // group separator
            currentActionGroupX += 96.0f;
            currentActionGroupStartX = currentActionGroupX;
        }
        
        if (ctrl.actionSubGroupIndex != currentSubGroupID) { // Sub-Group change
            if (currentSubGroupID != 0xFFFFFFFF) { currentActionSubGroupX += subGroupGap;}
            currentSubGroupID = ctrl.actionSubGroupIndex;

            pushRect(currentActionSubGroupX, subGroupLabelY, 110, 16, 0xFF2D2D30); // sub-group label bar
            currentActionSubGroupX += 96.0f; //TODO: To be computed based on actual button heights.
        }

        // Button geometry (fixed physical size)
        float btnWidth = buttonWidthPx;
        float btnHeight = buttonHeightPx;
        float btnY = topActionGroupY + 38.0f;

        if (ctrl.noOfVerticalSlots > 1) {btnY += ctrl.verticalSlotNo * buttonBaseHeight;}
        uint32_t baseColor = StableRandomUIColour((uint32_t)ctrl.action ^ ((uint32_t)i * 0x9E3779B9u));// Render
        uint32_t iconColor = StableRandomUIColour(((uint32_t)ctrl.action << 1) ^ 0xA511E9B3u ^ (uint32_t)i);

        if (ctrl.type == 1 || ctrl.type == 2) {                     // Button or Dropdown trigger
            bool hovered = ctrl.isEnabled && (input.mouseX >= currentX && input.mouseX < currentX + btnWidth &&
                input.mouseY >= btnY && input.mouseY < btnY + btnHeight);
            uint32_t drawColor = hovered && input.leftButtonDown ? 0xFF333333 : baseColor;
            if (hovered && !input.leftButtonDown) drawColor = 0xFF555555;
            pushRect(currentX, btnY, btnWidth, btnHeight, drawColor);
            const char32_t* buttonText = localizedString(ctrl.nameStringID);
            if (*buttonText == U'\0') continue;
            pushTextClipped(currentX, btnY, buttonText, btnWidth, 0xFF888888);

            bool clicked = hovered && input.leftButtonPressedThisFrame;

            if (clicked && ctrl.isEnabled) {
                PushUIAction((uint32_t)ctrl.action);
                if (ctrl.zIndex == 1) { // Dropdown trigger
                    window.activeDropdownAction = ctrl.action;
                }
            }

            if (!ctrl.isEnabled) { // Gray-out overlay for disabled controls
                pushRect(currentX, btnY, btnWidth, btnHeight, 0xAA333333);
            }
        }
        else if (ctrl.type == 3) {
            // Future textbox
            pushRect(currentX, btnY, btnWidth, btnHeight, 0xFF1E1E1E);
        }
        else {
            // Plain label
            pushRect(currentX, btnY, btnWidth, btnHeight, 0xFF2D2D30);
        }

        float iconX = currentX + (iconReservedWidthPx - iconSizePx) * 0.5f;
        float iconY = btnY + (btnHeight - iconSizePx) * 0.5f;
        pushRect(iconX, iconY, iconSizePx, iconSizePx, iconColor);

        if (ctrl.showText) {
            const char32_t* label = localizedControlLabel(ctrl);

            float textX = currentX + textStartOffsetPx;
            float textWidth = btnWidth - textStartOffsetPx - textEndInsetPx;
            pushTextClipped(textX, textBaselineY(btnY, btnHeight), label, textWidth, 0xFFFFFFFF);
        }

        currentX += btnWidth + buttonGap;
    }

    drawActionGroupLabel(currentActionGroupIndex, currentActionGroupStartX, currentX - buttonGap);

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
