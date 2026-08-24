// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

#include "RenderPage2D-DirectX12.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>

#include "colors.h"
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
/* One container's page content, already in GPU form (id.md §11.4, step 2b). The expansion writes
straight into these while holding cpuRecordsMutex, which is what deletes stages 3-4 of §11.1: the
CPU records are read in place and never copied. This replaced a Cad2DContainerRecords that held
seven vectors of full CPU records - two deep copies of the whole tab per batch. */
struct Cad2DContainerGpu {
    std::vector<Cad2DLineGPURecord> lines;
    std::vector<Cad2DCurveGPURecord> curves;
    std::vector<Cad2DTextVertex> textVertices;
    std::vector<uint32_t> textIndices;
};

constexpr uint32_t kMinPolygonLineSegmentCount = 3;
constexpr uint32_t kMaxPolygonLineSegmentCount = 16;
constexpr double kDegreesToRadians = 3.14159265358979323846 / 180.0;
constexpr uint32_t kCurveTypeCircle = 0;
constexpr uint32_t kCurveTypeEllipse = 1;
constexpr uint32_t kCurveTypeArc = 2;

uint32_t TopUIHeightPx(int monitorId, const DX12ResourcesPerWindow& winRes) {
    if (winRes.contentOnly) return 0; // Extracted view windows render content edge to edge.
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

/* CreateUploadWithData / UploadVector are gone (graphics.md, 10M plan Step 0). They committed a
fresh UPLOAD resource for every vector - up to six per container page, times every container, on
every batch - which is precisely the per-object staging allocation the ring exists to delete. Their
replacement lives inside ProcessCad2DCopyBatch as ring-backed lambdas, because it needs to be able
to FLUSH the recording when the ring fills, and only that function owns the command list. */

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
    gpuCurve.rotationRadians = static_cast<float>(ellipse.rotationRadians);
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
    gpuCurve.rotationRadians = static_cast<float>(arc.rotationRadians);
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

/* Publish activePages as a new immutable snapshot. Unlike the function this replaced it retires
NOTHING: pages are retired by whoever replaces them (a container rebuild), so a batch that only
appended into existing pages retires nothing and, if it opened no new page, does not even come
here - the append is published by the page's own count (id.md §11.2, step 2c). */
void PublishCad2DSnapshot(TabCad2DStorage& storage) {
    Cad2DPageSnapshot* newSnapshot = new Cad2DPageSnapshot();
    newSnapshot->pages.reserve(storage.activePages.size());
    for (const auto& page : storage.activePages) {
        newSnapshot->pages.push_back(page.get());
        // Pages never mix containers, so each lands in exactly one bucket.
        newSnapshot->pagesByContainer[page->containerMemoryId].push_back(page.get());
    }

    Cad2DPageSnapshot* oldSnapshot =
        storage.activeSnapshot.exchange(newSnapshot, std::memory_order_acq_rel);
    if (oldSnapshot) {
        storage.retiredSnapshots.push_back(
            { oldSnapshot, gpu.renderFenceValue.load(std::memory_order_acquire) });
    }
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
        storage.dx.lineRootSignature->SetName(L"Cad2D Line");

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
        storage.dx.curveRootSignature->SetName(L"Cad2D Curve");

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
        storage.dx.textRootSignature->SetName(L"Cad2D Text");

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
    // The view constant buffer is created per window on demand (RenderCad2DPage).
}

void CleanupCad2DTabResources(TabCad2DStorage& storage) {
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

    std::lock_guard<std::mutex> lock(storage.cpuRecordsMutex);
    storage.lineRecords.clear();
    storage.polylineRecords.clear();
    storage.polygonRecords.clear();
    storage.circleRecords.clear();
    storage.ellipseRecords.clear();
    storage.arcRecords.clear();
    storage.textRecords.clear();
    /* The two asset vectors used to survive this - a leftover from before the index existed, and
    harmless only while nothing looked a record up by id. It is not harmless now: tab slots are
    recycled and this storage is not destroyed with the tab, so a cleared index beside a populated
    vector would make the next load of an existing asset id APPEND a duplicate rather than update
    in place. They also had no business outliving the drawing they belong to. */
    storage.assetDefinitionRecords.clear();
    storage.assetInsertRecords.clear();
    storage.recordIndex.clear(); // Positions into the vectors just emptied.
    storage.demoLineCounter.store(0, std::memory_order_release);
    storage.demoTextQueued.store(false, std::memory_order_release);
    storage.bulkLineCounter.store(0, std::memory_order_release);
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
    storage.transform2DKind.store(0, std::memory_order_release);
    storage.transform2DStep.store(0, std::memory_order_release);
    storage.transform2DP1XCU.store(0.0, std::memory_order_release);
    storage.transform2DP1YCU.store(0.0, std::memory_order_release);
    storage.transform2DP2XCU.store(0.0, std::memory_order_release);
    storage.transform2DP2YCU.store(0.0, std::memory_order_release);
}

// Creates this window's Page2D view constant buffer on first use. Per window (not per tab) so two
// windows showing different Page2Ds of one tab don't overwrite each other's view in the shared
// monitor command list. Mirrors EnsureWindowUIBuffers.
static void EnsureWindowCad2DViewBuffer(DX12ResourcesPerWindow& winRes) {
    if (winRes.cad2dViewConstantBuffer) return;
    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
    auto desc = CD3DX12_RESOURCE_DESC::Buffer(256);
    ThrowIfFailed(gpu.device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
        &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&winRes.cad2dViewConstantBuffer)));
    CD3DX12_RANGE readRange(0, 0);
    ThrowIfFailed(winRes.cad2dViewConstantBuffer->Map(0, &readRange,
        reinterpret_cast<void**>(&winRes.pCad2DViewConstantDataBegin)));
}

