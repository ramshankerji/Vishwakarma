// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.

#pragma once

//DirectX 12 headers. Best Place to learn DirectX12 is original Microsoft documentation.
// https://learn.microsoft.com/en-us/windows/win32/direct3d12/direct3d-12-graphics
// You need a good dose of prior C++ knowledge and Computer Fundamentals before learning DirectX12.
// Expect to read at least 2 times before you start grasping it !

//Tell the HLSL compiler to include debug information into the shader blob.
#define D3DCOMPILE_DEBUG 1 //TODO: Remove from production build.
#include <d3d12.h> //Main DirectX12 API. Included from %WindowsSdkDir\Include%WindowsSDKVersion%\\um
//helper structures Library. MIT Licensed. Added to the project as git submodule.
//https://github.com/microsoft/DirectX-Headers/blob/main/include/directx/d3dx12.h
#include <d3dx12.h>
#include <dxgi1_6.h>
#include <wrl.h>
#include <d3dcompiler.h>
#include <DirectXMath.h> //Where from? https://github.com/Microsoft/DirectXMath ?
#include <vector>
#include <string>

using namespace Microsoft::WRL;
using namespace DirectX;

//DirectX12 Libraries.
#pragma comment(lib, "d3d12.lib") //%WindowsSdkDir\Lib%WindowsSDKVersion%\\um\arch
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxguid.lib")


// Struct for vertex data
struct Vertex {
    XMFLOAT3 position;
    XMFLOAT4 color;
};
/* Double buffering is preferred for CAD application due to low input lag.Caveat: If rendering time
exceeds frame refresh interval, than strutting distortion will appear. However
we low input latency outweighs the slight frame smoothness of triple buffering.
Double buffering (2x) is also 50% more memory efficient Triple Buffering (3x). */
const UINT FrameCount = 2; //Initially we are going with double buffering.

// Constants
const UINT MaxPyramids = 100; // MODIFICATION: Define a max pyramid count for pre-allocation.
const UINT MaxVertexCount = MaxPyramids * 4;
const UINT MaxIndexCount = MaxPyramids * 12;
const UINT MaxVertexBufferSize = MaxVertexCount * sizeof(Vertex);
const UINT MaxIndexBufferSize = MaxIndexCount * sizeof(UINT16);

//extern ComPtr<ID3D12Device> device;

struct OneMonitorController {
    // System Fetched information.
    bool isScreenInitalized = false;
    int screenPixelWidth = 800;
    int screenPixelHeight = 600;
    int screenPhysicalWidth = 0; // in mm
    int screenPhysicalHeight = 0; // in mm
    int WindowWidth = 800;//Current ViewPort ( Rendering area ) size. excluding task-bar etc.
    int WindowHeight = 600;

    HMONITOR hMonitor = NULL;                   // Monitor handle
    std::wstring deviceName;                    // Monitor device name (e.g., "\\\\.\\DISPLAY1")
    std::wstring friendlyName;                  // Human readable name (e.g., "Dell U2720Q")
    RECT monitorRect;                           // Full monitor rectangle
    RECT workAreaRect;                          // Work area (excluding task bar)
    int dpiX = 96;                              // DPI X
    int dpiY = 96;                              // DPI Y
    double scaleFactor = 1.0;                   // Scale factor (100% = 1.0, 125% = 1.25, etc.)
    bool isPrimary = false;                     // Is this the primary monitor?
    DWORD orientation = DMDO_DEFAULT;           // Monitor orientation
    int refreshRate = 60;                       // Refresh rate in Hz
    int colorDepth = 32;                        // Color depth in bits per pixel

    // D3D12 objects
    ComPtr<IDXGISwapChain3> swapChain;
    ComPtr<ID3D12CommandQueue> commandQueue;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    ComPtr<ID3D12Resource> renderTargets[FrameCount];
    ComPtr<ID3D12CommandAllocator> commandAllocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ComPtr<ID3D12Fence> fence;
    UINT rtvDescriptorSize;
    UINT frameIndex;
    HANDLE fenceEvent;
    UINT64 fenceValue = 0;

    // Pipeline objects
    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> pipelineState;
    ComPtr<ID3D12Resource> vertexBuffer; //TODO: Move from per monitor to per GPU for cross-monitor data sharing.
    ComPtr<ID3D12Resource> indexBuffer;
    D3D12_VERTEX_BUFFER_VIEW vertexBufferView;
    D3D12_INDEX_BUFFER_VIEW indexBufferView;

    ComPtr<ID3D12Resource> vertexBufferUpload;
    ComPtr<ID3D12Resource> indexBufferUpload;
    UINT8* pVertexDataBegin = nullptr; // MODIFICATION: Pointer for mapped vertex upload buffer
    UINT8* pIndexDataBegin = nullptr;  // MODIFICATION: Pointer for mapped index upload buffer

    ComPtr<ID3D12Resource> depthStencilBuffer;
    ComPtr<ID3D12DescriptorHeap> dsvHeap;
    ComPtr<ID3D12Resource> constantBuffer;
    ComPtr<ID3D12DescriptorHeap> cbvHeap;

    UINT8* cbvDataBegin;
};

// --- VRAM Manager ---
// This class handles the GPU memory dynamically.
// There will be exactly 1 object of this class in entire application. Hence the special name.
// भगवान शंकर की कृपा बानी रहे. Corresponding object is named "gpuRAMManager".
class शंकर{
public:
    UINT8* pVertexDataBegin = nullptr; // MODIFICATION: Pointer for mapped vertex upload buffer
    UINT8* pIndexDataBegin = nullptr;  // MODIFICATION: Pointer for mapped index upload buffer

    // Maps our CPU ObjectID to its resource info in VRAM
    std::unordered_map<uint64_t, GpuResourceInfo> resourceMap;

    // Simulates a simple heap allocator with 16MB chunks
    uint64_t m_nextFreeOffset = 0;
    const uint64_t CHUNK_SIZE = 16 * 1024 * 1024;
    uint64_t m_vram_capacity = 4 * CHUNK_SIZE; // Simulate 64MB VRAM

    // When an object is updated, the old VRAM is put here to be freed later.
    struct DeferredFree {
        uint64_t frameNumber; // The frame it became obsolete
        GpuResourceInfo resource;
    };
    std::list<DeferredFree> deferredFreeQueue;

    // Allocate space in VRAM. Returns the handle.
    std::optional<GpuResourceInfo> Allocate(size_t size);

    void ProcessDeferredFrees(uint64_t lastCompletedRenderFrame);
    
};

// 4 is the maximum number of simultaneous screen we are ever going to support. DO NOT CHANGE EVER.
extern int g_monitorCount;             // Global variable
extern OneMonitorController screen[4]; 

void FetchAllMonitorDetails();
BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData);

void InitD3D(HWND hwnd);
void PopulateCommandList();
void WaitForPreviousFrame();
void CleanupD3D();