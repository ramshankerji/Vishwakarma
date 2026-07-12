// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.

#include "MemoryManagerGPU-DirectX12.h"
#include "RenderPage2D-DirectX12.h"
#include "UserInterface-DirectX12.h"
#include "RenderScene3D.h"
#include "ShaderSceneVertex.h"
#include "ShaderScenePixel.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include "विश्वकर्मा.h"
#include <iomanip>
#include <unordered_set>
#include <colors.h>

// Global Variables declared in विश्वकर्मा.cpp
extern शंकर gpu;
UploadQueue gUploadQueue;
extern std::atomic<uint64_t> atlasFence;

void शंकर::InitD3DDeviceOnly() {
    UINT dxgiFactoryFlags = 0;
#if defined(_DEBUG)
    // Enable debug layer in debug mode
    ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
        debugController->EnableDebugLayer();
        dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
    }
#endif
    
    //Modern Adapter Selection. Prerequisite : Windows 10 1803+ / Windows 11
    ThrowIfFailed(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&factory6)));// Create DXGI factory
    ComPtr<IDXGIAdapter4> hardwareAdapter;
    SIZE_T maxDedicatedVideoMemory = 0, maxSharedMemory = 0;
    bool foundHardware = false;

    for (UINT adapterIndex = 0;; ++adapterIndex) {
        ComPtr<IDXGIAdapter4> adapter;
        // Prefer high performance GPUs (Discrete over Integrated)
        if (FAILED(factory6->EnumAdapterByGpuPreference(adapterIndex,
            DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)))) { break; }// No more adapters
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue; // Skip software adapters (WARP)
		// Check if adapter supports D3D12. Continue if Not D3D12 capable.
        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), nullptr))) continue;
        foundHardware = true;
        if (desc.DedicatedVideoMemory > maxDedicatedVideoMemory) { // Pick adapter with highest Dedicated VRAM
            maxDedicatedVideoMemory = desc.DedicatedVideoMemory;
            maxSharedMemory = desc.SharedSystemMemory;
            hardwareAdapter = adapter;
        }
        else if (maxDedicatedVideoMemory == 0 && desc.SharedSystemMemory > maxSharedMemory) {
            maxSharedMemory = desc.SharedSystemMemory; // UMA system case (all dedicated memory = 0)
            hardwareAdapter = adapter;
        }
    }

    if (hardwareAdapter) {
        DXGI_ADAPTER_DESC1 desc;
        hardwareAdapter->GetDesc1(&desc);
        std::wcout << L"Selected GPU: " << desc.Description << std::endl;
        std::wcout << L"Dedicated VRAM: " << (desc.DedicatedVideoMemory / (1024 * 1024)) << L" MB" << std::endl;
        std::wcout << L"Shared Memory:  " << (desc.SharedSystemMemory / (1024 * 1024))   << L" MB" << std::endl;
        ThrowIfFailed(D3D12CreateDevice(hardwareAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
    }
    else {
        std::wcout << L"No suitable hardware adapter found. Falling back to WARP." << std::endl;
        ComPtr<IDXGIAdapter> warpAdapter;
        ThrowIfFailed(factory6->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter)));
        ThrowIfFailed(D3D12CreateDevice(warpAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
    }

    // TODO: Upgrade for detection of Highest Feature level supported by the adapter in future.
    // D3D_FEATURE_LEVEL_12_0 should be available on all Windows 10+ GPUs.
    // We will require 12_0 as minimum in future when we use more advanced features.
    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_0 };

    gpu.rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    gpu.cbvSrvUavDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    //Copy thread is global. Hence it's variables are initialized here.
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY; // Distinct Copy Queue!
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&copyCommandQueue));
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&copyFence));
    copyFenceValue = 1;
    copyFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    rttFormat = DXGI_FORMAT_R8G8B8A8_UNORM; //Initially. Latter upgrade during HDR implementation.
    //When implementing HDR, check if hardware support this.
}

