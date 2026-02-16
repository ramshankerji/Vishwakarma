// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.

#include "MemoryManagerGPU-DirectX12.h"
#include "विश्वकर्मा.h"
#include <iomanip>

// Global Variables declared in विश्वकर्मा.cpp
extern शंकर gpu;

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

    //Copy thread is global. Hence it's variables are initialized here.
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY; // Distinct Copy Queue!
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&copyCommandQueue));
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&copyFence));
    copyFenceValue = 1;
    copyFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
}

// Implementation
void शंकर::InitD3DPerTab(DX12ResourcesPerTab& tabRes) {
    // Create Default Heap Properties
    // Now we will now pre-allocate large buffers that can be updated every frame.
    // --- Create Vertex Buffer Resources (Pre-allocation) ---
    auto defaultHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto uploadHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

    // Create Vertex Buffers (Jumbo)
    auto vbResourceDesc = CD3DX12_RESOURCE_DESC::Buffer(MaxVertexBufferSize);

    // Main Buffer (GPU Local). Create the main vertex buffer on the default heap (GPU-only access).
    ThrowIfFailed(gpu.device->CreateCommittedResource(
        &defaultHeapProps, D3D12_HEAP_FLAG_NONE,
        &vbResourceDesc, D3D12_RESOURCE_STATE_COMMON, // Starts Common
        nullptr, IID_PPV_ARGS(&tabRes.vertexBuffer)));

    // Upload Buffer (CPU Shared). Create an upload heap for the vertex buffer.
    ThrowIfFailed(gpu.device->CreateCommittedResource(
        &uploadHeapProps, D3D12_HEAP_FLAG_NONE,
        &vbResourceDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&tabRes.vertexBufferUpload)));

    // Create Index Buffers (Jumbo). Create Index Buffer Resources (Pre-allocation)
    auto ibResourceDesc = CD3DX12_RESOURCE_DESC::Buffer(MaxIndexBufferSize);

    // Create the main index buffer on the default heap.
    ThrowIfFailed(gpu.device->CreateCommittedResource(
        &defaultHeapProps, D3D12_HEAP_FLAG_NONE,
        &ibResourceDesc, D3D12_RESOURCE_STATE_COMMON,
        nullptr, IID_PPV_ARGS(&tabRes.indexBuffer)));

    // Create an upload heap for the index buffer.
    ThrowIfFailed(gpu.device->CreateCommittedResource(
        &uploadHeapProps, D3D12_HEAP_FLAG_NONE,
        &ibResourceDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&tabRes.indexBufferUpload)));

    // Map Persistent Pointers (Optimization)
    // We map once and keep it mapped for the lifetime of the Tab.
    // Persistently map the upload buffers. We won't unmap them until cleanup.
    // This is efficient as we avoid map/unmap calls every frame.
    CD3DX12_RANGE readRange(0, 0); // CPU won't read from these
    ThrowIfFailed(tabRes.vertexBufferUpload->Map(0, &readRange, reinterpret_cast<void**>(&tabRes.pVertexDataBegin)));
    ThrowIfFailed(tabRes.indexBufferUpload->Map(0, &readRange, reinterpret_cast<void**>(&tabRes.pIndexDataBegin)));

    // Initialize Views (To be updated later by Copy Thread/Render Thread as size changes)
    // Initialize the buffer views with default (but valid) values.
    // The sizes will be updated each frame in PopulateCommandList.
    tabRes.vertexBufferView.BufferLocation = tabRes.vertexBuffer->GetGPUVirtualAddress();
    tabRes.vertexBufferView.StrideInBytes = sizeof(Vertex);
    tabRes.vertexBufferView.SizeInBytes = 0; // Starts empty// Will be updated per frame

    tabRes.indexBufferView.BufferLocation = tabRes.indexBuffer->GetGPUVirtualAddress();
    tabRes.indexBufferView.Format = DXGI_FORMAT_R16_UINT;
    tabRes.indexBufferView.SizeInBytes = 0; // Starts empty// Will be updated per frame

    // Note: No Fence or Wait here. Resource creation is immediate.
    // Data upload sync happens in Copy Thread.
    std::cout << "Initialized Resources for Tab." << std::endl;
}