void RenderPage2D(ID3D12GraphicsCommandList* commandList, DX12ResourcesPerWindow& winRes,
    TabCad2DStorage& storage, DX12ResourcesUI& uiResources, int monitorId,
    uint64_t activeContainerMemoryId, const Cad2DViewState& view) {
    if (!commandList || activeContainerMemoryId == 0 || winRes.WindowHeight <= 0) return;
    if (!storage.dx.lineRootSignature) return;

    EnsureWindowCad2DViewBuffer(winRes);
    if (!winRes.pCad2DViewConstantDataBegin) return;

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
    const float cadBackground[] = { kCad2DBackgroundR, kCad2DBackgroundG, kCad2DBackgroundB, 1.0f };
    commandList->ClearRenderTargetView(rttHandle, cadBackground, 0, nullptr);

    Cad2DViewConstants constants{}; // `view` is the Viewport's pan/zoom, handed in by the compositor.
    constants.viewCenterCU = {
        static_cast<float>(view.centerXCU.load(std::memory_order_acquire)),
        static_cast<float>(view.centerYCU.load(std::memory_order_acquire))
    };
    constants.zoomPixelsPerCU =
        (std::max)(view.zoomPixelsPerCU.load(std::memory_order_acquire),
            kCad2DZoomMinPixelsPerCU);
    constants.dpiY = monitorId >= 0 && monitorId < gpu.currentMonitorCount
        ? static_cast<float>(gpu.screens[monitorId].physicalDpiY)
        : 96.0f;
    constants.viewportSizePx = {
        static_cast<float>(winRes.WindowWidth),
        static_cast<float>(sceneHeight)
    };
    constants.minLineWeightPx = 1.0f;
    memcpy(winRes.pCad2DViewConstantDataBegin, &constants, sizeof(constants));
    const D3D12_GPU_VIRTUAL_ADDRESS viewCBV = winRes.cad2dViewConstantBuffer->GetGPUVirtualAddress();

    /* Only this container's pages, through the snapshot's directory rather than a scan over every
    page in the tab (id.md §11.2). The three passes below filter by KIND, which preserves the draw
    order the single-page build had: all lines, then all curves, then all text. */
    Cad2DPageSnapshot* snapshot = storage.activeSnapshot.load(std::memory_order_acquire);
    if (!snapshot) return;
    auto containerPages = snapshot->pagesByContainer.find(activeContainerMemoryId);
    if (containerPages == snapshot->pagesByContainer.end()) return;
    const std::vector<Cad2DPageGPU*>& pages = containerPages->second;

    commandList->SetGraphicsRootSignature(storage.dx.lineRootSignature.Get());
    commandList->SetPipelineState(storage.dx.linePSO.Get());
    commandList->SetGraphicsRootConstantBufferView(0, viewCBV);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    for (Cad2DPageGPU* page : pages) {
        if (!page || page->kind != Cad2DPageKind::Line) continue;
        if (page->count.load(std::memory_order_acquire) == 0) continue;
        if (!page->buffer || !page->indirectBuffer) continue;
        commandList->SetGraphicsRootShaderResourceView(1, page->buffer->GetGPUVirtualAddress());
        commandList->ExecuteIndirect(storage.dx.lineCommandSignature.Get(), 1,
            page->indirectBuffer.Get(), 0, nullptr, 0);
    }

    if (storage.dx.curveRootSignature && storage.dx.curvePSO && storage.dx.curveCommandSignature) {
        commandList->SetGraphicsRootSignature(storage.dx.curveRootSignature.Get());
        commandList->SetPipelineState(storage.dx.curvePSO.Get());
        commandList->SetGraphicsRootConstantBufferView(0, viewCBV);
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        for (Cad2DPageGPU* page : pages) {
            if (!page || page->kind != Cad2DPageKind::Curve) continue;
            if (page->count.load(std::memory_order_acquire) == 0) continue;
            if (!page->buffer || !page->indirectBuffer) continue;
            commandList->SetGraphicsRootShaderResourceView(1, page->buffer->GetGPUVirtualAddress());
            commandList->ExecuteIndirect(storage.dx.curveCommandSignature.Get(), 1,
                page->indirectBuffer.Get(), 0, nullptr, 0);
        }
    }

    const bool textAtlasReady = atlasFence.load(std::memory_order_acquire) != 0 &&
        gpu.copyFence && gpu.copyFence->GetCompletedValue() >= atlasFence.load(std::memory_order_acquire);
    if (!textAtlasReady || !storage.dx.textRootSignature || !storage.dx.textPSO) return;

    // Bind this monitor's UI SRV heap. Page2D text only samples the English MSDF atlas (slot 0), which
    // is present in every monitor's heap; the heap is (re)built by BuildMonitorIconAtlas.
    ID3D12DescriptorHeap* monitorSrvHeap =
        (monitorId >= 0 && monitorId < MV_MAX_MONITORS) ? gpu.screens[monitorId].uiSrvHeap.Get() : nullptr;
    if (!monitorSrvHeap) return;

    commandList->SetGraphicsRootSignature(storage.dx.textRootSignature.Get());
    commandList->SetPipelineState(storage.dx.textPSO.Get());
    ID3D12DescriptorHeap* heaps[] = { monitorSrvHeap, uiResources.samplerHeap.Get() };
    commandList->SetDescriptorHeaps(_countof(heaps), heaps);
    commandList->SetGraphicsRootConstantBufferView(0, viewCBV);
    commandList->SetGraphicsRootDescriptorTable(1, monitorSrvHeap->GetGPUDescriptorHandleForHeapStart());
    commandList->SetGraphicsRootDescriptorTable(2, uiResources.samplerHeap->GetGPUDescriptorHandleForHeapStart());

    for (Cad2DPageGPU* page : pages) {
        if (!page || page->kind != Cad2DPageKind::Text) continue;
        if (!page->buffer || !page->indexBuffer) continue;
        // ONE load, both numbers: see the comment on Cad2DPageGPU::count.
        const uint32_t vertexCount = page->count.load(std::memory_order_acquire);
        const uint32_t indexCount = vertexCount / 4 * 6;
        if (indexCount == 0) continue;

        D3D12_VERTEX_BUFFER_VIEW vbv{};
        vbv.BufferLocation = page->buffer->GetGPUVirtualAddress();
        vbv.SizeInBytes = vertexCount * sizeof(Cad2DTextVertex);
        vbv.StrideInBytes = sizeof(Cad2DTextVertex);

        D3D12_INDEX_BUFFER_VIEW ibv{};
        ibv.BufferLocation = page->indexBuffer->GetGPUVirtualAddress();
        ibv.SizeInBytes = indexCount * sizeof(uint32_t);
        ibv.Format = DXGI_FORMAT_R32_UINT;

        commandList->IASetVertexBuffers(0, 1, &vbv);
        commandList->IASetIndexBuffer(&ibv);
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commandList->DrawIndexedInstanced(indexCount, 1, 0, 0, 0);
    }
}

