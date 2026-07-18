// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.

#include "MemoryManagerGPU-DirectX12.h"
#include "RenderPage2D-DirectX12.h"
#include "UserInterface-DirectX12.h"
#include "RenderScene3D.h"
#include "ShaderSceneVertex.h"
#include "ShaderScenePixel.h"
#include <algorithm>
#include <cmath>
#include "विश्वकर्मा.h"
#include <iomanip>
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
    
    hr = gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&commandAllocator));
    if (FAILED(hr)) std::cerr << "Failed to create Copy Allocator" << std::endl;
    hr = gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, commandAllocator.Get(),
        nullptr, IID_PPV_ARGS(&commandList));
    if (FAILED(hr)) std::cerr << "Failed to create Copy List" << std::endl;
    commandList->Close(); // Close initially so we can Reset in the loop

    int counter = 0;
    while (!shutdownSignal) { // Texture uploads are processed sequentially, Geometry updates are processed in batches.
		// TEXTURE UPLOADS (Processed immediately as they come in, to minimize latency for textures)
        uint32_t read = gUploadQueue.readIndex.load(std::memory_order_relaxed);
        uint32_t write = gUploadQueue.writeIndex.load(std::memory_order_acquire);
        while (read < write)  {
            UploadRequest& req = gUploadQueue.requests[read % MAX_UPLOAD_REQUESTS];
            if (req.type == UploadType::Texture2D) {
                try { ProcessTextureUpload(req); }
                catch (const HrException& e) {
                    std::cerr << "Copy thread: texture upload failed, hr=0x"
                        << std::hex << e.Error() << std::dec << std::endl;
                    // Unblock any CPU thread waiting on this upload's reserved fence value
                    // (UploadUIAtlasTexture / BuildMonitorIconAtlas wait INFINITE on it).
                    if (req.completionFence) {
                        gpu.copyFence->Signal(req.completionFence->load(std::memory_order_acquire));
                    }
                }
            }
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

            // Wake up if there is Geometry OR Texture Uploads OR a Tab release OR Shutdown
            toCopyThreadCV.wait(lock, [&] {
                bool hasGeometry = !commandToCopyThreadQueue.empty();
                bool hasTextures = gUploadQueue.readIndex.load(std::memory_order_relaxed)
                    < gUploadQueue.writeIndex.load(std::memory_order_relaxed);
                bool hasCad2D = HasPendingCad2DCopyCommands();
                bool hasTabRelease = gPendingTabGpuReleases.load(std::memory_order_relaxed) > 0;
                return hasGeometry || hasTextures || hasCad2D || hasTabRelease || shutdownSignal;
                });

            while (!commandToCopyThreadQueue.empty()) {
                batch.push_back(std::move(commandToCopyThreadQueue.front()));
                commandToCopyThreadQueue.pop();
            }
        } // lock released here. We have a local batch of commands to process without holding the lock.
        if (shutdownSignal) break; // Exit if shutdown was signaled while waiting.

        // A fence-gated tab release keeps the CV predicate true while render fences catch up to
        // the tag; pace the loop so those 1-2 frames don't spin this thread at 100% CPU.
        if (batch.empty() && gPendingTabGpuReleases.load(std::memory_order_acquire) > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }

        std::vector<CommandToCopyThread2D> cad2DBatch;
        PopAllCad2DCopyCommands(cad2DBatch);
        // An allocation failure (e.g. VRAM exhaustion) inside a batch must not abort the process:
        // this thread has no other exception handler, so an uncaught HrException == std::terminate.
        // The failed batch is dropped (logged); rendering continues from the last published snapshot.
        try {
            if (!cad2DBatch.empty()) {
                ProcessCad2DCopyBatch(cad2DBatch);
            }
            // 3D geometry batch: RCU page cloning/building/publish moved to RenderScene3D-DirectX12.cpp.
            ProcessScene3DCopyBatch(batch, commandAllocator, commandList);
        }
        catch (const HrException& e) {
            std::cerr << "Copy thread: geometry batch failed, hr=0x" << std::hex << e.Error()
                << std::dec << ". Batch dropped (" << batch.size() << " 3D / "
                << cad2DBatch.size() << " 2D commands)." << std::endl;
            commandList->Close(); // May be left open by the throw; Close so the next Reset is legal.
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

#ifdef _DEBUG
                // Retire-backlog sentinel: with healthy pruning this stays in single digits.
                // Sustained growth means some monitor's fence stopped advancing again.
                const size_t retireBacklog =
                    storage.retiredPages.size() + storage.retiredSnapshots.size();
                if (retireBacklog > 128 && retireBacklog % 128 == 0) {
                    std::cout << "[gpu][warn] tab " << tabList[i] << " retire backlog="
                              << retireBacklog << " safeRetireFence=" << safeRetireFence << std::endl;
                }
#endif
            }

            // Fence-gated teardown of closed tabs (requested by CleanupReleasedTabs on the UI
            // thread). Runs here because this thread owns all per-tab GPU state; safeRetireFence
            // guarantees no monitor still executes a frame referencing the tab's resources.
            // Closed tabs are unpublished, so the per-tab loop above never visits them.
            if (gPendingTabGpuReleases.load(std::memory_order_acquire) > 0) {
                for (uint16_t t = 0; t < MV_MAX_TABS; ++t) {
                    DATASETTAB& closingTab = allTabs[t];
                    const uint8_t releaseState =
                        closingTab.gpuReleaseState.load(std::memory_order_acquire);
                    if (releaseState == 1) {
                        // Tag with the current global fence; every monitor must complete past
                        // this before the tab's GPU resources can be destroyed.
                        closingTab.gpuReleaseFence =
                            gpu.renderFenceValue.load(std::memory_order_acquire);
                        closingTab.gpuReleaseState.store(2, std::memory_order_release);
                    }
                    else if (releaseState == 2 && closingTab.gpuReleaseFence <= safeRetireFence) {
                        ReleaseTabGpuGeometry(closingTab);          // Pages + snapshots (VRAM)
                        if (closingTab.cad2d) CleanupCad2DTabResources(*closingTab.cad2d);
                        gpu.CleanupTabResources(closingTab.dx);     // Upload heaps, matrix buffer, PSOs
                        cpu.notifyTabClosed(closingTab.tabNo); // CPU arena chunks
                        closingTab.gpuReleaseState.store(0, std::memory_order_release);
                        gPendingTabGpuReleases.fetch_sub(1, std::memory_order_release);
                        std::wcout << L"Closed tab " << t << L": GPU + arena memory released." << std::endl;
                    }
                }
            }
        } // End of cleanup section.
    } // End of while (!shutdownSignal)

    // Application is about to exit anyway.
    // Still we release as much memory ourselves as possible to reduce debug warnings.
    auto tabList = publishedTabIndexes.load(std::memory_order_acquire);
    auto tabCount = publishedTabCount.load(std::memory_order_acquire);
    for (uint16_t i = 0; i < tabCount; ++i) {
        // Render threads are joined before shutdownSignal is set, so releasing the ACTIVE pages
        // (not just retired ones) is safe here and keeps the debug-layer live-object report clean.
        ReleaseTabGpuGeometry(allTabs[tabList[i]]);
        if (allTabs[tabList[i]].cad2d) {
            ReleaseCad2DRetiredResources(*allTabs[tabList[i]].cad2d);
        }
    }
    std::wcout << "GPU Copy Thread shutting down." << std::endl;
}
