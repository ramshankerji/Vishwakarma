// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

#include "UserInterface-DirectX12.h"
#include <algorithm>
#include <d3dcompiler.h>
#include "ShaderUIVertex.h"
#include "ShaderUIPixel.h"
#include "FontManager.h"
#include "..\build\NotoSansMSDF_Compiled.h"
#include <MemoryManagerGPU-DirectX12.h>
#include "विश्वकर्मा.h"
#include "TextureSaver.h"
#include "DataTreeView.h"
#include "UserInterfaceTranslationCompiled.h"
#include <array>
#include <cmath>
#include <utility>
extern शंकर gpu;
extern std::atomic<uint16_t*> publishedTabIndexes;
extern std::atomic<uint16_t>  publishedTabCount;
extern void PrintHResult(int);
std::atomic<uint32_t> actionWriteIndex;
// ASCII Character set.
std::string charset = " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~";
std::atomic<uint64_t> atlasFence = 0;
constexpr uint32_t UI_FONT_BOLD_FLAG = 0x80000000u;

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
    std::array<char32_t, 4> dummyIconCodepoints{ U'\uE100', U'\uE101', U'\uE102', U'\uE103' };
    std::vector<char32_t> mixedIconCodepoints{};
};

static UIIconAtlasMetadata gIconAtlasMetadata{};

static const char* LogicalObjectName(VishwakarmaStorage::ObjectType objectType, const META_DATA* object) {
    if (!object) return "";

    switch (objectType) {
    case VishwakarmaStorage::ObjectType::Folder:
        return static_cast<const FOLDER*>(object)->name;
    case VishwakarmaStorage::ObjectType::Page2D:
        return static_cast<const PAGE2D*>(object)->name;
    case VishwakarmaStorage::ObjectType::Scene3D:
        return static_cast<const SCENE3D*>(object)->name;
    default:
        return "";
    }
}

static std::u32string BuildTreeNodeLabel(VishwakarmaStorage::ObjectType objectType,
    const META_DATA* object, uint64_t memoryId) {
    if (VishwakarmaStorage::IsLogicalObjectType(objectType)) {
        const char* name = LogicalObjectName(objectType, object);
        if (name && name[0] != '\0') return DataTreeView::AsciiToDisplayText(name);
        return DataTreeView::AsciiToDisplayText(VishwakarmaStorage::ObjectTypeDisplayName(objectType));
    }

    std::u32string label = DataTreeView::AsciiToDisplayText(
        VishwakarmaStorage::ObjectTypeDisplayName(objectType));
    label.push_back(U' ');
    std::u32string idText = DataTreeView::UInt64ToDecimalText(memoryId);
    label.append(idText);
    return label;
}

static UIAtlasRegion MakeAtlasRegion(int x, int y, int w, int h, int atlasW, int atlasH) {
    UIAtlasRegion region{};
    region.uvMinX = (float)x / (float)atlasW;
    region.uvMinY = (float)y / (float)atlasH;
    region.uvMaxX = (float)(x + w) / (float)atlasW;
    region.uvMaxY = (float)(y + h) / (float)atlasH;
    return region;
}

UIColors uiLightDefaultColors, uiActiveColors; // Initialized to default light theme colors.

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

static bool IsPrivateUseCodepoint(char32_t codepoint) {
    return (codepoint >= 0xE000 && codepoint <= 0xF8FF) ||
        (codepoint >= 0xF0000 && codepoint <= 0xFFFFD) ||
        (codepoint >= 0x100000 && codepoint <= 0x10FFFD);
}

static bool TryReserveIconCell(int iconIndex, int atlasW, int atlasH, int& outX, int& outY) {
    constexpr int iconCellSize = 24;
    constexpr int iconCellGap = 4;
    constexpr int iconStartY = 48;
    const int cellsPerRow = (atlasW + iconCellGap) / (iconCellSize + iconCellGap);
    if (cellsPerRow <= 0) return false;

    outX = (iconIndex % cellsPerRow) * (iconCellSize + iconCellGap);
    outY = iconStartY + (iconIndex / cellsPerRow) * (iconCellSize + iconCellGap);
    return outX + iconCellSize <= atlasW && outY + iconCellSize <= atlasH;
}

static void StoreIconCellGlyph(char32_t codepoint, int x, int y, int atlasW, int atlasH) {
    constexpr int iconCellSize = 24;
    Glyph glyph{};
    glyph.uvMinX = (float)x / atlasW;
    glyph.uvMinY = (float)y / atlasH;
    glyph.uvMaxX = (float)(x + iconCellSize) / atlasW;
    glyph.uvMaxY = (float)(y + iconCellSize) / atlasH;
    glyph.width = iconCellSize;
    glyph.height = iconCellSize;
    glyph.advanceX = iconCellSize;
    iconGlyphLookup[codepoint] = glyph;
    gIconAtlasMetadata.mixedIconCodepoints.push_back(codepoint);
}

static AtlasBitmap BuildIconAtlas() {
    constexpr int atlasW = 256;
    constexpr int atlasH = 256;
    constexpr int iconCellSize = 24;
    constexpr int proceduralIconDrawSize = 20;
    AtlasBitmap atlas{};
    atlas.width = atlasW;
    atlas.height = atlasH;
    atlas.pixels.resize(atlasW * atlasH, 0);

    // The source rounded rectangle is split into 9 texture regions at draw time.
    // Destination corners are resized to ~2 mm in screen space by PushRoundedRectangle.
    GenerateRoundedRectangleNineSlice(atlas, 0, 0, 32, 8, gIconAtlasMetadata.roundedRectangle);

    iconGlyphLookup.clear();
    gIconAtlasMetadata.mixedIconCodepoints.clear();

    std::array<int, 4> iconXs{};
    std::array<int, 4> iconYs{};
    for (int i = 0; i < 4; ++i) {
        if (!TryReserveIconCell(i, atlasW, atlasH, iconXs[i], iconYs[i])) continue;
        StoreIconCellGlyph(gIconAtlasMetadata.dummyIconCodepoints[i], iconXs[i], iconYs[i], atlasW, atlasH);
    }

    // Dummy icon 0: plus
    FillRect(atlas, iconXs[0] + 10, iconYs[0] + 4, 4, 16, 255);
    FillRect(atlas, iconXs[0] + 4, iconYs[0] + 10, 16, 4, 255);

    // Dummy icon 1: folder-like block
    FillRect(atlas, iconXs[1] + 4, iconYs[1] + 8, 16, 11, 255);
    FillRect(atlas, iconXs[1] + 6, iconYs[1] + 5, 7, 4, 255);

    // Dummy icon 2: ring
    for (int y = 0; y < proceduralIconDrawSize; ++y) {
        for (int x = 0; x < proceduralIconDrawSize; ++x) {
            const float dx = (float)x + 0.5f - 10.0f;
            const float dy = (float)y + 0.5f - 10.0f;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d >= 5.0f && d <= 8.0f) {
                atlas.pixels[(iconYs[2] + 2 + y) * atlasW + (iconXs[2] + 2 + x)] = 255;
            }
        }
    }

    // Dummy icon 3: 2x2 grid
    FillRect(atlas, iconXs[3] + 4, iconYs[3] + 4, 7, 7, 255);
    FillRect(atlas, iconXs[3] + 13, iconYs[3] + 4, 7, 7, 255);
    FillRect(atlas, iconXs[3] + 4, iconYs[3] + 13, 7, 7, 255);
    FillRect(atlas, iconXs[3] + 13, iconYs[3] + 13, 7, 7, 255);

    if (ftIconFace) {
        FT_Set_Pixel_Sizes(ftIconFace, 0, proceduralIconDrawSize);

        FT_UInt glyphIndex = 0;
        FT_ULong charCode = FT_Get_First_Char(ftIconFace, &glyphIndex);
        int iconIndex = (int)gIconAtlasMetadata.mixedIconCodepoints.size();
        while (glyphIndex != 0) {
            const char32_t codepoint = (char32_t)charCode;
            if (IsPrivateUseCodepoint(codepoint) &&
                std::find(gIconAtlasMetadata.dummyIconCodepoints.begin(),
                    gIconAtlasMetadata.dummyIconCodepoints.end(), codepoint) ==
                gIconAtlasMetadata.dummyIconCodepoints.end() &&
                FT_Load_Char(ftIconFace, charCode, FT_LOAD_RENDER) == 0) {
                int cellX = 0;
                int cellY = 0;
                if (!TryReserveIconCell(iconIndex, atlasW, atlasH, cellX, cellY)) break;

                FT_GlyphSlot g = ftIconFace->glyph;
                const int bitmapX = cellX + std::max(0, (iconCellSize - (int)g->bitmap.width) / 2);
                const int bitmapY = cellY + std::max(0, (iconCellSize - (int)g->bitmap.rows) / 2);
                const int copyW = std::min((int)g->bitmap.width, iconCellSize);
                const int copyH = std::min((int)g->bitmap.rows, iconCellSize);
                for (int y = 0; y < copyH; ++y) {
                    for (int x = 0; x < copyW; ++x) {
                        atlas.pixels[(bitmapY + y) * atlasW + (bitmapX + x)] =
                            g->bitmap.buffer[y * g->bitmap.pitch + x];
                    }
                }

                StoreIconCellGlyph(codepoint, cellX, cellY, atlasW, atlasH);
                ++iconIndex;
            }

            charCode = FT_Get_Next_Char(ftIconFace, charCode, &glyphIndex);
        }
    }

    return atlas;
}

