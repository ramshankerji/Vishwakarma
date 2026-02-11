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
#include <dxgidebug.h>
#include <wrl.h>
#include <d3dcompiler.h>
#include <DirectXMath.h> //Where from? https://github.com/Microsoft/DirectXMath ?
#include <vector>
#include <string>
#include <unordered_map>
#include <random>
#include <ctime>
#include <iostream>
#include <thread>
#include <chrono>
#include <map>
#include <list>

#include "ConstantsApplication.h"
#include "MemoryManagerGPU.h"
#include "डेटा.h"

using namespace Microsoft::WRL;

//DirectX12 Libraries.
#pragma comment(lib, "d3d12.lib") //%WindowsSdkDir\Lib%WindowsSDKVersion%\\um\arch
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxguid.lib")

/* Double buffering is preferred for CAD application due to low input lag.Caveat: If rendering time
exceeds frame refresh interval, than strutting distortion will appear. However
we low input latency outweighs the slight frame smoothness of triple buffering.
Double buffering (2x) is also 50% more memory efficient Triple Buffering (3x). */
const UINT FRAMES_PER_RENDERTARGETS = 2; //Initially we are going with double buffering.

// Constants
const UINT MaxPyramids = 100; // MODIFICATION: Define a max pyramid count for pre-allocation.
const UINT MaxVertexCount = MaxPyramids * 4;
const UINT MaxIndexCount = MaxPyramids * 12;
const UINT MaxVertexBufferSize = MaxVertexCount * sizeof(Vertex);
const UINT MaxIndexBufferSize = MaxIndexCount * sizeof(UINT16);

/* DirectX 12 resources are organized at 3 levels:
1. The Data   : Per Tab (Jumbo Buffers for geometry data, materials, textures, etc.)
2. The Target : Per Window (Swap Chain, Render Targets, Depth Stencil Buffer etc.)
3. The Worker : Per Render Thread. 1 For each monitor. (Command Queue, Command List etc.
    Resources shared across multiple windows on the same monitor) */

struct DX12ResourcesPerTab { // (The Data) Geometry Data
    // Since data is isolated per tab, these live here. We use a "Jumbo" buffer approach to reduce switching.
    ComPtr<ID3D12Resource> vertexBuffer;
    ComPtr<ID3D12Resource> indexBuffer;

    // Upload Heaps (CPU -> GPU Transfer)
    // Moved here because the Copy Thread writes to these when adding objects to the TAB.
    ComPtr<ID3D12Resource> vertexBufferUpload;
    ComPtr<ID3D12Resource> indexBufferUpload;

    // Persistent Mapped Pointers (CPU Address)
    UINT8* pVertexDataBegin = nullptr;// Pointer for mapped vertex upload buffer
    UINT8* pIndexDataBegin = nullptr;  // Pointer for mapped index upload buffer

    // Views into the buffers (to be bound during Draw)
    D3D12_VERTEX_BUFFER_VIEW vertexBufferView;
    D3D12_INDEX_BUFFER_VIEW indexBufferView;

	// TODO: We will generalize this to hold materials, shaders, textures etc. unique to this project/tab
    ComPtr<ID3D12DescriptorHeap> srvHeap;

    // Track how much of the jumbo buffer is used
    uint64_t vertexDataSize = 0;
    uint64_t indexDataSize = 0;

	CameraState camera; //Reference is updated per frame.
};

struct DX12ResourcesPerWindow {// Presentation Logic
    int WindowWidth = 800;//Current ViewPort ( Rendering area ) size. excluding task-bar etc.
    int WindowHeight = 600;
    ID3D12CommandQueue* creatorQueue = nullptr; // Track which queue this windows was created with. To assist with migrations.
    
    ComPtr<IDXGISwapChain3>         swapChain; // The link to the OS Window
	//ComPtr<ID3D12CommandQueue>    commandQueue; // Moved to OneMonitorController
    ComPtr<ID3D12DescriptorHeap>    rtvHeap;
    ComPtr<ID3D12Resource>          renderTargets[FRAMES_PER_RENDERTARGETS];
    UINT rtvDescriptorSize = 0;

    ComPtr<ID3D12RootSignature>     rootSignature;
    ComPtr<ID3D12PipelineState> pipelineState;

    ComPtr<ID3D12Resource> depthStencilBuffer;// Depth Buffer (Sized to the window dimensions)
    ComPtr<ID3D12DescriptorHeap> dsvHeap;

    D3D12_VIEWPORT viewport;// Viewport & Scissor (Dependent on Window Size). Not used yet.
    D3D12_RECT scissorRect;

    ComPtr<ID3D12Resource> constantBuffer;
    ComPtr<ID3D12DescriptorHeap> cbvHeap;
    UINT8* cbvDataBegin = nullptr;