#ifdef _DEBUG
// Diagnostics for the ReportIngestStats sentinel: per-type record counts and the bounding box
// of one container's live records. storage.cpuRecordsMutex must already be held by the caller.
static void ReportCad2DIngestStatsLocked(const TabCad2DStorage& storage, uint64_t containerMemoryId) {
    double minX = 0.0, minY = 0.0, maxX = 0.0, maxY = 0.0;
    bool hasBounds = false;
    auto include = [&](double x, double y) {
        if (!hasBounds) { minX = maxX = x; minY = maxY = y; hasBounds = true; return; }
        minX = (std::min)(minX, x); maxX = (std::max)(maxX, x);
        minY = (std::min)(minY, y); maxY = (std::max)(maxY, y);
    };
    auto wanted = [&](bool isDeleted, uint64_t recContainer) {
        return !isDeleted && recContainer == containerMemoryId;
    };

    size_t lines = 0, polylines = 0, polygons = 0, circles = 0, ellipses = 0, arcs = 0, texts = 0;
    for (const Cad2DLineRecordCPU& r : storage.lineRecords) {
        if (!wanted(r.isDeleted, r.containerMemoryId)) continue;
        ++lines;
        include(r.x1, r.y1); include(r.x2, r.y2);
    }
    for (const Cad2DPolylineRecordCPU& r : storage.polylineRecords) {
        if (!wanted(r.isDeleted, r.containerMemoryId)) continue;
        ++polylines;
        for (const Cad2DPoint2D& p : r.points) include(p.x, p.y);
    }
    for (const Cad2DPolygonRecordCPU& r : storage.polygonRecords) {
        if (!wanted(r.isDeleted, r.containerMemoryId)) continue;
        ++polygons;
        include(r.centerX - r.radius, r.centerY - r.radius);
        include(r.centerX + r.radius, r.centerY + r.radius);
    }
    for (const Cad2DCircleRecordCPU& r : storage.circleRecords) {
        if (!wanted(r.isDeleted, r.containerMemoryId)) continue;
        ++circles;
        include(r.centerX - r.radius, r.centerY - r.radius);
        include(r.centerX + r.radius, r.centerY + r.radius);
    }
    for (const Cad2DEllipseRecordCPU& r : storage.ellipseRecords) {
        if (!wanted(r.isDeleted, r.containerMemoryId)) continue;
        ++ellipses;
        const double radius = (std::max)(std::abs(r.radiusX), std::abs(r.radiusY));
        include(r.centerX - radius, r.centerY - radius);
        include(r.centerX + radius, r.centerY + radius);
    }
    for (const Cad2DArcRecordCPU& r : storage.arcRecords) {
        if (!wanted(r.isDeleted, r.containerMemoryId)) continue;
        ++arcs;
        const double radius = (std::max)(std::abs(r.radiusX), std::abs(r.radiusY));
        include(r.centerX - radius, r.centerY - radius);
        include(r.centerX + radius, r.centerY + radius);
    }
    for (const Cad2DTextRecordCPU& r : storage.textRecords) {
        if (!wanted(r.isDeleted, r.containerMemoryId)) continue;
        ++texts;
        include(r.x, r.y); include(r.x, r.y + (double)r.textHeightCU);
    }

    std::cout << "[cad2d][dbg] ingest complete, container " << containerMemoryId
              << ": total=" << (lines + polylines + polygons + circles + ellipses + arcs + texts)
              << " (lines=" << lines << ", polylines=" << polylines << ", polygons=" << polygons
              << ", circles=" << circles << ", ellipses=" << ellipses << ", arcs=" << arcs
              << ", texts=" << texts << ")";
    if (hasBounds) {
        std::cout << " bbox CU X[" << minX << " .. " << maxX << "] Y[" << minY << " .. " << maxY
                  << "] size " << (maxX - minX) << " x " << (maxY - minY);
    } else {
        std::cout << " (no live records)";
    }
    std::cout << std::endl;
}
#endif