static AtlasBitmap BuildMSDFFontAtlas() {
    AtlasBitmap atlas{};
    atlas.width = NotoSansMSDF_Width;
    atlas.height = NotoSansMSDF_Height;
    atlas.bytesPerPixel = 4;
    atlas.pixels.assign(NotoSansMSDF_Pixels,
        NotoSansMSDF_Pixels + (size_t)atlas.width * (size_t)atlas.height * (size_t)atlas.bytesPerPixel);

    glyphLookup.clear();
    for (const auto& entry : NotoSansMSDF_Glyphs) {
        const char32_t codepoint = entry.first;
        const MSDFGlyph& msdf = entry.second;

        Glyph glyph{};
        glyph.uvMinX = msdf.atlasLeft / (float)atlas.width;
        glyph.uvMaxX = msdf.atlasRight / (float)atlas.width;
        glyph.uvMinY = ((float)atlas.height - msdf.atlasTop) / (float)atlas.height;
        glyph.uvMaxY = ((float)atlas.height - msdf.atlasBottom) / (float)atlas.height;

        glyph.width = std::max(0, (int)std::ceil((msdf.planeRight - msdf.planeLeft) * NotoSansMSDF_Size));
        glyph.height = std::max(0, (int)std::ceil((msdf.planeTop - msdf.planeBottom) * NotoSansMSDF_Size));
        glyph.bearingX = (int)std::floor(msdf.planeLeft * NotoSansMSDF_Size);
        glyph.bearingY = (int)std::ceil(msdf.planeTop * NotoSansMSDF_Size);
        glyph.advanceX = std::max(0, (int)std::round(msdf.advance * NotoSansMSDF_Size));

        glyphLookup[codepoint] = glyph;
    }

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
    
    AtlasBitmap englishAtlas = BuildMSDFFontAtlas();
    AtlasBitmap iconAtlas = BuildIconAtlas();
    
    TextureUploadDesc desc = {};
    desc.width = englishAtlas.width;
    desc.height = englishAtlas.height;
    desc.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.pixels = englishAtlas.pixels.data();
    desc.rowPitch = englishAtlas.width * englishAtlas.bytesPerPixel;

    int bytesPerPixel = 1;

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
    if (uiRes.uiVertexBuffer) uiRes.uiVertexBuffer->Unmap(0, nullptr);
    if (uiRes.uiIndexBuffer) uiRes.uiIndexBuffer->Unmap(0, nullptr);
    if (uiRes.uiOrthoConstantBuffer) uiRes.uiOrthoConstantBuffer->Unmap(0, nullptr);

    uiRes = {};
}

static float TextScaleForHeight(float targetHeight) {
    auto glyphIt = glyphLookup.find(U'M');
    if (glyphIt == glyphLookup.end() || glyphIt->second.height <= 0) return 1.0f;

    return targetHeight / (float)glyphIt->second.height;
}

static float MeasureUIStringWidth(const char32_t* text, float scale) {
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
}

static const char32_t* LocalizedUIString(UITextID stringID) {
    const char32_t* text = GetUILocalizedString(stringID, UILanguage::English);
    return text ? text : U"";
}

static const char32_t* LocalizedControlLabel(const UIControlDefinition& ctrl) {
    const char32_t* label = LocalizedUIString(ctrl.nameStringID);
    if (*label != U'\0') return label;

    if (ctrl.action == Commands::INVALID || ctrl.type == 0 || ctrl.type == 3) {
        return LocalizedUIString(ctrl.nameStringID);
    }

    return U"";
}

static void ClampTopRibbonScroll(UITopRibbonLayout& layout, float viewportWidth) {
    const float maxScroll = std::max(0.0f, layout.totalContentWidthPx - viewportWidth);
    layout.scrollOffsetPx = std::clamp(layout.scrollOffsetPx, 0.0f, maxScroll);
}