	UINT frameIndex = 0; // Remember this is different from allocatorIndex in Render Thread. It can change even during windows resize.
};

struct DX12ResourcesPerRenderThread { // This one is created 1 for each monitor.
    // For convenience only. It simply points to OneMonitorController.commandQueue
	ComPtr<ID3D12CommandQueue> commandQueue;

    // Note that there are as many render thread as number of monitors attached.
    // Command Allocators MUST be unique to the thread.
    // We need one per frame-in-flight to avoid resetting while GPU is reading.
    ComPtr<ID3D12CommandAllocator> commandAllocators[FRAMES_PER_RENDERTARGETS];
	UINT allocatorIndex = 0; // Remember this is different from frameIndex available per Window.

    // The Command List (The recording pen). Can be reset and reused for multiple windows within the same frame.
    ComPtr<ID3D12GraphicsCommandList> commandList;

    // Synchronization (Per Window VSync)
    HANDLE fenceEvent = nullptr;
    ComPtr<ID3D12Fence> fence;
    UINT64 fenceValue = 0;
};

struct OneMonitorController { // Variables stored per monitor.
    // System Fetched information.
    bool isScreenInitalized = false;
    int screenPixelWidth = 800;
    int screenPixelHeight = 600;
    int screenPhysicalWidth = 0; // in mm
    int screenPhysicalHeight = 0; // in mm
    int WindowWidth = 800;//Current ViewPort ( Rendering area ) size. excluding task-bar etc.
    int WindowHeight = 600;

    HMONITOR hMonitor = NULL; // Monitor handle. Remains fixed as long as monitor is not disconnected / disabled.
    std::wstring monitorName;            // Monitor device name (e.g., "\\\\.\\DISPLAY1")
    std::wstring friendlyName;           // Human readable name (e.g., "Dell U2720Q")
    RECT monitorRect;                    // Full monitor rectangle
    RECT workAreaRect;                   // Work area (excluding task bar)
    int dpiX = 96;                       // DPI X
    int dpiY = 96;                       // DPI Y
    double scaleFactor = 1.0;            // Scale factor (100% = 1.0, 125% = 1.25, etc.)
    bool isPrimary = false;              // Is this the primary monitor?
    DWORD orientation = DMDO_DEFAULT;    // Monitor orientation
    int refreshRate = 60;                // Refresh rate in Hz
    int colorDepth = 32;                 // Color depth in bits per pixel

    bool isVirtualMonitor = false;       // To support headless mode.

    // DirectX12 Resources.
	ComPtr<ID3D12CommandQueue> commandQueue;    // Persistent. Survives thread restarts.
    bool hasActiveThread = false;// We need to know if this specific monitor is currently being serviced by a thread
};

// Commands sent from Generator thread(s) to the Copy thread
enum class CommandToCopyThreadType { ADD, MODIFY, REMOVE };
struct CommandToCopyThread
{
    CommandToCopyThreadType type;
    std::optional<GeometryData> geometry; // Present for ADD and MODIFY
    uint64_t id; // Always present
};
// Thread synchronization between Main Logic thread and Copy thread
extern std::mutex toCopyThreadMutex;
extern std::condition_variable toCopyThreadCV;
extern std::queue<CommandToCopyThread> commandToCopyThreadQueue;

extern std::atomic<bool> pauseRenderThreads; // Defined in Main.cpp
// Represents complete geometry and index data associated with 1 engineering object..
// This structure holds information about a resource allocated in GPU memory (VRAM)
struct GpuResourceVertexIndexInfo {
    ComPtr<ID3D12Resource> vertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW vertexBufferView;
    ComPtr<ID3D12Resource> indexBuffer;
    D3D12_INDEX_BUFFER_VIEW indexBufferView;
    UINT indexCount;

    //TODO: Latter on we will generalize this structure to hold textures, materials, shaders etc.
    // Currently we are letting the Drive manage the GPU memory fragmentation. Latter we will manage it ourselves.
    //uint64_t vramOffset; // Simulated VRAM address
    //uint64_t size;
    // In a real DX12 app, this would hold ID3D12Resource*, D3D12_VERTEX_BUFFER_VIEW, etc.
};

extern std::mutex objectsOnGPUMutex;
// Copy thread will update the following map whenever it adds/removes/modifies an object on GPU.
extern std::map<uint64_t, GpuResourceVertexIndexInfo> objectsOnGPU;

// Packet of work for a Render Thread for one frame
struct RenderPacket {
    uint64_t frameNumber;
    std::vector<uint64_t> visibleObjectIds;
};

class HrException : public std::runtime_error// Simple exception helper for HRESULT checks
{
public:
    HrException(HRESULT hr) : std::runtime_error("HRESULT Exception"), hr(hr) {}
    HRESULT Error() const { return hr; }
private:
    const HRESULT hr;
};

