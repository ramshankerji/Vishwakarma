// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

#include "MemoryManagerGPU2D-DirectX12.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <unordered_map>
#include <utility>

#include "MemoryManagerGPU-DirectX12.h"
#include "UserInterface-DirectX12.h"
#include "UserInterface.h"
#include "विश्वकर्मा.h"
#include "Shader2D_LineVertex.h"
#include "Shader2D_LinePixel.h"
#include "Shader2D_CurveVertex.h"
#include "Shader2D_CurvePixel.h"
#include "Shader2D_TextVertex.h"
#include "Shader2D_TextPixel.h"
#include "..\build\NotoSansMSDF_Compiled.h"

extern शंकर gpu;
extern std::atomic<uint64_t> atlasFence;
extern std::atomic<uint16_t*> publishedTabIndexes;
extern std::atomic<uint16_t> publishedTabCount;

namespace {
struct Cad2DContainerRecords {
    std::vector<Cad2DLineRecordCPU> lines;
    std::vector<Cad2DPolylineRecordCPU> polylines;
    std::vector<Cad2DPolygonRecordCPU> polygons;
    std::vector<Cad2DCircleRecordCPU> circles;
    std::vector<Cad2DEllipseRecordCPU> ellipses;
    std::vector<Cad2DArcRecordCPU> arcs;
    std::vector<Cad2DTextRecordCPU> texts;
};

constexpr uint32_t kMinPolygonLineSegmentCount = 3;
constexpr uint32_t kMaxPolygonLineSegmentCount = 16;
constexpr double kDegreesToRadians = 3.14159265358979323846 / 180.0;
constexpr uint32_t kCurveTypeCircle = 0;
constexpr uint32_t kCurveTypeEllipse = 1;
constexpr uint32_t kCurveTypeArc = 2;

uint32_t TopUIHeightPx(int monitorId, const DX12ResourcesPerWindow& winRes) {
    int topUITotalHeightPx = 0;
    if (monitorId >= 0 && monitorId < gpu.currentMonitorCount) {
        const UITopRibbonLayout& layout = gpu.screens[monitorId].topRibbonLayout;
        if (layout.isValid && layout.topUITotalHeightPx > 0.0f) {
            topUITotalHeightPx = static_cast<int>(std::round(layout.topUITotalHeightPx));
        }
        else {
            const float pixelsPerMMy = static_cast<float>(gpu.screens[monitorId].physicalDpiY) / 25.4f;
            topUITotalHeightPx = static_cast<int>(std::round((UI_TAB_BAR_HEIGHT_MM + UI_DIVIDER_GAP_PX +
                UI_ACTION_GROUP_LABEL_HEIGHT_MM + UI_DIVIDER_GAP_PX +
                UI_ACTION_GROUP_HEIGHT_MM + UI_DIVIDER_GAP_PX +
                UI_ACTION_GROUP_LABEL_HEIGHT_MM + UI_DIVIDER_GAP_PX +
                UI_INTERNAL_TAB_BAR_HEIGHT_MM) * pixelsPerMMy)) + 7;
        }
    }

    topUITotalHeightPx = std::clamp(topUITotalHeightPx, 0, winRes.WindowHeight);
    return static_cast<uint32_t>(topUITotalHeightPx);
}

void SerializeRootSignature(const CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC& rootDesc,
    ComPtr<ID3DBlob>& signature) {
    D3D12_FEATURE_DATA_ROOT_SIGNATURE featureData = {};
    featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
    if (FAILED(gpu.device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE,
        &featureData, sizeof(featureData)))) {
        featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
    }

    ComPtr<ID3DBlob> error;
    HRESULT hr = D3DX12SerializeVersionedRootSignature(
        &rootDesc, featureData.HighestVersion, &signature, &error);
    if (FAILED(hr) && error) {
        std::cerr << "2D root signature serialization failed:\n"
            << static_cast<const char*>(error->GetBufferPointer()) << std::endl;
    }
    ThrowIfFailed(hr);
}

ComPtr<ID3D12Resource> CreateDefaultBuffer(uint64_t sizeBytes) {
    ComPtr<ID3D12Resource> resource;
    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
    auto desc = CD3DX12_RESOURCE_DESC::Buffer((std::max<uint64_t>)(sizeBytes, 1));
    ThrowIfFailed(gpu.device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
        &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource)));
    return resource;
}

template <typename T>
ComPtr<ID3D12Resource> CreateUploadWithData(const std::vector<T>& data) {
    ComPtr<ID3D12Resource> upload;
    const uint64_t sizeBytes = (std::max<uint64_t>)(data.size() * sizeof(T), 1);
    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
    auto desc = CD3DX12_RESOURCE_DESC::Buffer(sizeBytes);
    ThrowIfFailed(gpu.device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
        &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)));

    if (!data.empty()) {
        uint8_t* mapped = nullptr;
        CD3DX12_RANGE readRange(0, 0);
        ThrowIfFailed(upload->Map(0, &readRange, reinterpret_cast<void**>(&mapped)));
        memcpy(mapped, data.data(), data.size() * sizeof(T));
        upload->Unmap(0, nullptr);
    }
    return upload;
}

template <typename T>
void UploadVector(ID3D12GraphicsCommandList* commandList, ComPtr<ID3D12Resource>& defaultBuffer,
    const std::vector<T>& data, std::vector<ComPtr<ID3D12Resource>>& uploads) {
    if (data.empty()) return;
    const uint64_t sizeBytes = data.size() * sizeof(T);
    defaultBuffer = CreateDefaultBuffer(sizeBytes);
    ComPtr<ID3D12Resource> upload = CreateUploadWithData(data);
    commandList->CopyBufferRegion(defaultBuffer.Get(), 0, upload.Get(), 0, sizeBytes);
    uploads.push_back(std::move(upload));
}

Cad2DLineGPURecord ToGpuLineRecord(const Cad2DLineRecordCPU& line) {
    Cad2DLineGPURecord gpuLine{};
    gpuLine.x1 = static_cast<float>(line.x1);
    gpuLine.y1 = static_cast<float>(line.y1);
    gpuLine.x2 = static_cast<float>(line.x2);
    gpuLine.y2 = static_cast<float>(line.y2);
    gpuLine.lineWeight = line.lineWeight;
    gpuLine.lineWeightMode = static_cast<uint32_t>(line.lineWeightMode);
    gpuLine.colorABGR = line.colorABGR;
    return gpuLine;
}