// Implementation
void शंकर::InitD3DPerTab(DX12ResourcesPerTab& tabRes) {

    // Map Persistent Pointers (Optimization). We map once and keep it mapped for the lifetime of the Tab.
    auto uploadHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    // Create Vertex Buffers (Jumbo)
    auto vbResourceDesc = CD3DX12_RESOURCE_DESC::Buffer(MaxVertexBufferSize);

    // Upload Buffer (CPU Shared). Create an upload heap for the vertex buffer.
    ThrowIfFailed(gpu.device->CreateCommittedResource(
        &uploadHeapProps, D3D12_HEAP_FLAG_NONE, &vbResourceDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&tabRes.vertexBufferUpload)));
        
    // Create Index Buffers (Jumbo). Create Index Buffer Resources (Pre-allocation)
    auto ibResourceDesc = CD3DX12_RESOURCE_DESC::Buffer(MaxIndexBufferSize);
    ThrowIfFailed(gpu.device->CreateCommittedResource(// Create an upload heap for the index buffer.
        &uploadHeapProps, D3D12_HEAP_FLAG_NONE, &ibResourceDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&tabRes.indexBufferUpload)));
    // Persistently map the upload buffers. We won't unmap them until cleanup.
    CD3DX12_RANGE readRange(0, 0); // CPU won't read from these
    ThrowIfFailed(tabRes.vertexBufferUpload->Map(0, &readRange, reinterpret_cast<void**>(&tabRes.pVertexDataBegin)));
    ThrowIfFailed(tabRes.indexBufferUpload->Map(0, &readRange, reinterpret_cast<void**>(&tabRes.pIndexDataBegin)));

    // World Matrix structured buffer. (UPLOAD heap, persistently mapped).
    auto matrixDesc = CD3DX12_RESOURCE_DESC::Buffer(tabRes.matrixCapacity * sizeof(DirectX::XMFLOAT4X4));
    auto uploadProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    ThrowIfFailed(gpu.device->CreateCommittedResource( &uploadProps, D3D12_HEAP_FLAG_NONE, &matrixDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&tabRes.worldMatrixBuffer)));

    ThrowIfFailed(tabRes.worldMatrixBuffer->Map(0, &readRange,
        reinterpret_cast<void**>(&tabRes.pWorldMatrixDataBegin)));

    // SRV on per-tab shader-visible heap (already declared in struct)
    D3D12_DESCRIPTOR_HEAP_DESC srvDesc = {};
    srvDesc.NumDescriptors = 1;
    srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(gpu.device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&tabRes.srvHeap)));

    D3D12_SHADER_RESOURCE_VIEW_DESC srvView = {};
    srvView.Format = DXGI_FORMAT_UNKNOWN;
    srvView.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvView.Buffer.FirstElement = 0;
    srvView.Buffer.NumElements = tabRes.matrixCapacity;
    srvView.Buffer.StructureByteStride = sizeof(DirectX::XMFLOAT4X4);
    gpu.device->CreateShaderResourceView(tabRes.worldMatrixBuffer.Get(), &srvView,
        tabRes.srvHeap->GetCPUDescriptorHandleForHeapStart());

    tabRes.matrixCount = 0;
    tabRes.freeMatrixSlots.clear();

    // Create root signature with constant buffer
    D3D12_FEATURE_DATA_ROOT_SIGNATURE featureData = {};
    featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
    //Check if the GPU supports Root Signature version 1.1 (newer, more efficient) or fall back to 1.0.
    if (FAILED(gpu.device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &featureData, sizeof(featureData)))) {
        featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
    }

    CD3DX12_DESCRIPTOR_RANGE1 ranges[2] = {}; // Not used now. Kept for reference/future use. Do not remove.
    ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV,
        1, // There will be total 1 Nos. of Constant Buffer Descriptors passed to shaders.
        0, // Base shader register i.e register(b0) in HLSL
        0, // Register space 0. Since we are greenfield project, we don't need separate space 1
        //D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC); //Optimization hint - data doesn't change often
        D3D12_DESCRIPTOR_RANGE_FLAG_NONE); //Now it does change every frame, so no static flag.
    ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE);

    CD3DX12_ROOT_PARAMETER1 rootParameters[3] = {}; //3: Number of descriptor ranges in this table

    // No descriptor ranges needed!
    // b0 : ViewProj Constant Buffer (Root Descriptor)
    rootParameters[0].InitAsConstantBufferView(0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE, D3D12_SHADER_VISIBILITY_VERTEX);
    // t0 : WorldMatrices Structured Buffer (Root Descriptor)
    rootParameters[1].InitAsShaderResourceView(0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE, D3D12_SHADER_VISIBILITY_VERTEX);
    // b1 : matrixIndex (Root Constant - 1 uint) (32-bit constant)
    rootParameters[2].InitAsConstants(1, 1, 0, D3D12_SHADER_VISIBILITY_VERTEX);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc;
    rootSignatureDesc.Init_1_1(_countof(rootParameters), rootParameters,
        0, nullptr, // Static samplers (none used here)
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT); //allows vertex input

    //Root Signatures ( basically a data-structure storing constant buffer, descriptor table ranges etc.
    //are required to be serialized, i.e. to be converted into a binary data GPU can understand.
    //Root signatures are immutable once created and optimized for the GPU's command processor. 
    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    D3DX12SerializeVersionedRootSignature(&rootSignatureDesc, featureData.HighestVersion, &signature, &error);
    gpu.device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
        IID_PPV_ARGS(&tabRes.rootSignature));

    /* Note that The root signature is like a function declaration. It defines the interface but doesn't
    contain actual data. Now that we have declared the data layout, time to prepare for movement
    of the data from CPU RAM to GPU RAM. This way we update the constant every frame without
    changing the root signature. */

    // Shaders are compiled to DXIL during the build and embedded into the executable.

    // Define the vertex input layout
    D3D12_INPUT_ELEMENT_DESC inputElementDescs[] = {
        // Position: 3 Floats (12 bytes)
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        // Normal: 4 Bytes Packed (Packed into 1 element, Offset 12)
        // DXGI_FORMAT_R8G8B8A8_SNORM automatically unpacks 0..255 to -1.0..1.0 float in shader
        { "NORMAL"  , 0, DXGI_FORMAT_R8G8B8A8_SNORM,  0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },// Offset 12
        // Note that DXGI_FORMAT_R16G16B16A16_FLOAT has 10 bits for Precision, so it is already HDR capable.
        // Additional sign bit and exponent bit enable lighting calculation to exceed the [ 0 , 1 ] bracket.
        // Eventually they are clamped by the GPU, when sending to Swap Chain.
        { "COLOR"   , 0, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }// Offset 12+4=16
    };

    // Create the pipeline state object with depth testing enabled
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
    psoDesc.pRootSignature = tabRes.rootSignature.Get();
    psoDesc.VS = CD3DX12_SHADER_BYTECODE(g_sceneVertexShader, sizeof(g_sceneVertexShader));
    psoDesc.PS = CD3DX12_SHADER_BYTECODE(g_scenePixelShader, sizeof(g_scenePixelShader));
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT); //(default = replace)
    psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT); // Enable depth testing
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = gpu.rttFormat;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT; // Set depth stencil format
    psoDesc.SampleDesc.Count = 1;
    gpu.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&tabRes.pipelineState));
    signature.Reset();
    if (error) error.Reset();

    // Following part of the code is to facilitate Indirect Drawing.
    // Command Signature
    D3D12_INDIRECT_ARGUMENT_DESC args[2] = {};
    args[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    args[0].Constant.RootParameterIndex = 2;   // b1 matrixIndex
    args[0].Constant.Num32BitValuesToSet = 1;
    args[0].Constant.DestOffsetIn32BitValues = 0;
    args[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    D3D12_COMMAND_SIGNATURE_DESC sigDesc = {};
    sigDesc.pArgumentDescs = args;
    sigDesc.NumArgumentDescs = _countof(args);
    sigDesc.ByteStride = sizeof(IndirectCommand);
    ThrowIfFailed(gpu.device->CreateCommandSignature(// root signature NOT required (already bound)
        &sigDesc, tabRes.rootSignature.Get(), IID_PPV_ARGS(&tabRes.commandSignature)));

    // 3D click-selection GPU resources (pick / highlight PSOs, rotation-cube pipeline).
    // Requires tabRes.rootSignature (created above), so initialize it here.
    InitSelection3DResources(tabRes);

    // Note: No Fence or Wait here. Resource creation is immediate.
    // Data upload sync happens in Copy Thread.
    std::wcout << "Initialized Resources for Tab." << std::endl;
}

void शंकर::WaitForPreviousFrame(const DX12ResourcesPerRenderThread& dx) {
    // Signal and increment the fence value
    const UINT64 currentFenceValue = gpu.renderFenceValue.fetch_add(1);
    //Tells the GPU command queue to "signal" (mark) the fence with the current fence value when it 
    //finishes executing all previously submitted commands.
    dx.commandQueue->Signal(dx.fence.Get(), currentFenceValue);

    // Wait until the previous frame is finished
    if (dx.fence->GetCompletedValue() < currentFenceValue) {
        dx.fence->SetEventOnCompletion(currentFenceValue, dx.fenceEvent);
        //This is the one halting CPU thread till fence completes.
        WaitForSingleObject(dx.fenceEvent, INFINITE);
    }

    // Update the frame index. Now done by render thead itself.
    // dx.frameIndex = dx.swapChain->GetCurrentBackBufferIndex();
}

void शंकर::CleanupTabResources(DX12ResourcesPerTab& tabRes) {
    // Unmap the CPU-visible Upload Heaps
    if (tabRes.pVertexDataBegin) {
        tabRes.vertexBufferUpload->Unmap(0, nullptr);
        tabRes.pVertexDataBegin = nullptr;
    }
    if (tabRes.pIndexDataBegin) {
        tabRes.indexBufferUpload->Unmap(0, nullptr);
        tabRes.pIndexDataBegin = nullptr;
    }

    // Release the GPU Resources
    tabRes.vertexBufferUpload.Reset();
    tabRes.indexBufferUpload.Reset();
    tabRes.srvHeap.Reset();

    if (tabRes.pWorldMatrixDataBegin) {
        tabRes.worldMatrixBuffer->Unmap(0, nullptr);
        tabRes.pWorldMatrixDataBegin = nullptr;
    }
    tabRes.worldMatrixBuffer.Reset();
    tabRes.freeMatrixSlots.clear();
    tabRes.matrixCount = 0;

    CleanupSelection3DResources(tabRes);
    CleanupPickPassContext(tabRes.pickCtx);

    tabRes.rootSignature.Reset();
    tabRes.pipelineState.Reset();

    std::wcout << "Cleaned up Tab Geometry Resources." << std::endl;
}

void शंकर::CleanupD3DGlobal() {
    // 1. Sync and Cleanup Copy Engine
    if (copyCommandQueue && copyFence) {
        // Simple wait: Signal and wait for it
        uint64_t flushValue = copyFenceValue.fetch_add(1);
        copyCommandQueue->Signal(copyFence.Get(), flushValue);
        if (copyFence->GetCompletedValue() < flushValue) {
            copyFence->SetEventOnCompletion(flushValue, copyFenceEvent);
            WaitForSingleObject(copyFenceEvent, INFINITE);
        }
    }

    if (copyFenceEvent) {
        CloseHandle(copyFenceEvent);
        copyFenceEvent = nullptr;
    }

    copyCommandQueue.Reset();
    copyFence.Reset();

    // Release Device & Factory. Note: If any other ComPtrs (like buffers) are still alive elsewhere, 
    // the Device won't truly be destroyed here, triggering Debug Layer warnings.
    // Ensure CleanupWindowResources/CleanupTabResources are called first!
    device.Reset();
    factory6.Reset();
    hardwareAdapter.Reset();

#if defined(_DEBUG)
    // Optional: Report Live Objects
    ComPtr<IDXGIDebug1> dxgiDebug;
    if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgiDebug)))) {
        dxgiDebug->ReportLiveObjects(DXGI_DEBUG_ALL,
            DXGI_DEBUG_RLO_FLAGS(DXGI_DEBUG_RLO_SUMMARY | DXGI_DEBUG_RLO_IGNORE_INTERNAL));
    }
