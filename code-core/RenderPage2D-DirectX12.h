// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <d3dx12.h>
#include <DirectXMath.h>
#include <wrl.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "ConstantsApplication.h" // MV_MAX_SUBTABS: one Cad2DViewState per sub-tab slot.
#include "RenderPage2D.h"

struct DX12ResourcesPerWindow;
struct DX12ResourcesUI;

using Microsoft::WRL::ComPtr;

struct Cad2DLineGPURecord {
    float x1;
    float y1;
    float x2;
    float y2;
    float lineWeight;
    uint32_t lineWeightMode;
    uint32_t colorABGR;
    uint32_t flags;
};
static_assert(sizeof(Cad2DLineGPURecord) == 32, "Cad2DLineGPURecord must be 32 bytes.");

struct Cad2DCurveGPURecord {
    float centerX;
    float centerY;
    float radiusX;
    float radiusY;
    float startX;
    float startY;
    float endX;
    float endY;
    float lineWeight;
    uint32_t lineWeightMode;
    uint32_t colorABGR;
    uint32_t curveType;
    uint32_t flags;
    float rotationRadians; // CCW rotation of the radius axes about the center.
    uint32_t padding1;
    uint32_t padding2;
};
static_assert(sizeof(Cad2DCurveGPURecord) == 64, "Cad2DCurveGPURecord must be 64 bytes.");

struct Cad2DTextVertex {
    float x;
    float y;
    float u;
    float v;
    uint32_t colorABGR;
    uint32_t atlasIndex;
};
static_assert(sizeof(Cad2DTextVertex) == 24, "Cad2DTextVertex must match Shader2D_TextVertex input.");

struct Cad2DViewConstants {
    DirectX::XMFLOAT2 viewCenterCU;
    float zoomPixelsPerCU;
    float dpiY;
    DirectX::XMFLOAT2 viewportSizePx;
    float minLineWeightPx;
    float padding0;
};
static_assert(sizeof(Cad2DViewConstants) == 32, "Cad2DViewConstants must stay 16-byte aligned.");

struct Cad2DViewState {
    std::atomic<double> centerXCU{ 0.0 };
    std::atomic<double> centerYCU{ 0.0 };
    std::atomic<float> zoomPixelsPerCU{ 2.0f };

    // Back to the default view. Used when a sub-tab slot is (re)assigned to a Page2D so a recycled
    // slot does not inherit the previous Page2D's pan/zoom.
    void Reset() {
        centerXCU.store(0.0, std::memory_order_release);
        centerYCU.store(0.0, std::memory_order_release);
        zoomPixelsPerCU.store(2.0f, std::memory_order_release);
    }
};

struct Cad2DPageGPU {
    uint64_t containerMemoryId = 0;

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

struct Cad2DPageSnapshot {
    std::vector<Cad2DPageGPU*> pages;
};

struct DX12Resources2DPerTab {
    ComPtr<ID3D12RootSignature> lineRootSignature;
    ComPtr<ID3D12PipelineState> linePSO;
    ComPtr<ID3D12CommandSignature> lineCommandSignature;

    ComPtr<ID3D12RootSignature> curveRootSignature;
    ComPtr<ID3D12PipelineState> curvePSO;
    ComPtr<ID3D12CommandSignature> curveCommandSignature;

    ComPtr<ID3D12RootSignature> textRootSignature;
    ComPtr<ID3D12PipelineState> textPSO;
    // The view constant buffer is per window (DX12ResourcesPerWindow::cad2dViewConstantBuffer),
    // not here, so two windows showing different Page2Ds of this tab render independent views.
};

struct TabCad2DStorage {
    DX12Resources2DPerTab dx;
    // Pan/zoom is per view: each open Page2D sub-tab slot owns its own view state, so the same
    // tab's two Page2Ds (inline + extracted, or two extracted windows) pan/zoom independently.
    // Indexed by sub-tab slot; input resolves the interacting slot, render/print the shown slot.
    Cad2DViewState views[MV_MAX_SUBTABS];

    // 2D click-selection (CPU hit-testing). Selected object ids; the copy thread reads this while
    // rebuilding pages and stamps kCad2DSelectedFlag into the matching GPU records.
    std::mutex selection2DMutex;
    std::unordered_set<uint64_t> selectedObjectIds;