void AppendPolylineLineRecords(const Cad2DPolylineRecordCPU& polyline,
    std::vector<Cad2DLineGPURecord>& gpuLines) {
    if (polyline.points.size() < 2) return;

    for (size_t i = 1; i < polyline.points.size(); ++i) {
        Cad2DLineGPURecord gpuLine{};
        gpuLine.x1 = static_cast<float>(polyline.points[i - 1].x);
        gpuLine.y1 = static_cast<float>(polyline.points[i - 1].y);
        gpuLine.x2 = static_cast<float>(polyline.points[i].x);
        gpuLine.y2 = static_cast<float>(polyline.points[i].y);
        gpuLine.lineWeight = polyline.lineWeight;
        gpuLine.lineWeightMode = static_cast<uint32_t>(polyline.lineWeightMode);
        gpuLine.colorABGR = polyline.colorABGR;
        gpuLines.push_back(gpuLine);
    }
}

uint32_t ClampedPolygonLineSegmentCount(uint32_t lineSegmentCount) {
    return std::clamp(lineSegmentCount, kMinPolygonLineSegmentCount, kMaxPolygonLineSegmentCount);
}

void AppendPolygonLineRecords(const Cad2DPolygonRecordCPU& polygon,
    std::vector<Cad2DLineGPURecord>& gpuLines) {
    if (polygon.radius <= 0.0) return;

    const uint32_t lineSegmentCount = ClampedPolygonLineSegmentCount(polygon.lineSegmentCount);
    const double angleStep = 360.0 / static_cast<double>(lineSegmentCount);
    for (uint32_t i = 0; i < lineSegmentCount; ++i) {
        const double angle0 = (polygon.rotationDegrees + angleStep * static_cast<double>(i)) * kDegreesToRadians;
        const double angle1 = (polygon.rotationDegrees + angleStep * static_cast<double>((i + 1) % lineSegmentCount)) *
            kDegreesToRadians;

        Cad2DLineGPURecord gpuLine{};
        gpuLine.x1 = static_cast<float>(polygon.centerX + std::sin(angle0) * polygon.radius);
        gpuLine.y1 = static_cast<float>(polygon.centerY + std::cos(angle0) * polygon.radius);
        gpuLine.x2 = static_cast<float>(polygon.centerX + std::sin(angle1) * polygon.radius);
        gpuLine.y2 = static_cast<float>(polygon.centerY + std::cos(angle1) * polygon.radius);
        gpuLine.lineWeight = polygon.lineWeight;
        gpuLine.lineWeightMode = static_cast<uint32_t>(polygon.lineWeightMode);
        gpuLine.colorABGR = polygon.colorABGR;
        gpuLines.push_back(gpuLine);
    }
}

Cad2DCurveGPURecord ToGpuCircleRecord(const Cad2DCircleRecordCPU& circle) {
    Cad2DCurveGPURecord gpuCurve{};
    gpuCurve.centerX = static_cast<float>(circle.centerX);
    gpuCurve.centerY = static_cast<float>(circle.centerY);
    gpuCurve.radiusX = static_cast<float>(circle.radius);
    gpuCurve.radiusY = static_cast<float>(circle.radius);
    gpuCurve.startX = gpuCurve.centerX + gpuCurve.radiusX;
    gpuCurve.startY = gpuCurve.centerY;
    gpuCurve.endX = gpuCurve.startX;
    gpuCurve.endY = gpuCurve.startY;
    gpuCurve.lineWeight = circle.lineWeight;
    gpuCurve.lineWeightMode = static_cast<uint32_t>(circle.lineWeightMode);
    gpuCurve.colorABGR = circle.colorABGR;
    gpuCurve.curveType = kCurveTypeCircle;
    return gpuCurve;
}

Cad2DCurveGPURecord ToGpuEllipseRecord(const Cad2DEllipseRecordCPU& ellipse) {
    Cad2DCurveGPURecord gpuCurve{};
    gpuCurve.centerX = static_cast<float>(ellipse.centerX);
    gpuCurve.centerY = static_cast<float>(ellipse.centerY);
    gpuCurve.radiusX = static_cast<float>(ellipse.radiusX);
    gpuCurve.radiusY = static_cast<float>(ellipse.radiusY);
    gpuCurve.startX = gpuCurve.centerX + gpuCurve.radiusX;
    gpuCurve.startY = gpuCurve.centerY;
    gpuCurve.endX = gpuCurve.startX;
    gpuCurve.endY = gpuCurve.startY;
    gpuCurve.lineWeight = ellipse.lineWeight;
    gpuCurve.lineWeightMode = static_cast<uint32_t>(ellipse.lineWeightMode);
    gpuCurve.colorABGR = ellipse.colorABGR;
    gpuCurve.curveType = kCurveTypeEllipse;
    return gpuCurve;
}

Cad2DCurveGPURecord ToGpuArcRecord(const Cad2DArcRecordCPU& arc) {
    Cad2DCurveGPURecord gpuCurve{};
    gpuCurve.centerX = static_cast<float>(arc.centerX);
    gpuCurve.centerY = static_cast<float>(arc.centerY);
    gpuCurve.radiusX = static_cast<float>(arc.radiusX);
    gpuCurve.radiusY = static_cast<float>(arc.radiusY);
    gpuCurve.startX = static_cast<float>(arc.startX);
    gpuCurve.startY = static_cast<float>(arc.startY);
    gpuCurve.endX = static_cast<float>(arc.endX);
    gpuCurve.endY = static_cast<float>(arc.endY);
    gpuCurve.lineWeight = arc.lineWeight;
    gpuCurve.lineWeightMode = static_cast<uint32_t>(arc.lineWeightMode);
    gpuCurve.colorABGR = arc.colorABGR;
    gpuCurve.curveType = kCurveTypeArc;
    return gpuCurve;
}

struct PendingGlyphQuad {
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = 0.0f;
    float v1 = 0.0f;
    uint32_t colorABGR = 0xFF000000u;
};

