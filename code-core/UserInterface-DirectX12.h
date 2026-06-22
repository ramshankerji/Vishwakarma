// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX   //
#include <windows.h>   // MUST be before d3d12.h
#include <d3d12.h>
#include <d3dx12.h>
#include <dxgi1_6.h>
#include <wrl.h>
#include <vector>
#include <array>
#include <unordered_map>
#include <iostream>
#include <atomic>

#include "ConstantsApplication.h"
#include "UserInterface.h" // It also includes "UserInterface-TextTranslations.h"

// Do not #include "विश्वकर्मा.h" otherwise it will lead to circular dependency error. Declare this struct exist.
struct SingleUIWindow; // Add this forward declaration:
struct InternalSubTab;

using Microsoft::WRL::ComPtr;

struct DX12ResourcesUI { // GPU resources
    std::array<ComPtr<ID3D12Resource>, UI_MAX_ATLAS_TEXTURES> uiAtlasTextures; // 1024×1024 or 2048×2048 RGBA (or R8 for alpha-only)
    ComPtr<ID3D12Resource> uiVertexBuffer; // Dynamic upload buffer for vertices
    ComPtr<ID3D12Resource> uiIndexBuffer;  // Dynamic upload buffer for indices

    UINT8* pVertexDataBegin = nullptr; // Mapped pointer for immediate writing
    UINT8* pIndexDataBegin = nullptr;
    UINT8* pOrthoDataBegin = nullptr;

    ComPtr<ID3D12PipelineState> uiPSO;
    ComPtr<ID3D12RootSignature> uiRootSignature;
    ComPtr<ID3D12Resource> uiOrthoConstantBuffer;
    ComPtr<ID3D12DescriptorHeap> srvHeap;
    ComPtr<ID3D12DescriptorHeap> samplerHeap;

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
void InitUIResources(DX12ResourcesUI& uiRes, ID3D12Device* device);
void CleanupUIResources(DX12ResourcesUI& uiRes);

void PushRect(UIDrawContext& ctx, float x, float y, float w, float h, uint32_t color, DX12ResourcesUI& uiRes);
void PushRoundedRectangle(UIDrawContext& ctx, float x, float y, float w, float h, float radiusPx,
    uint32_t color, DX12ResourcesUI& uiRes);
void PushTopRoundedRectangle(UIDrawContext& ctx, float x, float y, float w, float h, float radiusPx,
    uint32_t color, DX12ResourcesUI& uiRes);
void PushText(UIDrawContext& ctx, float x, float y, const char* text, uint32_t color, DX12ResourcesUI& uiRes);

// Slots 0 and 1 are currently reserved for the mandatory English and Icon atlases.
// Future script atlases can use slots [UI_FIRST_DYNAMIC_SCRIPT_ATLAS_SLOT, UI_MAX_ATLAS_TEXTURES).
bool UploadUIAtlasTexture(DX12ResourcesUI& uiRes, ID3D12Device* device, uint32_t atlasSlot,
    const AtlasBitmap& atlas);

void PrecomputeTopRibbonLayout(UITopRibbonLayout& layout, float monitorDPIX, float monitorDPIY);

void RenderUIOverlay(SingleUIWindow& window, ID3D12GraphicsCommandList* cmdList,
    DX12ResourcesUI& uiRes, UITopRibbonLayout& topRibbonLayout,
    float monitorDPIX, float monitorDPIY, const UIInput& input,
    const std::vector<InternalSubTab>& internalSubTabs, uint64_t activeInternalSubTabMemoryId);
