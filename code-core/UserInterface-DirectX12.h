// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>   // MUST be before d3d12.h
#include <d3d12.h>
#include <d3dx12.h>
#include <dxgi1_6.h>
#include <wrl.h>
#include <vector>
#include <unordered_map>
#include <iostream>
#include <atomic>

#include "ConstantsApplication.h"
#include "UserInterface.h" // It also includes "UserInterface-TextTranslations.h"

// Do not #include "विश्वकर्मा.h" otherwise it will lead to circular dependency error. Declare this struct exist.
struct SingleUIWindow; // Add this forward declaration:

using Microsoft::WRL::ComPtr;

struct DX12ResourcesUI { // GPU resources
    ComPtr<ID3D12Resource> uiAtlasTexture; // 1024×1024 or 2048×2048 RGBA (or R8 for alpha-only)
    ComPtr<ID3D12Resource> uiVertexBuffer; // Dynamic upload buffer for vertices
    ComPtr<ID3D12Resource> uiIndexBuffer;  // Dynamic upload buffer for indices

    UINT8* pVertexDataBegin = nullptr; // Mapped pointer for immediate writing
    UINT8* pIndexDataBegin = nullptr;
    UINT8* pOrthoDataBegin = nullptr;

    ID3D12Resource* iconAtlas; // Required ?
    ComPtr<ID3D12PipelineState> uiPSO;
    ComPtr<ID3D12RootSignature> uiRootSignature;
    ComPtr<ID3D12Resource> uiOrthoConstantBuffer;

    uint32_t maxVertices = 65536;
    uint32_t maxIndices = 65536 * 3;
};

struct UIDrawContext { // Draw context
    UIVertex* vertexPtr;
    uint16_t* indexPtr;
    uint32_t vertexCount, indexCount;
};

// DirectX12 Immediate Mode UI System (Phase 4A). Tab Bar Rendering Only
// External interfaces of User Interface sub module of the code.
void InitUIResources( DX12ResourcesUI& uiRes, ID3D12Device* device);
void CleanupUIResources( DX12ResourcesUI& uiRes);

void PushRect( UIDrawContext& ctx, float x, float y, float w, float h, uint32_t color, DX12ResourcesUI& uiRes);
void RenderUIOverlay( SingleUIWindow& window, ID3D12GraphicsCommandList* cmdList,
    DX12ResourcesUI& uiRes, float monitorDPI, const UIInput& input);