void AppendTextRecordGeometry(const Cad2DTextRecordCPU& text,
    std::vector<Cad2DTextVertex>& vertices, std::vector<uint32_t>& indices) {
    if (text.text.empty() || text.textHeightCU <= 0.0f || text.font != 0) return;

    const float scale = text.textHeightCU / NotoSansMSDF_Size;
    std::vector<PendingGlyphQuad> quads;
    quads.reserve(text.text.size());

    float cursorX = 0.0f;
    float minX = FLT_MAX, minY = FLT_MAX, maxX = -FLT_MAX, maxY = -FLT_MAX;
    for (unsigned char c : text.text) {
        if (c > 0x7F) continue;
        const auto glyphIt = glyphLookup.find(static_cast<char32_t>(c));
        if (glyphIt == glyphLookup.end()) continue;

        const Glyph& glyph = glyphIt->second;
        if (glyph.width <= 0 || glyph.height <= 0) {
            cursorX += static_cast<float>(glyph.advanceX) * scale;
            continue;
        }

        const float x0 = cursorX + static_cast<float>(glyph.bearingX) * scale;
        const float topDown = -static_cast<float>(glyph.bearingY) * scale;
        const float bottomDown = topDown + static_cast<float>(glyph.height) * scale;
        const float x1 = x0 + static_cast<float>(glyph.width) * scale;
        const float y0 = -bottomDown;
        const float y1 = -topDown;

        quads.push_back({ x0, y0, x1, y1, glyph.uvMinX, glyph.uvMinY,
            glyph.uvMaxX, glyph.uvMaxY, text.colorABGR });

        minX = (std::min)(minX, x0);
        minY = (std::min)(minY, y0);
        maxX = (std::max)(maxX, x1);
        maxY = (std::max)(maxY, y1);
        cursorX += static_cast<float>(glyph.advanceX) * scale;
    }

    if (quads.empty()) return;

    float alignX = 0.0f;
    float alignY = 0.0f;
    switch (text.justification) {
    case Cad2DTextJustification::TopLeft:
    case Cad2DTextJustification::MiddleLeft:
    case Cad2DTextJustification::BottomLeft:
        alignX = -minX;
        break;
    case Cad2DTextJustification::TopMiddle:
    case Cad2DTextJustification::Center:
    case Cad2DTextJustification::BottomCenter:
        alignX = -(minX + maxX) * 0.5f;
        break;
    case Cad2DTextJustification::TopRight:
    case Cad2DTextJustification::MiddleRight:
    case Cad2DTextJustification::BottomRight:
        alignX = -maxX;
        break;
    }

    switch (text.justification) {
    case Cad2DTextJustification::TopLeft:
    case Cad2DTextJustification::TopMiddle:
    case Cad2DTextJustification::TopRight:
        alignY = -maxY;
        break;
    case Cad2DTextJustification::MiddleLeft:
    case Cad2DTextJustification::Center:
    case Cad2DTextJustification::MiddleRight:
        alignY = -(minY + maxY) * 0.5f;
        break;
    case Cad2DTextJustification::BottomLeft:
    case Cad2DTextJustification::BottomCenter:
    case Cad2DTextJustification::BottomRight:
        alignY = -minY;
        break;
    }

    const float cosA = std::cos(text.rotationRadians);
    const float sinA = std::sin(text.rotationRadians);
    const float originX = static_cast<float>(text.x) + text.xOffsetCU;
    const float originY = static_cast<float>(text.y) + text.yOffsetCU;

    auto transformPoint = [&](float localX, float localY) -> DirectX::XMFLOAT2 {
        localX += alignX;
        localY += alignY;
        return {
            originX + localX * cosA - localY * sinA,
            originY + localX * sinA + localY * cosA
        };
    };

    for (const PendingGlyphQuad& quad : quads) {
        const uint32_t baseVertex = static_cast<uint32_t>(vertices.size());
        const DirectX::XMFLOAT2 p0 = transformPoint(quad.x0, quad.y0);
        const DirectX::XMFLOAT2 p1 = transformPoint(quad.x1, quad.y0);
        const DirectX::XMFLOAT2 p2 = transformPoint(quad.x1, quad.y1);
        const DirectX::XMFLOAT2 p3 = transformPoint(quad.x0, quad.y1);

        vertices.push_back({ p0.x, p0.y, quad.u0, quad.v1, quad.colorABGR, UI_ENGLISH_ATLAS_SLOT });
        vertices.push_back({ p1.x, p1.y, quad.u1, quad.v1, quad.colorABGR, UI_ENGLISH_ATLAS_SLOT });
        vertices.push_back({ p2.x, p2.y, quad.u1, quad.v0, quad.colorABGR, UI_ENGLISH_ATLAS_SLOT });
        vertices.push_back({ p3.x, p3.y, quad.u0, quad.v0, quad.colorABGR, UI_ENGLISH_ATLAS_SLOT });

        indices.push_back(baseVertex + 0);
        indices.push_back(baseVertex + 1);
        indices.push_back(baseVertex + 2);
        indices.push_back(baseVertex + 0);
        indices.push_back(baseVertex + 2);
        indices.push_back(baseVertex + 3);
    }
}

void PublishCad2DPages(TabCad2DStorage& storage, std::vector<std::unique_ptr<Cad2DPageGPU>> pages) {
    const uint64_t retireFence = gpu.renderFenceValue.load(std::memory_order_acquire);

    for (auto& page : storage.activePages) {
        storage.retiredPages.push_back({ std::move(page), retireFence });
    }
    storage.activePages = std::move(pages);

    Cad2DPageSnapshot* newSnapshot = new Cad2DPageSnapshot();
    newSnapshot->pages.reserve(storage.activePages.size());
    for (const auto& page : storage.activePages) {
        newSnapshot->pages.push_back(page.get());
    }

    Cad2DPageSnapshot* oldSnapshot =
        storage.activeSnapshot.exchange(newSnapshot, std::memory_order_acq_rel);
    if (oldSnapshot) storage.retiredSnapshots.push_back({ oldSnapshot, retireFence });
}
}