void शंकर::InitD3DPerWindow(DX12ResourcesPerWindow& dx, HWND hwnd, ID3D12CommandQueue* commandQueue) {
    int i = 0; // Latter to be iterated over number of screens.
    dx.creatorQueue = commandQueue; // Track which queue this windows was created with. To assist with migrations.

    // commandAllocator is per render thread (i.e. per monitor), not per window.
    // gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&dx.commandAllocator));

    // Create swap chain
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = FRAMES_PER_RENDERTARGETS;
    swapChainDesc.Width = dx.WindowWidth;
    swapChainDesc.Height = dx.WindowHeight;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;

    ComPtr<IDXGISwapChain1> tempSwapChain;
    factory6->CreateSwapChainForHwnd(commandQueue, hwnd, &swapChainDesc, nullptr, nullptr, &tempSwapChain);

    tempSwapChain.As(&dx.swapChain);
    dx.frameIndex = dx.swapChain->GetCurrentBackBufferIndex();

    dx.viewport = CD3DX12_VIEWPORT( 0.0f, 0.0f, 
        static_cast<float>(dx.WindowWidth), static_cast<float>(dx.WindowHeight));
    dx.scissorRect = CD3DX12_RECT( 0, 0, dx.WindowWidth, dx.WindowHeight);

    // Create descriptor heap for render target views
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = FRAMES_PER_RENDERTARGETS; //One RTV per frame buffer for multi-buffering support
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    gpu.device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&dx.rtvHeap));

    dx.rtvDescriptorSize = gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // Create depth stencil descriptor heap
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    gpu.device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&dx.dsvHeap));

    // Create depth stencil buffer
    D3D12_CLEAR_VALUE depthOptimizedClearValue = {};
    depthOptimizedClearValue.Format = DXGI_FORMAT_D32_FLOAT;
    depthOptimizedClearValue.DepthStencil.Depth = 1.0f;
    depthOptimizedClearValue.DepthStencil.Stencil = 0;

    auto depthHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto depthResourceDesc = CD3DX12_RESOURCE_DESC::Tex2D(
        DXGI_FORMAT_D32_FLOAT, dx.WindowWidth, dx.WindowHeight,
        1, 0, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);

    gpu.device->CreateCommittedResource(
        &depthHeapProps, D3D12_HEAP_FLAG_NONE,
        &depthResourceDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &depthOptimizedClearValue, IID_PPV_ARGS(&dx.depthStencilBuffer)
    );
    //Observe that depthStencilBuffer has been created on D3D12_HEAP_TYPE_DEFAULT, i.e. main GPU memory
    //dsvHeap is created on D3D12_DESCRIPTOR_HEAP_TYPE_DSV. 
    //All DESCRIPTOR HEAPs are stored on Small Fast gpu memory. It is normal D3D12 design.

    gpu.device->CreateDepthStencilView(dx.depthStencilBuffer.Get(), nullptr,
        dx.dsvHeap->GetCPUDescriptorHandleForHeapStart());

    // Create render target views
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(dx.rtvHeap->GetCPUDescriptorHandleForHeapStart());
    for (UINT j = 0; j < FRAMES_PER_RENDERTARGETS; j++) {
        dx.swapChain->GetBuffer(j, IID_PPV_ARGS(&dx.renderTargets[j]));
        gpu.device->CreateRenderTargetView(dx.renderTargets[j].Get(), nullptr, rtvHandle);
        rtvHandle.Offset(1, dx.rtvDescriptorSize);
    }

    // CREATE RENDER TEXTURES
    D3D12_DESCRIPTOR_HEAP_DESC rttRtvHeapDesc = {};
    rttRtvHeapDesc.NumDescriptors = FRAMES_PER_RENDERTARGETS;
    rttRtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    gpu.device->CreateDescriptorHeap(&rttRtvHeapDesc, IID_PPV_ARGS(&dx.rttRtvHeap));
    dx.rtvDescriptorSize = gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC rttSrvHeapDesc = {};
    rttSrvHeapDesc.NumDescriptors = FRAMES_PER_RENDERTARGETS;
    rttSrvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    rttSrvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    gpu.device->CreateDescriptorHeap(&rttSrvHeapDesc, IID_PPV_ARGS(&dx.rttSrvHeap));

    CD3DX12_CPU_DESCRIPTOR_HANDLE rttRtvHandle(dx.rttRtvHeap->GetCPUDescriptorHandleForHeapStart());
    CD3DX12_CPU_DESCRIPTOR_HANDLE rttSrvHandle(dx.rttSrvHeap->GetCPUDescriptorHandleForHeapStart());
    UINT cbvSrvUavDescriptorSize = gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	//Gemini placed clearValue  / texDesc outside the loop. ChatGPT placed it inside the loop. 
    //Since clearValue doesn't change per iteration, we can optimize by defining it once outside the loop.
    auto texDesc = CD3DX12_RESOURCE_DESC::Tex2D(dx.rttFormat, dx.WindowWidth, dx.WindowHeight,
        1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    D3D12_CLEAR_VALUE clearValue{ .Format = dx.rttFormat, .Color = {0.0f, 0.2f, 0.4f, 1.0f} }; //C++20 allows this beauty!

    for (UINT i = 0; i < FRAMES_PER_RENDERTARGETS; i++) {
        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        gpu.device->CreateCommittedResource( // Create the Resource in RENDER_TARGET state by default
            &heapProps, D3D12_HEAP_FLAG_NONE,  &texDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearValue, IID_PPV_ARGS(&dx.renderTextures[i]) );

        gpu.device->CreateRenderTargetView(dx.renderTextures[i].Get(), nullptr, rttRtvHandle); // Create RTV
        rttRtvHandle.Offset(1, gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV));

        // Create SRV (For passing into Pixel Shader later). Can we create this struct also outside the loop ?
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = dx.rttFormat;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;

        gpu.device->CreateShaderResourceView(dx.renderTextures[i].Get(), &srvDesc, rttSrvHandle);
        //rttSrvHandle.Offset(1, gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV));//ChatGPT5.2
        rttSrvHandle.Offset(1, cbvSrvUavDescriptorSize); //Gemini 3 Pro
    }

    // Create root signature with constant buffer
    D3D12_FEATURE_DATA_ROOT_SIGNATURE featureData = {};
    featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
    //Check if the GPU supports Root Signature version 1.1 (newer, more efficient) or fall back to 1.0.
    if (FAILED(gpu.device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &featureData, sizeof(featureData)))) {
        featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
    }

    CD3DX12_DESCRIPTOR_RANGE1 ranges[1] = {};
    ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV,
        1, // There will be total 1 Nos. of Constant Buffer Descriptors passed to shaders.
        0, // Base shader register i.e register(b0) in HLSL
        0, // Register space 0. Since we are greenfield project, we don't need separate space 1
        D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC); //Optimization hint - data doesn't change often

    CD3DX12_ROOT_PARAMETER1 rootParameters[1] = {}; //1: Number of descriptor ranges in this table
    rootParameters[0].InitAsDescriptorTable(1, &ranges[0], D3D12_SHADER_VISIBILITY_VERTEX);

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
        IID_PPV_ARGS(&dx.rootSignature));

    /* Note that The root signature is like a function declaration. It defines the interface but doesn't
    contain actual data. Now that we have declared the data layout, time to prepare for movement
    of the data from CPU RAM to GPU RAM. This way we update the constant every frame without
    changing the root signature. */

    // Create constant buffer descriptor heap
    D3D12_DESCRIPTOR_HEAP_DESC cbvHeapDesc = {};
    cbvHeapDesc.NumDescriptors = 1;
    cbvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    cbvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    gpu.device->CreateDescriptorHeap(&cbvHeapDesc, IID_PPV_ARGS(&dx.cbvHeap));

    // Create constant buffer
    /* Our HLSL constant buffer contains: float4x4 worldViewProjection; 64 bytes (4x4 floats = 16*4=64)
    float4x4 world; 64 bytes. Total: 128 bytes, but padded to 256 bytes for D3D12 alignment requirements */
    auto cbHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto cbResourceDesc = CD3DX12_RESOURCE_DESC::Buffer(256); // Constant buffers must be 256-byte aligned

    gpu.device->CreateCommittedResource(&cbHeapProps, D3D12_HEAP_FLAG_NONE,
        &cbResourceDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&dx.constantBuffer));

    // Create constant buffer view
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
    cbvDesc.BufferLocation = dx.constantBuffer->GetGPUVirtualAddress();
    cbvDesc.SizeInBytes = 256;
    gpu.device->CreateConstantBufferView(&cbvDesc, dx.cbvHeap->GetCPUDescriptorHandleForHeapStart());

    // Map constant buffer i.e. Map Constant Buffer for CPU Access
    CD3DX12_RANGE readRange(0, 0); //CPU won't read from this buffer (write-only optimization)
    dx.constantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&dx.cbvDataBegin));

    // Create the shader
    ComPtr<ID3DBlob> vertexShader;
    ComPtr<ID3DBlob> pixelShader;