inline void ThrowIfFailed(HRESULT hr)
{
    if (FAILED(hr)) { throw HrException(hr); }
}


class ThreadSafeQueueGPU {
public:
    void push(CommandToCopyThread value) {
        std::lock_guard<std::mutex> lock(mutex);
        fifoQueue.push(std::move(value));
        cond.notify_one();
    }

    // Non-blocking pop
    bool try_pop(CommandToCopyThread& value) {
        std::lock_guard<std::mutex> lock(mutex);
        if (fifoQueue.empty()) {
            return false;
        }
        value = std::move(fifoQueue.front());
        fifoQueue.pop();
        return true;
    }

    // Shuts down the queue, waking up any waiting threads
    void shutdownQueue() {
        std::lock_guard<std::mutex> lock(mutex);
        shutdown = true;
        cond.notify_all();
    }

private:
    std::queue<CommandToCopyThread> fifoQueue; // fifo = First-In First-Out
    std::mutex mutex;
    std::condition_variable cond;
    bool shutdown = false;
};

inline ThreadSafeQueueGPU g_gpuCommandQueue;

// VRAM Manager : This class handles the GPU memory dynamically.
// There will be exactly 1 object of this class in entire application. Hence the special name.
// भगवान शंकर की कृपा बनी रहे. Corresponding object is named "gpu".
class शंकर {
public:
    //std::vector<OneMonitorController> screens;
    OneMonitorController screens[MV_MAX_MONITORS];
    int currentMonitorCount = 0; // Global monitor count. It can be 0 when no monitors are found (headless mode)

    ComPtr<IDXGIFactory4> factory; //The OS-level display system manager. Can iterate over GPUs.
    //ComPtr<IDXGIFactory6> dxgiFactory; 
    ComPtr<IDXGIAdapter1> hardwareAdapter;// Represents a physical GPU device.
    //Represents 1 logical GPU device on above GPU adapter. Helps create all DirectX12 memory / resources / comments etc.

	ComPtr<ID3D12Device> device; //Very Important: We support EXACTLY 1 GPU device only in this version.
    bool isGPUEngineInitialized = false; //TODO: To be implemented.
    
    //Following to be added latter.
    //ID3D12DescriptorHeapMgr    ← Global descriptor allocator
    //Shader& PSO Cache         ← Shared by all threads
    //AdapterInfo                ← For device selection / VRAM stats

    /* We will have 1 Render Queue per monitor, which is local to Render Thread.
    IMPORTANT: All GPU have only 1 physical hardware engine, and can execute 1 command at a time only.
    Even if 4 commands list are submitted to 4 independent queue, graphics driver / WDDM serializes them.
    Still we need to have 4 separate queue to properly handle different refresh rate.

    Ex: If we put all 4 window on same queue: Window A (60Hz) submits a Present command. The Queue STALLS
    waiting for Monitor A's VSync interval. Window B (144Hz) submits draw comand. 
    Window B cannot be processed because the Queue is blocked by Windows A's VSync wait. 
    By using 4 Queues, Queue A can sit blocked waiting for VSync, 
    while Queue B immediately push work work to the GPU for the faster monitor.*/

    ComPtr<ID3D12CommandQueue> renderCommandQueue; // Only used by Monitor No. 0 i.e. 1st Render Thread.
    ComPtr<ID3D12Fence> renderFence;// Synchronization for Render Queue
    UINT64 renderFenceValue = 0;
    HANDLE renderFenceEvent = nullptr;

	ComPtr<ID3D12CommandQueue> copyCommandQueue; // There is only 1 across the application.
    ComPtr<ID3D12Fence> copyFence;// Synchronization for Copy Queue
    UINT64 copyFenceValue = 0;
    HANDLE copyFenceEvent = nullptr;

public:
    UINT8* pVertexDataBegin = nullptr; // MODIFICATION: Pointer for mapped vertex upload buffer
    UINT8* pIndexDataBegin = nullptr;  // MODIFICATION: Pointer for mapped index upload buffer

    // Maps our CPU ObjectID to its resource info in VRAM
    std::unordered_map<uint64_t, GpuResourceVertexIndexInfo> resourceMap;

    // Simulates a simple heap allocator with 16MB chunks
    uint64_t m_nextFreeOffset = 0;
    const uint64_t CHUNK_SIZE = 16 * 1024 * 1024;
    uint64_t m_vram_capacity = 4 * CHUNK_SIZE; // Simulate 64MB VRAM

    // When an object is updated, the old VRAM is put here to be freed later.
    struct DeferredFree {
        uint64_t frameNumber; // The frame it became obsolete
        GpuResourceVertexIndexInfo resource;
    };
    std::list<DeferredFree> deferredFreeQueue;

	// Allocate space in VRAM. Returns the handle. What is this used for?
    // std::optional<GpuResourceVertexIndexInfo> Allocate(size_t size);