void PrecomputeTopRibbonLayout(UITopRibbonLayout& layout, float monitorDPIX, float monitorDPIY) {
    const float previousScroll = layout.scrollOffsetPx;
    layout = UITopRibbonLayout{};

    layout.dpiX = monitorDPIX;
    layout.dpiY = monitorDPIY;

    const float pixelsPerMMx = monitorDPIX / 25.4f;
    const float pixelsPerMMy = monitorDPIY / 25.4f;
    layout.buttonWidthPx = std::round(UI_BUTTON_WIDTH_MM * pixelsPerMMx);
    layout.iconSizePx = std::round(UI_ICON_SIZE_MM * pixelsPerMMy);
    layout.textHeightPx = std::round(UI_TEXT_HEIGHT_MM * pixelsPerMMy);
    layout.buttonHeightPx = std::max(std::round(UI_BUTTON_HEIGHT_MM * pixelsPerMMy),
        std::max(layout.iconSizePx, layout.textHeightPx) + 4.0f);
    layout.iconReservedWidthPx = layout.iconSizePx + 4.0f;
    layout.textStartOffsetPx = layout.iconReservedWidthPx + 4.0f;
    layout.textEndInsetPx = 6.0f;
    layout.buttonGapPx = UI_BUTTON_GAP_MM * pixelsPerMMx;
    layout.tabBarHeightPx = std::round(UI_TAB_BAR_HEIGHT_MM * pixelsPerMMy);
    layout.roundedCornerRadiusPx = std::max(1.0f, std::round(UI_BUTTON_CORNER_RADIUS_MM * pixelsPerMMy));
    layout.uiTextScale = TextScaleForHeight(layout.textHeightPx);
    layout.actionGroupLabelY = (UI_TAB_BAR_HEIGHT_MM + UI_DIVIDER_GAP_PX) * pixelsPerMMy;
    layout.actionGroupLabelHeightPx = UI_ACTION_GROUP_LABEL_HEIGHT_MM * pixelsPerMMy;
    layout.topActionGroupY = (UI_TAB_BAR_HEIGHT_MM + UI_DIVIDER_GAP_PX +
        UI_ACTION_GROUP_LABEL_HEIGHT_MM) * pixelsPerMMy + 5.0f;
    layout.actionSubGroupLabelY = (UI_TAB_BAR_HEIGHT_MM + UI_DIVIDER_GAP_PX +
        UI_ACTION_GROUP_LABEL_HEIGHT_MM + UI_DIVIDER_GAP_PX +
        UI_ACTION_GROUP_HEIGHT_MM + UI_DIVIDER_GAP_PX) * pixelsPerMMy + 5.0f;
    layout.internalTabBarY = std::round((UI_TAB_BAR_HEIGHT_MM + UI_DIVIDER_GAP_PX +
        UI_ACTION_GROUP_LABEL_HEIGHT_MM + UI_DIVIDER_GAP_PX +
        UI_ACTION_GROUP_HEIGHT_MM + UI_DIVIDER_GAP_PX +
        UI_ACTION_GROUP_LABEL_HEIGHT_MM + UI_DIVIDER_GAP_PX) * pixelsPerMMy) + 7.0f;
    layout.internalTabBarHeightPx = std::round(UI_INTERNAL_TAB_BAR_HEIGHT_MM * pixelsPerMMy);
    layout.topUITotalHeightPx = layout.internalTabBarY + layout.internalTabBarHeightPx;


    const float groupNavPaddingPx = 8.0f;
    const float groupNavGapPx = 2.0f * pixelsPerMMx;
    float groupNavX = 0.0f;
    for (size_t i = 0; i < TotalTopUIActionGroups; ++i) {
        const char32_t* label = LocalizedUIString(topUIActionGroupNames[i].labelStringID);
        layout.actionGroups[i].navX = groupNavX;
        layout.actionGroups[i].navWidth = std::ceil(MeasureUIStringWidth(label, layout.uiTextScale) +
            2.0f * groupNavPaddingPx);
        groupNavX += layout.actionGroups[i].navWidth + groupNavGapPx;
    }
    layout.actionGroupNavTotalWidthPx = groupNavX > 0.0f ? groupNavX - groupNavGapPx : 0.0f;

    std::array<bool, TotalTopUIActionGroups> groupSeen{};
    float currentX = 5.0f;
    float verticalSlotMaxSize = 0.0f;
    float contentRight = currentX;
    int activeSubGroupIndex = -1;
    size_t activeSubGroupRun = 0;

    for (size_t i = 0; i < TotalUIControls; ++i) {
        const UIControlDefinition& ctrl = AllUIControls[i];
        const size_t groupIndex = ctrl.actionGroupIndex;
        const size_t subGroupIndex = ctrl.actionSubGroupIndex;

        if (groupIndex < TotalTopUIActionGroups && !groupSeen[groupIndex]) {
            layout.actionGroups[groupIndex].contentStartX = currentX;
            layout.actionGroups[groupIndex].contentEndX = currentX;
            groupSeen[groupIndex] = true;
        }

        if (subGroupIndex < TotalTopUIActionSubGroups &&
            !layout.actionSubGroups[subGroupIndex].hasControls) {
            layout.actionSubGroups[subGroupIndex].contentStartX = currentX;
            layout.actionSubGroups[subGroupIndex].contentEndX = currentX;
            layout.actionSubGroups[subGroupIndex].hasControls = true;
        }

        if (activeSubGroupIndex != (int)subGroupIndex &&
            layout.actionSubGroupRunCount < layout.actionSubGroupRuns.size()) {
            activeSubGroupIndex = (int)subGroupIndex;
            activeSubGroupRun = layout.actionSubGroupRunCount++;
            layout.actionSubGroupRuns[activeSubGroupRun].subGroupIndex = ctrl.actionSubGroupIndex;
            layout.actionSubGroupRuns[activeSubGroupRun].contentStartX = currentX;
            layout.actionSubGroupRuns[activeSubGroupRun].contentEndX = currentX;
        }

        float btnWidth = layout.buttonWidthPx;
        float btnY = layout.topActionGroupY;
        const char32_t* label = LocalizedControlLabel(ctrl);
        if (ctrl.showText && *label != U'\0') {
            float contentWidth = layout.textStartOffsetPx +
                MeasureUIStringWidth(label, layout.uiTextScale) + layout.textEndInsetPx;
            btnWidth = std::max(btnWidth, contentWidth);
        }
        if (ctrl.noOfVerticalSlots > 1) {
            btnY += ctrl.verticalSlotNo * (layout.buttonHeightPx + 1.0f);
        }

        layout.controls[i] = { currentX, btnY, btnWidth, layout.buttonHeightPx };
        verticalSlotMaxSize = std::max(verticalSlotMaxSize, btnWidth);

        const bool endOfColumn = (i + 1 == TotalUIControls) || (AllUIControls[i + 1].verticalSlotNo == 0);
        if (endOfColumn) {
            const float columnRight = currentX + verticalSlotMaxSize;
            if (groupIndex < TotalTopUIActionGroups) {
                layout.actionGroups[groupIndex].contentEndX =
                    std::max(layout.actionGroups[groupIndex].contentEndX, columnRight);
            }
            if (subGroupIndex < TotalTopUIActionSubGroups) {
                layout.actionSubGroups[subGroupIndex].contentEndX =
                    std::max(layout.actionSubGroups[subGroupIndex].contentEndX, columnRight);
            }
            if (layout.actionSubGroupRunCount > 0) {
                layout.actionSubGroupRuns[activeSubGroupRun].contentEndX =
                    std::max(layout.actionSubGroupRuns[activeSubGroupRun].contentEndX, columnRight);
            }
            contentRight = std::max(contentRight, columnRight);

            if (i + 1 < TotalUIControls) {
                currentX = columnRight + layout.buttonGapPx;
            }
            verticalSlotMaxSize = 0.0f;
        }
    }

    for (size_t gIdx = 0; gIdx < TotalTopUIActionGroups; ++gIdx) {
        layout.actionGroups[gIdx].contentWidth = std::max(0.0f, layout.actionGroups[gIdx].contentEndX - layout.actionGroups[gIdx].contentStartX);
    }

    layout.totalContentWidthPx = contentRight + 30.0f;
    layout.scrollOffsetPx = previousScroll;
    layout.isValid = true;
}