void ProcessCad2DCopyBatch(const std::vector<CommandToCopyThread2D>& batch,
    ComPtr<ID3D12CommandAllocator>& commandAllocator,
    ComPtr<ID3D12GraphicsCommandList>& commandList) {
    if (batch.empty()) return;

    /* Step 2a instrumentation (id.md §11.4). Every counter measures one of the seven O(N) stages
    §11.1 lists, so a later sub-step is checked by watching its own number fall rather than by
    re-reasoning about the code. Locals first, atomics once at the end: the per-batch line is what
    a human reads, the cumulative totals are what the heartbeat reads.

    2b took `indexed` to one hash lookup per command and `copied` to zero. 2c takes `expanded`,
    `staged` and `pages` to the batch's own objects whenever a batch only APPENDS - a batch that
    modifies an object or changes the selection still rebuilds that container, and its numbers
    still show it. They all stay: a number that must remain small is the cheapest regression
    guard there is. */
    const auto batchStart = std::chrono::steady_clock::now();
    uint64_t statRecordsIndexed = 0, statRecordsExpanded = 0;
    uint64_t statBytesStaged = 0, statPagesBuilt = 0, statPagesRetired = 0;
    constexpr uint64_t statRecordsCopied = 0; // Nothing on this path deep-copies a record any more.

    // Pointers, not copies, exactly as ProcessScene3DCopyBatch does it: CommandToCopyThread2D is
    // 736 bytes and holds every record type as a simultaneous member (id.md §10), so grouping by
    // value copied ~73 MB for a 100k import before a single record was read.
    std::unordered_map<uint64_t, std::vector<const CommandToCopyThread2D*>> byTab;
    for (const CommandToCopyThread2D& command : batch) {
        byTab[command.tabID].push_back(&command);
    }

    for (auto& [tabID, commands] : byTab) {
        if (tabID >= MV_MAX_TABS) continue;
        DATASETTAB& tab = allTabs[tabID];
        if (!tab.cad2d) continue;
        TabCad2DStorage& storage = *tab.cad2d;

        /* Selection first, and its mutex is RELEASED before cpuRecordsMutex is taken. The
        engineering thread acquires them in that same order (Cad2DCreateAssetFromSelection), so
        holding both here is the one place the two orders could invert. */
        std::unordered_set<uint64_t> selected2D; // Objects to stamp with kCad2DSelectedFlag.
        {
            std::lock_guard<std::mutex> lock(storage.selection2DMutex);
            selected2D = storage.selectedObjectIds;
        }

        /* Two kinds of work come out of the locked section (id.md §11.4, step 2c):

             appendWork  - the GPU records of objects this batch CREATED, to be appended to the
                           tail pages of their container. O(batch).
             rebuildWork - every record of a container that cannot be appended to, because
                           something in the batch modified an existing object or refreshed the
                           selection. O(container), and step 2d is what removes it.

        A container in rebuildWork is never also in appendWork: the rebuild already covers its
        new objects. */
        std::map<uint64_t, Cad2DContainerGpu> appendWork;
        std::map<uint64_t, Cad2DContainerGpu> rebuildWork;
        {
            std::lock_guard<std::mutex> lock(storage.cpuRecordsMutex);

            std::set<uint64_t> rebuildContainers;
            std::map<uint64_t, std::vector<uint64_t>> newByContainer;

            /* Insert-or-update through the tab's PERSISTENT index (step 2b): one hash lookup per
            command, where this used to rebuild an index of every record in the tab on every
            batch. Returns true when the record is NEW - which is exactly the test for whether
            this command can be appended rather than force a rebuild. An update keeps the
            already-assigned persistedId / persistedParentId when the incoming record carries
            none. */
            auto upsert = [&](auto& records, const auto& incoming) -> bool {
                ++statRecordsIndexed;
                auto found = storage.recordIndex.find(incoming.objectId);
                if (found == storage.recordIndex.end()) {
                    records.push_back(incoming);
                    Cad2DIndexAppendedRecord(storage, records);
                    // First sighting by definition - the index is what the old per-batch knownIds
                    // set was rebuilt from tab.allIDsInThisTab to answer.
                    tab.allIDsInThisTab.push_back(incoming.objectId);
                    return true;
                }
                if (found->second.type != Cad2DKindOf(incoming) ||
                    found->second.index >= records.size()) {
                    // Cannot happen while ids are unique and the vectors are append-only, but the
                    // consequence if it ever did is an out-of-range write into the wrong vector.
#ifdef _DEBUG
                    std::cout << "[cad2d][warn] recordIndex inconsistent for objectId="
                              << incoming.objectId << "; command dropped." << std::endl;
#endif
                    return false;
                }
                auto updated = incoming;
                auto& existing = records[found->second.index];
                if (updated.persistedId == 0) updated.persistedId = existing.persistedId;
                if (updated.persistedParentId == 0) updated.persistedParentId = existing.persistedParentId;
                existing = std::move(updated);
                return false;
            };
            auto classify = [&](uint64_t container, uint64_t objectId, bool isNew) {
                if (isNew) newByContainer[container].push_back(objectId);
                else rebuildContainers.insert(container);
            };

            for (const CommandToCopyThread2D* command : commands) {
                const uint64_t container = command->containerMemoryId;
                if (container == 0) continue;
                switch (command->type) {
                case CommandToCopyThread2DType::AddLine:
#ifdef _DEBUG
                    // Corruption checkpoint: was the record still sane when it crossed the queue?
                    if (std::abs(command->line.x1) > 1.0e8 || std::abs(command->line.y1) > 1.0e8 ||
                        std::abs(command->line.x2) > 1.0e8 || std::abs(command->line.y2) > 1.0e8) {
                        std::cout << "[cad2d][dbg] OUTLIER AT INGEST line objectId="
                                  << command->line.objectId << " container=" << container
                                  << " (" << command->line.x1 << ", " << command->line.y1
                                  << ") -> (" << command->line.x2 << ", " << command->line.y2
                                  << ")" << std::endl;
                    }
#endif
                    classify(container, command->line.objectId,
                        upsert(storage.lineRecords, command->line));
                    break;
                case CommandToCopyThread2DType::AddPolyline:
                    classify(container, command->polyline.objectId,
                        upsert(storage.polylineRecords, command->polyline));
                    break;
                case CommandToCopyThread2DType::AddPolygon:
                    classify(container, command->polygon.objectId,
                        upsert(storage.polygonRecords, command->polygon));
                    break;
                case CommandToCopyThread2DType::AddCircle:
                    classify(container, command->circle.objectId,
                        upsert(storage.circleRecords, command->circle));
                    break;
                case CommandToCopyThread2DType::AddEllipse:
                    classify(container, command->ellipse.objectId,
                        upsert(storage.ellipseRecords, command->ellipse));
                    break;
                case CommandToCopyThread2DType::AddArc:
                    classify(container, command->arc.objectId,
                        upsert(storage.arcRecords, command->arc));
                    break;
                case CommandToCopyThread2DType::AddText:
                    classify(container, command->text.objectId,
                        upsert(storage.textRecords, command->text));
                    break;
                case CommandToCopyThread2DType::SelectionRefresh:
                    // Carries no geometry: it exists to re-stamp kCad2DSelectedFlag, which today
                    // means rebuilding the container. Step 2d turns it into a 4-byte store.
                    rebuildContainers.insert(container);
                    break;
#ifdef _DEBUG
                case CommandToCopyThread2DType::ReportIngestStats:
                    ReportCad2DIngestStatsLocked(storage, container);
                    break;
#endif
                default:
                    break;
                }
            }

            // ---- expansion: CPU records -> GPU records, read in place under this lock ----
            auto stampLines = [&](std::vector<Cad2DLineGPURecord>& gpuLines, size_t firstIndex,
                uint64_t objectId) {
                    if (selected2D.find(objectId) == selected2D.end()) return;
                    for (size_t i = firstIndex; i < gpuLines.size(); ++i) {
                        gpuLines[i].flags |= kCad2DSelectedFlag;
                    }
                };
            auto stampCurve = [&](std::vector<Cad2DCurveGPURecord>& gpuCurves, uint64_t objectId) {
                if (selected2D.find(objectId) != selected2D.end() && !gpuCurves.empty()) {
                    gpuCurves.back().flags |= kCad2DSelectedFlag;
                }
                };
            auto addLine = [&](const Cad2DLineRecordCPU& r, Cad2DContainerGpu& out) {
                const size_t before = out.lines.size();
                out.lines.push_back(ToGpuLineRecord(r));
                stampLines(out.lines, before, r.objectId);
                };
            auto addPolyline = [&](const Cad2DPolylineRecordCPU& r, Cad2DContainerGpu& out) {
                const size_t before = out.lines.size();
                AppendPolylineLineRecords(r, out.lines);
                stampLines(out.lines, before, r.objectId);
                };
            auto addPolygon = [&](const Cad2DPolygonRecordCPU& r, Cad2DContainerGpu& out) {
                const size_t before = out.lines.size();
                AppendPolygonLineRecords(r, out.lines);
                stampLines(out.lines, before, r.objectId);
                };
            auto addCircle = [&](const Cad2DCircleRecordCPU& r, Cad2DContainerGpu& out) {
                if (r.radius <= 0.0) return;
                out.curves.push_back(ToGpuCircleRecord(r));
                stampCurve(out.curves, r.objectId);
                };
            auto addEllipse = [&](const Cad2DEllipseRecordCPU& r, Cad2DContainerGpu& out) {
                if (r.radiusX <= 0.0 || r.radiusY <= 0.0) return;
                out.curves.push_back(ToGpuEllipseRecord(r));
                stampCurve(out.curves, r.objectId);
                };
            auto addArc = [&](const Cad2DArcRecordCPU& r, Cad2DContainerGpu& out) {
                if (r.radiusX <= 0.0 || r.radiusY <= 0.0) return;
                out.curves.push_back(ToGpuArcRecord(r));
                stampCurve(out.curves, r.objectId);
                };
            auto addText = [&](const Cad2DTextRecordCPU& r, Cad2DContainerGpu& out) {
                AppendTextRecordGeometry(r, out.textVertices, out.textIndices);
                };

            /* Whole-container expansion. The per-type order is what a page's record order comes
            from - lines, then polylines, then polygons into the line array; circles, then
            ellipses, then arcs into the curve array. Do not reorder these loops. */
            for (uint64_t container : rebuildContainers) {
                Cad2DContainerGpu& out = rebuildWork[container];
                for (const Cad2DLineRecordCPU& r : storage.lineRecords) {
                    if (r.containerMemoryId == container) addLine(r, out);
                }
                for (const Cad2DPolylineRecordCPU& r : storage.polylineRecords) {
                    if (r.containerMemoryId == container) addPolyline(r, out);
                }
                for (const Cad2DPolygonRecordCPU& r : storage.polygonRecords) {
                    if (r.containerMemoryId == container) addPolygon(r, out);
                }
                for (const Cad2DCircleRecordCPU& r : storage.circleRecords) {
                    if (r.containerMemoryId == container) addCircle(r, out);
                }
                for (const Cad2DEllipseRecordCPU& r : storage.ellipseRecords) {
                    if (r.containerMemoryId == container) addEllipse(r, out);
                }
                for (const Cad2DArcRecordCPU& r : storage.arcRecords) {
                    if (r.containerMemoryId == container) addArc(r, out);
                }
                for (const Cad2DTextRecordCPU& r : storage.textRecords) {
                    if (r.containerMemoryId == container) addText(r, out);
                }
            }

            /* Append expansion: only the objects this batch created, in COMMAND order. That
            differs from the per-type order a rebuild produces, which is the draw-order change
            §11.6 accepts - invisible for opaque strokes, visible only where coloured 2D geometry
            overlaps. */
            for (const auto& [container, objectIds] : newByContainer) {
                if (rebuildContainers.count(container) != 0) continue; // The rebuild covers them.
                Cad2DContainerGpu& out = appendWork[container];
                for (uint64_t objectId : objectIds) {
                    auto found = storage.recordIndex.find(objectId);
                    if (found == storage.recordIndex.end()) continue;
                    const uint32_t at = found->second.index;
                    switch (found->second.type) {
                    case VishwakarmaStorage::ObjectType::Line2D:
                        if (at < storage.lineRecords.size()) addLine(storage.lineRecords[at], out);
                        break;
                    case VishwakarmaStorage::ObjectType::Polyline2D:
                        if (at < storage.polylineRecords.size()) addPolyline(storage.polylineRecords[at], out);
                        break;
                    case VishwakarmaStorage::ObjectType::Polygon2D:
                        if (at < storage.polygonRecords.size()) addPolygon(storage.polygonRecords[at], out);
                        break;
                    case VishwakarmaStorage::ObjectType::Circle2D:
                        if (at < storage.circleRecords.size()) addCircle(storage.circleRecords[at], out);
                        break;
                    case VishwakarmaStorage::ObjectType::Ellipse2D:
                        if (at < storage.ellipseRecords.size()) addEllipse(storage.ellipseRecords[at], out);
                        break;
                    case VishwakarmaStorage::ObjectType::Arc2D:
                        if (at < storage.arcRecords.size()) addArc(storage.arcRecords[at], out);
                        break;
                    case VishwakarmaStorage::ObjectType::Text2D:
                        if (at < storage.textRecords.size()) addText(storage.textRecords[at], out);
                        break;
                    default:
                        break;
                    }
                }
            }
        }

        for (const auto& [container, content] : rebuildWork) {
            statRecordsExpanded += content.lines.size() + content.curves.size() + content.textVertices.size();
        }
        for (const auto& [container, content] : appendWork) {
            statRecordsExpanded += content.lines.size() + content.curves.size() + content.textVertices.size();
        }
        if (appendWork.empty() && rebuildWork.empty()) continue; // Diagnostics-only batch.

        /* Staging for this tab (graphics.md, 10M plan Step 0). The allocator and list are the copy
        thread's, handed in rather than created per batch, and every byte goes through the one
        global ring instead of a committed UPLOAD resource per vector. */
        ThrowIfFailed(commandAllocator->Reset());
        ThrowIfFailed(commandList->Reset(commandAllocator.Get(), nullptr));

        std::vector<ComPtr<ID3D12Resource>> oversizeStaging;
        bool snapshotDirty = false;
        /* Fill state for the pages this batch touches. A page's own `count` stays at the value
        the render threads are drawing until every copy is fence-waited, so the running fill has
        to live somewhere else until then. */
        std::unordered_map<Cad2DPageGPU*, uint32_t> pendingCount;
        std::unordered_set<Cad2DPageGPU*> freshPages; // Created here: no published frame can read them.

        // Close, execute, signal, tag the ring, and wait. `reopen` distinguishes a mid-batch flush
        // from the final submit, which must leave the list closed for the Scene3D batch that runs
        // next. The CPU wait is the back-pressure, not a stall to optimise away.
        auto SubmitRecording = [&](bool reopen) {
            ThrowIfFailed(commandList->Close());
            ID3D12CommandList* lists[] = { commandList.Get() };
            gpu.copyCommandQueue->ExecuteCommandLists(1, lists);
            const uint64_t fenceValue = gpu.copyFenceValue.fetch_add(1);
            gpu.copyCommandQueue->Signal(gpu.copyFence.Get(), fenceValue);
            gpu.uploadRing.TagSubmission(fenceValue); // Ring space returns when this fence passes.
            if (gpu.copyFence->GetCompletedValue() < fenceValue) {
                gpu.copyFence->SetEventOnCompletion(fenceValue, gpu.copyFenceEvent);
                WaitForSingleObject(gpu.copyFenceEvent, INFINITE);
            }
            oversizeStaging.clear(); // Fallback buffers are safe to free once the copies completed.
            if (!reopen) return;
            ThrowIfFailed(commandAllocator->Reset());
            ThrowIfFailed(commandList->Reset(commandAllocator.Get(), nullptr));
            };

        // Mirrors the Scene3D AcquireStaging, plus the flush-and-retry the 2D path needs. A payload
        // larger than the whole ring can never be satisfied by waiting, so it goes straight to a
        // one-off committed buffer rather than flushing pointlessly first.
        auto AcquireStaging = [&](uint64_t bytes, uint8_t*& outCpu,
            ID3D12Resource*& outResource, uint64_t& outOffset) {
                if (gpu.uploadRing.Allocate(bytes, outCpu, outOffset)) {
                    outResource = gpu.uploadRing.buffer.Get();
                    gCopyStats.ringBytes.fetch_add(bytes, std::memory_order_relaxed);
                    return;
                }
                if (bytes <= GpuUploadRing::kCapacity) {
                    // The ring is merely full. Flush what is staged; the fence wait inside releases
                    // every in-flight region, so the retry cannot fail.
                    SubmitRecording(true);
                    if (gpu.uploadRing.Allocate(bytes, outCpu, outOffset)) {
                        outResource = gpu.uploadRing.buffer.Get();
                        gCopyStats.ringBytes.fetch_add(bytes, std::memory_order_relaxed);
                        return;
                    }
                }
                ComPtr<ID3D12Resource> fallback;
                CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
                auto uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(bytes);
                ThrowIfFailed(gpu.device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE,
                    &uploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                    IID_PPV_ARGS(&fallback)));
                CD3DX12_RANGE readRange(0, 0);
                ThrowIfFailed(fallback->Map(0, &readRange, reinterpret_cast<void**>(&outCpu)));
                outResource = fallback.Get();
                outOffset = 0;
                oversizeStaging.push_back(std::move(fallback));
                gCopyStats.oversizeStaging.fetch_add(1, std::memory_order_relaxed);
            };

        // Stage bytes and record the copy. Order matters: AcquireStaging may flush, which closes
        // and re-opens the command list, so the CopyBufferRegion must be recorded AFTER it.
        auto StageInto = [&](ID3D12Resource* destination, uint64_t destinationOffset,
            const void* source, uint64_t bytes) {
                if (bytes == 0) return;
                statBytesStaged += bytes;
                uint8_t* mapped = nullptr;
                ID3D12Resource* stagingResource = nullptr;
                uint64_t stagingOffset = 0;
                AcquireStaging(bytes, mapped, stagingResource, stagingOffset);
                memcpy(mapped, source, bytes);
                commandList->CopyBufferRegion(destination, destinationOffset, stagingResource,
                    stagingOffset, bytes);
            };

        auto FillOf = [&](Cad2DPageGPU* page) -> uint32_t {
            auto found = pendingCount.find(page);
            return found != pendingCount.end() ? found->second
                                               : page->count.load(std::memory_order_relaxed);
            };
        /* The last page of this container and kind, if it still has room. Only the last one can:
        pages are filled in order and a new one is opened only when its predecessor is full. */
        auto TailPage = [&](uint64_t container, Cad2DPageKind kind) -> Cad2DPageGPU* {
            for (auto it = storage.activePages.rbegin(); it != storage.activePages.rend(); ++it) {
                Cad2DPageGPU* page = it->get();
                if (!page || page->containerMemoryId != container || page->kind != kind) continue;
                return FillOf(page) < page->capacity ? page : nullptr;
            }
            return nullptr;
            };
        auto CreatePage = [&](uint64_t container, Cad2DPageKind kind, uint32_t capacity) {
            auto owned = std::make_unique<Cad2DPageGPU>();
            Cad2DPageGPU* page = owned.get();
            page->containerMemoryId = container;
            page->kind = kind;
            page->capacity = capacity;
            const uint64_t stride = kind == Cad2DPageKind::Line ? sizeof(Cad2DLineGPURecord)
                : kind == Cad2DPageKind::Curve ? sizeof(Cad2DCurveGPURecord) : sizeof(Cad2DTextVertex);
            page->buffer = CreateDefaultBuffer(static_cast<uint64_t>(capacity) * stride);
            if (kind == Cad2DPageKind::Text) {
                page->indexCapacity = capacity / 4 * 6;
                page->indexBuffer =
                    CreateDefaultBuffer(static_cast<uint64_t>(page->indexCapacity) * sizeof(uint32_t));
            } else {
                page->indirectBuffer = CreateDefaultBuffer(sizeof(D3D12_DRAW_ARGUMENTS));
            }
            storage.activePages.push_back(std::move(owned));
            freshPages.insert(page);
            ++statPagesBuilt;
            snapshotDirty = true; // A new page has to reach the render threads through a snapshot.
            return page;
            };

        /* Append fixed-stride records to the container's tail page of this kind, opening new pages
        as it fills them. An object that does not fit a standard page gets one sized to hold it -
        a 40,000-segment polyline has to land somewhere. */
        auto AppendRecords = [&](uint64_t container, Cad2DPageKind kind, const void* data,
            size_t count, uint32_t stride, uint32_t standardCapacity) {
                size_t done = 0;
                while (done < count) {
                    Cad2DPageGPU* page = TailPage(container, kind);
                    if (!page) {
                        const size_t remaining = count - done;
                        page = CreatePage(container, kind, static_cast<uint32_t>(
                            remaining > standardCapacity ? remaining : standardCapacity));
                    }
                    const uint32_t at = FillOf(page);
                    const size_t take = (std::min)(static_cast<size_t>(page->capacity - at), count - done);
                    StageInto(page->buffer.Get(), static_cast<uint64_t>(at) * stride,
                        static_cast<const uint8_t*>(data) + done * stride, take * stride);
                    pendingCount[page] = at + static_cast<uint32_t>(take);
                    done += take;
                }
            };

        auto AppendText = [&](uint64_t container, const std::vector<Cad2DTextVertex>& vertices,
            const std::vector<uint32_t>& indices) {
                /* Glyph quads are 4 vertices to 6 indices, emitted in lockstep, so a split on a
                quad boundary is safe. The indices are relative to `vertices`, so a quad landing at
                page vertex base `at` shifts every index by (at - 4 * done). */
                const size_t quads = vertices.size() / 4;
                size_t done = 0;
                std::vector<uint32_t> shifted;
                while (done < quads) {
                    Cad2DPageGPU* page = TailPage(container, Cad2DPageKind::Text);
                    if (!page) {
                        const size_t remainingVertices = (quads - done) * 4;
                        page = CreatePage(container, Cad2DPageKind::Text, static_cast<uint32_t>(
                            remainingVertices > kCad2DTextPageVertexCapacity
                                ? remainingVertices : kCad2DTextPageVertexCapacity));
                    }
                    const uint32_t at = FillOf(page);
                    const size_t take =
                        (std::min)(static_cast<size_t>((page->capacity - at) / 4), quads - done);
                    StageInto(page->buffer.Get(), static_cast<uint64_t>(at) * sizeof(Cad2DTextVertex),
                        &vertices[done * 4], take * 4 * sizeof(Cad2DTextVertex));

                    shifted.resize(take * 6);
                    for (size_t i = 0; i < take * 6; ++i) {
                        shifted[i] = indices[done * 6 + i] - static_cast<uint32_t>(done * 4) + at;
                    }
                    StageInto(page->indexBuffer.Get(),
                        static_cast<uint64_t>(at / 4 * 6) * sizeof(uint32_t),
                        shifted.data(), take * 6 * sizeof(uint32_t));

                    pendingCount[page] = at + static_cast<uint32_t>(take * 4);
                    done += take;
                }
            };

        auto RetireContainerPages = [&](uint64_t container) {
            const uint64_t retireFence = gpu.renderFenceValue.load(std::memory_order_acquire);
            for (size_t i = storage.activePages.size(); i-- > 0; ) {
                if (!storage.activePages[i] ||
                    storage.activePages[i]->containerMemoryId != container) {
                    continue;
                }
                ++statPagesRetired;
                storage.retiredPages.push_back({ std::move(storage.activePages[i]), retireFence });
                storage.activePages.erase(storage.activePages.begin() + i);
            }
            snapshotDirty = true;
            };

        auto AppendContent = [&](uint64_t container, const Cad2DContainerGpu& content) {
            AppendRecords(container, Cad2DPageKind::Line, content.lines.data(), content.lines.size(),
                sizeof(Cad2DLineGPURecord), kCad2DLinePageCapacity);
            AppendRecords(container, Cad2DPageKind::Curve, content.curves.data(), content.curves.size(),
                sizeof(Cad2DCurveGPURecord), kCad2DCurvePageCapacity);
            AppendText(container, content.textVertices, content.textIndices);
            };

        /* A rebuild is just "retire this container's pages, then append everything" - which is why
        there is one filling path and not two. */
        for (const auto& [container, content] : rebuildWork) {
            RetireContainerPages(container);
            AppendContent(container, content);
        }
        for (const auto& [container, content] : appendWork) {
            AppendContent(container, content);
        }

        // A fresh page is in no published snapshot, so its argument buffer can be written once,
        // here, with the count it ends the batch on. No patch, no second submit.
        for (Cad2DPageGPU* page : freshPages) {
            if (page->kind == Cad2DPageKind::Text) continue; // Text draws from the count directly.
            D3D12_DRAW_ARGUMENTS drawArgs{};
            drawArgs.VertexCountPerInstance = 6;
            drawArgs.InstanceCount = FillOf(page);
            StageInto(page->indirectBuffer.Get(), 0, &drawArgs, sizeof(drawArgs));
        }

        /* Pages that were already PUBLISHED and grew. Their new records went into the tail no
        frame reads; what makes them drawn is InstanceCount, and that must not move until the
        record copy has completed - hence a second submit after the first one's fence wait. Text
        pages never appear here: they carry no argument buffer, and the atomic store below is what
        publishes them. */
        std::vector<Cad2DPageGPU*> pagesToPatch;
        for (const auto& [page, newCount] : pendingCount) {
            if (freshPages.count(page) == 0 && page->kind != Cad2DPageKind::Text) {
                pagesToPatch.push_back(page);
            }
        }

        if (pagesToPatch.empty()) {
            SubmitRecording(false);
        } else {
            SubmitRecording(true); // Waits: every appended record is in VRAM before the count moves.
            for (Cad2DPageGPU* page : pagesToPatch) {
                const uint32_t newCount = pendingCount[page];
                StageInto(page->indirectBuffer.Get(), offsetof(D3D12_DRAW_ARGUMENTS, InstanceCount),
                    &newCount, sizeof(uint32_t));
            }
            SubmitRecording(false);
        }
        gCopyStats.ringHighWater.store(gpu.uploadRing.highWaterBytes, std::memory_order_relaxed);

        // Every copy is fence-waited, so the counts may now become visible to the render threads.
        for (const auto& [page, newCount] : pendingCount) {
            page->count.store(newCount, std::memory_order_release);
        }
        if (snapshotDirty) PublishCad2DSnapshot(storage);
    }

    const uint64_t batchMicros = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - batchStart).count());
    gCopyStats.cad2dBatches.fetch_add(1, std::memory_order_relaxed);
    gCopyStats.cad2dCommands.fetch_add(batch.size(), std::memory_order_relaxed);
    gCopyStats.cad2dRecordsIndexed.fetch_add(statRecordsIndexed, std::memory_order_relaxed);
    gCopyStats.cad2dRecordsCopied.fetch_add(statRecordsCopied, std::memory_order_relaxed);
    gCopyStats.cad2dRecordsExpanded.fetch_add(statRecordsExpanded, std::memory_order_relaxed);
    gCopyStats.cad2dBytesStaged.fetch_add(statBytesStaged, std::memory_order_relaxed);
    gCopyStats.cad2dPagesBuilt.fetch_add(statPagesBuilt, std::memory_order_relaxed);
    gCopyStats.cad2dPagesRetired.fetch_add(statPagesRetired, std::memory_order_relaxed);
    gCopyStats.cad2dBatchMicros.fetch_add(batchMicros, std::memory_order_relaxed);
    gCopyStats.cad2dLastBatchMicros.store(batchMicros, std::memory_order_relaxed);

#ifdef _DEBUG
    /* One line per batch, and it is the whole measurement: `cmds` against the three record
    counters is the ratio §11.1 is about. Debug-only because it prints on every 2D edit, and
    Debug is what the benchmark runs in anyway. */
    std::cout << "[cad2d][perf] cmds=" << batch.size()
              << " indexed=" << statRecordsIndexed
              << " copied=" << statRecordsCopied
              << " expanded=" << statRecordsExpanded
              << " staged=" << statBytesStaged << " B" // Bytes, not KB: step 2c's criterion is 160.
              << " pages=+" << statPagesBuilt << "/-" << statPagesRetired
              << " in " << (batchMicros / 1000.0) << " ms" << std::endl;
#endif
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