#endif

    std::wcout << "Global D3D Shutdown Complete." << std::endl;
}

// Free memory that is guaranteed to be no longer in use by any rendering frame.
void शंकर::ProcessDeferredFrees(uint64_t lastCompletedRenderFrame) {
    // A real implementation would be more robust. This is a simple version.
    // Free any resource that became obsolete >= 2 frames ago.
    auto it = deferredFreeQueue.begin();
    while (it != deferredFreeQueue.end()) {
        if (it->frameNumber <= lastCompletedRenderFrame) {
            //std::wcout << "VRAM MANAGER: Reclaiming " << it->resource.size << " bytes." << std::endl;
            // In a real allocator, this space would be added to a free list.
            // In our simple bump allocator, we can't easily reuse it without compaction.
            it = deferredFreeQueue.erase(it);
        }
        else {
            ++it;
        }
    }
}

void ProcessTextureUpload(UploadRequest& req){
    auto& desc = req.texture;
    //std::wcout << "Debugging Process Texture Upload.";
    
    // Create DEFAULT heap texture
    auto texDesc = CD3DX12_RESOURCE_DESC::Tex2D(desc.format, desc.width, desc.height);
    ComPtr<ID3D12Resource> texture;
    CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
    // Note that we create the texture in COMMON state. Copy Queue automatically transitions it to COPY_DEST when we copy,
	// Similarly, Render Queue will transition it to PIXEL_SHADER_RESOURCE when binding to shader.
	// This allows us to avoid explicit resource barriers. COMMON is the recommended state for resoure across multiple queues.
    ThrowIfFailed(gpu.device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE,
        &texDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&texture)));
    
    UINT64 uploadSize; //Create upload buffer
    gpu.device->GetCopyableFootprints(&texDesc, 0, 1, 0, nullptr, nullptr, nullptr, &uploadSize);
    
    ComPtr<ID3D12Resource> uploadBuffer;
    auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);
    CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
    ThrowIfFailed(gpu.device->CreateCommittedResource( &uploadHeap, D3D12_HEAP_FLAG_NONE,
        &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuffer) ));
    
    // Create command list (copy queue)
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> cmd;
    gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&allocator));
    gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, allocator.Get(), nullptr, IID_PPV_ARGS(&cmd));
    // Copy data
    D3D12_SUBRESOURCE_DATA data = {};
    data.pData = desc.pixels;
    data.RowPitch = desc.rowPitch;
    data.SlicePitch = desc.rowPitch * desc.height;
    UpdateSubresources(cmd.Get(), texture.Get(), uploadBuffer.Get(), 0, 0, 1, &data);
    
    cmd->Close();
    ID3D12CommandList* lists[] = { cmd.Get() }; // Now execute on copy queue.
    gpu.copyCommandQueue->ExecuteCommandLists(1, lists);
    
    // Use the pre-reserved fence value from InitUIResources
    uint64_t fenceValueToSignal = 0;
    if (req.completionFence) {
        // This is the value we reserved in InitUIResources with fetch_add(1)
        fenceValueToSignal = req.completionFence->load(std::memory_order_acquire);
    } else {
        // Fallback for any future non-UI uploads
        fenceValueToSignal = gpu.copyFenceValue.fetch_add(1, std::memory_order_relaxed);
    }
    // Signal the exact fence value that the UI thread is waiting for
    gpu.copyCommandQueue->Signal(gpu.copyFence.Get(), fenceValueToSignal);
    if (req.completionFence) { // Write back the final value (render thread reads it)
        req.completionFence->store(fenceValueToSignal, std::memory_order_release);
    }
    
    *req.outResource = texture;   // Give caller the final texture
    // uploadBuffer is kept alive until this function returns (safe)

    // Force the CPU to wait here until the GPU has finished executing the commands.
    // This ensures 'uploadBuffer', 'allocator', and 'cmd' are not destroyed while in use.
    if (gpu.copyFence->GetCompletedValue() < fenceValueToSignal) {
        gpu.copyFence->SetEventOnCompletion(fenceValueToSignal, gpu.copyFenceEvent);
        WaitForSingleObject(gpu.copyFenceEvent, INFINITE);
    }

    if (req.completionFence) { // Write back the final value (render thread reads it)
        req.completionFence->store(fenceValueToSignal, std::memory_order_release);
    }
}