#if defined(_DEBUG)
    UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    UINT compileFlags = 0;
#endif

    /* 3D shader code with matrix transformations.
    Shaders are like a mini sub-program, which runs on the GPU FOR EACH VERTEX. Massively parallel.
    In the following shader code, we do only 1 transformation: Transform the vertex 3D co-ordinate
    to screen co-ordinate. Color is passed forward as it is without change.
    TODO: In future, we will implement index color system using some transformation here. */
    static const char* const vertexShaderCode = R"(
        cbuffer ConstantBuffer : register(b0) {
            float4x4 worldViewProjection;
            float4x4 world;
        };

        struct PSInput {
            float4 position : SV_POSITION;
            float4 color : COLOR;
            float3 normal : NORMAL;
        };

        PSInput VSMain(float3 position : POSITION, float4 normal : NORMAL, float4 color : COLOR)
        {
            PSInput result;
            // Transform position to homogeneous clip space
            result.position = mul(float4(position, 1.0f), worldViewProjection);

            // Transform normal to world space
            // Note: If 'world' contains non-uniform scaling, we should use the inverse-transpose.
            // For now, assuming uniform scaling, casting to float3x3 works.
            result.normal = mul(normal.xyz, (float3x3)world);

            result.color = color;
            return result;
        }
    )";

    /* Simple pass - through shader that outputs the interpolated vertex color for each pixel
    No lighting calculations or texture sampling - just renders solid colors */
    static const char* const pixelShaderCode = R"(
        struct PSInput {
            float4 position : SV_POSITION;
            float4 color : COLOR;
            float3 normal : NORMAL;
        };

        float4 PSMain(PSInput input) : SV_TARGET {
            float3 norm = normalize(input.normal);// Re-normalize interpolants

            // Hemispherical Lighting Settings
            float3 up = float3(0.0f, 0.0f, 1.0f); // World Up
            // Sky: Bright, slightly bluish white (multiplies your vertex color by ~0.95)
            float3 skyColor = float3(0.9f, 0.95f, 1.0f);
            // Ground: Mid-grey (multiplies by ~0.4). This ensures the shadowed parts are still visible, not pitch black.
            float3 groundColor = float3(0.4f, 0.4f, 0.45f);

            // Calculate blend factor [-1, 1] -> [0, 1] . Dot product gives 1.0 facing up, -1.0 facing down.
            float t = 0.5f * (dot(norm, up) + 1.0f);
            float3 ambientLight = lerp(groundColor, skyColor, t); // Interpolate lighting
            return float4(input.color.rgb * ambientLight, input.color.a); // Apply lighting to surface color
        }
    )";

    D3DCompile(vertexShaderCode, strlen(vertexShaderCode), nullptr, nullptr, nullptr,
        "VSMain", "vs_5_0", compileFlags, 0, &vertexShader, nullptr);
    D3DCompile(pixelShaderCode, strlen(pixelShaderCode), nullptr, nullptr, nullptr,
        "PSMain", "ps_5_0", compileFlags, 0, &pixelShader, nullptr);

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
    psoDesc.pRootSignature = dx.rootSignature.Get();
    psoDesc.VS = CD3DX12_SHADER_BYTECODE(vertexShader.Get());
    psoDesc.PS = CD3DX12_SHADER_BYTECODE(pixelShader.Get());
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT); //(default = replace)
    psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT); // Enable depth testing
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT; // Set depth stencil format
    psoDesc.SampleDesc.Count = 1;
    gpu.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&dx.pipelineState));
    vertexShader.Reset(); //Release memory once we have created pipelineState.
    pixelShader.Reset();  //Release memory suggested by Claude code review.
    signature.Reset();
    if (error) error.Reset();

    // Upto this point, setting of Graphics Engine is complete. Now we generate the actual 
    
	// DO WE NEED THIS ? Is GPU INITIALIZED flag needed ?
    //WaitForPreviousFrame(dx);// Wait for initialization to complete
}