void InitCad2DTabResources(TabCad2DStorage& storage) {
    if (storage.dx.lineRootSignature && storage.dx.curveRootSignature && storage.dx.textRootSignature) return;

    {
        CD3DX12_ROOT_PARAMETER1 rootParams[2] = {};
        rootParams[0].InitAsConstantBufferView(0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE,
            D3D12_SHADER_VISIBILITY_ALL);
        rootParams[1].InitAsShaderResourceView(0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE,
            D3D12_SHADER_VISIBILITY_VERTEX);

        CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootDesc;
        rootDesc.Init_1_1(_countof(rootParams), rootParams, 0, nullptr,
            D3D12_ROOT_SIGNATURE_FLAG_NONE);

        ComPtr<ID3DBlob> signature;
        SerializeRootSignature(rootDesc, signature);
        ThrowIfFailed(gpu.device->CreateRootSignature(0, signature->GetBufferPointer(),
            signature->GetBufferSize(), IID_PPV_ARGS(&storage.dx.lineRootSignature)));

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.InputLayout = { nullptr, 0 };
        psoDesc.pRootSignature = storage.dx.lineRootSignature.Get();
        psoDesc.VS = CD3DX12_SHADER_BYTECODE(g_2dLineVertexShader, sizeof(g_2dLineVertexShader));
        psoDesc.PS = CD3DX12_SHADER_BYTECODE(g_2dLinePixelShader, sizeof(g_2dLinePixelShader));
        psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
        psoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
        psoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        psoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        psoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        psoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        psoDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        psoDesc.DepthStencilState.DepthEnable = FALSE;
        psoDesc.DepthStencilState.StencilEnable = FALSE;
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = gpu.rttFormat;
        psoDesc.SampleDesc.Count = 1;
        ThrowIfFailed(gpu.device->CreateGraphicsPipelineState(&psoDesc,
            IID_PPV_ARGS(&storage.dx.linePSO)));

        D3D12_INDIRECT_ARGUMENT_DESC arg = {};
        arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
        D3D12_COMMAND_SIGNATURE_DESC sigDesc = {};
        sigDesc.pArgumentDescs = &arg;
        sigDesc.NumArgumentDescs = 1;
        sigDesc.ByteStride = sizeof(D3D12_DRAW_ARGUMENTS);
        ThrowIfFailed(gpu.device->CreateCommandSignature(&sigDesc, nullptr,
            IID_PPV_ARGS(&storage.dx.lineCommandSignature)));
    }

    {
        CD3DX12_ROOT_PARAMETER1 rootParams[2] = {};
        rootParams[0].InitAsConstantBufferView(0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE,
            D3D12_SHADER_VISIBILITY_ALL);
        rootParams[1].InitAsShaderResourceView(0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE,
            D3D12_SHADER_VISIBILITY_VERTEX);

        CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootDesc;
        rootDesc.Init_1_1(_countof(rootParams), rootParams, 0, nullptr,
            D3D12_ROOT_SIGNATURE_FLAG_NONE);

        ComPtr<ID3DBlob> signature;
        SerializeRootSignature(rootDesc, signature);
        ThrowIfFailed(gpu.device->CreateRootSignature(0, signature->GetBufferPointer(),
            signature->GetBufferSize(), IID_PPV_ARGS(&storage.dx.curveRootSignature)));

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.InputLayout = { nullptr, 0 };
        psoDesc.pRootSignature = storage.dx.curveRootSignature.Get();
        psoDesc.VS = CD3DX12_SHADER_BYTECODE(g_2dCurveVertexShader, sizeof(g_2dCurveVertexShader));
        psoDesc.PS = CD3DX12_SHADER_BYTECODE(g_2dCurvePixelShader, sizeof(g_2dCurvePixelShader));
        psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
        psoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
        psoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        psoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        psoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        psoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        psoDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        psoDesc.DepthStencilState.DepthEnable = FALSE;
        psoDesc.DepthStencilState.StencilEnable = FALSE;
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = gpu.rttFormat;
        psoDesc.SampleDesc.Count = 1;
        ThrowIfFailed(gpu.device->CreateGraphicsPipelineState(&psoDesc,
            IID_PPV_ARGS(&storage.dx.curvePSO)));

        D3D12_INDIRECT_ARGUMENT_DESC arg = {};
        arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
        D3D12_COMMAND_SIGNATURE_DESC sigDesc = {};
        sigDesc.pArgumentDescs = &arg;
        sigDesc.NumArgumentDescs = 1;
        sigDesc.ByteStride = sizeof(D3D12_DRAW_ARGUMENTS);
        ThrowIfFailed(gpu.device->CreateCommandSignature(&sigDesc, nullptr,
            IID_PPV_ARGS(&storage.dx.curveCommandSignature)));
    }

    {
        CD3DX12_DESCRIPTOR_RANGE1 ranges[2] = {};
        ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UI_MAX_ATLAS_TEXTURES, 0, 0,
            D3D12_DESCRIPTOR_RANGE_FLAG_NONE);
        ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0, 0,
            D3D12_DESCRIPTOR_RANGE_FLAG_NONE);

        CD3DX12_ROOT_PARAMETER1 rootParams[3] = {};
        rootParams[0].InitAsConstantBufferView(0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE,
            D3D12_SHADER_VISIBILITY_VERTEX);
        rootParams[1].InitAsDescriptorTable(1, &ranges[0], D3D12_SHADER_VISIBILITY_PIXEL);
        rootParams[2].InitAsDescriptorTable(1, &ranges[1], D3D12_SHADER_VISIBILITY_PIXEL);

        CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootDesc;
        rootDesc.Init_1_1(_countof(rootParams), rootParams, 0, nullptr,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        ComPtr<ID3DBlob> signature;
        SerializeRootSignature(rootDesc, signature);
        ThrowIfFailed(gpu.device->CreateRootSignature(0, signature->GetBufferPointer(),
            signature->GetBufferSize(), IID_PPV_ARGS(&storage.dx.textRootSignature)));

        D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_R32_UINT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 1, DXGI_FORMAT_R32_UINT, 0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
        psoDesc.pRootSignature = storage.dx.textRootSignature.Get();
        psoDesc.VS = CD3DX12_SHADER_BYTECODE(g_2dTextVertexShader, sizeof(g_2dTextVertexShader));
        psoDesc.PS = CD3DX12_SHADER_BYTECODE(g_2dTextPixelShader, sizeof(g_2dTextPixelShader));
        psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
        psoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
        psoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        psoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        psoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        psoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        psoDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        psoDesc.DepthStencilState.DepthEnable = FALSE;
        psoDesc.DepthStencilState.StencilEnable = FALSE;
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = gpu.rttFormat;
        psoDesc.SampleDesc.Count = 1;
        ThrowIfFailed(gpu.device->CreateGraphicsPipelineState(&psoDesc,
            IID_PPV_ARGS(&storage.dx.textPSO)));
    }

    {
        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
        auto desc = CD3DX12_RESOURCE_DESC::Buffer(256);
        ThrowIfFailed(gpu.device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
            &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&storage.dx.viewConstantBuffer)));
        CD3DX12_RANGE readRange(0, 0);
        ThrowIfFailed(storage.dx.viewConstantBuffer->Map(0, &readRange,
            reinterpret_cast<void**>(&storage.dx.pViewConstantDataBegin)));
    }
}