static float MapRibbonToNav(float x, const UITopRibbonLayout& layout) {
    if (TotalTopUIActionGroups == 0) return 0.0f;

    const auto& firstGroup = layout.actionGroups[0];
    if (x <= firstGroup.contentStartX) {
        return firstGroup.navX;
    }
    const auto& lastGroup = layout.actionGroups[TotalTopUIActionGroups - 1];
    if (x >= lastGroup.contentEndX) {
        return lastGroup.navX + lastGroup.navWidth;
    }

    for (size_t i = 0; i < TotalTopUIActionGroups; ++i) {
        const auto& grp = layout.actionGroups[i];
        if (x >= grp.contentStartX && x <= grp.contentEndX) {
            float width = grp.contentWidth;
            if (width <= 0.0f) return grp.navX;
            float t = (x - grp.contentStartX) / width;
            return grp.navX + t * grp.navWidth;
        }
        if (i + 1 < TotalTopUIActionGroups) {
            const auto& nextGrp = layout.actionGroups[i + 1];
            if (x > grp.contentEndX && x < nextGrp.contentStartX) {
                float gapWidth = nextGrp.contentStartX - grp.contentEndX;
                if (gapWidth <= 0.0f) return nextGrp.navX;
                float t = (x - grp.contentEndX) / gapWidth;
                float startNav = grp.navX + grp.navWidth;
                float endNav = nextGrp.navX;
                return startNav + t * (endNav - startNav);
            }
        }
    }

    return 0.0f;
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

    /*
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            PushTexturedQuad(ctx, xCuts[col], yCuts[row],
                xCuts[col + 1] - xCuts[col], yCuts[row + 1] - yCuts[row],
                gIconAtlasMetadata.roundedRectangle.regions[row][col],
                UI_ICON_ATLAS_SLOT, color, uiRes);
        }
    }*/
	// Unrolled the above loops for better performance (fewer function calls, better instruction-level parallelism)
    PushTexturedQuad(ctx, xCuts[0], yCuts[0], xCuts[1] - xCuts[0], yCuts[1] - yCuts[0],
        gIconAtlasMetadata.roundedRectangle.regions[0][0], UI_ICON_ATLAS_SLOT, color, uiRes);
    PushTexturedQuad(ctx, xCuts[1], yCuts[0], xCuts[2] - xCuts[1], yCuts[1] - yCuts[0],
        gIconAtlasMetadata.roundedRectangle.regions[0][1], UI_ICON_ATLAS_SLOT, color, uiRes);
    PushTexturedQuad(ctx, xCuts[2], yCuts[0], xCuts[3] - xCuts[2], yCuts[1] - yCuts[0],
        gIconAtlasMetadata.roundedRectangle.regions[0][2], UI_ICON_ATLAS_SLOT, color, uiRes);
    PushTexturedQuad(ctx, xCuts[0], yCuts[1], xCuts[1] - xCuts[0], yCuts[2] - yCuts[1],
        gIconAtlasMetadata.roundedRectangle.regions[1][0], UI_ICON_ATLAS_SLOT, color, uiRes);
    PushTexturedQuad(ctx, xCuts[1], yCuts[1], xCuts[2] - xCuts[1], yCuts[2] - yCuts[1],
        gIconAtlasMetadata.roundedRectangle.regions[1][1], UI_ICON_ATLAS_SLOT, color, uiRes);
    PushTexturedQuad(ctx, xCuts[2], yCuts[1], xCuts[3] - xCuts[2], yCuts[2] - yCuts[1],
        gIconAtlasMetadata.roundedRectangle.regions[1][2], UI_ICON_ATLAS_SLOT, color, uiRes);
    PushTexturedQuad(ctx, xCuts[0], yCuts[2], xCuts[1] - xCuts[0], yCuts[3] - yCuts[2],
        gIconAtlasMetadata.roundedRectangle.regions[2][0], UI_ICON_ATLAS_SLOT, color, uiRes);
    PushTexturedQuad(ctx, xCuts[1], yCuts[2], xCuts[2] - xCuts[1], yCuts[3] - yCuts[2],
        gIconAtlasMetadata.roundedRectangle.regions[2][1], UI_ICON_ATLAS_SLOT, color, uiRes);
    PushTexturedQuad(ctx, xCuts[2], yCuts[2], xCuts[3] - xCuts[2], yCuts[3] - yCuts[2],
        gIconAtlasMetadata.roundedRectangle.regions[2][2], UI_ICON_ATLAS_SLOT, color, uiRes);
}

