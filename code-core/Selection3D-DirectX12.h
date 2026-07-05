// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
#pragma once

/* 3D click-selection and GPU picking (DirectX12 backend).
Design document: website/content/software/selection.md

Split of responsibilities:
  - SelectionState        : per-tab runtime state shared between the logic thread (writes pick
                            requests, reads results, updates the selection set) and the render
                            thread (services the pick, draws the highlight/overlay). Plain data.
  - Selection3DResources  : per-tab GPU objects (PSOs, overlay cube geometry). Created once from
                            the scene root signature; lives inside DX12ResourcesPerTab.
  - PickPassContext       : render-thread-local scratch (pick render targets + readback buffers +
                            in-flight bookkeeping). One per render thread.

Future Vulkan / Metal backends will mirror Selection3DResources / PickPassContext behind the same
SelectionState. */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <d3dx12.h>
#include <DirectXMath.h>
#include <wrl.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

// Forward declarations (full definitions live in MemoryManagerGPU-DirectX12.h).
struct DX12ResourcesPerTab;
struct DX12ResourcesPerWindow;
struct TabGeometryStorage;

enum class PickPurpose : uint32_t {
    None = 0,
    Select = 1,   // Left click: select object + recenter orbit on its AABB center.
    Recenter = 2  // Mouse wheel: recenter orbit on the surface point under the cursor.
};

// Deep blue selection override, matching ShaderSceneHighlightPixel.hlsl (kept for CPU reference).
constexpr float kSelectionBlue[3] = { 0.05f, 0.15f, 0.65f };

// N x N pixel neighborhood sampled around the cursor (odd, so the cursor is centered).
constexpr int kPickBoxSize = 5;

// Milliseconds the rotation-center cube stays visible after the last navigation interaction.
constexpr uint64_t kNavCubeVisibleMs = 700;

// --- Shared per-tab selection state (no GPU objects) --------------------------------------------
struct SelectionState {
    // Selected object ids. Written by the logic thread, copied by the render thread each frame.
    std::mutex selectedMutex;
    std::vector<uint64_t> selectedObjectIds;

    // Pick request: logic thread -> render thread.
    std::atomic<bool> pickRequested{ false };
    std::atomic<int> pickX{ 0 };               // Raw client-space pixel (full window).
    std::atomic<int> pickY{ 0 };
    std::atomic<uint32_t> pickPurpose{ 0 };     // PickPurpose.

    // Pick result: render thread -> logic thread.
    std::atomic<bool> resultReady{ false };
    std::mutex resultMutex;
    bool resultHit = false;
    uint64_t resultObjectId = 0;
    DirectX::XMFLOAT3 resultCG{ 0.0f, 0.0f, 0.0f };       // World AABB center of the hit object.
    DirectX::XMFLOAT3 resultSurface{ 0.0f, 0.0f, 0.0f };  // World surface point under the cursor.
    uint32_t resultPurpose = 0;

    // Rotation-cube visibility timer (GetTickCount64 milliseconds of last orbit/pan/zoom).
    std::atomic<uint64_t> lastNavInteractionMs{ 0 };
};

// --- Per-tab GPU objects (created once from the scene root signature) ---------------------------
struct Selection3DResources {
    bool initialized = false;

    Microsoft::WRL::ComPtr<ID3D12PipelineState> pickPSO;       // scene root sig, MRT id+depth.
    Microsoft::WRL::ComPtr<ID3D12PipelineState> highlightPSO;  // scene root sig, deep-blue PS.

    // Rotation-center overlay cube (independent minimal pipeline).
    Microsoft::WRL::ComPtr<ID3D12RootSignature> cubeRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> cubePSO;
    Microsoft::WRL::ComPtr<ID3D12Resource> cubeVertexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> cubeIndexBuffer;
    D3D12_VERTEX_BUFFER_VIEW cubeVBV{};
    D3D12_INDEX_BUFFER_VIEW cubeIBV{};
    uint32_t cubeIndexCount = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> cubeConstantBuffer; // Per-frame MVP + color (mapped).
    uint8_t* cubeConstantData = nullptr;
};

// --- Render-thread-local pick scratch -----------------------------------------------------------
struct PickPassContext {
    enum class State { Idle, Recorded, InFlight };
    State state = State::Idle;

    UINT width = 0;   // Cached target size (== scene viewport).
    UINT height = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource> idTexture;      // R32_UINT
    Microsoft::WRL::ComPtr<ID3D12Resource> depthColor;     // R32_FLOAT (NDC depth)
    Microsoft::WRL::ComPtr<ID3D12Resource> depthStencil;   // D32_FLOAT
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap;  // 2 RTVs (id, depthColor)
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsvHeap;  // 1 DSV
    Microsoft::WRL::ComPtr<ID3D12Resource> readbackId;     // kPickBoxSize rows, aligned
    Microsoft::WRL::ComPtr<ID3D12Resource> readbackDepth;
    UINT readbackRowPitch = 0;

    // In-flight bookkeeping (valid while state != Idle).
    uint64_t pendingFence = 0;
    uint32_t purpose = 0;
    int boxX = 0, boxY = 0;   // Top-left of the sampled box, in scene-viewport pixels.
    int boxW = 0, boxH = 0;   // Clamped box size.
    int sceneW = 0, sceneH = 0;
    DirectX::XMMATRIX invViewProj = DirectX::XMMatrixIdentity(); // inverse(view*proj), row-vector
};

// --- API ----------------------------------------------------------------------------------------
void InitSelection3DResources(DX12ResourcesPerTab& tabRes);
void CleanupSelection3DResources(DX12ResourcesPerTab& tabRes);

// Draw selection highlight + rotation-center cube into the currently bound scene render target.
// Must be called after PopulateCommandList (so b0 view-proj constants are already populated) and
// while the scene RTV/DSV + scene viewport are still bound.
void RecordSelectionOverlays(ID3D12GraphicsCommandList* commandList, DX12ResourcesPerWindow& winRes,
    DX12ResourcesPerTab& tabRes, TabGeometryStorage& storage, SelectionState& selection,
    const DirectX::XMMATRIX& viewProj, int topUIHeightPx, int sceneWidth, int sceneHeight,
    uint64_t activeContainerMemoryId);

// Service a pending pick request (records an id+depth pass + readback) and publish any completed
// pick result back to SelectionState. Uses winRes.constantBuffer for the b0 view-proj constants.
void ServicePick(ID3D12GraphicsCommandList* commandList, DX12ResourcesPerWindow& winRes,
    DX12ResourcesPerTab& tabRes, TabGeometryStorage& storage, SelectionState& selection,
    PickPassContext& ctx, int monitorId, const DirectX::XMMATRIX& viewProj,
    int topUIHeightPx, int sceneWidth, int sceneHeight, uint64_t activeContainerMemoryId);

// Called after the frame's fence is signaled: promotes a just-recorded pick to in-flight.
void FinalizePickFence(PickPassContext& ctx, uint64_t signaledFenceValue);

void CleanupPickPassContext(PickPassContext& ctx);