    void ProcessDeferredFrees(uint64_t lastCompletedRenderFrame);

	शंकर() {}; // Our Main function inilsizes DirectX12 global resources by calling InitD3DDeviceOnly().
    void InitD3DDeviceOnly();
    void InitD3DPerTab(DX12ResourcesPerTab& tabRes); // Call this when a new Tab is created
    void InitD3DPerWindow(DX12ResourcesPerWindow& dx, HWND hwnd, ID3D12CommandQueue* commandQueue);
    void PopulateCommandList(ID3D12GraphicsCommandList* cmdList, //Called by per monitor render thead.
        DX12ResourcesPerWindow& winRes, const DX12ResourcesPerTab& tabRes);
    void WaitForPreviousFrame(DX12ResourcesPerRenderThread dx);

    // Called when a monitor is unplugged or window is destroyed. Destroys SwapChain/RTVs but KEEPS Geometry.
    void CleanupWindowResources(DX12ResourcesPerWindow& winRes);
    // Called when a TAB is closed by the user. Destroys the Jumbo Vertex/Index Buffers.
    void CleanupTabResources(DX12ResourcesPerTab& tabRes);
    // Called ONLY at application exit (wWinMain end).Destroys the Device, Factory, and Global Copy Queue.
	// Thread resources are cleaned up by the Render Thread itself before exit.
    void CleanupD3DGlobal();
};

void FetchAllMonitorDetails();
BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData);

/*
IID_PPV_ARGS is a MACRO used in DirectX (and COM programming in general) to help safely and correctly
retrieve interface pointers during object creation or querying. It helps reduce repetitive typing of codes.
COM interfaces are identified by unique GUIDs. Than GUID pointer is converted to appropriate pointer type.

Ex: IID_PPV_ARGS(&device) expands to following:
IID iid = __uuidof(ID3D12Device);
void** ppv = reinterpret_cast<void**>(&device);
*/

// Structure to hold transformation matrices
struct ConstantBuffer {
    DirectX::XMFLOAT4X4 worldViewProjection;
    DirectX::XMFLOAT4X4 world;
};

// Externs for communication 
extern std::atomic<bool> shutdownSignal;
extern ThreadSafeQueueGPU g_gpuCommandQueue;

// Logic Thread "Fence"
extern std::mutex g_logicFenceMutex;
extern std::condition_variable g_logicFenceCV;
extern uint64_t g_logicFrameCount;

// Copy Thread "Fence"
extern std::mutex g_copyFenceMutex;
extern std::condition_variable g_copyFenceCV;
extern uint64_t g_copyFrameCount;

//TODO: Implement this. In a real allocator, we would manage free lists and possibly defragment memory.
/*
std::optional<GpuResourceVertexIndexInfo> शंकर::Allocate(size_t size) {

    if (nextFreeOffset + size > m_vram_capacity) {
        std::cerr << "VRAM MANAGER: Out of memory!" << std::endl;
        // Here, the Main Logic thread would be signaled to reduce LOD.
        return std::nullopt;
    }
    GpuResourceVertexIndexInfo info{ nextFreeOffset, size };
    nextFreeOffset += size; // Simple bump allocator
    return info;
}*/

// =================================================================================================
// Utility Functions
// =================================================================================================

// Waits for the previous frame to complete rendering.
inline void WaitForGpu(DX12ResourcesPerWindow dx)
{   //Where are we using this function?
    /*
    dx.commandQueue->Signal(dx.fence.Get(), dx.fenceValue);
    dx.fence->SetEventOnCompletion(dx.fenceValue, dx.fenceEvent);
    WaitForSingleObjectEx(dx.fenceEvent, INFINITE, FALSE);
    dx.fenceValue++;*/
}

// Waits for a specific fence value to be reached
inline void WaitForFenceValue(DX12ResourcesPerWindow dx, UINT64 fenceValue)
{ // Where are we using this?
    /*
    if (dx.fence->GetCompletedValue() < fenceValue)
    {
        ThrowIfFailed(dx.fence->SetEventOnCompletion(fenceValue, dx.fenceEvent));
        WaitForSingleObjectEx(dx.fenceEvent, INFINITE, FALSE);
    }*/
}

// =================================================================================================
// Thread Functions
// =================================================================================================
// Thread synchronization between Main Logic thread and Copy thread
inline std::mutex toCopyThreadMutex;
inline std::condition_variable toCopyThreadCV;
inline std::queue<CommandToCopyThread> commandToCopyThreadQueue;
inline  std::mutex objectsOnGPUMutex;
// Copy thread will update the following map whenever it adds/removes/modifies an object on GPU.
inline  std::map<uint64_t, GpuResourceVertexIndexInfo> objectsOnGPU;

// Thread Functions - Just Declaration!
void GpuCopyThread();
void GpuRenderThread(int monitorId, int refreshRate);