void CleanupCad2DTabResources(TabCad2DStorage& storage) {
    if (storage.dx.viewConstantBuffer && storage.dx.pViewConstantDataBegin) {
        storage.dx.viewConstantBuffer->Unmap(0, nullptr);
        storage.dx.pViewConstantDataBegin = nullptr;
    }

    Cad2DPageSnapshot* snapshot = storage.activeSnapshot.exchange(nullptr, std::memory_order_acq_rel);
    delete snapshot;
    for (auto& retired : storage.retiredSnapshots) delete retired.snapshot;
    storage.retiredSnapshots.clear();
    storage.retiredPages.clear();
    storage.activePages.clear();

    storage.dx.lineCommandSignature.Reset();
    storage.dx.linePSO.Reset();
    storage.dx.lineRootSignature.Reset();
    storage.dx.curveCommandSignature.Reset();
    storage.dx.curvePSO.Reset();
    storage.dx.curveRootSignature.Reset();
    storage.dx.textPSO.Reset();
    storage.dx.textRootSignature.Reset();
    storage.dx.viewConstantBuffer.Reset();

    std::lock_guard<std::mutex> lock(storage.cpuRecordsMutex);
    storage.lineRecords.clear();
    storage.polylineRecords.clear();
    storage.polygonRecords.clear();
    storage.circleRecords.clear();
    storage.ellipseRecords.clear();
    storage.arcRecords.clear();
    storage.textRecords.clear();
    storage.demoLineCounter.store(0, std::memory_order_release);
    storage.demoTextQueued.store(false, std::memory_order_release);
    storage.lineCreationMode.store(false, std::memory_order_release);
    storage.lineCreationHasPreviousPoint.store(false, std::memory_order_release);
    storage.polylineCreationMode.store(false, std::memory_order_release);
    storage.polylineCreationObjectId = 0;
    storage.polylineCreationPoints.clear();
    storage.polygonCreationMode.store(false, std::memory_order_release);
    storage.polygonCreationHasCenter.store(false, std::memory_order_release);
    storage.polygonCreationCenterXCU.store(0.0, std::memory_order_release);
    storage.polygonCreationCenterYCU.store(0.0, std::memory_order_release);
    storage.circleCreationMode.store(false, std::memory_order_release);
    storage.circleCreationHasCenter.store(false, std::memory_order_release);
    storage.circleCreationCenterXCU.store(0.0, std::memory_order_release);
    storage.circleCreationCenterYCU.store(0.0, std::memory_order_release);
    storage.ellipseCreationMode.store(false, std::memory_order_release);
    storage.ellipseCreationStep.store(0, std::memory_order_release);
    storage.ellipseCreationCenterXCU.store(0.0, std::memory_order_release);
    storage.ellipseCreationCenterYCU.store(0.0, std::memory_order_release);
    storage.ellipseCreationRadiusXCU.store(0.0, std::memory_order_release);
    storage.arcCreationMode.store(false, std::memory_order_release);
    storage.arcCreationStep.store(0, std::memory_order_release);
    storage.arcCreationCenterXCU.store(0.0, std::memory_order_release);
    storage.arcCreationCenterYCU.store(0.0, std::memory_order_release);
    storage.arcCreationStartXCU.store(0.0, std::memory_order_release);
    storage.arcCreationStartYCU.store(0.0, std::memory_order_release);
    storage.textCreationMode.store(false, std::memory_order_release);
    storage.textCreationHasAnchor.store(false, std::memory_order_release);
    storage.textCreationXCU.store(0.0, std::memory_order_release);
    storage.textCreationYCU.store(0.0, std::memory_order_release);
    storage.textCreationObjectId = 0;
    storage.textCreationDraft.clear();
}

void RenderCad2DPage(ID3D12GraphicsCommandList* commandList, DX12ResourcesPerWindow& winRes,
    TabCad2DStorage& storage, DX12ResourcesUI& uiResources, int monitorId,
    uint64_t activeContainerMemoryId) {
    if (!commandList || activeContainerMemoryId == 0 || winRes.WindowHeight <= 0) return;
    if (!storage.dx.lineRootSignature || !storage.dx.viewConstantBuffer) return;

    const uint32_t topUI = TopUIHeightPx(monitorId, winRes);
    const int sceneHeight = winRes.WindowHeight - static_cast<int>(topUI);
    if (sceneHeight <= 0) return;

    CD3DX12_VIEWPORT viewport(0.0f, static_cast<float>(topUI),
        static_cast<float>(winRes.WindowWidth), static_cast<float>(sceneHeight));
    CD3DX12_RECT scissor(0, static_cast<LONG>(topUI), winRes.WindowWidth, winRes.WindowHeight);
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);

    CD3DX12_CPU_DESCRIPTOR_HANDLE rttHandle(winRes.rttRtvHeap->GetCPUDescriptorHandleForHeapStart(),
        winRes.frameIndex, gpu.rtvDescriptorSize);
    const float cadBackground[] = { 230.0f / 255.0f, 230.0f / 255.0f, 230.0f / 255.0f, 1.0f };
    commandList->ClearRenderTargetView(rttHandle, cadBackground, 0, nullptr);

    Cad2DViewConstants constants{};
    constants.viewCenterCU = {
        static_cast<float>(storage.view.centerXCU.load(std::memory_order_acquire)),
        static_cast<float>(storage.view.centerYCU.load(std::memory_order_acquire))
    };
    constants.zoomPixelsPerCU =
        (std::max)(storage.view.zoomPixelsPerCU.load(std::memory_order_acquire), 0.02f);
    constants.dpiY = monitorId >= 0 && monitorId < gpu.currentMonitorCount
        ? static_cast<float>(gpu.screens[monitorId].physicalDpiY)
        : 96.0f;
    constants.viewportSizePx = {
        static_cast<float>(winRes.WindowWidth),
        static_cast<float>(sceneHeight)
    };
    constants.minLineWeightPx = 1.0f;
    memcpy(storage.dx.pViewConstantDataBegin, &constants, sizeof(constants));

    Cad2DPageSnapshot* snapshot = storage.activeSnapshot.load(std::memory_order_acquire);
    if (!snapshot) return;

    commandList->SetGraphicsRootSignature(storage.dx.lineRootSignature.Get());
    commandList->SetPipelineState(storage.dx.linePSO.Get());
    commandList->SetGraphicsRootConstantBufferView(0,
        storage.dx.viewConstantBuffer->GetGPUVirtualAddress());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    for (Cad2DPageGPU* page : snapshot->pages) {
        if (!page || page->containerMemoryId != activeContainerMemoryId) continue;
        if (page->lineCount > 0 && page->lineBuffer && page->lineIndirectBuffer) {
            commandList->SetGraphicsRootShaderResourceView(1, page->lineBuffer->GetGPUVirtualAddress());
            commandList->ExecuteIndirect(storage.dx.lineCommandSignature.Get(), 1,
                page->lineIndirectBuffer.Get(), 0, nullptr, 0);
        }
    }

    if (storage.dx.curveRootSignature && storage.dx.curvePSO && storage.dx.curveCommandSignature) {
        commandList->SetGraphicsRootSignature(storage.dx.curveRootSignature.Get());
        commandList->SetPipelineState(storage.dx.curvePSO.Get());
        commandList->SetGraphicsRootConstantBufferView(0,
            storage.dx.viewConstantBuffer->GetGPUVirtualAddress());
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        for (Cad2DPageGPU* page : snapshot->pages) {
            if (!page || page->containerMemoryId != activeContainerMemoryId) continue;
            if (page->curveCount > 0 && page->curveBuffer && page->curveIndirectBuffer) {
                commandList->SetGraphicsRootShaderResourceView(1, page->curveBuffer->GetGPUVirtualAddress());
                commandList->ExecuteIndirect(storage.dx.curveCommandSignature.Get(), 1,
                    page->curveIndirectBuffer.Get(), 0, nullptr, 0);
            }
        }
    }

    const bool textAtlasReady = atlasFence.load(std::memory_order_acquire) != 0 &&
        gpu.copyFence && gpu.copyFence->GetCompletedValue() >= atlasFence.load(std::memory_order_acquire);
    if (!textAtlasReady || !storage.dx.textRootSignature || !storage.dx.textPSO) return;

    commandList->SetGraphicsRootSignature(storage.dx.textRootSignature.Get());
    commandList->SetPipelineState(storage.dx.textPSO.Get());
    ID3D12DescriptorHeap* heaps[] = { uiResources.srvHeap.Get(), uiResources.samplerHeap.Get() };
    commandList->SetDescriptorHeaps(_countof(heaps), heaps);
    commandList->SetGraphicsRootConstantBufferView(0,
        storage.dx.viewConstantBuffer->GetGPUVirtualAddress());
    commandList->SetGraphicsRootDescriptorTable(1, uiResources.srvHeap->GetGPUDescriptorHandleForHeapStart());
    commandList->SetGraphicsRootDescriptorTable(2, uiResources.samplerHeap->GetGPUDescriptorHandleForHeapStart());

    for (Cad2DPageGPU* page : snapshot->pages) {
        if (!page || page->containerMemoryId != activeContainerMemoryId) continue;
        if (page->textIndexCount == 0 || !page->textVertexBuffer || !page->textIndexBuffer) continue;

        D3D12_VERTEX_BUFFER_VIEW vbv{};
        vbv.BufferLocation = page->textVertexBuffer->GetGPUVirtualAddress();
        vbv.SizeInBytes = page->textVertexCount * sizeof(Cad2DTextVertex);
        vbv.StrideInBytes = sizeof(Cad2DTextVertex);

        D3D12_INDEX_BUFFER_VIEW ibv{};
        ibv.BufferLocation = page->textIndexBuffer->GetGPUVirtualAddress();
        ibv.SizeInBytes = page->textIndexCount * sizeof(uint32_t);
        ibv.Format = DXGI_FORMAT_R32_UINT;

        commandList->IASetVertexBuffers(0, 1, &vbv);
        commandList->IASetIndexBuffer(&ibv);
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commandList->DrawIndexedInstanced(page->textIndexCount, 1, 0, 0, 0);
    }
}