void GpuCopyThread() {
    /* Different monitors have their own render threads, running at different refresh rates.
    The Copy thread must never ask : What frame is rendering?.
    Instead it must guarantee : I will only write to a buffer that NO render thread can currently read.
    */

    std::wcout << "GPU Copy Thread started." << std::endl;
    uint64_t lastProcessedFrame = 0;

    // Setup Thread-Local Copy Resources. Must use COPY type to match the Copy Command Queue
    ComPtr<ID3D12CommandAllocator> commandAllocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    HRESULT hr;
    uint64_t fenceValue = 0;
    
    hr = gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&commandAllocator));
    if (FAILED(hr)) std::cerr << "Failed to create Copy Allocator" << std::endl;
    hr = gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, commandAllocator.Get(),
        nullptr, IID_PPV_ARGS(&commandList));
    if (FAILED(hr)) std::cerr << "Failed to create Copy List" << std::endl;
    commandList->Close(); // Close initially so we can Reset in the loop

    auto PublishPages = [&](TabGeometryStorage& storage, const std::vector<GeometryPage*>& oldPagesToReplace,
        std::vector<std::unique_ptr<GeometryPage>> replacementPages,
        std::vector<std::unique_ptr<GeometryPage>> newPagesToAppend)
        {
            if (oldPagesToReplace.size() != replacementPages.size()) {// Ensure vectors are matched for replacement
                std::cerr << "RCU Error: Mismatch between old pages and replacement pages count." << std::endl;
                return;
            }

            // Mark all new/replacement pages as published before exposing them
            for (auto& page : replacementPages) page->published.store(true, std::memory_order_release);
            for (auto& page : newPagesToAppend) page->published.store(true, std::memory_order_release);

            // Tag with current render frame
            uint64_t currentRenderFence = gpu.renderFenceValue.load(std::memory_order_acquire);

            // Update the writer's authoritative list (activePages). Replace old pages
            for (size_t i = 0; i < oldPagesToReplace.size(); ++i) {
                GeometryPage* targetOldPage = oldPagesToReplace[i];

                // Find the unique_ptr in activePages that matches this raw pointer
                auto it = std::find_if(storage.activePages.begin(), storage.activePages.end(),
                    [targetOldPage](const std::unique_ptr<GeometryPage>& p) { return p.get() == targetOldPage; });

                if (it != storage.activePages.end()) { // Move the old page into the retirement queue
                    storage.retiredPages.push_back({ std::move(*it), currentRenderFence });
                    *it = std::move(replacementPages[i]);// Slot the replacement page into the exact same position
                }
            }
            for (auto& newPage : newPagesToAppend) { // Append new pages
                storage.activePages.push_back(std::move(newPage));
            }

            // Build the new RCU Snapshot
            GeometryPageSnapshot* newSnapshot = new GeometryPageSnapshot();
            newSnapshot->pages.reserve(storage.activePages.size());
            for (const auto& pagePtr : storage.activePages) {
                newSnapshot->pages.push_back(pagePtr.get()); // Read-only pointers for the Render thread
            }
            // Atomically Publish the new snapshot.  exchange() swaps the pointer and returns the old one.
            GeometryPageSnapshot* oldSnapshot = storage.activeSnapshot.exchange(newSnapshot, std::memory_order_acq_rel);
            // Retire the old snapshot (the struct itself) so it can be deleted later
            if (oldSnapshot) storage.retiredSnapshots.push_back({ oldSnapshot, currentRenderFence });

            /* Future Notes: DO NOT try to implement versioned page arrays technique.
            Since our page size is 4MB, and currently none of state of art graphics card has exceed 128 GB Memory,
            our worst case is still ~32000 Pages, in real world we expect it to be less than 1000.
            Hence no need to add additional complexity. RCU is already complex enough !
            TODO: Add page count to telemetry.*/
        };

    struct ObjectLocation {
        GeometryPage* page;   // page where object currently resides
        uint32_t slot;        // index in page->objects
    };
    // Copy-thread-private bookkeeping: objectIndex maps each. objectID to which VRAM page (raw ptr) currently owns it.
    std::unordered_map<uint64_t, ObjectLocation> objectLocation;
    int counter = 0;
    while (!shutdownSignal) { // Texture uploads are processed sequentially, Geometry updates are processed in batches.
		// TEXTURE UPLOADS (Processed immediately as they come in, to minimize latency for textures)
        uint32_t read = gUploadQueue.readIndex.load(std::memory_order_relaxed);
        uint32_t write = gUploadQueue.writeIndex.load(std::memory_order_acquire);
        while (read < write)  {
            UploadRequest& req = gUploadQueue.requests[read % MAX_UPLOAD_REQUESTS];
            if (req.type == UploadType::Texture2D) { ProcessTextureUpload(req); }
            read++;
            gUploadQueue.readIndex.store(read, std::memory_order_release);
        }

		// GEOMETRY UPDATE BATCH PROCESSING
        CommandToCopyThread cmd;
        gpu.copyFenceValue.fetch_add(1); // Move forward wrt previous completed fence value.
        // Intentionally +1 here to decouple current iteration of while loop from previous iteration.

        // Make a local copy of all commands to process in this iteration, to minimize lock holding time.
        // TODO: Add throttling here ? Like only 100k commands are processed at once ? Or some % of GPU VRAM Capacity?
        std::vector<CommandToCopyThread> batch;
        {
            std::unique_lock<std::mutex> lock(toCopyThreadMutex);

            // Wake up if there is Geometry OR Texture Uploads OR Shutdown
            toCopyThreadCV.wait(lock, [&] {
                bool hasGeometry = !commandToCopyThreadQueue.empty();
                bool hasTextures = gUploadQueue.readIndex.load(std::memory_order_relaxed) 
                    < gUploadQueue.writeIndex.load(std::memory_order_relaxed);
                bool hasCad2D = HasPendingCad2DCopyCommands();
                return hasGeometry || hasTextures || hasCad2D || shutdownSignal;
                });

            while (!commandToCopyThreadQueue.empty()) {
                batch.push_back(std::move(commandToCopyThreadQueue.front()));
                commandToCopyThreadQueue.pop();
            }
        } // lock released here. We have a local batch of commands to process without holding the lock.
        if (shutdownSignal) break; // Exit if shutdown was signaled while waiting.

        std::vector<CommandToCopyThread2D> cad2DBatch;
        PopAllCad2DCopyCommands(cad2DBatch);
        if (!cad2DBatch.empty()) {
            ProcessCad2DCopyBatch(cad2DBatch);
        }

        uint16_t* tabList = publishedTabIndexes.load(std::memory_order_acquire);
        uint16_t tabCount = publishedTabCount.load(std::memory_order_acquire);

        std::vector<uint16_t> sortedTabs(tabList, tabList + tabCount);
        std::sort(sortedTabs.begin(), sortedTabs.end()); //Sort such that tab 0 is always processed 1st. Don't remove.

        std::unordered_map<uint64_t, ObjectLocation> batchLocationOverride;

        for (uint16_t tabID : sortedTabs) { //Process 1 tab at a time.
            bool tabTouched = false;

            DATASETTAB& targetTab = allTabs[tabID];
            TabGeometryStorage& storage = targetTab.geometry;
            DX12ResourcesPerTab& tabRes = targetTab.dx;

            std::unordered_set<GeometryPage*> affectedPages;
            std::unordered_map<GeometryPage*, std::unique_ptr<GeometryPage>> clonedPages;
            std::vector<std::unique_ptr<GeometryPage>> newPages;

            // Pre-Pass: Deduplicate commands for this tab. If the same object is modified twice,
            // or added then modified, etc., we need only FINAL state in this batch to persist.
            std::unordered_map<uint64_t, size_t> latestCommandIndex;
            for (size_t i = 0; i < batch.size(); ++i) {
                if (batch[i].tabID == tabID) { latestCommandIndex[batch[i].id] = i; }
            }

            std::vector<CommandToCopyThread> deduplicatedBatch;
            deduplicatedBatch.reserve(batch.size());
            for (size_t i = 0; i < batch.size(); ++i) {
                if (batch[i].tabID != tabID) continue;
                // Only keep the command if it is the absolute latest operation for this ID
                if (latestCommandIndex[batch[i].id] == i) { deduplicatedBatch.push_back(batch[i]); }
            }

            // Update the tabTouched flag based on our deduplicated list
            if (deduplicatedBatch.empty()) continue; // No command for this tab. Skip this tab.
            tabTouched = true;

            // Pass 1: Identify affected pages. We will clone these pages,
            //apply modifications to the clones, and then publish atomically.
            for (auto& cmd : deduplicatedBatch) {
                if (cmd.type == CommandToCopyThreadType::ADD) continue; // handled later
                auto it = objectLocation.find(cmd.id);
                if (it != objectLocation.end()) affectedPages.insert(it->second.page);
            }

            // Find the page with the largest contiguous middle gap for each container receiving geometry.
            // Pages never mix containers, so inactive pages can be hidden with ExecuteIndirect count 0.
            std::unordered_set<uint64_t> containersNeedingAppend;
            for (const auto& cmd : deduplicatedBatch) {
                if (!cmd.geometry.has_value()) continue;
                uint64_t containerMemoryId = cmd.containerMemoryId;
                auto existingIt = objectLocation.find(cmd.id);
                if (containerMemoryId == 0 && existingIt != objectLocation.end()) {
                    containerMemoryId = existingIt->second.page->containerMemoryId;
                }
                containersNeedingAppend.insert(containerMemoryId);
            }

            std::unordered_map<uint64_t, GeometryPage*> bestAppendCandidates;
            std::unordered_map<uint64_t, size_t> maxHoleByContainer;
            for (const auto& pagePtr : storage.activePages) {
                GeometryPage* p = pagePtr.get();
                if (!p->published.load(std::memory_order_acquire)) continue; //Just for extra safety.
                if (containersNeedingAppend.find(p->containerMemoryId) == containersNeedingAppend.end()) continue;

                size_t hole = p->indexTail - p->vertexHead;  // middle contiguous free space
                size_t& maxHole = maxHoleByContainer[p->containerMemoryId];
                if (hole > maxHole) {
                    maxHole = hole;
                    bestAppendCandidates[p->containerMemoryId] = p;
                }
            }
            // Force-clone each append candidate so Pass 3 never mutates a published page.
            for (const auto& [containerMemoryId, candidate] : bestAppendCandidates) {
                affectedPages.insert(candidate);
            }

            commandAllocator->Reset(); // Prepare command allocator for more work !
            commandList->Reset(commandAllocator.Get(), nullptr); // Opens command list for closing.

            //Pass 2: Clone Affected Pages (RCU copy)
            for (GeometryPage* oldPage : affectedPages) {
                auto clonedPage = CreateNewPage(oldPage->containerMemoryId);

                // Following 2 commands are for copying the GPU VRAM data.
                commandList->CopyResource(clonedPage->buffer.Get(), oldPage->buffer.Get());
                // TODO: Copy with defragmentation using metadata.
                commandList->CopyResource(clonedPage->indirectBuffer.Get(), oldPage->indirectBuffer.Get());

                clonedPage->objects = oldPage->objects; //These commands are CPU side metadata copy.
                clonedPage->vertexHead = oldPage->vertexHead;
                clonedPage->indexTail = oldPage->indexTail;
                clonedPage->objectCount = oldPage->objectCount;
                clonedPage->version = oldPage->version + 1;
                clonedPage->holeBytes = oldPage->holeBytes;

                clonedPages[oldPage] = std::move(clonedPage);
            }

            for (auto& [oldRaw, clone] : clonedPages) {
                for (uint32_t i = 0; i < clone->objects.size(); ++i) {
                    auto& obj = clone->objects[i];
                    if (obj.isDeleted) continue;

                    auto it = objectLocation.find(obj.objectID);
                    if (it != objectLocation.end() && it->second.page == oldRaw) {
                        it->second.page = clone.get();
                        it->second.slot = i;
                    }
                }
            }

            std::unordered_map<uint64_t, GeometryPage*> addTargetPages;
            if (!affectedPages.empty()) { // If no pages cloned, we don't need these GPU operations.
                ThrowIfFailed(commandList->Close());
                ID3D12CommandList* lists[] = { commandList.Get() };
                gpu.copyCommandQueue->ExecuteCommandLists(1, lists);
                fenceValue = gpu.copyFenceValue.fetch_add(1);
                gpu.copyCommandQueue->Signal(gpu.copyFence.Get(), fenceValue);

                if (gpu.copyFence->GetCompletedValue() < fenceValue) {
                    // TODO: Can we delay this wait until just before we need to access the cloned pages ? 
                    // This would allow some CPU-GPU parallelism.
                    gpu.copyFence->SetEventOnCompletion(fenceValue, gpu.copyFenceEvent);
                    WaitForSingleObject(gpu.copyFenceEvent, INFINITE);
                }

                for (const auto& [containerMemoryId, candidate] : bestAppendCandidates) {
                    auto cloneIt = clonedPages.find(candidate);
                    addTargetPages[containerMemoryId] = cloneIt != clonedPages.end()
                        ? cloneIt->second.get()
                        : candidate;
                }

                // Re-open the command list for Pass-3 GPU work (geometry uploads for ADD/MODIFY-grow cases).
                commandAllocator->Reset();
                commandList->Reset(commandAllocator.Get(), nullptr);
            }

            //std::wcout << "activePages: " << storage.activePages.size() << 
            //    ", clonedPages:" << clonedPages.size() << std::endl;

            // Pass 3 — Apply every command in the batch to the (already-cloned) pages.
            
            // Staging uploads produced during this pass. Kept alive until after ExecuteCommandLists + fence-wait.
            std::vector<ComPtr<ID3D12Resource>> pass3Uploads;

            // Common lambda: write vertex+index data into a page Used by both ADD (to last/new page) and MODIFY-grow paths.
            // Records CopyBufferRegion into the open commandList.Returns the filled-in placement record; caller appends it.
            auto RecordGeometryUpload = [&](GeometryPage* dstPage, const GeometryData& geo, uint32_t matrixIndex)
                -> GeometryPlacementRecordInPage {
                const uint32_t vertexBytes = static_cast<uint32_t>(geo.vertices.size() * sizeof(Vertex));
                const uint32_t indexBytes = static_cast<uint32_t>(geo.indices.size() * sizeof(uint16_t));

                const uint32_t vOffset = GeometryPage::AlignUp(dstPage->vertexHead, 16);
                const uint32_t iOffset = GeometryPage::AlignDown(dstPage->indexTail - indexBytes, 4);

                // CPU-side staging upload buffer (vertex + index packed)
                ComPtr<ID3D12Resource> upload;
                CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
                auto uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(vertexBytes + indexBytes);

                ThrowIfFailed(gpu.device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)));

                uint8_t* mapped = nullptr;
                CD3DX12_RANGE readRange(0, 0);
                upload->Map(0, &readRange, reinterpret_cast<void**>(&mapped));
                memcpy(mapped, geo.vertices.data(), vertexBytes);
                memcpy(mapped + vertexBytes, geo.indices.data(), indexBytes);
                upload->Unmap(0, nullptr);

                // Record GPU copies (no Execute yet — batched at end of Pass 3)
                commandList->CopyBufferRegion(dstPage->buffer.Get(), vOffset, upload.Get(), 0, vertexBytes);
                commandList->CopyBufferRegion(dstPage->buffer.Get(), iOffset, upload.Get(), vertexBytes, indexBytes);
                pass3Uploads.push_back(std::move(upload)); // Keep upload buffer alive until the fence fires

                // Build and return the placement record (caller updates page state)
                GeometryPlacementRecordInPage rec{};
                rec.objectID = geo.id;
                rec.vertexByteOffset = vOffset;
                rec.vertexSize = vertexBytes;
                rec.indexByteOffset = iOffset;
                rec.indexSize = indexBytes;
                rec.indexCount = static_cast<uint32_t>(geo.indices.size());
                rec.matrixIndex = matrixIndex;

                // Local-space AABB. Consumed by GPU picking / selection re-centering (Selection3D).
                if (!geo.vertices.empty()) {
                    float minX = FLT_MAX, minY = FLT_MAX, minZ = FLT_MAX;
                    float maxX = -FLT_MAX, maxY = -FLT_MAX, maxZ = -FLT_MAX;
                    for (const Vertex& v : geo.vertices) {
                        minX = (std::min)(minX, v.position.x); maxX = (std::max)(maxX, v.position.x);
                        minY = (std::min)(minY, v.position.y); maxY = (std::max)(maxY, v.position.y);
                        minZ = (std::min)(minZ, v.position.z); maxZ = (std::max)(maxZ, v.position.z);
                    }
                    rec.minX = minX; rec.minY = minY; rec.minZ = minZ;
                    rec.maxX = maxX; rec.maxY = maxY; rec.maxZ = maxZ;
                }
                return rec;
                };

            uint32_t matrixIndex; // Allocate a matrix slot (new object)
            XMMATRIX worldMat;
            std::unordered_map<uint64_t, GeometryPage*> newestPagesByContainer;

            auto AcquireAppendPage = [&](uint64_t containerMemoryId,
                uint32_t incomingVertexBytes, uint32_t incomingIndexBytes) -> GeometryPage* {
                GeometryPage*& targetPage = addTargetPages[containerMemoryId];
                if (targetPage && !targetPage->IsFull(incomingVertexBytes, incomingIndexBytes)) {
                    return targetPage;
                }

                GeometryPage*& newestPage = newestPagesByContainer[containerMemoryId];
                if (!newestPage || newestPage->IsFull(incomingVertexBytes, incomingIndexBytes)) {
                    newPages.push_back(CreateNewPage(containerMemoryId));
                    newestPage = newPages.back().get();
                }
                targetPage = newestPage;
                return targetPage;
            };

            for (auto& cmd : deduplicatedBatch) { // Iterate over batch
                if (cmd.tabID != tabID) continue;
                // Find the targe tab. Our static array of tabs is thread-safe for reading.

                uint32_t vertexBytes = 0, indexBytes = 0, newVertexBytes = 0, newIndexBytes = 0;

                decltype(objectLocation)::iterator locIt;
                decltype(clonedPages)::iterator cloneIt;

                GeometryPage* workPage = nullptr;
                GeometryPage* oldPage = nullptr;
                GeometryPage* modifyTargetPage = nullptr;
                const GeometryData* geo = nullptr;
                GeometryPlacementRecordInPage* oldRec = nullptr;
                GeometryPlacementRecordInPage rec;
                uint32_t slotIndex = 0;
                uint64_t targetContainerMemoryId = cmd.containerMemoryId;
                matrixIndex = 0;

                // We mandatorily check if ID still exist even if the command is ADD as a safety measure.
                // This is also necessary when REMOVE + ADD command is received in same batch and deduped.
                // TODO: Future, add a fast path ADDINITIAL in future for quick initial loading at startup.
                switch (cmd.type)// Process Command
                {
                case CommandToCopyThreadType::ADD:
                handle_add:
                {
                    //std::wcout << "Adding New object ID: " << cmd.id << std::endl;
                    if (!cmd.geometry.has_value()) break;
                    if (objectLocation.find(cmd.id) != objectLocation.end()) {goto handle_modify;}

                    geo = &(cmd.geometry.value());
                    vertexBytes = static_cast<uint32_t>(geo->vertices.size() * sizeof(Vertex));
                    indexBytes = static_cast<uint32_t>(geo->indices.size() * sizeof(uint16_t));
                    if (vertexBytes == 0 || indexBytes == 0) {
                        std::wcout << "Warning: Skipping upload of empty geometry ID " << cmd.id << std::endl;
                        break; // Exit this case, process next command
                    }

                    if (!tabRes.freeMatrixSlots.empty()) {
                        matrixIndex = tabRes.freeMatrixSlots.back();
                        tabRes.freeMatrixSlots.pop_back();
                    } else {
                        matrixIndex = tabRes.matrixCount++;
                        if (matrixIndex >= tabRes.matrixCapacity) matrixIndex = 0; // TODO: handle growth
                    }

                    // Copy transposed world matrix to upload buffer
                    worldMat = XMLoadFloat4x4(&geo->worldMatrix);
                    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(
                        tabRes.pWorldMatrixDataBegin + matrixIndex * sizeof(XMFLOAT4X4)),
                        XMMatrixTranspose(worldMat));

                    GeometryPage* addTargetPage =
                        AcquireAppendPage(targetContainerMemoryId, vertexBytes, indexBytes);

                    // Record the geometry upload into commandList
                    rec = RecordGeometryUpload(addTargetPage, *geo, matrixIndex);

                    // Update page CPU state
                    addTargetPage->objects.push_back(rec);
                    addTargetPage->vertexHead = rec.vertexByteOffset + rec.vertexSize;
                    addTargetPage->indexTail = rec.indexByteOffset;
                    addTargetPage->objectCount++;

                    // Update the copy-thread's private location map
                    slotIndex = static_cast<uint32_t>(addTargetPage->objects.size() - 1);
                    objectLocation[cmd.id] = { addTargetPage, slotIndex };

                    //std::wcout << "Added New object ID: " << cmd.id << std::endl;
                    break;
                }

                case CommandToCopyThreadType::MODIFY:
                handle_modify: // This is GOTO jump from ADD thread, if the ID already existed.
                    // TODO: Latter we will improve to use existing pages if it fits in place.
                    if (!cmd.geometry.has_value()) break;

                    locIt = objectLocation.find(cmd.id);
                    if (locIt == objectLocation.end()) { goto handle_add; }// treat as ADD

                    geo = &(cmd.geometry.value());

                    newVertexBytes = static_cast<uint32_t>(geo->vertices.size() * sizeof(Vertex));
                    newIndexBytes = static_cast<uint32_t>(geo->indices.size() * sizeof(uint16_t));

                    if (newVertexBytes == 0 || newIndexBytes == 0) break;

                    // Object exists — work on its owning cloned page
                    oldPage = locIt->second.page;
                    slotIndex = locIt->second.slot;
                    if (targetContainerMemoryId == 0) {
                        targetContainerMemoryId = oldPage->containerMemoryId;
                    }

                    // Resolve which mutable page we are working with: It will be in clonedPages (was in affectedPages 
                    // from Pass 1), because Pass 1 already included pages from existing objectLocation.

                    cloneIt = clonedPages.find(oldPage);
                    if (cloneIt != clonedPages.end()) workPage = cloneIt->second.get();
                    else workPage = oldPage;   // page created this batch

                    oldRec = &(workPage->objects[slotIndex]);
                    oldRec->isDeleted = true;

                    workPage->holeBytes += oldRec->vertexSize + oldRec->indexSize;
                    workPage->objectCount--;

                    modifyTargetPage =
                        AcquireAppendPage(targetContainerMemoryId, newVertexBytes, newIndexBytes);

                    // Allocate a fresh matrix slot for the relocated geometry
                    if (!tabRes.freeMatrixSlots.empty()) {
                        matrixIndex = tabRes.freeMatrixSlots.back();
                        tabRes.freeMatrixSlots.pop_back();
                    } else {
                        matrixIndex = tabRes.matrixCount++;
                        if (matrixIndex >= tabRes.matrixCapacity) matrixIndex = 0; // TODO: Overflow safety. Improve latter.
                    }
                    // Free the old matrix slot
                    worldMat = XMLoadFloat4x4(&geo->worldMatrix);
                    tabRes.freeMatrixSlots.push_back(oldRec->matrixIndex);
                    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(tabRes.pWorldMatrixDataBegin +
                        matrixIndex * sizeof(XMFLOAT4X4)), XMMatrixTranspose(worldMat));

                    rec = RecordGeometryUpload(modifyTargetPage, *geo, matrixIndex);
                    modifyTargetPage->objects.push_back(rec);
                    modifyTargetPage->vertexHead = rec.vertexByteOffset + rec.vertexSize;
                    modifyTargetPage->indexTail = rec.indexByteOffset;
                    modifyTargetPage->objectCount++;

                    objectLocation[cmd.id] = {
                        modifyTargetPage, static_cast<uint32_t>(modifyTargetPage->objects.size() - 1) };
                    break;

                case CommandToCopyThreadType::REMOVE:
                
                    locIt = objectLocation.find(cmd.id);
                    if (locIt == objectLocation.end()) break; // not on GPU, nothing to do

                    oldPage = locIt->second.page;
                    slotIndex = locIt->second.slot;

                    // Resolve mutable clone
                    cloneIt = clonedPages.find(oldPage);
                    if (cloneIt != clonedPages.end()) workPage = cloneIt->second.get();
                    else workPage = oldPage;   // page created this batch

                    rec = workPage->objects[slotIndex];

                    // Soft-delete: mark the slot; IndirectBuffer rebuild will skip it
                    rec.isDeleted = true;
                    workPage->holeBytes += rec.vertexSize + rec.indexSize;
                    workPage->objectCount--;
                    tabRes.freeMatrixSlots.push_back(rec.matrixIndex); // Free the matrix slot for reuse
                    objectLocation.erase(locIt);// Remove from local bookkeeping
                    break;
                

                default: break;
                } // End of switch (cmd.type)// Process Command
            } // end for (batch)

            // Single GPU Execute for all Pass-3 geometry uploads
            if (!pass3Uploads.empty()) {
                ThrowIfFailed(commandList->Close());
                {
                    ID3D12CommandList* lists[] = { commandList.Get() };
                    gpu.copyCommandQueue->ExecuteCommandLists(1, lists);
                }
                fenceValue = gpu.copyFenceValue.fetch_add(1);
                gpu.copyCommandQueue->Signal(gpu.copyFence.Get(), fenceValue);
                if (gpu.copyFence->GetCompletedValue() < fenceValue) {
                    gpu.copyFence->SetEventOnCompletion(fenceValue, gpu.copyFenceEvent);
                    WaitForSingleObject(gpu.copyFenceEvent, INFINITE);
                }
                pass3Uploads.clear();// Upload staging buffers are now safe to release

                commandAllocator->Reset(); // Prepare for next stage.
                commandList->Reset(commandAllocator.Get(), nullptr);
            }

            // Rebuild Indirect Buffers (outside the per-command loop) Runs once per modified or new page, 
            // after all commands are applied. Only live objects (isDeleted == false) are emitted.
            // We rebuild into CPU staging, then do one more command record
            // + execute (or we can reuse the same allocator/list with a Reset).
            
            std::vector<ComPtr<ID3D12Resource>> indirectUploads;

            // Helper that rebuilds a single page's indirect buffer
            auto RebuildIndirectBuffer = [&](GeometryPage* page) {
                std::vector<IndirectCommand> commands;
                commands.reserve(page->objects.size());

                for (const auto& obj : page->objects) {
                    if (obj.isDeleted) continue; // skip soft-deleted slots

                    IndirectCommand ic{};
                    ic.matrixIndex = obj.matrixIndex;
                    ic.drawArguments.IndexCountPerInstance = obj.indexCount;
                    ic.drawArguments.InstanceCount = 1;
                    ic.drawArguments.StartIndexLocation = (obj.indexByteOffset - page->indexTail) / sizeof(uint16_t);
                    ic.drawArguments.BaseVertexLocation = obj.vertexByteOffset / sizeof(Vertex);
                    ic.drawArguments.StartInstanceLocation = 0;

                    commands.push_back(ic);
                }

                page->indirectCount = static_cast<uint32_t>(commands.size());
                if (commands.empty()) return; // nothing to upload

                ComPtr<ID3D12Resource> indirectUpload;
                CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
                auto iuDesc = CD3DX12_RESOURCE_DESC::Buffer(commands.size() * sizeof(IndirectCommand));

                ThrowIfFailed(gpu.device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &iuDesc,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&indirectUpload)));

                uint8_t* mapped = nullptr;
                CD3DX12_RANGE readRange(0, 0);
                indirectUpload->Map(0, &readRange, reinterpret_cast<void**>(&mapped));
                memcpy(mapped, commands.data(), commands.size() * sizeof(IndirectCommand));
                indirectUpload->Unmap(0, nullptr);

                commandList->CopyBufferRegion(page->indirectBuffer.Get(), 0, indirectUpload.Get(), 0,
                    commands.size() * sizeof(IndirectCommand));

                indirectUploads.push_back(std::move(indirectUpload));
                };

            // Rebuild for every cloned (modified) page
            for (auto& [oldRaw, clonedPage] : clonedPages) RebuildIndirectBuffer(clonedPage.get());
            // Rebuild for every brand-new page
            for (auto& page : newPages) RebuildIndirectBuffer(page.get());

            if (clonedPages.empty() && newPages.empty()) continue; //Skip everything if no commands?

            // Sync Copy queue.
            ThrowIfFailed(commandList->Close());
            ID3D12CommandList* lists[] = { commandList.Get() };
            gpu.copyCommandQueue->ExecuteCommandLists(1, lists);
            fenceValue = gpu.copyFenceValue.fetch_add(1);
            gpu.copyCommandQueue->Signal(gpu.copyFence.Get(), fenceValue);
            if (gpu.copyFence->GetCompletedValue() < fenceValue) {
                gpu.copyFence->SetEventOnCompletion(fenceValue, gpu.copyFenceEvent);
                WaitForSingleObject(gpu.copyFenceEvent, INFINITE);
            }

            indirectUploads.clear(); // staging buffers safe to free

            // Final RCU Publish (Single Atomic Operation). Gather per-tab publish work. Since commands can span multiple
            // tabs, group replacements and appends by their TabGeometryStorage.
            // Because clonedPages and newPages can belong to different tabs we need to route them to the correct storage.
            // The clonedPages map already contains the raw oldPage ptr which we can match
            // back to its owning storage via the batch's tabID.
            std::vector<GeometryPage*> oldPages;
            std::vector<std::unique_ptr<GeometryPage>> replacements;
            for (auto& [oldRaw, clone] : clonedPages)
            {
                oldPages.push_back(oldRaw);
                replacements.push_back(std::move(clone));
            }

            PublishPages(storage, oldPages, std::move(replacements), std::move(newPages));
            // End of Pass 3 + Publish 
        }

        /* UpdateSubresources may introduce ResourceBarriers internally ! Which is not allowed on Copy Queues.
        with the raw copy command, which is safe because our buffers are already created
        in a valid state for copying (COMMON or GENERIC_READ).
        DirectX 12 has a feature called "Implicit State Promotion".
        If a Buffer is in COMMON state, and you bind it as a Vertex Buffer (Read-Only),
        the driver automatically promotes it to the correct state without you issuing a barrier.
        ResourceBarriers are REMOVED here. Copy Queues cannot execute barriers.
        The Render Thread will handle the transition from COPY_DEST/COMMON to VERTEX_BUFFER when it draws.*/

        // TODO: Throttle this to run at max once every 100ms or so. Or maybe every 1 second.
        // Retire stale snapshots and pages
        // A retired object is safe to destroy only after every active render thread has completed a frame 
        // AFTER the retire fence was tagged.
        bool foundAny = false;
        uint64_t minCompleted = UINT64_MAX;

        for (int i = 0; i < gpu.currentMonitorCount; ++i) {
            // Skip monitors whose render thread never started or has already exited and torn down its fence.
            if (!gpu.screens[i].renderFence) continue; // fence not created yet
            // renderFenceValue starts with 1 at the time of monitor creation.
            if (gpu.screens[i].renderFenceValue < 2) continue; // thread never signalled
            if (!gpu.screens[i].isScreenInitalized)  continue; // Skip inactive monitors

            // GetCompletedValue() is safe to call from any thread at any time;
            // it reads a value the GPU writes atomically.
            uint64_t completed = gpu.screens[i].renderFence->GetCompletedValue();
            // GetCompletedValue returns UINT64_MAX on device-lost.
            if (completed == UINT64_MAX) continue; // Treat completed.
            //Treat that monitor as if it has retired everything (it's already dead).
            if (completed < minCompleted) minCompleted = completed;
            foundAny = true;
        }
        if (foundAny) { //TODO : Fix if all monitors have exited and no render thead is running !
            uint64_t safeRetireFence = minCompleted;
            uint16_t* tabList = publishedTabIndexes.load(std::memory_order_acquire);
            uint16_t tabCount = publishedTabCount.load(std::memory_order_acquire);
            for (uint16_t i = 0; i < tabCount; ++i) {
                TabGeometryStorage& storage = allTabs[tabList[i]].geometry;
                // Retire old RCU snapshots: 
                // The snapshot struct itself is heap-allocated (via `new` in PublishPages).
                // It is safe to delete once no render thread can still be iterating its pages vector.
                storage.retiredSnapshots.erase( std::remove_if(
                    storage.retiredSnapshots.begin(), storage.retiredSnapshots.end(),
                    [&](const auto& rs) {
                        if (rs.retireFence <= safeRetireFence) {
                            delete rs.snapshot; // The GeometryPageSnapshot* itself
                            return true;        // Remove from the retirement list
                        }
                        return false;
                    }), storage.retiredSnapshots.end()
                );

                // Retire old geometry pages:
                // unique_ptr<GeometryPage> releases the GPU resource (ComPtr members) when it goes out of scope here.
                storage.retiredPages.erase(std::remove_if(
                    storage.retiredPages.begin(), storage.retiredPages.end(),
                    [&](const auto& rp){
                        // Strict <= : the fence value tagged at publish time is the frame during 
                        // which the old page was still live. Once all monitors have
                        // completed that frame, it is safe to free.
                        return rp.retireFence <= safeRetireFence;
                        // unique_ptr destructs automatically on removal
                    }), storage.retiredPages.end()
                );

                if (allTabs[tabList[i]].cad2d) {
                    PruneCad2DRetiredResources(*allTabs[tabList[i]].cad2d, safeRetireFence);
                }
            }
        } // End of cleanup section.
    } // End of while (!shutdownSignal)

    // Application is about to exit anyway.
    // Still we release as much memory ourselves as possible to reduce debug warnings.
    auto tabList = publishedTabIndexes.load(std::memory_order_acquire);
    auto tabCount = publishedTabCount.load(std::memory_order_acquire);
    for (uint16_t i = 0; i < tabCount; ++i) {
        TabGeometryStorage& storage = allTabs[tabList[i]].geometry;
        for (auto& rs : storage.retiredSnapshots) { delete rs.snapshot; }
        storage.retiredSnapshots.clear();
        storage.retiredPages.clear();
        if (allTabs[tabList[i]].cad2d) {
            ReleaseCad2DRetiredResources(*allTabs[tabList[i]].cad2d);
        }
    }
    std::wcout << "GPU Copy Thread shutting down." << std::endl;
}