void PushTopRoundedRectangle(UIDrawContext& ctx, float x, float y, float w, float h, float radiusPx,
    uint32_t color, DX12ResourcesUI& uiRes) {
    if (w <= 0.0f || h <= 0.0f) return;
    if (ctx.vertexCount + 24 > uiRes.maxVertices) return;
    if (ctx.indexCount + 36 > uiRes.maxIndices) return;

    const float clampedRadius = std::max(1.0f, std::min(radiusPx, 0.5f * std::min(w, h)));
    const float xCuts[4] = { x, x + clampedRadius, x + w - clampedRadius, x + w };
    const float yCuts[3] = { y, y + clampedRadius, y + h };

    // Row 0 (Top part with rounded corners)
    PushTexturedQuad(ctx, xCuts[0], yCuts[0], xCuts[1] - xCuts[0], yCuts[1] - yCuts[0],
        gIconAtlasMetadata.roundedRectangle.regions[0][0], UI_ICON_ATLAS_SLOT, color, uiRes);
    PushTexturedQuad(ctx, xCuts[1], yCuts[0], xCuts[2] - xCuts[1], yCuts[1] - yCuts[0],
        gIconAtlasMetadata.roundedRectangle.regions[0][1], UI_ICON_ATLAS_SLOT, color, uiRes);
    PushTexturedQuad(ctx, xCuts[2], yCuts[0], xCuts[3] - xCuts[2], yCuts[1] - yCuts[0],
        gIconAtlasMetadata.roundedRectangle.regions[0][2], UI_ICON_ATLAS_SLOT, color, uiRes);

    // Row 1 (Bottom part with sharp corners utilizing the flat middle row)
    PushTexturedQuad(ctx, xCuts[0], yCuts[1], xCuts[1] - xCuts[0], yCuts[2] - yCuts[1],
        gIconAtlasMetadata.roundedRectangle.regions[1][0], UI_ICON_ATLAS_SLOT, color, uiRes);
    PushTexturedQuad(ctx, xCuts[1], yCuts[1], xCuts[2] - xCuts[1], yCuts[2] - yCuts[1],
        gIconAtlasMetadata.roundedRectangle.regions[1][1], UI_ICON_ATLAS_SLOT, color, uiRes);
    PushTexturedQuad(ctx, xCuts[2], yCuts[1], xCuts[3] - xCuts[2], yCuts[2] - yCuts[1],
        gIconAtlasMetadata.roundedRectangle.regions[1][2], UI_ICON_ATLAS_SLOT, color, uiRes);
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

// This function renders the list of tabs, all top menu buttons (with dropdowns if required),
// side favorite / frequent buttons bars, right side property window, bottom status bar.
// This is also responsible for all relevant DirectX12 configurations required for rendering User Interface.
void RenderUIOverlay(SingleUIWindow& window, ID3D12GraphicsCommandList* cmd, DX12ResourcesUI& uiRes,
    UITopRibbonLayout& topRibbonLayout, float monitorDPIX, float monitorDPIY, const UIInput& input,
    const std::vector<InternalSubTab>& internalSubTabs, uint64_t activeInternalSubTabMemoryId) {
    
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
    if (!topRibbonLayout.isValid || topRibbonLayout.dpiX != monitorDPIX || topRibbonLayout.dpiY != monitorDPIY) {
        PrecomputeTopRibbonLayout(topRibbonLayout, monitorDPIX, monitorDPIY);
    }
    if (input.mouseWheelDelta != 0 && input.mouseY >= 0.0f && input.mouseY < topRibbonLayout.topUITotalHeightPx) {
        const float wheelSteps = input.mouseWheelDelta / (float)WHEEL_DELTA;
        const float scrollStepPx = std::max(topRibbonLayout.buttonWidthPx * 2.0f, 120.0f);
        topRibbonLayout.scrollOffsetPx -= wheelSteps * scrollStepPx;
    }
    ClampTopRibbonScroll(topRibbonLayout, W);

    float pixelsPerMMx = monitorDPIX / 25.4f;
    float pixelsPerMMy = monitorDPIY / 25.4f;
    float iconSizePx = topRibbonLayout.iconSizePx;
    float buttonHeightPx = topRibbonLayout.buttonHeightPx;
	float iconReservedWidthPx = topRibbonLayout.iconReservedWidthPx;
    float textStartOffsetPx = topRibbonLayout.textStartOffsetPx;
    float textEndInsetPx = topRibbonLayout.textEndInsetPx;
    float tabBarHeightPx = topRibbonLayout.tabBarHeightPx;
    float topUITotalHeightPx = topRibbonLayout.topUITotalHeightPx;
    float roundedCornerRadiusPx = topRibbonLayout.roundedCornerRadiusPx;

    auto canPushRect = [&]() {
        return ctx.vertexCount + 4 <= uiRes.maxVertices &&
            ctx.indexCount + 6 <= uiRes.maxIndices;
    };

    auto incrementVertexIndexCounters = [&]() {
        ctx.vertexPtr += 4;
        ctx.indexPtr += 6;
        ctx.vertexCount += 4;
        ctx.indexCount += 6;
    };

    auto pushRect = [&](float x, float y, float w, float h, uint32_t color) {
        bool pushed = canPushRect();
        PushRect(ctx, x, y, w, h, color, uiRes);
        if (pushed) incrementVertexIndexCounters();
    };

    const float uiTextScale = topRibbonLayout.uiTextScale;

    auto pushTextClipped = [&](float x, float y, const char32_t* text, float maxWidth, uint32_t color,
        float scale, bool bold = false) {
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
            const uint32_t atlasIndex = UI_ENGLISH_ATLAS_SLOT | (bold ? UI_FONT_BOLD_FLAG : 0u);
            ctx.vertexPtr[0] = { xpos,                  ypos,                   g.uvMinX, g.uvMinY, color, atlasIndex };
            ctx.vertexPtr[1] = { xpos + glyphWidth,     ypos,                   g.uvMaxX, g.uvMinY, color, atlasIndex };
            ctx.vertexPtr[2] = { xpos + glyphWidth,     ypos + glyphHeight,     g.uvMaxX, g.uvMaxY, color, atlasIndex };
            ctx.vertexPtr[3] = { xpos,                  ypos + glyphHeight,     g.uvMinX, g.uvMaxY, color, atlasIndex };

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

    // ENGINEERING / PROJECT TABs
    // Action ids for engineering thread control (UI -> engineering)
    constexpr uint32_t ACTION_ENGINEERING_CLOSE = 0xE0000001u;
    constexpr uint32_t ACTION_ENGINEERING_CREATE = 0xE0000002u;

    float currentX = 0.0f;

    uint16_t tabCount = publishedTabCount.load(std::memory_order_acquire);
    uint16_t* tabList = publishedTabIndexes.load(std::memory_order_acquire);
    
    if (canPushRect()) {
        PushRect(ctx, 0.0f, 0.0f, 5000.0f, topUITotalHeightPx, uiActiveColors.actionGroupBackground, uiRes);//
        incrementVertexIndexCounters();
    }

    if (canPushRect()) {
        PushRect(ctx, 0.0f, 0.0f, 5000.0f, tabBarHeightPx, uiActiveColors.tabBackground, uiRes);//
        incrementVertexIndexCounters();
    }
    // We will allow tabs to shrink progressively when too many tabs exist.
    // Compute sizing constraints
    const float defaultTabWidth = 160.0f; // legacy fixed width in pixels
    const float plusButtonWidth = buttonHeightPx; // reserve square area for '+'
    const float minTabWidth = std::max(4.0f * pixelsPerMMx, 8.0f); // 4mm minimum as requested, but at least 8px

    // Determine how many slots we need to fit: tabs + one slot for '+' button
    uint16_t slotsNeeded = tabCount + 1;
    float availableForTabs = std::max(0.0f, W - plusButtonWidth);

    float tentativeWidth = availableForTabs / (float)slotsNeeded;
    float tabWidthPx = defaultTabWidth;
    uint16_t visibleTabs = tabCount;

    if (tentativeWidth >= defaultTabWidth) {
        tabWidthPx = defaultTabWidth;
    } else if (tentativeWidth >= minTabWidth) {
        tabWidthPx = tentativeWidth;
    } else {
        // If tentative width is below minimum, we must hide some tabs.
        visibleTabs = (uint16_t)std::floor(availableForTabs / minTabWidth);
        if (visibleTabs > tabCount) visibleTabs = tabCount;
        tabWidthPx = minTabWidth;
    }

    // Render visible tabs only; hidden tabs are not drawn (will be handled by horizontal scroll in future)
    // Gap between tabs: 0.5 mm on either side
    float gapPx = 0.5f * pixelsPerMMx;
    for (uint16_t i = 0; i < visibleTabs; i++) {
        uint16_t tabID = tabList[i];
        bool isActive = (window.activeTabIndex == tabID);

        // area for this tab (slot)
        float tabX = currentX;
        float tabW = tabWidthPx;

        // content area inset by half-mm gaps on either side
        float contentX = tabX + gapPx;
        float contentW = std::max(0.0f, tabW - 2.0f * gapPx);

        // X (close) button sizing — square inside tab on the right
        float xBtnSize = std::round(std::min(tabW * 0.5f, std::max( (float)std::round(UI_ICON_SIZE_MM * pixelsPerMMx), 10.0f)));
        if (xBtnSize + 4.0f > tabW) xBtnSize = std::max(4.0f, tabW - 4.0f);
        float xBtnX = tabX + tabW - xBtnSize - 4.0f;
        float xBtnY = std::floor((tabBarHeightPx - xBtnSize) * 0.5f);

        // Entire tab background — draw only inside content area leaving gaps between tabs
        if (isActive) {
            PushTopRoundedRectangle(ctx, contentX, 0.0f, contentW, tabBarHeightPx, roundedCornerRadiusPx, uiActiveColors.actionGroupBackground, uiRes);
        } else {
            bool pushed = canPushRect();
            PushRect(ctx, contentX, 0.0f, contentW, tabBarHeightPx, uiActiveColors.tabBackground, uiRes);
            if (pushed) incrementVertexIndexCounters();
        }

        // Check clicks on the X button first to avoid activating the tab when user intends to close
        bool xHovered = input.mouseX >= xBtnX && input.mouseX < xBtnX + xBtnSize &&
            input.mouseY >= xBtnY && input.mouseY < xBtnY + xBtnSize;
        if (xHovered && input.leftButtonPressedThisFrame) {
            // Signal close intent to engineering thread. Pass tabID in parameter p1.
            PushUIAction(ACTION_ENGINEERING_CLOSE, (uint32_t)tabID, 0);
        }

        // If user clicked on non-X area of tab, activate it
        bool tabHovered = input.mouseX >= tabX && input.mouseX < tabX + tabW &&
            input.mouseY >= 0 && input.mouseY < tabBarHeightPx;
        if (!xHovered && tabHovered && input.leftButtonPressedThisFrame) {
            window.activeTabIndex = tabID; // Render thread will draw this tab's geometry on the next frame.
        }

        // Draw the X button: only draw rounded background when hovered, otherwise render as plain text
        char32_t xChar[2] = { U'x', U'\0' };
        if (xHovered) {
            PushRoundedRectangle(ctx, xBtnX, xBtnY, xBtnSize, xBtnSize, std::max(1.0f, roundedCornerRadiusPx * 0.6f),
                0xFF444444, uiRes);
            pushTextClipped(xBtnX + 2.0f, textBaselineY(xBtnY, xBtnSize, uiTextScale), xChar, xBtnSize - 4.0f, 0xFFFFFFFF, uiTextScale);
        } else {
            // Render as plain small text matching tab text color
            pushTextClipped(xBtnX + 2.0f, textBaselineY(xBtnY, xBtnSize, uiTextScale), xChar, xBtnSize - 4.0f, uiActiveColors.tabBackgroundText, uiTextScale);
        }

        // Draw label clipped to remaining area (avoid overlapping with X)
        std::u32string tabLabel;
        tabLabel.reserve(allTabs[tabID].fileName.size());
        for (wchar_t ch : allTabs[tabID].fileName) {
            if (ch <= 0x7F) tabLabel.push_back(static_cast<char32_t>(ch));
        }

        float labelMaxWidth = contentW - (8.0f + xBtnSize + 4.0f);
        pushTextClipped(contentX + 8.0f, textBaselineY(0.0f, tabBarHeightPx, uiTextScale), tabLabel.c_str(), labelMaxWidth, uiActiveColors.tabBackgroundText, uiTextScale);

        currentX += tabW;

        // Draw 1px vertical separator centered in the gap between tabs (only between tabs)
        if (i + 1 < visibleTabs) {
            float sepX = tabX + tabW; // center of gap between this tab and next
            // align to pixel for crispness
            float sepXi = std::floor(sepX + 0.5f);
            pushRect(sepXi, 2.0f, 1.0f, tabBarHeightPx - 4.0f, uiActiveColors.actionGroupSeperator);
        }
    }

    // If some tabs are hidden, we may draw a subtle indicator (ellipsis) — skip for now

    // Render '+' create new thread button at the end of tab bar
    float plusX = currentX + 6.0f; // small padding before plus
    float plusSize = std::max(plusButtonWidth, std::round(UI_ICON_SIZE_MM * pixelsPerMMy) + 8.0f);
    bool plusHovered = input.mouseX >= plusX && input.mouseX < plusX + plusSize &&
        input.mouseY >= (tabBarHeightPx - plusSize) * 0.5f && input.mouseY < (tabBarHeightPx - plusSize) * 0.5f + plusSize;
    // '+' button: show rounded background only on hover; otherwise render as plain icon/text
    if (plusHovered) {
        PushRoundedRectangle(ctx, plusX, (tabBarHeightPx - plusSize) * 0.5f, plusSize, plusSize, roundedCornerRadiusPx,
            0xFF444444, uiRes);
        if (!gIconAtlasMetadata.mixedIconCodepoints.empty()) {
            PushIcon(ctx, plusX + (plusSize - iconSizePx) * 0.5f, (tabBarHeightPx - iconSizePx) * 0.5f,
                iconSizePx, iconSizePx, gIconAtlasMetadata.mixedIconCodepoints[0], 0xFFFFFFFF, uiRes);
        }
    } else {
        if (!gIconAtlasMetadata.mixedIconCodepoints.empty()) {
            PushIcon(ctx, plusX + (plusSize - iconSizePx) * 0.5f, (tabBarHeightPx - iconSizePx) * 0.5f,
                iconSizePx, iconSizePx, gIconAtlasMetadata.mixedIconCodepoints[0], uiActiveColors.tabBackgroundText, uiRes);
        }
    }
    if (plusHovered && input.leftButtonPressedThisFrame) {
        PushUIAction(ACTION_ENGINEERING_CREATE, 0, 0);
    }

    // TOP BUTTONS (ACTION GROUP BAR)
    const float buttonGap = topRibbonLayout.buttonGapPx;
    const float actionGroupLabelY = topRibbonLayout.actionGroupLabelY;
	const float groupLabelHeight = topRibbonLayout.actionGroupLabelHeightPx;
    const float topActionGroupY = topRibbonLayout.topActionGroupY;
    const float actionSubGroupLabelY = topRibbonLayout.actionSubGroupLabelY;
    const float ribbonScrollX = topRibbonLayout.scrollOffsetPx;

    // Draw the 5-pixel high extent-of-ribbon-visible visualization bar in the 5px gap below Action Group labels.
    // The gap starts at topActionGroupY - 5.0f.
    float extentX = MapRibbonToNav(topRibbonLayout.scrollOffsetPx, topRibbonLayout);
    float extentRight = MapRibbonToNav(topRibbonLayout.scrollOffsetPx + W, topRibbonLayout);
    float extentW = std::max(1.0f, extentRight - extentX);
    // Draw active indicator (orange)
    pushRect(extentX, topActionGroupY - 5.0f, extentW, 5.0f, 0xFF3399FF);

    for (size_t groupIndex = 0; groupIndex < TotalTopUIActionGroups; ++groupIndex) {
        const UIActionGroupNames& group = topUIActionGroupNames[groupIndex];
        const UITopRibbonActionGroupLayout& groupLayout = topRibbonLayout.actionGroups[groupIndex];
        const char32_t* label = LocalizedUIString(group.labelStringID);
        const bool hovered = group.isEnabled &&
            input.mouseX >= groupLayout.navX && input.mouseX < groupLayout.navX + groupLayout.navWidth &&
            input.mouseY >= actionGroupLabelY && input.mouseY < actionGroupLabelY + groupLabelHeight;

        if (hovered) {
            pushRect(groupLayout.navX, actionGroupLabelY, groupLayout.navWidth, groupLabelHeight,
                uiActiveColors.tabBackgroundHover);
        }
        if (hovered && input.leftButtonPressedThisFrame) {
            topRibbonLayout.scrollOffsetPx = groupLayout.contentStartX;
            ClampTopRibbonScroll(topRibbonLayout, W);
        }

        pushTextClipped(groupLayout.navX + 4.0f, textBaselineY(actionGroupLabelY, groupLabelHeight, uiTextScale),
            label, groupLayout.navWidth - 8.0f, uiActiveColors.actionText, uiTextScale);
    }

    for (size_t runIndex = 0; runIndex < topRibbonLayout.actionSubGroupRunCount; ++runIndex) {
        const UITopRibbonSubGroupRunLayout& run = topRibbonLayout.actionSubGroupRuns[runIndex];
        if (run.subGroupIndex >= TotalTopUIActionSubGroups) continue;

        const UIActionGroupNames& subGroup = topUIActionSubGroupNames[run.subGroupIndex];
        const char32_t* label = LocalizedUIString(subGroup.labelStringID);
        const float runX = run.contentStartX - ribbonScrollX;
        const float runWidth = std::max(0.0f, run.contentEndX - run.contentStartX);
        const float labelWidth = MeasureUIStringWidth(label, uiTextScale);
        const float labelX = runX + std::max(4.0f, (runWidth - labelWidth) * 0.5f);

        pushTextClipped(labelX, textBaselineY(actionSubGroupLabelY, groupLabelHeight, uiTextScale),
            label, std::max(0.0f, runWidth - 8.0f), uiActiveColors.actionText, uiTextScale);

        if (runIndex + 1 < topRibbonLayout.actionSubGroupRunCount) {
            const float lineX = std::floor(run.contentEndX + buttonGap * 0.5f - ribbonScrollX);
            const float lineHeight = 3.0f * topRibbonLayout.buttonHeightPx + 2.0f;
            if (lineX >= -1.0f && lineX <= W + 1.0f) {
                pushRect(lineX, topActionGroupY, 1.0f, lineHeight, 0xFF555555);
            }
        }
    }

    for (size_t i = 0; i < TotalUIControls; ++i) {
        const auto& ctrl = AllUIControls[i];
        const UITopRibbonControlLayout& ctrlLayout = topRibbonLayout.controls[i];
        const float btnX = ctrlLayout.x - ribbonScrollX;
        const float btnY = ctrlLayout.y;
        const float btnWidth = ctrlLayout.width;
        const float btnHeight = ctrlLayout.height;
        const char32_t* label = LocalizedControlLabel(ctrl);
        uint32_t baseColor = StableRandomUIColour((uint32_t)ctrl.action ^ ((uint32_t)i * 0x9E3779B9u));// Render
        uint32_t iconColor = StableRandomUIColour(((uint32_t)ctrl.action << 1) ^ 0xA511E9B3u ^ (uint32_t)i);
        const bool controlVisible = btnX + btnWidth >= 0.0f && btnX <= W;
        bool hovered = false;

        if (ctrl.type == 1 || ctrl.type == 2) {                     // Button or Dropdown trigger
            hovered = controlVisible && ctrl.isEnabled && (input.mouseX >= btnX && input.mouseX < btnX + btnWidth &&
                input.mouseY >= btnY && input.mouseY < btnY + btnHeight);
            uint32_t drawColor = hovered && input.leftButtonDown ? 0xFF333333 : baseColor;
            if (hovered && !input.leftButtonDown) drawColor = 0xFF555555;
            if (controlVisible) {
                if (hovered) {
                    PushRoundedRectangle(ctx, btnX, btnY, btnWidth, btnHeight, roundedCornerRadiusPx,
                        drawColor, uiRes);
                } else {
                    float highlightWidth = ctrl.showText ? iconReservedWidthPx : btnWidth;
                    PushRoundedRectangle(ctx, btnX, btnY, highlightWidth, btnHeight, roundedCornerRadiusPx,
                        baseColor, uiRes);
                }
            }

            bool clicked = hovered && input.leftButtonPressedThisFrame;

            if (clicked && ctrl.isEnabled) {
                PushUIAction((uint32_t)ctrl.action);
                if (ctrl.zIndex == 1) { // Dropdown trigger
                    window.activeDropdownAction = ctrl.action;
                }
            }

            if (!ctrl.isEnabled) { // Gray-out overlay for disabled controls
                if (controlVisible) {
                    float highlightWidth = ctrl.showText ? iconReservedWidthPx : btnWidth;
                    PushRoundedRectangle(ctx, btnX, btnY, highlightWidth, btnHeight, roundedCornerRadiusPx,
                        0xAA333333, uiRes);
                }
            }
        }
        else if (ctrl.type == 3) {
            // Future textbox
            if (controlVisible) {
                PushRoundedRectangle(ctx, btnX, btnY, btnWidth, btnHeight, roundedCornerRadiusPx,
                    0xFF1E1E1E, uiRes);
            }
        }
        else {
            // Plain label
            if (controlVisible) {
                PushRoundedRectangle(ctx, btnX, btnY, btnWidth, btnHeight, roundedCornerRadiusPx,
                    0xFF2D2D30, uiRes);
            }
        }

        if (!controlVisible) continue;

        float iconX = btnX + (iconReservedWidthPx - iconSizePx) * 0.5f;
        float iconY = btnY + (btnHeight - iconSizePx) * 0.5f;
        if (!gIconAtlasMetadata.mixedIconCodepoints.empty()) {
            const uint32_t randomIconIndex =
                ((uint32_t)ctrl.action ^ (uint32_t)i) % (uint32_t)gIconAtlasMetadata.mixedIconCodepoints.size();
            PushIcon(ctx, iconX, iconY, iconSizePx, iconSizePx,
                gIconAtlasMetadata.mixedIconCodepoints[randomIconIndex], iconColor, uiRes);
        }

        if (ctrl.showText) {
            float textX = btnX + textStartOffsetPx;
            float textWidth = btnWidth - textStartOffsetPx - textEndInsetPx;
            uint32_t textColor = 0xFFFFFFFF; // default hovered/active color (white)
            if (!hovered) {
                textColor = ctrl.isEnabled ? uiActiveColors.actionText : 0xAA888888;
            }
            pushTextClipped(textX, textBaselineY(btnY, btnHeight, uiTextScale),
                label, textWidth, textColor, uiTextScale);
        }
    }

    const int activeTabIndex = window.activeTabIndex;

    // INTERNAL HIGH-LEVEL CONTAINER TABS (Scene3D, Page2D, and future container types).
    const float internalBarY = topRibbonLayout.internalTabBarY;
    const float internalBarHeight = topRibbonLayout.internalTabBarHeightPx;
    if (internalBarHeight > 0.0f) {
        pushRect(0.0f, internalBarY, W, internalBarHeight, uiActiveColors.tabBackground);

        const size_t internalTabCount = internalSubTabs.size();
        if (internalTabCount > 0) {
            const float defaultInternalTabWidth = 180.0f;
            const float minimumInternalTabWidth = std::max(4.0f * pixelsPerMMx, 8.0f);
            const float internalGapPx = 0.5f * pixelsPerMMx;
            const float availableWidth = std::max(0.0f, W);
            float internalTabWidth = std::min(defaultInternalTabWidth,
                availableWidth / static_cast<float>(internalTabCount));
            internalTabWidth = std::max(minimumInternalTabWidth, internalTabWidth);
            size_t visibleInternalTabs = std::min(internalTabCount,
                static_cast<size_t>(std::floor(availableWidth / internalTabWidth)));

            float internalX = 0.0f;
            for (size_t i = 0; i < visibleInternalTabs; ++i) {
                const InternalSubTab& subTab = internalSubTabs[i];
                const bool isActive = subTab.containerMemoryId == activeInternalSubTabMemoryId;
                const float tabX = internalX;
                const float contentX = tabX + internalGapPx;
                const float contentWidth = std::max(0.0f, internalTabWidth - 2.0f * internalGapPx);
                const float closeSize = std::max(8.0f, internalBarHeight * 0.62f);
                const float closeX = contentX + contentWidth - closeSize - 2.0f;
                const float closeY = internalBarY + (internalBarHeight - closeSize) * 0.5f;

                const bool tabHovered =
                    input.mouseX >= tabX && input.mouseX < tabX + internalTabWidth &&
                    input.mouseY >= internalBarY && input.mouseY < internalBarY + internalBarHeight;
                const bool closeHovered =
                    input.mouseX >= closeX && input.mouseX < closeX + closeSize &&
                    input.mouseY >= closeY && input.mouseY < closeY + closeSize;

                uint32_t tabColor = isActive ? uiActiveColors.tabActive : uiActiveColors.tabBackground;
                if (!isActive && tabHovered) tabColor = uiActiveColors.tabBackgroundHover;
                PushTopRoundedRectangle(ctx, contentX, internalBarY, contentWidth, internalBarHeight,
                    roundedCornerRadiusPx, tabColor, uiRes);

                if (input.leftButtonPressedThisFrame) {
                    if (closeHovered) {
                        PushUIAction(InternalSubTabs::kCloseUIAction,
                            static_cast<uint32_t>(activeTabIndex), subTab.containerMemoryId);
                    } else if (tabHovered) {
                        PushUIAction(InternalSubTabs::kActivateUIAction,
                            static_cast<uint32_t>(activeTabIndex), subTab.containerMemoryId);
                    }
                }

                std::u32string title = DataTreeView::AsciiToDisplayText(subTab.title.c_str());
                const uint32_t textColor = isActive
                    ? uiActiveColors.tabActiveText
                    : uiActiveColors.tabBackgroundText;
                pushTextClipped(contentX + 6.0f,
                    textBaselineY(internalBarY, internalBarHeight, uiTextScale),
                    title.c_str(), std::max(0.0f, closeX - contentX - 10.0f),
                    textColor, uiTextScale, isActive);

                const char32_t closeText[2] = { U'x', U'\0' };
                if (closeHovered) {
                    PushRoundedRectangle(ctx, closeX, closeY, closeSize, closeSize,
                        std::max(1.0f, roundedCornerRadiusPx * 0.6f), 0xFF444444, uiRes);
                }
                pushTextClipped(closeX + closeSize * 0.28f,
                    textBaselineY(closeY, closeSize, uiTextScale), closeText,
                    closeSize * 0.55f, textColor, uiTextScale);

                internalX += internalTabWidth;
            }
        }
    }

    if (activeTabIndex >= 0 && activeTabIndex < MV_MAX_TABS) {
        DATASETTAB& tab = allTabs[activeTabIndex];
        DataTreeView::StateSnapshot dataTreeState = DataTreeView::Snapshot(tab.dataTreeView);

        if (dataTreeState.isVisible) {
            DataTreeView::BuildRequest treeRequest;
            treeRequest.tabName = tab.fileName;
            treeRequest.activeBranchObjectId = 0;
            treeRequest.viewportTopPx = topUITotalHeightPx;
            treeRequest.viewportHeightPx = std::max(0.0f, H - topUITotalHeightPx);
            treeRequest.pixelsPerMMX = pixelsPerMMx;
            treeRequest.pixelsPerMMY = pixelsPerMMy;

            const DataTreeView::LayoutMetrics treeLayout = DataTreeView::CalculateLayout(treeRequest);
            const bool treeHovered =
                DataTreeView::ContainsPoint(treeLayout, input.mouseX, input.mouseY);
            if (treeHovered && input.mouseWheelDelta != 0) {
                const float wheelSteps = input.mouseWheelDelta / static_cast<float>(WHEEL_DELTA);
                int64_t rowDelta = static_cast<int64_t>(std::llround(-wheelSteps * 3.0f));
                if (rowDelta == 0) rowDelta = input.mouseWheelDelta > 0 ? -1 : 1;
                DataTreeView::ScrollRows(tab.dataTreeView, rowDelta);
                dataTreeState = DataTreeView::Snapshot(tab.dataTreeView);
            }

            std::vector<DataTreeView::Node> treeNodes;
            std::vector<uint64_t> expandedNodeIds;
            uint64_t activeBranchObjectId = 0;
            if (tab.storageObjectsMutex) {
                std::lock_guard<std::mutex> lock(*tab.storageObjectsMutex);
                treeNodes.reserve(tab.storageLogicalObjects.size() + tab.storageObjects3D.size());
                expandedNodeIds = tab.expandedDataTreeNodeIds;
                activeBranchObjectId = tab.activeScene3DMemoryId;

                for (const StoredLogicalObject& object : tab.storageLogicalObjects) {
                    if (!object.object) continue;
                    DataTreeView::Node node;
                    node.objectId = object.object->memoryID;
                    node.parentObjectId = object.object->memoryIDParent;
                    node.label = BuildTreeNodeLabel(object.objectType, object.object, object.memoryId);
                    node.canBecomeActiveBranch =
                        object.objectType == VishwakarmaStorage::ObjectType::Scene3D ||
                        object.objectType == VishwakarmaStorage::ObjectType::Page2D;
                    treeNodes.push_back(std::move(node));
                }

                for (const StoredGeometryObject3D& object : tab.storageObjects3D) {
                    if (!object.object) continue;
                    DataTreeView::Node node;
                    node.objectId = object.object->memoryID;
                    node.parentObjectId = object.object->memoryIDParent;
                    node.label = BuildTreeNodeLabel(object.objectType, object.object, object.memoryId);
                    treeNodes.push_back(std::move(node));
                }
            }

            treeRequest.nodes = &treeNodes;
            treeRequest.expandedNodeIds = &expandedNodeIds;
            treeRequest.activeBranchObjectId = activeBranchObjectId;

            DataTreeView::BuildResult treeResult =
                DataTreeView::BuildRows(treeRequest, dataTreeState);
            if (treeResult.firstVisibleRow != dataTreeState.firstVisibleRow) {
                DataTreeView::SetFirstVisibleRow(
                    tab.dataTreeView, static_cast<uint64_t>(treeResult.firstVisibleRow));
                dataTreeState.firstVisibleRow = treeResult.firstVisibleRow;
            }

            bool scrollbarDragging =
                tab.dataTreeView.scrollbarDragging.load(std::memory_order_acquire);
            if (input.leftButtonReleasedThisFrame || !input.leftButtonDown ||
                !treeResult.scrollbar.isScrollable) {
                scrollbarDragging = false;
                tab.dataTreeView.scrollbarDragging.store(false, std::memory_order_release);
            }

            const bool scrollbarTrackHovered = DataTreeView::HitTestScrollbarTrack(
                treeResult.scrollbar, input.mouseX, input.mouseY);
            const bool scrollbarThumbHovered = DataTreeView::HitTestScrollbarThumb(
                treeResult.scrollbar, input.mouseX, input.mouseY);

            if (input.leftButtonPressedThisFrame && scrollbarTrackHovered &&
                treeResult.scrollbar.isScrollable) {
                float grabOffsetPx = scrollbarThumbHovered
                    ? input.mouseY - treeResult.scrollbar.thumbY
                    : treeResult.scrollbar.thumbHeight * 0.5f;
                tab.dataTreeView.scrollbarDragGrabOffsetPx.store(
                    grabOffsetPx, std::memory_order_release);
                tab.dataTreeView.scrollbarDragging.store(true, std::memory_order_release);
                scrollbarDragging = true;

                const uint64_t targetRow = DataTreeView::ScrollbarRowForMouseY(
                    treeResult, input.mouseY, grabOffsetPx);
                DataTreeView::SetFirstVisibleRow(tab.dataTreeView, targetRow);
                dataTreeState = DataTreeView::Snapshot(tab.dataTreeView);
                treeResult = DataTreeView::BuildRows(treeRequest, dataTreeState);
            } else if (scrollbarDragging && input.leftButtonDown) {
                const float grabOffsetPx =
                    tab.dataTreeView.scrollbarDragGrabOffsetPx.load(std::memory_order_acquire);
                const uint64_t targetRow = DataTreeView::ScrollbarRowForMouseY(
                    treeResult, input.mouseY, grabOffsetPx);
                if (targetRow != dataTreeState.firstVisibleRow) {
                    DataTreeView::SetFirstVisibleRow(tab.dataTreeView, targetRow);
                    dataTreeState = DataTreeView::Snapshot(tab.dataTreeView);
                    treeResult = DataTreeView::BuildRows(treeRequest, dataTreeState);
                }
            }

            const uint32_t scrollbarTrackColor = 0x66333333;
            const uint32_t scrollbarThumbColor = scrollbarDragging
                ? 0xFF3399FF
                : (scrollbarThumbHovered ? 0xFF5CB4FF : 0xCC8A8A8A);
            pushRect(treeResult.scrollbar.trackX, treeResult.scrollbar.trackY,
                treeResult.scrollbar.trackWidth, treeResult.scrollbar.trackHeight,
                scrollbarTrackColor);
            pushRect(treeResult.scrollbar.thumbX, treeResult.scrollbar.thumbY,
                treeResult.scrollbar.thumbWidth, treeResult.scrollbar.thumbHeight,
                scrollbarThumbColor);

            const std::vector<DataTreeView::Row>& treeRows = treeResult.rows;
            uint64_t hoveredToggleObjectId = 0;
            const bool toggleHovered =
                DataTreeView::HitTestToggle(treeRows, input.mouseX, input.mouseY, hoveredToggleObjectId);
            uint64_t hoveredBranchObjectId = 0;
            const bool branchHovered = !toggleHovered &&
                DataTreeView::HitTestActiveBranch(
                    treeRows, input.mouseX, input.mouseY, hoveredBranchObjectId);

            if (input.leftButtonPressedThisFrame) {
                if (toggleHovered) {
                    if (hoveredToggleObjectId == 0) {
                        PushUIAction(DataTreeView::kToggleEverythingUIAction, static_cast<uint32_t>(activeTabIndex), 0);
                    } else {
                        PushUIAction(DataTreeView::kToggleNodeUIAction,
                            static_cast<uint32_t>(activeTabIndex), hoveredToggleObjectId);
                    }
                } else if (branchHovered) {
                    PushUIAction(DataTreeView::kSetActiveBranchUIAction,
                        static_cast<uint32_t>(activeTabIndex), hoveredBranchObjectId);
                    if (input.leftButtonDoubleClickedThisFrame) {
                        PushUIAction(InternalSubTabs::kOpenUIAction,
                            static_cast<uint32_t>(activeTabIndex), hoveredBranchObjectId);
                    }
                }
            }

            for (const DataTreeView::Row& row : treeRows) {
                const bool rowToggleHovered = toggleHovered && row.objectId == hoveredToggleObjectId;
                const bool rowBranchHovered = branchHovered && row.objectId == hoveredBranchObjectId;
                const uint32_t labelColor = row.isActiveBranch
                    ? 0xFF3399FF // Indian flag saffron/orange: RGB #FF9933.
                    : (rowBranchHovered ? 0xFFFF9933 : 0xFFFFFFFF);
                const uint32_t toggleColor = row.isActiveBranch
                    ? 0xFF3399FF
                    : (rowToggleHovered ? 0xFFFF9933 : 0xFFFFFFFF);
                const float baselineY = textBaselineY(row.y, row.height, uiTextScale);

                if (row.hasToggle) {
                    const char32_t toggleText[2] = { row.isExpanded ? U'-' : U'+', U'\0' };
                    pushTextClipped(row.toggleX, baselineY, toggleText, row.toggleWidth,
                        toggleColor, uiTextScale);
                }

                pushTextClipped(row.textX, baselineY, row.label.c_str(), row.textMaxWidth,
                    labelColor, uiTextScale, row.isActiveBranch);
            }
        }
    }

    // ACTIVE DROPDOWN (placeholder)
    if (window.activeDropdownAction != Commands::INVALID) {
        float dropX = 400.0f;   // TODO: track real button X for proper positioning
        float dropY = topActionGroupY + 80.0f;
        pushRect(dropX, dropY, 160, 220, 0xFF1E1E1E);
        window.activeDropdownAction = Commands::INVALID;   // immediate-mode auto-close
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