void शंकर::PopulateCommandList(ID3D12GraphicsCommandList* commandList,
    DX12ResourcesPerWindow& winRes, const DX12ResourcesPerTab& tabRes) {
    //int i = 0; // Latter to be iterated over number of screens.
    // Update constant buffer with transformation matrices

    // Create view matrix (camera looking at scene from distance)
    XMVECTOR eyePosition = XMLoadFloat3(&tabRes.camera.position);
    XMVECTOR focusPoint = XMLoadFloat3(&tabRes.camera.target);
    XMVECTOR upDirection = XMLoadFloat3(&tabRes.camera.up);
    DirectX::XMMATRIX viewMatrix = DirectX::XMMatrixLookAtLH(eyePosition, focusPoint, upDirection);

    // Create projection matrix
    float aspectRatio = static_cast<float>(winRes.WindowWidth) / static_cast<float>(winRes.WindowHeight);

    XMMATRIX projectionMatrix =  XMMatrixPerspectiveFovLH(
        tabRes.camera.fov, aspectRatio, tabRes.camera.nearZ,  tabRes.camera.farZ );

    // Create world matrix with rotation. Now the camera rotates, not the world !
    DirectX::XMMATRIX worldMatrix = DirectX::XMMatrixIdentity();

    // Combine matrices
    DirectX::XMMATRIX worldViewProjectionMatrix = worldMatrix * viewMatrix * projectionMatrix;

    // Update constant buffer
    ConstantBuffer constantBufferData;
    DirectX::XMStoreFloat4x4(&constantBufferData.worldViewProjection, DirectX::XMMatrixTranspose(worldViewProjectionMatrix));
    DirectX::XMStoreFloat4x4(&constantBufferData.world, DirectX::XMMatrixTranspose(worldMatrix));

    memcpy(winRes.cbvDataBegin, &constantBufferData, sizeof(constantBufferData));

    // Set necessary state
    commandList->SetGraphicsRootSignature(winRes.rootSignature.Get());
    commandList->SetPipelineState(winRes.pipelineState.Get());

    // Set descriptor heaps
    ID3D12DescriptorHeap* ppHeaps[] = { winRes.cbvHeap.Get() };
    commandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);

    // Set root descriptor table
    commandList->SetGraphicsRootDescriptorTable(0, winRes.cbvHeap->GetGPUDescriptorHandleForHeapStart());

    // Create named variables (l‑values)
    CD3DX12_VIEWPORT viewport(0.0f, 0.0f,
        static_cast<float>(winRes.WindowWidth),
        static_cast<float>(winRes.WindowHeight)
    );

    CD3DX12_RECT scissorRect(0, 0, winRes.WindowWidth, winRes.WindowHeight);

    // Now you can take their addresses and call the methods
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissorRect);

    /* Transition from D3D12_RESOURCE_STATE_PRESENT to D3D12_RESOURCE_STATE_RENDER_TARGET
    Already done by parent function i.e. Render Thread.*/

    // Record commands
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(winRes.rtvHeap->GetCPUDescriptorHandleForHeapStart(),
        winRes.frameIndex, winRes.rtvDescriptorSize);
    CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle(winRes.dsvHeap->GetCPUDescriptorHandleForHeapStart());
	//commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle); //Removed. Already done by GpuRenderThread.

    // Clear render target and depth stencil
    const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f }; // Example color, adjust as needed
    //commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr); //Removed. Already done by GpuRenderThread.
    commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Lock and Iterate through GPU resources to draw them.
    // Lock the TAB'S mutex and check the TAB'S map.
    std::lock_guard<std::mutex> lock(tabRes.objectsOnGPUMutex);

    // Bind TAB Geometry (Vertex/Index Buffers). IMPORTANT: Verify the tab has valid buffers before binding!
    // TODO : vertexDataSize not yet populated! Removes reliance on "vertexDataSize" which is currently always 0.
    if (!tabRes.objectsOnGPU.empty()) {// Check if the map is empty.
        // Use the Jumbo Buffer if available (currently unused, but good to keep the binding logic)
        if (tabRes.vertexDataSize > 0) {
            commandList->IASetVertexBuffers(0, 1, &tabRes.vertexBufferView);
            commandList->IASetIndexBuffer(&tabRes.indexBufferView);
        }

        for (const auto& pair : tabRes.objectsOnGPU)
        {
            // TODO: In the future, you will filter this list based on "Visible in this View"
            // uint64_t id = pair.first; // Unused variable warning fix
            const GpuResourceVertexIndexInfo& res = pair.second;
            if (res.vertexBufferView.SizeInBytes > 0) {
                commandList->IASetVertexBuffers(0, 1, &res.vertexBufferView);
                commandList->IASetIndexBuffer(&res.indexBufferView);
                commandList->DrawIndexedInstanced(res.indexCount, 1, 0, 0, 0);// 2. Draw
            }
        }
    }

    /* Transition from D3D12_RESOURCE_STATE_RENDER_TARGET to D3D12_RESOURCE_STATE_RENDER_PRESENT
    taken care by parent function. i.e. Render Thread */
    //commandList->Close();// No longer required here. It will be done by render thread.
}

void शंकर::WaitForPreviousFrame(DX12ResourcesPerRenderThread dx) {
    // Signal and increment the fence value
    const UINT64 currentFenceValue = dx.fenceValue;
    //Tells the GPU command queue to "signal" (mark) the fence with the current fence value when it 
    //finishes executing all previously submitted commands.
    dx.commandQueue->Signal(dx.fence.Get(), currentFenceValue);
    dx.fenceValue++;

    // Wait until the previous frame is finished
    if (dx.fence->GetCompletedValue() < currentFenceValue) {
        dx.fence->SetEventOnCompletion(currentFenceValue, dx.fenceEvent);
        //This is the one halting CPU thread till fence completes.
        WaitForSingleObject(dx.fenceEvent, INFINITE);
    }

    // Update the frame index. Now done by render thead itself.
    // dx.frameIndex = dx.swapChain->GetCurrentBackBufferIndex();
}

void शंकर::CleanupWindowResources(DX12ResourcesPerWindow& winRes) {
    // Wait for GPU to finish with this window's current frame
    if (winRes.cbvDataBegin) { // Unmap Window-Specific Constant Buffers
        winRes.constantBuffer->Unmap(0, nullptr);
        winRes.cbvDataBegin = nullptr;
    }

    // Release Window-Specific D3D Objects
    // Smart Pointers (ComPtr) will automatically Release() when reset.
    // We explicitly reset them to ensure deterministic destruction order.

    // SwapChain & Targets
    winRes.swapChain.Reset();
    for (int i = 0; i < FRAMES_PER_RENDERTARGETS; ++i) {
        winRes.renderTargets[i].Reset();
    }
    winRes.rtvHeap.Reset();

    // Depth Buffer
    winRes.depthStencilBuffer.Reset();
    winRes.dsvHeap.Reset();

    // Pipeline Objects (Specific to this window context)
    winRes.constantBuffer.Reset();
    winRes.cbvHeap.Reset();
    winRes.rootSignature.Reset();
    winRes.pipelineState.Reset();

    std::cout << "Cleaned up Window Resources." << std::endl;
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
    tabRes.vertexBuffer.Reset();
    tabRes.indexBuffer.Reset();
    tabRes.vertexBufferUpload.Reset();
    tabRes.indexBufferUpload.Reset();
    tabRes.srvHeap.Reset();

    // Reset Size trackers
    tabRes.vertexDataSize = 0;
    tabRes.indexDataSize = 0;

    std::cout << "Cleaned up Tab Geometry Resources." << std::endl;
}

