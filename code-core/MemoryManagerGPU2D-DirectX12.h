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
#include <vector>

#include "MemoryManagerGPU2D.h"

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
};

struct Cad2DPageGPU {
    uint64_t containerMemoryId = 0;

    ComPtr<ID3D12Resource> lineBuffer;
    ComPtr<ID3D12Resource> lineIndirectBuffer;
    uint32_t lineCount = 0;

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

    ComPtr<ID3D12RootSignature> textRootSignature;
    ComPtr<ID3D12PipelineState> textPSO;

    ComPtr<ID3D12Resource> viewConstantBuffer;
    uint8_t* pViewConstantDataBegin = nullptr;
};

struct TabCad2DStorage {
    DX12Resources2DPerTab dx;
    Cad2DViewState view;

    std::mutex cpuRecordsMutex;
    std::vector<Cad2DLineRecordCPU> lineRecords;
    std::vector<Cad2DPolylineRecordCPU> polylineRecords;
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
};

void InitCad2DTabResources(TabCad2DStorage& storage);
void CleanupCad2DTabResources(TabCad2DStorage& storage);
void RenderCad2DPage(ID3D12GraphicsCommandList* commandList, DX12ResourcesPerWindow& winRes,
    TabCad2DStorage& storage, DX12ResourcesUI& uiResources, int monitorId,
    uint64_t activeContainerMemoryId);
void ProcessCad2DCopyBatch(const std::vector<CommandToCopyThread2D>& batch);
void PruneCad2DRetiredResources(TabCad2DStorage& storage, uint64_t safeRetireFence);
void ReleaseCad2DRetiredResources(TabCad2DStorage& storage);