    std::mutex cpuRecordsMutex;
    std::vector<Cad2DLineRecordCPU> lineRecords;
    std::vector<Cad2DPolylineRecordCPU> polylineRecords;
    std::vector<Cad2DPolygonRecordCPU> polygonRecords;
    std::vector<Cad2DCircleRecordCPU> circleRecords;
    std::vector<Cad2DEllipseRecordCPU> ellipseRecords;
    std::vector<Cad2DArcRecordCPU> arcRecords;
    std::vector<Cad2DTextRecordCPU> textRecords;

    std::atomic<Cad2DPageSnapshot*> activeSnapshot{ nullptr };
    std::vector<std::unique_ptr<Cad2DPageGPU>> activePages;

    struct RetiredSnapshot { Cad2DPageSnapshot* snapshot = nullptr; uint64_t retireFence = 0; };
    struct RetiredPage { std::unique_ptr<Cad2DPageGPU> page; uint64_t retireFence = 0; };
    std::vector<RetiredSnapshot> retiredSnapshots;
    std::vector<RetiredPage> retiredPages;

    std::atomic<uint32_t> demoLineCounter{ 0 };
    std::atomic<bool> demoTextQueued{ false };

    std::atomic<bool> lineCreationMode{ false };
    std::atomic<bool> lineCreationHasPreviousPoint{ false };
    std::atomic<double> lineCreationPreviousXCU{ 0.0 };
    std::atomic<double> lineCreationPreviousYCU{ 0.0 };

    std::atomic<bool> polylineCreationMode{ false };
    uint64_t polylineCreationObjectId = 0;
    std::vector<Cad2DPoint2D> polylineCreationPoints;

    std::atomic<bool> polygonCreationMode{ false };
    std::atomic<bool> polygonCreationHasCenter{ false };
    std::atomic<double> polygonCreationCenterXCU{ 0.0 };
    std::atomic<double> polygonCreationCenterYCU{ 0.0 };

    std::atomic<bool> circleCreationMode{ false };
    std::atomic<bool> circleCreationHasCenter{ false };
    std::atomic<double> circleCreationCenterXCU{ 0.0 };
    std::atomic<double> circleCreationCenterYCU{ 0.0 };

    std::atomic<bool> ellipseCreationMode{ false };
    std::atomic<uint32_t> ellipseCreationStep{ 0 };
    std::atomic<double> ellipseCreationCenterXCU{ 0.0 };
    std::atomic<double> ellipseCreationCenterYCU{ 0.0 };
    std::atomic<double> ellipseCreationRadiusXCU{ 0.0 };

    std::atomic<bool> arcCreationMode{ false };
    std::atomic<uint32_t> arcCreationStep{ 0 };
    std::atomic<double> arcCreationCenterXCU{ 0.0 };
    std::atomic<double> arcCreationCenterYCU{ 0.0 };
    std::atomic<double> arcCreationStartXCU{ 0.0 };
    std::atomic<double> arcCreationStartYCU{ 0.0 };

    std::atomic<bool> textCreationMode{ false };
    std::atomic<bool> textCreationHasAnchor{ false };
    std::atomic<double> textCreationXCU{ 0.0 };
    std::atomic<double> textCreationYCU{ 0.0 };
    uint64_t textCreationObjectId = 0;
    std::string textCreationDraft;

    // Selection transform mode (Cad2DTransformKind). The kind atomic is also read by the render
    // thread to trail the EDIT_* command icon next to the cursor.
    std::atomic<uint32_t> transform2DKind{ 0 };
    std::atomic<uint32_t> transform2DStep{ 0 };
    std::atomic<double> transform2DP1XCU{ 0.0 };
    std::atomic<double> transform2DP1YCU{ 0.0 };
    std::atomic<double> transform2DP2XCU{ 0.0 };
    std::atomic<double> transform2DP2YCU{ 0.0 };
};

void InitCad2DTabResources(TabCad2DStorage& storage);
void CleanupCad2DTabResources(TabCad2DStorage& storage);
void RenderPage2D(ID3D12GraphicsCommandList* commandList, DX12ResourcesPerWindow& winRes,
    TabCad2DStorage& storage, DX12ResourcesUI& uiResources, int monitorId,
    uint64_t activeContainerMemoryId, int viewSlot);
void ProcessCad2DCopyBatch(const std::vector<CommandToCopyThread2D>& batch);
void PruneCad2DRetiredResources(TabCad2DStorage& storage, uint64_t safeRetireFence);
void ReleaseCad2DRetiredResources(TabCad2DStorage& storage);