void शंकर::CleanupD3DGlobal() {
    // 1. Sync and Cleanup Copy Engine
    if (copyCommandQueue && copyFence) {
        // Simple wait: Signal and wait for it
        copyCommandQueue->Signal(copyFence.Get(), copyFenceValue);
        if (copyFence->GetCompletedValue() < copyFenceValue) {
            copyFence->SetEventOnCompletion(copyFenceValue, copyFenceEvent);
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

    std::cout << "Global D3D Shutdown Complete." << std::endl;
}

// Free memory that is guaranteed to be no longer in use by any rendering frame.
void शंकर::ProcessDeferredFrees(uint64_t lastCompletedRenderFrame) {
    // A real implementation would be more robust. This is a simple version.
    // Free any resource that became obsolete >= 2 frames ago.
    auto it = deferredFreeQueue.begin();
    while (it != deferredFreeQueue.end()) {
        if (it->frameNumber <= lastCompletedRenderFrame) {
            //std::cout << "VRAM MANAGER: Reclaiming " << it->resource.size << " bytes." << std::endl;
            // In a real allocator, this space would be added to a free list.
            // In our simple bump allocator, we can't easily reuse it without compaction.
            it = deferredFreeQueue.erase(it);
        }
        else {
            ++it;
        }
    }
}

void GpuCopyThread() {
    std::cout << "GPU Copy Thread started." << std::endl;
    uint64_t lastProcessedFrame = -1;

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
    while (!shutdownSignal) {
        CommandToCopyThread cmd;
        {
            std::unique_lock<std::mutex> lock(toCopyThreadMutex);
            toCopyThreadCV.wait(lock, [] { return !commandToCopyThreadQueue.empty() || shutdownSignal; });

            if (shutdownSignal && commandToCopyThreadQueue.empty()) break;

            cmd = commandToCopyThreadQueue.front();
            commandToCopyThreadQueue.pop();
        }

		// Find the targe tab. Our static array of tabs is thread-safe for reading.
        DATASETTAB& targetTab = allTabs[cmd.tabID];

        switch (cmd.type)// Process Command
        {
        case CommandToCopyThreadType::ADD:
        case CommandToCopyThreadType::MODIFY:
        {
            GeometryData geo = cmd.geometry.value();
            const UINT vertexBufferSize = static_cast<UINT>(geo.vertices.size() * sizeof(Vertex));
            const UINT indexBufferSize = static_cast<UINT>(geo.indices.size() * sizeof(uint16_t));

            if (vertexBufferSize == 0 || indexBufferSize == 0) {
                std::cout << "Warning: Skipping upload of empty geometry ID " << cmd.id << std::endl;
                break; // Exit this case, process next command
            }

            GpuResourceVertexIndexInfo newResource;
            newResource.indexCount = static_cast<UINT>(geo.indices.size());

            // Create an upload heap to transfer data to the GPU.
            ComPtr<ID3D12Resource> vertexUploadHeap;
            ComPtr<ID3D12Resource> indexUploadHeap;

            // Create Destination Resources (Default Heap). Note: Created in COMMON state (implicit) or COPY_DEST
            CD3DX12_HEAP_PROPERTIES defaultHeapProps(D3D12_HEAP_TYPE_DEFAULT);
            CD3DX12_RESOURCE_DESC vertexBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(vertexBufferSize);
            gpu.device->CreateCommittedResource(&defaultHeapProps, D3D12_HEAP_FLAG_NONE, &vertexBufferDesc,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&newResource.vertexBuffer));

            // Create the index buffer resource on the default heap.
            CD3DX12_RESOURCE_DESC indexBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(indexBufferSize);
            gpu.device->CreateCommittedResource(&defaultHeapProps, D3D12_HEAP_FLAG_NONE, &indexBufferDesc,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&newResource.indexBuffer));

            // Create Source Resources (Upload Heap). This is in CPU RAM.
            CD3DX12_HEAP_PROPERTIES uploadHeapProps(D3D12_HEAP_TYPE_UPLOAD);
            // Upload heaps must be GENERIC_READ
            gpu.device->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &vertexBufferDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&vertexUploadHeap));
            gpu.device->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &indexBufferDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&indexUploadHeap));

            commandAllocator->Reset();// Record copy commands
            commandList->Reset(commandAllocator.Get(), nullptr);

            /* UpdateSubresources may introduce ResourceBarriers internally ! Which is not allowed on Copy Queues.
            with the raw copy command, which is safe because your buffers are already created
            in a valid state for copying (COMMON or GENERIC_READ).*/
            
            // Copy vertex data to the upload heap. Map the Upload Heap (CPU writes data)
            void* pVertexDataBegin;
            CD3DX12_RANGE readRange(0, 0); // We do not intend to read from this resource on the CPU.
            vertexUploadHeap->Map(0, &readRange, &pVertexDataBegin);
            memcpy(pVertexDataBegin, geo.vertices.data(), vertexBufferSize);
            vertexUploadHeap->Unmap(0, nullptr);

            void* pIndexDataBegin;
            indexUploadHeap->Map(0, &readRange, &pIndexDataBegin);
            memcpy(pIndexDataBegin, geo.indices.data(), indexBufferSize);
            indexUploadHeap->Unmap(0, nullptr);

            // Record the Copy Command (GPU copies from Upload -> Default)
            // Note: Destination is in COMMON, which is valid for CopyBufferRegion
            commandList->CopyBufferRegion(newResource.vertexBuffer.Get(), 0, 
                vertexUploadHeap.Get(), 0, vertexBufferSize);
            commandList->CopyBufferRegion(newResource.indexBuffer.Get(), 0, 
                indexUploadHeap.Get(), 0, indexBufferSize);

            /*DirectX 12 has a feature called "Implicit State Promotion". 
            If a Buffer is in COMMON state, and you bind it as a Vertex Buffer (Read-Only), 
            the driver automatically promotes it to the correct state without you issuing a barrier.*/
            // ResourceBarriers are REMOVED here. Copy Queues cannot execute barriers.
            // The Render Thread will handle the transition from COPY_DEST/COMMON to VERTEX_BUFFER when it draws.

            commandList->Close();

            ID3D12CommandList* ppCommandLists[] = { commandList.Get() };
            gpu.copyCommandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

            // Synchronization (Critical) 
            // We must wait for the copy to finish before 'vertexUploadHeap' goes out of scope.
            // In a more advanced engine, we would use a ring buffer, but here we block.
            UINT64 fenceToWaitFor = gpu.copyFenceValue;
            gpu.copyCommandQueue->Signal(gpu.copyFence.Get(), fenceToWaitFor);
            gpu.copyFenceValue++; // Increment for next time

            // B. Wait on the CPU side
            if (gpu.copyFence->GetCompletedValue() < fenceToWaitFor) {
                gpu.copyFence->SetEventOnCompletion(fenceToWaitFor, gpu.copyFenceEvent);
                WaitForSingleObject(gpu.copyFenceEvent, INFINITE);// Wait for the copy to complete
            }

            // Finalize and make available to Render thread
            newResource.vertexBufferView.BufferLocation = newResource.vertexBuffer->GetGPUVirtualAddress();
            newResource.vertexBufferView.StrideInBytes = sizeof(Vertex);
            newResource.vertexBufferView.SizeInBytes = vertexBufferSize;

            newResource.indexBufferView.BufferLocation = newResource.indexBuffer->GetGPUVirtualAddress();
            newResource.indexBufferView.Format = DXGI_FORMAT_R16_UINT;
            newResource.indexBufferView.SizeInBytes = indexBufferSize;

            {
                std::lock_guard<std::mutex> lock(targetTab.dx.objectsOnGPUMutex);
                targetTab.dx.objectsOnGPU[cmd.id] = newResource; // This will add or overwrite
            }
            break;
        }
        case CommandToCopyThreadType::REMOVE:
        {
            std::lock_guard<std::mutex> lock(targetTab.dx.objectsOnGPUMutex); // Remove from the specific tab
            targetTab.dx.objectsOnGPU.erase(cmd.id);
            break;
        }
        } // End of switch (cmd.type)// Process Command
    }

    std::cout << "GPU Copy Thread shutting down." << std::endl;
}