void ProcessCad2DCopyBatch(const std::vector<CommandToCopyThread2D>& batch) {
    if (batch.empty()) return;

    std::unordered_map<uint64_t, std::vector<CommandToCopyThread2D>> byTab;
    for (const CommandToCopyThread2D& command : batch) {
        byTab[command.tabID].push_back(command);
    }

    for (auto& [tabID, commands] : byTab) {
        if (tabID >= MV_MAX_TABS) continue;
        DATASETTAB& tab = allTabs[tabID];
        if (!tab.cad2d) continue;
        TabCad2DStorage& storage = *tab.cad2d;

        std::vector<Cad2DLineRecordCPU> lines;
        std::vector<Cad2DPolylineRecordCPU> polylines;
        std::vector<Cad2DPolygonRecordCPU> polygons;
        std::vector<Cad2DCircleRecordCPU> circles;
        std::vector<Cad2DEllipseRecordCPU> ellipses;
        std::vector<Cad2DArcRecordCPU> arcs;
        std::vector<Cad2DTextRecordCPU> texts;
        {
            std::lock_guard<std::mutex> lock(storage.cpuRecordsMutex);
            for (const CommandToCopyThread2D& command : commands) {
                if (command.containerMemoryId == 0) continue;
                if (command.type == CommandToCopyThread2DType::AddLine) {
                    auto existing = std::find_if(storage.lineRecords.begin(), storage.lineRecords.end(),
                        [&](const Cad2DLineRecordCPU& line) {
                            return line.objectId == command.line.objectId;
                        });
                    if (existing == storage.lineRecords.end()) {
                        storage.lineRecords.push_back(command.line);
                        if (std::find(tab.allIDsInThisTab.begin(), tab.allIDsInThisTab.end(),
                            command.line.objectId) == tab.allIDsInThisTab.end()) {
                            tab.allIDsInThisTab.push_back(command.line.objectId);
                        }
                    }
                    else {
                        *existing = command.line;
                    }
                }
                else if (command.type == CommandToCopyThread2DType::AddPolyline) {
                    auto existing = std::find_if(storage.polylineRecords.begin(), storage.polylineRecords.end(),
                        [&](const Cad2DPolylineRecordCPU& polyline) {
                            return polyline.objectId == command.polyline.objectId;
                        });
                    if (existing == storage.polylineRecords.end()) {
                        storage.polylineRecords.push_back(command.polyline);
                        if (std::find(tab.allIDsInThisTab.begin(), tab.allIDsInThisTab.end(),
                            command.polyline.objectId) == tab.allIDsInThisTab.end()) {
                            tab.allIDsInThisTab.push_back(command.polyline.objectId);
                        }
                    }
                    else {
                        Cad2DPolylineRecordCPU updated = command.polyline;
                        if (updated.persistedId == 0) updated.persistedId = existing->persistedId;
                        if (updated.persistedParentId == 0) updated.persistedParentId = existing->persistedParentId;
                        *existing = std::move(updated);
                    }
                }
                else if (command.type == CommandToCopyThread2DType::AddPolygon) {
                    auto existing = std::find_if(storage.polygonRecords.begin(), storage.polygonRecords.end(),
                        [&](const Cad2DPolygonRecordCPU& polygon) {
                            return polygon.objectId == command.polygon.objectId;
                        });
                    if (existing == storage.polygonRecords.end()) {
                        storage.polygonRecords.push_back(command.polygon);
                        if (std::find(tab.allIDsInThisTab.begin(), tab.allIDsInThisTab.end(),
                            command.polygon.objectId) == tab.allIDsInThisTab.end()) {
                            tab.allIDsInThisTab.push_back(command.polygon.objectId);
                        }
                    }
                    else {
                        Cad2DPolygonRecordCPU updated = command.polygon;
                        if (updated.persistedId == 0) updated.persistedId = existing->persistedId;
                        if (updated.persistedParentId == 0) updated.persistedParentId = existing->persistedParentId;
                        *existing = updated;
                    }
                }
                else if (command.type == CommandToCopyThread2DType::AddCircle) {
                    auto existing = std::find_if(storage.circleRecords.begin(), storage.circleRecords.end(),
                        [&](const Cad2DCircleRecordCPU& circle) {
                            return circle.objectId == command.circle.objectId;
                        });
                    if (existing == storage.circleRecords.end()) {
                        storage.circleRecords.push_back(command.circle);
                        if (std::find(tab.allIDsInThisTab.begin(), tab.allIDsInThisTab.end(),
                            command.circle.objectId) == tab.allIDsInThisTab.end()) {
                            tab.allIDsInThisTab.push_back(command.circle.objectId);
                        }
                    }
                    else {
                        Cad2DCircleRecordCPU updated = command.circle;
                        if (updated.persistedId == 0) updated.persistedId = existing->persistedId;
                        if (updated.persistedParentId == 0) updated.persistedParentId = existing->persistedParentId;
                        *existing = updated;
                    }
                }
                else if (command.type == CommandToCopyThread2DType::AddEllipse) {
                    auto existing = std::find_if(storage.ellipseRecords.begin(), storage.ellipseRecords.end(),
                        [&](const Cad2DEllipseRecordCPU& ellipse) {
                            return ellipse.objectId == command.ellipse.objectId;
                        });
                    if (existing == storage.ellipseRecords.end()) {
                        storage.ellipseRecords.push_back(command.ellipse);
                        if (std::find(tab.allIDsInThisTab.begin(), tab.allIDsInThisTab.end(),
                            command.ellipse.objectId) == tab.allIDsInThisTab.end()) {
                            tab.allIDsInThisTab.push_back(command.ellipse.objectId);
                        }
                    }
                    else {
                        Cad2DEllipseRecordCPU updated = command.ellipse;
                        if (updated.persistedId == 0) updated.persistedId = existing->persistedId;
                        if (updated.persistedParentId == 0) updated.persistedParentId = existing->persistedParentId;
                        *existing = updated;
                    }
                }
                else if (command.type == CommandToCopyThread2DType::AddArc) {
                    auto existing = std::find_if(storage.arcRecords.begin(), storage.arcRecords.end(),
                        [&](const Cad2DArcRecordCPU& arc) {
                            return arc.objectId == command.arc.objectId;
                        });
                    if (existing == storage.arcRecords.end()) {
                        storage.arcRecords.push_back(command.arc);
                        if (std::find(tab.allIDsInThisTab.begin(), tab.allIDsInThisTab.end(),
                            command.arc.objectId) == tab.allIDsInThisTab.end()) {
                            tab.allIDsInThisTab.push_back(command.arc.objectId);
                        }
                    }
                    else {
                        Cad2DArcRecordCPU updated = command.arc;
                        if (updated.persistedId == 0) updated.persistedId = existing->persistedId;
                        if (updated.persistedParentId == 0) updated.persistedParentId = existing->persistedParentId;
                        *existing = updated;
                    }
                }
                else if (command.type == CommandToCopyThread2DType::AddText) {
                    auto existing = std::find_if(storage.textRecords.begin(), storage.textRecords.end(),
                        [&](const Cad2DTextRecordCPU& text) {
                            return text.objectId == command.text.objectId;
                        });
                    if (existing == storage.textRecords.end()) {
                        storage.textRecords.push_back(command.text);
                        if (std::find(tab.allIDsInThisTab.begin(), tab.allIDsInThisTab.end(),
                            command.text.objectId) == tab.allIDsInThisTab.end()) {
                            tab.allIDsInThisTab.push_back(command.text.objectId);
                        }
                    }
                    else {
                        Cad2DTextRecordCPU updated = command.text;
                        if (updated.persistedId == 0) updated.persistedId = existing->persistedId;
                        if (updated.persistedParentId == 0) updated.persistedParentId = existing->persistedParentId;
                        *existing = std::move(updated);
                    }
                }
            }
            lines = storage.lineRecords;
            polylines = storage.polylineRecords;
            polygons = storage.polygonRecords;
            circles = storage.circleRecords;
            ellipses = storage.ellipseRecords;
            arcs = storage.arcRecords;
            texts = storage.textRecords;
        }

        std::unordered_set<uint64_t> selected2D; // Objects to stamp with kCad2DSelectedFlag.
        {
            std::lock_guard<std::mutex> lock(storage.selection2DMutex);
            selected2D = storage.selectedObjectIds;
        }

        std::map<uint64_t, Cad2DContainerRecords> containers;
        for (const Cad2DLineRecordCPU& line : lines) {
            if (line.containerMemoryId != 0) containers[line.containerMemoryId].lines.push_back(line);
        }
        for (const Cad2DPolylineRecordCPU& polyline : polylines) {
            if (polyline.containerMemoryId != 0) containers[polyline.containerMemoryId].polylines.push_back(polyline);
        }
        for (const Cad2DPolygonRecordCPU& polygon : polygons) {
            if (polygon.containerMemoryId != 0) containers[polygon.containerMemoryId].polygons.push_back(polygon);
        }
        for (const Cad2DCircleRecordCPU& circle : circles) {
            if (circle.containerMemoryId != 0) containers[circle.containerMemoryId].circles.push_back(circle);
        }
        for (const Cad2DEllipseRecordCPU& ellipse : ellipses) {
            if (ellipse.containerMemoryId != 0) containers[ellipse.containerMemoryId].ellipses.push_back(ellipse);
        }
        for (const Cad2DArcRecordCPU& arc : arcs) {
            if (arc.containerMemoryId != 0) containers[arc.containerMemoryId].arcs.push_back(arc);
        }
        for (const Cad2DTextRecordCPU& text : texts) {
            if (text.containerMemoryId != 0) containers[text.containerMemoryId].texts.push_back(text);
        }

        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commandList;
        ThrowIfFailed(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY,
            IID_PPV_ARGS(&allocator)));
        ThrowIfFailed(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY,
            allocator.Get(), nullptr, IID_PPV_ARGS(&commandList)));

        std::vector<ComPtr<ID3D12Resource>> uploads;
        std::vector<std::unique_ptr<Cad2DPageGPU>> pages;

        for (const auto& [containerMemoryId, records] : containers) {
            auto page = std::make_unique<Cad2DPageGPU>();
            page->containerMemoryId = containerMemoryId;

            std::vector<Cad2DLineGPURecord> gpuLines;
            size_t polylineSegmentCount = 0;
            for (const Cad2DPolylineRecordCPU& polyline : records.polylines) {
                if (polyline.points.size() >= 2) polylineSegmentCount += polyline.points.size() - 1;
            }
            size_t polygonSegmentCount = 0;
            for (const Cad2DPolygonRecordCPU& polygon : records.polygons) {
                if (polygon.radius > 0.0) {
                    polygonSegmentCount += ClampedPolygonLineSegmentCount(polygon.lineSegmentCount);
                }
            }
            gpuLines.reserve(records.lines.size() + polylineSegmentCount + polygonSegmentCount);
            // Stamp kCad2DSelectedFlag on the GPU records belonging to selected objects (all the
            // segments an object expands into), so the 2D vertex shaders draw them in deep blue.
            auto stampSelected = [&](size_t firstIndex, uint64_t objectId) {
                if (selected2D.find(objectId) == selected2D.end()) return;
                for (size_t i = firstIndex; i < gpuLines.size(); ++i) gpuLines[i].flags |= kCad2DSelectedFlag;
            };
            for (const Cad2DLineRecordCPU& line : records.lines) {
                const size_t before = gpuLines.size();
                gpuLines.push_back(ToGpuLineRecord(line));
                stampSelected(before, line.objectId);
            }
            for (const Cad2DPolylineRecordCPU& polyline : records.polylines) {
                const size_t before = gpuLines.size();
                AppendPolylineLineRecords(polyline, gpuLines);
                stampSelected(before, polyline.objectId);
            }
            for (const Cad2DPolygonRecordCPU& polygon : records.polygons) {
                const size_t before = gpuLines.size();
                AppendPolygonLineRecords(polygon, gpuLines);
                stampSelected(before, polygon.objectId);
            }
            UploadVector(commandList.Get(), page->lineBuffer, gpuLines, uploads);
            page->lineCount = static_cast<uint32_t>(gpuLines.size());

            if (!gpuLines.empty()) {
                std::vector<D3D12_DRAW_ARGUMENTS> drawArgs(1);
                drawArgs[0].VertexCountPerInstance = 6;
                drawArgs[0].InstanceCount = page->lineCount;
                drawArgs[0].StartVertexLocation = 0;
                drawArgs[0].StartInstanceLocation = 0;
                UploadVector(commandList.Get(), page->lineIndirectBuffer, drawArgs, uploads);
            }

            std::vector<Cad2DCurveGPURecord> gpuCurves;
            gpuCurves.reserve(records.circles.size() + records.ellipses.size() + records.arcs.size());
            auto stampCurveSelected = [&](uint64_t objectId) {
                if (selected2D.find(objectId) != selected2D.end() && !gpuCurves.empty()) {
                    gpuCurves.back().flags |= kCad2DSelectedFlag;
                }
            };
            for (const Cad2DCircleRecordCPU& circle : records.circles) {
                if (circle.radius > 0.0) {
                    gpuCurves.push_back(ToGpuCircleRecord(circle));
                    stampCurveSelected(circle.objectId);
                }
            }
            for (const Cad2DEllipseRecordCPU& ellipse : records.ellipses) {
                if (ellipse.radiusX > 0.0 && ellipse.radiusY > 0.0) {
                    gpuCurves.push_back(ToGpuEllipseRecord(ellipse));
                    stampCurveSelected(ellipse.objectId);
                }
            }
            for (const Cad2DArcRecordCPU& arc : records.arcs) {
                if (arc.radiusX > 0.0 && arc.radiusY > 0.0) {
                    gpuCurves.push_back(ToGpuArcRecord(arc));
                    stampCurveSelected(arc.objectId);
                }
            }
            UploadVector(commandList.Get(), page->curveBuffer, gpuCurves, uploads);
            page->curveCount = static_cast<uint32_t>(gpuCurves.size());

            if (!gpuCurves.empty()) {
                std::vector<D3D12_DRAW_ARGUMENTS> drawArgs(1);
                drawArgs[0].VertexCountPerInstance = 6;
                drawArgs[0].InstanceCount = page->curveCount;
                drawArgs[0].StartVertexLocation = 0;
                drawArgs[0].StartInstanceLocation = 0;
                UploadVector(commandList.Get(), page->curveIndirectBuffer, drawArgs, uploads);
            }

            std::vector<Cad2DTextVertex> textVertices;
            std::vector<uint32_t> textIndices;
            for (const Cad2DTextRecordCPU& text : records.texts) {
                AppendTextRecordGeometry(text, textVertices, textIndices);
            }
            UploadVector(commandList.Get(), page->textVertexBuffer, textVertices, uploads);
            UploadVector(commandList.Get(), page->textIndexBuffer, textIndices, uploads);
            page->textVertexCount = static_cast<uint32_t>(textVertices.size());
            page->textIndexCount = static_cast<uint32_t>(textIndices.size());

            pages.push_back(std::move(page));
        }

        ThrowIfFailed(commandList->Close());
        ID3D12CommandList* lists[] = { commandList.Get() };
        gpu.copyCommandQueue->ExecuteCommandLists(1, lists);
        const uint64_t fenceValue = gpu.copyFenceValue.fetch_add(1);
        gpu.copyCommandQueue->Signal(gpu.copyFence.Get(), fenceValue);
        if (gpu.copyFence->GetCompletedValue() < fenceValue) {
            gpu.copyFence->SetEventOnCompletion(fenceValue, gpu.copyFenceEvent);
            WaitForSingleObject(gpu.copyFenceEvent, INFINITE);
        }
        uploads.clear();

        PublishCad2DPages(storage, std::move(pages));
    }
}

void PruneCad2DRetiredResources(TabCad2DStorage& storage, uint64_t safeRetireFence) {
    storage.retiredSnapshots.erase(std::remove_if(storage.retiredSnapshots.begin(),
        storage.retiredSnapshots.end(), [&](const TabCad2DStorage::RetiredSnapshot& retired) {
            if (retired.retireFence <= safeRetireFence) {
                delete retired.snapshot;
                return true;
            }
            return false;
        }), storage.retiredSnapshots.end());

    storage.retiredPages.erase(std::remove_if(storage.retiredPages.begin(), storage.retiredPages.end(),
        [&](const TabCad2DStorage::RetiredPage& retired) {
            return retired.retireFence <= safeRetireFence;
        }), storage.retiredPages.end());
}

void ReleaseCad2DRetiredResources(TabCad2DStorage& storage) {
    for (auto& retired : storage.retiredSnapshots) delete retired.snapshot;
    storage.retiredSnapshots.clear();
    storage.retiredPages.clear();
}