void GpuRenderThread(int monitorId, int refreshRate) {
    // Our architecture is 1 GPU Render thead per Monitor. 
    std::cout << "Render Thread (Monitor " << monitorId << ", " << refreshRate << "Hz) started." << std::endl;
    
	// Ge the persistent Command Queue for this monitor. Do NOT create a new queue.
    ID3D12CommandQueue* pCommandQueue = gpu.screens[monitorId].commandQueue.Get();

    // Initialize Thread-Local Resources (CommandQueue/Allocator/CommandList)
	// Command queue is per monitor to enable different refresh rate monitors to operate independently.
    // Otherwise present on slower monitor would block present on faster monitor.
    DX12ResourcesPerRenderThread threadRes;

    // We just store the pointer for convenience, we don't own it (ComPtr assignment adds ref)
    threadRes.commandQueue = pCommandQueue;
    
    // Create one allocator per frame-in-flight (Double Buffering)
    for (UINT i = 0; i < FRAMES_PER_RENDERTARGETS; i++) {
        ThrowIfFailed(gpu.device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&threadRes.commandAllocators[i])
        ));
    }
    // Create the Command List (Only 1 needed, we reset it repeatedly)
    ThrowIfFailed(gpu.device->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        threadRes.commandAllocators[0].Get(),
        nullptr, // Pipeline state set later
        IID_PPV_ARGS(&threadRes.commandList)
    ));

    // Command lists are created in the recording state, but our loop expects them closed initially.
    threadRes.commandList->Close();

    uint64_t lastRenderedFrame = -1;
    const auto frameDuration = std::chrono::milliseconds(1000 / refreshRate);

    // Create synchronization objects
    ThrowIfFailed(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&threadRes.fence)));
    threadRes.fenceValue = 1;
    threadRes.fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    while (!shutdownSignal && !pauseRenderThreads) {
        auto frameStart = std::chrono::high_resolution_clock::now();
        
		// Reset Thread Allocator & List.We must do this outside the window loop,
		// since we send the command list of all windows in one go to the GPU, to render all windows on this monitor.
        auto& allocator = threadRes.commandAllocators[threadRes.allocatorIndex];
        allocator->Reset();
        threadRes.commandList->Reset(allocator.Get(), nullptr); // Pass 'nullptr' for the PSO here so we don't enforce a state yet.

        bool didRender = false;

        uint16_t* windowList = publishedWindowIndexes.load(std::memory_order_acquire);
        uint16_t windowCount = publishedWindowCount.load(std::memory_order_acquire);

        for (uint16_t wi = 0; wi < windowCount; ++wi) {
            SingleUIWindow& window = allWindows[windowList[wi]];

            if (window.currentMonitorIndex != monitorId) continue; // Skip the windows not on this monitor.
            // The Safety Switch: If migrating, pretend this window doesn't exist for now.
            if (window.isMigrating) continue;
            if (window.isResizing) continue;
            
            // TODO: Ideally, it should be handled in WM_MOVE or nearby. Following is simply safeguard for bugs elsewhere.
            // Check if the window is physically on this monitor, but chemically bound to another queue
            if (window.dx.swapChain && window.dx.creatorQueue != threadRes.commandQueue.Get()) {
                std::cout << "Monitor Mismatch detected! Recreating SwapChain for new Queue." << std::endl;
                // Ensure the GPU is done with the OLD queue resources before destroying them
                // (In a production engine, you would use a fence wait here on the OLD queue)
                gpu.WaitForPreviousFrame(threadRes);
                gpu.CleanupWindowResources(window.dx);// Clean up resources tied to the old queue
                // Re-initialize resources on the CURRENT thread's queue
                // Note: We assume 'window.hwnd' is accessible here. If not, add it to SingleUIWindow struct.
                gpu.InitD3DPerWindow(window.dx, window.hWnd, threadRes.commandQueue.Get());
            }

            DX12ResourcesPerWindow& winRes = window.dx;// Get Window Resources (Swap chain, RTV)

            // CONTEXT SWITCHING. Set the Viewport/Scissor for THIS window (Critical!)
            threadRes.commandList->RSSetViewports(1, &winRes.viewport);
            threadRes.commandList->RSSetScissorRects(1, &winRes.scissorRect);
            
            auto barrierStart = CD3DX12_RESOURCE_BARRIER::Transition( winRes.renderTextures[winRes.frameIndex].Get(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            threadRes.commandList->ResourceBarrier(1, &barrierStart);

            CD3DX12_CPU_DESCRIPTOR_HANDLE rttHandle( winRes.rttRtvHeap->GetCPUDescriptorHandleForHeapStart(),
                winRes.frameIndex, gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV));
            CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle( winRes.rtvHeap->GetCPUDescriptorHandleForHeapStart(),
                winRes.frameIndex, winRes.rtvDescriptorSize);
            CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle( winRes.dsvHeap->GetCPUDescriptorHandleForHeapStart());

            threadRes.commandList->OMSetRenderTargets(1, &rttHandle, FALSE, &dsvHandle);

            const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f };// Clear
            threadRes.commandList->ClearRenderTargetView(rttHandle, clearColor, 0, nullptr);
            threadRes.commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

            // GET TAB DATA & RECORD GEOMETRY
			// Also Sets the Unique Root Signature for THIS window. Root Signature and Pipeline State are per-window.
			// In fact the Populate Command List can change it multiple times per window if needed.
            int tabIndex = window.activeTabIndex;

            if (tabIndex >= 0) {
                DATASETTAB& tab = allTabs[tabIndex];
                DX12ResourcesPerTab& tabRes = tab.dx;
                tabRes.camera = tab.camera; // Update camera from Tab.
                gpu.PopulateCommandList(threadRes.commandList.Get(), winRes, tabRes);// Renders geometry.
            }

            // Transition RTT: PIXEL_SHADER_RESOURCE → COPY_SOURCE
            auto rttToCopySource = CD3DX12_RESOURCE_BARRIER::Transition( winRes.renderTextures[winRes.frameIndex].Get(),
                D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            threadRes.commandList->ResourceBarrier(1, &rttToCopySource);

            // Transition BackBuffer: PRESENT → COPY_DEST
            auto bbToCopyDest = CD3DX12_RESOURCE_BARRIER::Transition( winRes.renderTargets[winRes.frameIndex].Get(),
                    D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
            threadRes.commandList->ResourceBarrier(1, &bbToCopyDest);

            // Copy RTT → BackBuffer
            threadRes.commandList->CopyResource( winRes.renderTargets[winRes.frameIndex].Get(), // DEST
                winRes.renderTextures[winRes.frameIndex].Get()); // SRC

            // Transition BackBuffer: COPY_DEST → PRESENT
            auto bbToPresent = CD3DX12_RESOURCE_BARRIER::Transition( winRes.renderTargets[winRes.frameIndex].Get(),
                    D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
            threadRes.commandList->ResourceBarrier(1, &bbToPresent);

            // (Optional) Transition RTT back to SRV for next frame
            auto rttBackToSRV = CD3DX12_RESOURCE_BARRIER::Transition( winRes.renderTextures[winRes.frameIndex].Get(),
                    D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            threadRes.commandList->ResourceBarrier(1, &rttBackToSRV);

            didRender = true;
		} // End of loop over all windows on this monitor.

        // Close & Execute ONCE after recording all windows.
		// TODO: Future Optimization: Spawn separate thread for each recording command list of each window. Synchronization will be complex though.
        threadRes.commandList->Close();
        if (didRender) {
            ID3D12CommandList* ppCommandLists[] = { threadRes.commandList.Get() };
            threadRes.commandQueue->ExecuteCommandLists(1, ppCommandLists);

            // Present ALL windows and wait
            /*  The first parameter 1 enables VSync!This, tells the GPU to wait for the monitor's vertical blank interval before presenting the frame
            Synchronize frame presentation with the display's refresh rate. It Throttle application to match the monitor's Hz
            This is more energy efficient way. We are engineering application, not some 1st person shooter video game maximizing fps !
            Without VSync, it was going 650fps(FullHD) on Laptop GPU with 25 Pyramid only geometry.
            */
            uint16_t* windowList = publishedWindowIndexes.load(std::memory_order_acquire);
            uint16_t windowCount = publishedWindowCount.load(std::memory_order_acquire);

            for (uint16_t wi = 0; wi < windowCount; ++wi) {
                SingleUIWindow& window = allWindows[windowList[wi]];

                if (window.currentMonitorIndex != monitorId) continue;
                HRESULT hr = window.dx.swapChain->Present(1, 0);
                if (FAILED(hr)) { std::cerr << "Present failed: " << hr << std::endl; }
				// Do NOT Handle Fences here. It will be handled after all windows are presented.

                // Update THIS window's specific buffer index. Update the frame index immediately so the NEXT loop uses the correct buffer.
                window.dx.frameIndex = window.dx.swapChain->GetCurrentBackBufferIndex();
            }

            // SIGNAL the fence for the current frame
            const UINT64 currentFenceValue = threadRes.fenceValue;
            threadRes.commandQueue->Signal(threadRes.fence.Get(), currentFenceValue);

            // WAIT: Throttle CPU (Double Buffering Logic). We need to reuse the Command Allocator for the *next* frame.
            // If we have 2 allocators (Double Buffering), we must ensure the GPU is finished with Frame (Current - 1).
            if (threadRes.fenceValue >= FRAMES_PER_RENDERTARGETS) {
                const UINT64 fenceValueToWaitFor = threadRes.fenceValue - FRAMES_PER_RENDERTARGETS + 1;

                // If the GPU hasn't reached that point yet, we sleep the CPU thread.
                if (threadRes.fence->GetCompletedValue() < fenceValueToWaitFor) {
                    threadRes.fence->SetEventOnCompletion(fenceValueToWaitFor, threadRes.fenceEvent);
                    WaitForSingleObject(threadRes.fenceEvent, INFINITE);
                }
            }
            threadRes.fenceValue++; // ADVANCE: Prepare for the next loop
            // Update the Thread's allocator index (0 -> 1 -> 0 -> 1). This is separate from the window's back buffer index!
            threadRes.allocatorIndex = (threadRes.allocatorIndex + 1) % FRAMES_PER_RENDERTARGETS;
        }
        else { // Idle handling
            // If I have no windows, I just wait (maybe for a VSync or sleep). Maybe we are running without a monitor !
            // This fulfills "waiting for some windows to be dragged into it" ?
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }

#ifdef _DEBUG
        // Update FPS counter (Debug build only)
        // FPS calculation variables - add these as global or class members
        static auto lastFpsTime = std::chrono::high_resolution_clock::now();
        static int frameCount = 0;
        static const double FPS_REPORT_INTERVAL = 10.0; // Report every 10 seconds
        frameCount++;
        auto currentTime = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration<double>(currentTime - lastFpsTime).count();
        if (elapsed >= FPS_REPORT_INTERVAL) {
            double fps = frameCount / elapsed;
            std::wcout << L"FPS: " << std::fixed << std::setprecision(2) << fps
                << L" (" << frameCount << L" frames in "
                << std::setprecision(1) << elapsed << L" seconds)" << std::endl;
            frameCount = 0;// Reset counters
            lastFpsTime = currentTime;
        }
#endif

        // TODO: After rendering, perform garbage collection on VRAM. Currently lastRenderedFrame is always 1.
        // In a real engine, the last COMPLETED GPU frame is tracked via fences.
        if (lastRenderedFrame > 2) {
            gpu.ProcessDeferredFrees(lastRenderedFrame - 2);
        }
    }

	// Cleanup Thread-Local Resources. We cannot destroy resources currently being read by the GPU!
    const UINT64 fenceValueToWaitFor = threadRes.fenceValue;

    // If the GPU hasn't reached that point yet, we sleep the CPU thread.
    if (threadRes.fence->GetCompletedValue() < fenceValueToWaitFor) {
        threadRes.fence->SetEventOnCompletion(fenceValueToWaitFor, threadRes.fenceEvent);
        //At this cleanup stage, do not waith INFINITE. Otherwise we may get stuck waiting forever.
        DWORD waitResult = WaitForSingleObject(threadRes.fenceEvent, 5000);  // 5 sec timeout
        if (waitResult != WAIT_OBJECT_0) {
            std::cerr << "Render thead cleanup Fence wait timed out!\n" << std::endl;
            // Force exit or log
        }
    }

    // Close Synchronization Handles
    if (threadRes.fenceEvent) {
        CloseHandle(threadRes.fenceEvent);
        threadRes.fenceEvent = nullptr;
    }

    threadRes.commandQueue.Reset();// Command Objects cleanup.
    threadRes.fence.Reset();
    std::cout << "Render Thread (Monitor " << monitorId << ") shutting down.\n" << std::endl;
}

// Following function is currently called in main UI thread, latter this responsibility will be moved to Render thread.
void शंकर::ResizeD3DWindow(DX12ResourcesPerWindow& dx, UINT newWidth, UINT newHeight)
{
    if (!dx.swapChain) return;
    if (newWidth == 0 || newHeight == 0) return; // Minimized

    ComPtr<ID3D12Fence> resizeFence;// Wait for GPU to finish using current buffers
    ThrowIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&resizeFence)));
    dx.creatorQueue->Signal(resizeFence.Get(), 1);

    HANDLE hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (resizeFence->GetCompletedValue() < 1) {
        resizeFence->SetEventOnCompletion(1, hEvent);
        WaitForSingleObject(hEvent, INFINITE);
    }
    CloseHandle(hEvent);

    for (UINT i = 0; i < FRAMES_PER_RENDERTARGETS; ++i) dx.renderTargets[i].Reset();// Release old back buffers
	dx.depthStencilBuffer.Reset(); // Release old depth buffer
    for (UINT i = 0; i < FRAMES_PER_RENDERTARGETS; ++i) dx.renderTextures[i].Reset();// Release RTT textures

    DXGI_SWAP_CHAIN_DESC desc = {};
    dx.swapChain->GetDesc(&desc);
    ThrowIfFailed(dx.swapChain->ResizeBuffers( FRAMES_PER_RENDERTARGETS, // Resize swapchain buffers
        newWidth, newHeight, desc.BufferDesc.Format, desc.Flags));
    dx.frameIndex = dx.swapChain->GetCurrentBackBufferIndex();

    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(dx.rtvHeap->GetCPUDescriptorHandleForHeapStart());// Recreate RTVs
    for (UINT i = 0; i < FRAMES_PER_RENDERTARGETS; i++) {
        ThrowIfFailed(dx.swapChain->GetBuffer(i, IID_PPV_ARGS(&dx.renderTargets[i])));
        device->CreateRenderTargetView(dx.renderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.Offset(1, dx.rtvDescriptorSize);
    }

    // Recreate depth buffer
    D3D12_CLEAR_VALUE depthClear = {};
    depthClear.Format = DXGI_FORMAT_D32_FLOAT;
    depthClear.DepthStencil.Depth = 1.0f;
    depthClear.DepthStencil.Stencil = 0;
    auto depthHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto depthDesc = CD3DX12_RESOURCE_DESC::Tex2D( DXGI_FORMAT_D32_FLOAT,
        newWidth, newHeight, 1, 0, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);

    ThrowIfFailed(device->CreateCommittedResource( &depthHeapProps, D3D12_HEAP_FLAG_NONE,
        &depthDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear, IID_PPV_ARGS(&dx.depthStencilBuffer)));

    device->CreateDepthStencilView( dx.depthStencilBuffer.Get(), nullptr,
        dx.dsvHeap->GetCPUDescriptorHandleForHeapStart());

    // Recreate RTT Textures
    CD3DX12_CPU_DESCRIPTOR_HANDLE rttRtvHandle(dx.rttRtvHeap->GetCPUDescriptorHandleForHeapStart());
    CD3DX12_CPU_DESCRIPTOR_HANDLE rttSrvHandle(dx.rttSrvHeap->GetCPUDescriptorHandleForHeapStart());
    UINT rttRtvIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    UINT rttSrvIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CLEAR_VALUE clearValue{ .Format = dx.rttFormat, .Color = {0.0f, 0.2f, 0.4f, 1.0f} };

    for (UINT i = 0; i < FRAMES_PER_RENDERTARGETS; i++) {
        auto texDesc = CD3DX12_RESOURCE_DESC::Tex2D(dx.rttFormat,
            newWidth, newHeight, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        ThrowIfFailed(device->CreateCommittedResource(&heapProps,
            D3D12_HEAP_FLAG_NONE, &texDesc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            &clearValue, IID_PPV_ARGS(&dx.renderTextures[i])));
        device->CreateRenderTargetView(dx.renderTextures[i].Get(), nullptr, rttRtvHandle);// RTV
        rttRtvHandle.Offset(1, rttRtvIncrement);

        // SRV
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = dx.rttFormat;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(dx.renderTextures[i].Get(), &srvDesc, rttSrvHandle);
        rttSrvHandle.Offset(1, rttSrvIncrement);
    }

    dx.WindowWidth = newWidth; // Update stored dimensions
    dx.WindowHeight = newHeight;

    // Update viewport
    dx.viewport = CD3DX12_VIEWPORT(0.0f, 0.0f, static_cast<float>(newWidth), static_cast<float>(newHeight));
    dx.scissorRect = CD3DX12_RECT(0, 0, newWidth, newHeight);
}
