// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.

#include "MemoryManagerGPU-DirectX12.h"
#include "विश्वकर्मा.h"
#include <iomanip>
#include <unordered_set>

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
            // We will pack more data here in future, like time, animation parameters etc.
            float4x4 viewProj;
        };
        StructuredBuffer<float4x4> WorldMatrices : register(t0);
        cbuffer PerDraw : register(b1) { uint matrixIndex; };

        struct PSInput {
            float4 position : SV_POSITION;
            float4 color : COLOR;
            float3 normal : NORMAL;
        };

        PSInput VSMain(float3 position : POSITION, float4 normal : NORMAL, float4 color : COLOR)
        {
            PSInput result;
            float4x4 world = WorldMatrices[matrixIndex];
            //float4x4 world = float4x4( 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1); //Used for debugging.
            float4 worldPos = mul(float4(position, 1.0f), world);

            // Transform position to homogeneous clip space
            result.position = mul(worldPos, viewProj); // correct order with transposed matrices

            // Transform normal to world space
            // Note: If 'world' contains non-uniform scaling, we should use the inverse-transpose.
            // For now, assuming uniform scaling, casting to float3x3 works.
            // Normal (good enough for CAD; inverse-transpose later if non-uniform scale)
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

    // Helper: compile a shader and throw with the error message if it fails.
    auto CompileShader = []( const char* code, const char* entryPoint, const char* target,
        UINT flags, ComPtr<ID3DBlob>& outBlob) {
        ComPtr<ID3DBlob> errorBlob;
        HRESULT hr = D3DCompile( code, strlen(code),
            nullptr,    // source name (optional, used in error messages)
            nullptr,    // defines
            nullptr,    // includes
            entryPoint, target, flags, 0, &outBlob, &errorBlob);

        if (FAILED(hr)) {
            if (errorBlob) {
                // errorBlob contains the HLSL compiler's human-readable error text.
                std::string msg(static_cast<const char*>(errorBlob->GetBufferPointer()),
                    errorBlob->GetBufferSize());
                OutputDebugStringA(("[Shader Compile Error] " + msg).c_str());
                std::cerr << "[Shader Compile Error] " << msg << std::endl;
            }
            ThrowIfFailed(hr);  // Will throw HrException, unwinding the stack cleanly.
        }
    };

    CompileShader(vertexShaderCode, "VSMain", "vs_5_0", compileFlags, vertexShader);
    CompileShader(pixelShaderCode, "PSMain", "ps_5_0", compileFlags, pixelShader);
    /*
    D3DCompile(vertexShaderCode, strlen(vertexShaderCode), nullptr, nullptr, nullptr,
        "VSMain", "vs_5_0", compileFlags, 0, &vertexShader, nullptr);
    D3DCompile(pixelShaderCode, strlen(pixelShaderCode), nullptr, nullptr, nullptr,
        "PSMain", "ps_5_0", compileFlags, 0, &pixelShader, nullptr);
    */

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
    psoDesc.VS = CD3DX12_SHADER_BYTECODE(vertexShader.Get());
    psoDesc.PS = CD3DX12_SHADER_BYTECODE(pixelShader.Get());
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
    vertexShader.Reset(); //Release memory once we have created pipelineState.
    pixelShader.Reset();  //Release memory suggested by Claude code review.
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

    // Note: No Fence or Wait here. Resource creation is immediate.
    // Data upload sync happens in Copy Thread.
    std::wcout << "Initialized Resources for Tab." << std::endl;
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
    swapChainDesc.Format = gpu.rttFormat;
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
        rtvHandle.Offset(1, gpu.rtvDescriptorSize);
    }

    // CREATE RENDER TEXTURES
    D3D12_DESCRIPTOR_HEAP_DESC rttRtvHeapDesc = {};
    rttRtvHeapDesc.NumDescriptors = FRAMES_PER_RENDERTARGETS;
    rttRtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    gpu.device->CreateDescriptorHeap(&rttRtvHeapDesc, IID_PPV_ARGS(&dx.rttRtvHeap));

    D3D12_DESCRIPTOR_HEAP_DESC rttSrvHeapDesc = {};
    rttSrvHeapDesc.NumDescriptors = FRAMES_PER_RENDERTARGETS;
    rttSrvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    rttSrvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    gpu.device->CreateDescriptorHeap(&rttSrvHeapDesc, IID_PPV_ARGS(&dx.rttSrvHeap));

    CD3DX12_CPU_DESCRIPTOR_HANDLE rttRtvHandle(dx.rttRtvHeap->GetCPUDescriptorHandleForHeapStart());
    CD3DX12_CPU_DESCRIPTOR_HANDLE rttSrvHandle(dx.rttSrvHeap->GetCPUDescriptorHandleForHeapStart());
    UINT cbvSrvUavDescriptorSize = gpu.cbvSrvUavDescriptorSize;

	//Gemini placed clearValue  / texDesc outside the loop. ChatGPT placed it inside the loop. 
    //Since clearValue doesn't change per iteration, we can optimize by defining it once outside the loop.
    auto texDesc = CD3DX12_RESOURCE_DESC::Tex2D(gpu.rttFormat, dx.WindowWidth, dx.WindowHeight,
        1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    D3D12_CLEAR_VALUE clearValue{ .Format = gpu.rttFormat, .Color = {0.0f, 0.2f, 0.4f, 1.0f} }; //C++20 allows this beauty!

    for (UINT i = 0; i < FRAMES_PER_RENDERTARGETS; i++) {
        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        gpu.device->CreateCommittedResource( // Create the Resource in RENDER_TARGET state by default
            &heapProps, D3D12_HEAP_FLAG_NONE,  &texDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearValue, IID_PPV_ARGS(&dx.renderTextures[i]) );

        gpu.device->CreateRenderTargetView(dx.renderTextures[i].Get(), nullptr, rttRtvHandle); // Create RTV
        rttRtvHandle.Offset(1, gpu.rtvDescriptorSize);

        // Create SRV (For passing into Pixel Shader later). Can we create this struct also outside the loop ?
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = gpu.rttFormat;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;

        gpu.device->CreateShaderResourceView(dx.renderTextures[i].Get(), &srvDesc, rttSrvHandle);
        rttSrvHandle.Offset(1, cbvSrvUavDescriptorSize); //Gemini 3 Pro
    }

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

    // Upto this point, setting of Graphics Engine is complete. Now we generate the actual 
    
	// DO WE NEED THIS ? Is GPU INITIALIZED flag needed ?
    //WaitForPreviousFrame(dx);// Wait for initialization to complete
}

void शंकर::PopulateCommandList(ID3D12GraphicsCommandList* commandList,
    DX12ResourcesPerWindow& winRes, const DX12ResourcesPerTab& tabRes, TabGeometryStorage& storage) {
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
    XMMATRIX viewProj = viewMatrix * projectionMatrix;
    
    // Create world matrix with rotation. Now the camera rotates, not the world !
    // Update constant buffer
    ConstantBuffer constantBufferData;
    XMStoreFloat4x4(&constantBufferData.viewProj, XMMatrixTranspose(viewProj));
    memcpy(winRes.cbvDataBegin, &constantBufferData, sizeof(constantBufferData));
    
	// Root Signature: The maximum size of a root signature is 64 DWORDs. 1 DWORD = 4 bytes, so that's 256 bytes total.
    // Root constants: 1 DWORD, i.e. 32-bit values. Root descriptors(64 - bit GPU virtual addresses) cost 2 DWORDs each.
    // https://learn.microsoft.com/en-us/windows/win32/direct3d12/root-signature-limits
    commandList->SetGraphicsRootSignature(tabRes.rootSignature.Get());
    commandList->SetPipelineState(tabRes.pipelineState.Get());
    // Set root descriptor table. No longer used.
    // Bind directly using GPU Virtual Addresses!
    commandList->SetGraphicsRootConstantBufferView(0, winRes.constantBuffer->GetGPUVirtualAddress());
    commandList->SetGraphicsRootShaderResourceView(1, tabRes.worldMatrixBuffer->GetGPUVirtualAddress());

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
        winRes.frameIndex, gpu.rtvDescriptorSize);
    CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle(winRes.dsvHeap->GetCPUDescriptorHandleForHeapStart());
	//commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle); //Removed. Already done by GpuRenderThread.

    // Clear render target and depth stencil
    const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f }; // Example color, adjust as needed
    //commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr); //Removed. Already done by GpuRenderThread.
    commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // PAGE-BASED RENDERING (Solid Opaque Only)
    GeometryPageSnapshot* snapshot = storage.activeSnapshot.load(std::memory_order_acquire);
    if (snapshot) {
        for (GeometryPage* pagePtr : snapshot->pages) {
            GeometryPage& page = *pagePtr;
            if (!page.published.load(std::memory_order_acquire)) continue;
            if (page.indirectCount == 0) continue; //Some safety checks.
            if (page.vertexHead == 0) continue;
            if (page.indexTail == page.pageSize) continue;

            D3D12_VERTEX_BUFFER_VIEW vbv{};
            vbv.BufferLocation = page.buffer->GetGPUVirtualAddress();
            vbv.SizeInBytes = page.vertexHead;
            vbv.StrideInBytes = sizeof(Vertex);

            D3D12_INDEX_BUFFER_VIEW ibv{};
            ibv.BufferLocation = page.buffer->GetGPUVirtualAddress() + page.indexTail;
            ibv.SizeInBytes = page.pageSize - page.indexTail;
            ibv.Format = DXGI_FORMAT_R16_UINT;

            commandList->IASetVertexBuffers(0, 1, &vbv);
            commandList->IASetIndexBuffer(&ibv);

            commandList->ExecuteIndirect( tabRes.commandSignature.Get(),
                page.indirectCount, page.indirectBuffer.Get(), 0, nullptr, 0);
        }
    } // End of if (snapshot)
	// TODO: Add support for transparent pages with proper sorting and blending states.
    // TODO: Similarly for all varients of geometry, like wireframe, hugoObjects etc. all unique PSO.

    /* Transition from D3D12_RESOURCE_STATE_RENDER_TARGET to D3D12_RESOURCE_STATE_RENDER_PRESENT
    taken care by parent function. i.e. Render Thread */
    //commandList->Close();// No longer required here. It will be done by render thread.

	//Following mutex will release automatically when this function returns, i.e. when lock goes out of scope.
    //std::lock_guard<std::mutex> lock(tabRes.objectsOnGPUMutex);
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

    for (int i = 0; i < FRAMES_PER_RENDERTARGETS; ++i) {
        winRes.renderTextures[i].Reset();
    }
    winRes.rttRtvHeap.Reset();
    winRes.rttSrvHeap.Reset();

    std::wcout << "Cleaned up Window Resources." << std::endl;
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

    tabRes.rootSignature.Reset();
    tabRes.pipelineState.Reset();

    std::wcout << "Cleaned up Tab Geometry Resources." << std::endl;
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

std::unique_ptr<GeometryPage> CreateNewPage() //Do not make this static function. It accesses global gpu singleton.
{
    auto page = std::make_unique<GeometryPage>();
    page->pageSize = 4 * 1024 * 1024;
    page->indexTail = page->pageSize;

    CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
    auto desc = CD3DX12_RESOURCE_DESC::Buffer(page->pageSize);
    gpu.device->CreateCommittedResource( &heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&page->buffer));
    auto indirectDesc = CD3DX12_RESOURCE_DESC::Buffer(65536 * sizeof(IndirectCommand));
    gpu.device->CreateCommittedResource( &heap, D3D12_HEAP_FLAG_NONE, &indirectDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&page->indirectBuffer));

    return page;
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

        uint64_t currentRenderFence = gpu.renderFenceValue; // Tag with current render frame

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

    while (!shutdownSignal) {
        CommandToCopyThread cmd;

        // Make a local copy of all commands to process in this iteration, to minimize lock holding time.
        // TODO: Add throttling here ? Like only 100k commands are processed at once ? Or some % of GPU VRAM Capacity?
        std::vector<CommandToCopyThread> batch;
        {
            std::unique_lock<std::mutex> lock(toCopyThreadMutex);
            toCopyThreadCV.wait(lock, [] { return !commandToCopyThreadQueue.empty() || shutdownSignal; });

            while (!commandToCopyThreadQueue.empty()) {
                batch.push_back(std::move(commandToCopyThreadQueue.front()));
                commandToCopyThreadQueue.pop();
            }
        } // lock released here. We have a local batch of commands to process without holding the lock.
        if (shutdownSignal) break; // Exit if shutdown was signaled while waiting.

        std::unordered_set<GeometryPage*> affectedPages;
        std::unordered_map<GeometryPage*, std::unique_ptr<GeometryPage>> clonedPages;
        std::vector<std::unique_ptr<GeometryPage>> newPages;

        // Pass 1: Identify affected pages. We will clone these pages,
        //apply modifications to the clones, and then publish atomically.
        for (auto& cmd : batch) {
            if (cmd.type == CommandToCopyThreadType::ADD) continue; // handled later
            auto it = objectLocation.find(cmd.id);
            if (it != objectLocation.end()) affectedPages.insert(it->second.page);
        }

        //Pass 2: Clone Affected Pages (RCU copy)
        commandAllocator->Reset();
        commandList->Reset(commandAllocator.Get(), nullptr);

        for (GeometryPage* oldPage : affectedPages) {
            auto newPage = CreateNewPage();

            commandList->CopyResource(newPage->buffer.Get(), oldPage->buffer.Get());
            // TODO: Copy with defragmentation using metadata.
            commandList->CopyResource(newPage->indirectBuffer.Get(), oldPage->indirectBuffer.Get());

            newPage->objects = oldPage->objects;
            newPage->vertexHead = oldPage->vertexHead;
            newPage->indexTail = oldPage->indexTail;
            newPage->objectCount = oldPage->objectCount;
            newPage->version = oldPage->version + 1;

            clonedPages[oldPage] = std::move(newPage);
        }

        ThrowIfFailed(commandList->Close());
        ID3D12CommandList* lists[] = { commandList.Get() };
        gpu.copyCommandQueue->ExecuteCommandLists(1, lists);
        uint64_t fenceValue = gpu.copyFenceValue.fetch_add(1);
        gpu.copyCommandQueue->Signal(gpu.copyFence.Get(), fenceValue);

        if (gpu.copyFence->GetCompletedValue() < fenceValue) {
            // TODO: Can we delay this wait until just before we need to access the cloned pages ? 
            // This would allow some CPU-GPU parallelism.
            gpu.copyFence->SetEventOnCompletion(fenceValue, gpu.copyFenceEvent);
            WaitForSingleObject(gpu.copyFenceEvent, INFINITE);
        }

        // Pass 3 — Apply every command in the batch to the (already-cloned) pages.
        // Re-open the command list for Pass-3 GPU work (geometry uploads for ADD/MODIFY-grow cases).
        commandAllocator->Reset();
        commandList->Reset(commandAllocator.Get(), nullptr);

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
            return rec;
            };

        uint32_t matrixIndex; // Allocate a matrix slot (new object)
        XMMATRIX worldMat;

        for (auto& cmd : batch) { // Iterate over batch
            // Find the targe tab. Our static array of tabs is thread-safe for reading.
            DATASETTAB& targetTab = allTabs[cmd.tabID];
            DX12ResourcesPerTab& tabRes = targetTab.dx;
            TabGeometryStorage& storage = targetTab.geometry; // per-tab storage
            //GeometryData geo;

            switch (cmd.type)// Process Command
            {
            case CommandToCopyThreadType::ADD:
            {
                //std::wcout << "Adding New object ID: " << cmd.id << std::endl;
                if (!cmd.geometry.has_value()) break;
                const GeometryData& geo = cmd.geometry.value();

                const uint32_t vertexBytes = static_cast<uint32_t>(geo.vertices.size() * sizeof(Vertex));
                const uint32_t indexBytes =  static_cast<uint32_t>(geo.indices.size() * sizeof(uint16_t));
                if (vertexBytes == 0 || indexBytes == 0) {
                    std::wcout << "Warning: Skipping upload of empty geometry ID " << cmd.id << std::endl;
                    break; // Exit this case, process next command
                }

                // ADD is treated as MODIFY if object already exists anywhere.
                // (fall through by re-routing; handled in MODIFY case below)
                auto locIt = objectLocation.find(cmd.id);
                if (locIt != objectLocation.end()) { goto handle_modify; }

                if (!tabRes.freeMatrixSlots.empty()) {
                    matrixIndex = tabRes.freeMatrixSlots.back();
                    tabRes.freeMatrixSlots.pop_back();
                } else {
                    matrixIndex = tabRes.matrixCount++;
                    if (matrixIndex >= tabRes.matrixCapacity) matrixIndex = 0; // TODO: handle growth
                }

                // Copy transposed world matrix to upload buffer
                worldMat = XMLoadFloat4x4(&geo.worldMatrix);
                XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(
                    tabRes.pWorldMatrixDataBegin + matrixIndex * sizeof(XMFLOAT4X4)),
                    XMMatrixTranspose(worldMat));
                    
                //We will try to fit this geometry in the last page for better packing.
                // If it doesn't fit, we will create a new page.
                GeometryPage* dstPage = nullptr;// Find or create the destination page

                if (!newPages.empty() && !newPages.back()->IsFull(vertexBytes, indexBytes)) {
                    dstPage = newPages.back().get();// Reuse the last newly-created page if it still has room
                } else if (!storage.activePages.empty() &&
                    !storage.activePages.back()->IsFull(vertexBytes, indexBytes)) {
                    // The last active page has room — we need its clone.
                    // Clone it now if not already cloned (it wasn't in affectedPages because this is an ADD).
                    GeometryPage* lastActive = storage.activePages.back().get();

                    if (clonedPages.find(lastActive) == clonedPages.end()) {
                        auto cloned = CreateNewPage();
                        commandList->CopyResource(cloned->buffer.Get(), lastActive->buffer.Get());
                        commandList->CopyResource(cloned->indirectBuffer.Get(), lastActive->indirectBuffer.Get());
                        cloned->objects = lastActive->objects;
                        cloned->vertexHead = lastActive->vertexHead;
                        cloned->indexTail = lastActive->indexTail;
                        cloned->objectCount = lastActive->objectCount;
                        cloned->version = lastActive->version + 1;
                        clonedPages[lastActive] = std::move(cloned);
                    }
                    dstPage = clonedPages[lastActive].get();
                } else { // No room anywhere — allocate a brand-new page
                    newPages.push_back(CreateNewPage());
                    dstPage = newPages.back().get();
                }

                // Record the geometry upload into commandList
                auto rec = RecordGeometryUpload(dstPage, geo, matrixIndex);

                // Update page CPU state
                dstPage->objects.push_back(rec);
                dstPage->vertexHead = rec.vertexByteOffset + rec.vertexSize;
                dstPage->indexTail = rec.indexByteOffset;
                dstPage->objectCount++;

                // Update the copy-thread's private location map
                uint32_t slot = static_cast<uint32_t>(dstPage->objects.size() - 1);
                objectLocation[cmd.id] = { dstPage, slot };
                
                //std::wcout << "Added New object ID: " << cmd.id << std::endl;
                break;
            }

            case CommandToCopyThreadType::MODIFY:
            handle_modify: // This is GOTO jump from ADD thread, if the ID already existed.
            {
                if (!cmd.geometry.has_value()) break;
                const GeometryData& geo = cmd.geometry.value();

                const uint32_t newVertexBytes = static_cast<uint32_t>(geo.vertices.size() * sizeof(Vertex));
                const uint32_t newIndexBytes = static_cast<uint32_t>(geo.indices.size() * sizeof(uint16_t));

                if (newVertexBytes == 0 || newIndexBytes == 0) break;

                auto locIt = objectLocation.find(cmd.id);
                if (locIt == objectLocation.end()) {
                    // Object not yet on GPU — treat as a plain ADD
                    // (Re-route: set type to ADD and fall through next iteration
                    //  is not possible here, so we inline the ADD path.)
                    CommandToCopyThread addCmd = cmd;
                    addCmd.type = CommandToCopyThreadType::ADD;
                    // Push back to batch so it is processed below (safe because
                    // we only append and the range-for already captured its end).
                    // Simpler: just handle inline like ADD-new path.
                    if (!tabRes.freeMatrixSlots.empty()) {
                        matrixIndex = tabRes.freeMatrixSlots.back();
                        tabRes.freeMatrixSlots.pop_back();
                    } else {
                        matrixIndex = tabRes.matrixCount++;
                        if (matrixIndex >= tabRes.matrixCapacity)
                            matrixIndex = 0;
                    }
                    worldMat = XMLoadFloat4x4(&geo.worldMatrix);
                    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(tabRes.pWorldMatrixDataBegin +
                        matrixIndex * sizeof(XMFLOAT4X4)), XMMatrixTranspose(worldMat));

                    GeometryPage* dstPage = nullptr;
                    if (!newPages.empty() && !newPages.back()->IsFull(newVertexBytes, newIndexBytes))
                        dstPage = newPages.back().get();
                    else if (!storage.activePages.empty() &&
                        !storage.activePages.back()->IsFull(newVertexBytes, newIndexBytes)) {
                        GeometryPage* last = storage.activePages.back().get();
                        if (clonedPages.find(last) == clonedPages.end()) {
                            auto cloned = CreateNewPage();
                            commandList->CopyResource(cloned->buffer.Get(), last->buffer.Get());
                            commandList->CopyResource(cloned->indirectBuffer.Get(), last->indirectBuffer.Get());
                            cloned->objects = last->objects;
                            cloned->vertexHead = last->vertexHead;
                            cloned->indexTail = last->indexTail;
                            cloned->objectCount = last->objectCount;
                            cloned->version = last->version + 1;
                            clonedPages[last] = std::move(cloned);
                        }
                        dstPage = clonedPages[last].get();
                    } else {
                        newPages.push_back(CreateNewPage());
                        dstPage = newPages.back().get();
                    }
                    auto rec = RecordGeometryUpload(dstPage, geo, matrixIndex);
                    dstPage->objects.push_back(rec);
                    dstPage->vertexHead = rec.vertexByteOffset + rec.vertexSize;
                    dstPage->indexTail = rec.indexByteOffset;
                    dstPage->objectCount++;
                    objectLocation[cmd.id] = { dstPage, static_cast<uint32_t>(dstPage->objects.size() - 1) };
                    break;
                }

                // Object exists — work on its owning cloned page
                GeometryPage* oldPage = locIt->second.page;
                uint32_t      slotIndex = locIt->second.slot;

                // Resolve which mutable page we are working with: It will be in clonedPages (was in affectedPages 
                // from Pass 1), because Pass 1 already included pages from existing objectLocation.
                GeometryPage* workPage = nullptr;
                auto cloneIt = clonedPages.find(oldPage);
                if (cloneIt != clonedPages.end()) workPage = cloneIt->second.get();
                else workPage = oldPage; // Fallback (shouldn't happen in correct flow)

                GeometryPlacementRecordInPage& oldRec = workPage->objects[slotIndex];

                // Update world matrix (slot is reused)
                XMMATRIX worldMat = XMLoadFloat4x4(&geo.worldMatrix);
                XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(tabRes.pWorldMatrixDataBegin +
                    oldRec.matrixIndex * sizeof(XMFLOAT4X4)), XMMatrixTranspose(worldMat));

                const bool fitsInPlace = (newVertexBytes <= oldRec.vertexSize) && (newIndexBytes <= oldRec.indexSize);

                if (fitsInPlace) {
                    // MODIFY-shrink/equal: overwrite in-place . Upload new geometry directly over the old region
                    ComPtr<ID3D12Resource> upload;
                    CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
                    auto uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(newVertexBytes + newIndexBytes);
                    ThrowIfFailed(gpu.device->CreateCommittedResource( &uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)));

                    uint8_t* mapped = nullptr;
                    CD3DX12_RANGE readRange(0, 0);
                    upload->Map(0, &readRange, reinterpret_cast<void**>(&mapped));
                    memcpy(mapped, geo.vertices.data(), newVertexBytes);
                    memcpy(mapped + newVertexBytes, geo.indices.data(), newIndexBytes);
                    upload->Unmap(0, nullptr);

                    // Overwrite existing region on the cloned page
                    commandList->CopyBufferRegion(workPage->buffer.Get(), oldRec.vertexByteOffset, 
                        upload.Get(), 0, newVertexBytes);
                    commandList->CopyBufferRegion(workPage->buffer.Get(), oldRec.indexByteOffset,
                        upload.Get(), newVertexBytes, newIndexBytes);

                    pass3Uploads.push_back(std::move(upload));

                    // Update metadata (sizes may have shrunk; offsets unchanged)
                    oldRec.vertexSize = newVertexBytes;
                    oldRec.indexSize = newIndexBytes;
                    oldRec.indexCount = static_cast<uint32_t>(geo.indices.size());
                    // objectLocation slot index stays the same
                } else { // MODIFY-grow: mark old slot free, append to last page ─
                    oldRec.isDeleted = true;   // soft-delete in metadata
                    workPage->holeBytes += oldRec.vertexSize + oldRec.indexSize;
                    workPage->objectCount--;

                    GeometryPage* dstPage = nullptr;// Find (or create) a page with room for the larger geometry

                    // Try the last active page's clone first
                    if (!newPages.empty() && !newPages.back()->IsFull(newVertexBytes, newIndexBytes)) {
                        dstPage = newPages.back().get();
                    }
                    else if (!storage.activePages.empty()) {
                        GeometryPage* last = storage.activePages.back().get();
                        auto lastCloneIt = clonedPages.find(last);
                        GeometryPage* lastClone =
                            (lastCloneIt != clonedPages.end())
                            ? lastCloneIt->second.get()
                            : nullptr;

                        if (lastClone && !lastClone->IsFull(newVertexBytes, newIndexBytes)) dstPage = lastClone;
                    }

                    if (!dstPage) {
                        newPages.push_back(CreateNewPage());
                        dstPage = newPages.back().get();
                    }

                    uint32_t matrixIndex; // Allocate a fresh matrix slot for the relocated geometry
                    if (!tabRes.freeMatrixSlots.empty()) {
                        matrixIndex = tabRes.freeMatrixSlots.back();
                        tabRes.freeMatrixSlots.pop_back();
                    } else {
                        matrixIndex = tabRes.matrixCount++;
                        if (matrixIndex >= tabRes.matrixCapacity) matrixIndex = 0; // TODO: Overflow safety. Improve latter.
                    }
                    // Free the old matrix slot
                    tabRes.freeMatrixSlots.push_back(oldRec.matrixIndex);
                    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(tabRes.pWorldMatrixDataBegin +
                        matrixIndex * sizeof(XMFLOAT4X4)), XMMatrixTranspose(worldMat));

                    auto rec = RecordGeometryUpload(dstPage, geo, matrixIndex);
                    dstPage->objects.push_back(rec);
                    dstPage->vertexHead = rec.vertexByteOffset + rec.vertexSize;
                    dstPage->indexTail = rec.indexByteOffset;
                    dstPage->objectCount++;

                    objectLocation[cmd.id] = { dstPage, static_cast<uint32_t>(dstPage->objects.size() - 1) };
                }
                break;
            }

            case CommandToCopyThreadType::REMOVE:
            {
                auto locIt = objectLocation.find(cmd.id);
                if (locIt == objectLocation.end()) break; // not on GPU, nothing to do

                GeometryPage* oldPage = locIt->second.page;
                uint32_t      slotIndex = locIt->second.slot;

                // Resolve mutable clone
                GeometryPage* workPage = nullptr;
                auto cloneIt = clonedPages.find(oldPage);
                if (cloneIt != clonedPages.end()) workPage = cloneIt->second.get();
                else workPage = oldPage;

                GeometryPlacementRecordInPage& rec = workPage->objects[slotIndex];

                // Soft-delete: mark the slot; IndirectBuffer rebuild will skip it
                rec.isDeleted = true;
                workPage->holeBytes += rec.vertexSize + rec.indexSize;
                workPage->objectCount--;
                tabRes.freeMatrixSlots.push_back(rec.matrixIndex); // Free the matrix slot for reuse
                objectLocation.erase(locIt);// Remove from local bookkeeping
                break;
            }

            default: break;
            } // End of switch (cmd.type)// Process Command
        } // end for (batch)

        // Single GPU Execute for all Pass-3 geometry uploads
        ThrowIfFailed(commandList->Close());
        {
            ID3D12CommandList* lists[] = { commandList.Get() };
            gpu.copyCommandQueue->ExecuteCommandLists(1, lists);
        }
        {
            uint64_t fenceVal = gpu.copyFenceValue.fetch_add(1);
            gpu.copyCommandQueue->Signal(gpu.copyFence.Get(), fenceVal);
            if (gpu.copyFence->GetCompletedValue() < fenceVal) {
                gpu.copyFence->SetEventOnCompletion(fenceVal, gpu.copyFenceEvent);
                WaitForSingleObject(gpu.copyFenceEvent, INFINITE);
            }
        }
        pass3Uploads.clear();// Upload staging buffers are now safe to release

        // Rebuild Indirect Buffers (outside the per-command loop) Runs once per modified or new page, 
        // after all commands are applied. Only live objects (isDeleted == false) are emitted.
        // We rebuild into CPU staging, then do one more command record
        // + execute (or we can reuse the same allocator/list with a Reset).
        {
            commandAllocator->Reset();
            commandList->Reset(commandAllocator.Get(), nullptr);

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

            // Sync Copy queue.
            ThrowIfFailed(commandList->Close());
            ID3D12CommandList* lists[] = { commandList.Get() };
            gpu.copyCommandQueue->ExecuteCommandLists(1, lists);
            uint64_t fenceValue = gpu.copyFenceValue.fetch_add(1);
            gpu.copyCommandQueue->Signal(gpu.copyFence.Get(), fenceValue);
            if (gpu.copyFence->GetCompletedValue() < fenceValue) {
                gpu.copyFence->SetEventOnCompletion(fenceValue, gpu.copyFenceEvent);
                WaitForSingleObject(gpu.copyFenceEvent, INFINITE);
            }

            indirectUploads.clear(); // staging buffers safe to free
        }

        // Final RCU Publish (Single Atomic Operation). Gather per-tab publish work. Since commands can span multiple
        // tabs, group replacements and appends by their TabGeometryStorage.
        // Because clonedPages and newPages can belong to different tabs we need to route them to the correct storage.
        // The clonedPages map already contains the raw oldPage ptr which we can match
        // back to its owning storage via the batch's tabID.
        {
            // Build a per-storage publish manifest
            struct PublishWork {
                std::vector<GeometryPage*>                 oldPages;
                std::vector<std::unique_ptr<GeometryPage>> replacements;
                std::vector<std::unique_ptr<GeometryPage>> appends;
            };
            // Key: pointer to TabGeometryStorage (stable address)
            std::unordered_map<TabGeometryStorage*, PublishWork> publishMap;
            uint16_t* tabList = publishedTabIndexes.load(std::memory_order_acquire);

            // Route cloned (replacement) pages
            for (auto& [oldRaw, clonedPage] : clonedPages) {
                // Find which storage owns this oldRaw page
                TabGeometryStorage* ownerStorage = nullptr;
                for (auto& cmd2 : batch) {
                    TabGeometryStorage& s = allTabs[tabList[cmd2.tabID]].geometry;
                    for (auto& ap : s.activePages) {
                        if (ap.get() == oldRaw) { ownerStorage = &s; break; }
                    }
                    if (ownerStorage) break;
                }
                if (!ownerStorage) continue; // shouldn't happen

                auto& work = publishMap[ownerStorage];
                work.oldPages.push_back(oldRaw);
                work.replacements.push_back(std::move(clonedPage));
            }

            // Route new (append) pages — they belong to the last tab that
            // triggered a page allocation; track this via newPages ownership.
            // Since newPages are always appended to the storage of the command
            // that caused them, we tag each during allocation above (see below).
            // For now, associate them with the tab of the last ADD/MODIFY cmd.
            // (A production system would track this per-page; sufficient here.)
            for (auto& page : newPages) {
                // Find the storage that the objectLocation points to for this page
                TabGeometryStorage* ownerStorage = nullptr;
                for (auto& cmd2 : batch) {
                    TabGeometryStorage& s = allTabs[cmd2.tabID].geometry;
                    // Check if any existing active page or cloned page matches —
                    // or if this is a fresh page we should attach to this tab.
                    // We simply attach new pages to the tab of the first ADD cmd.
                    if (cmd2.type == CommandToCopyThreadType::ADD ||
                        cmd2.type == CommandToCopyThreadType::MODIFY) {
                        ownerStorage = &allTabs[cmd2.tabID].geometry;
                        break;
                    }
                }
                if (ownerStorage)
                    publishMap[ownerStorage].appends.push_back(std::move(page));
            }
            newPages.clear(); // ownership transferred

            // Execute one PublishPages call per affected tab
            for (auto& [storagePtr, work] : publishMap) {
                PublishPages( *storagePtr, work.oldPages, std::move(work.replacements),
                    std::move(work.appends));
            }

        }
        // End of Pass 3 + Publish 

        ///////////////////////////////////////////////////////////////

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
            if (gpu.screens[i].renderFenceValue == 0) continue; // thread never signalled
            if (!gpu.screens[i].isActive)  continue; // Skip inactive monitors

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
                storage.retiredPages.erase( std::remove_if(
                    storage.retiredPages.begin(), storage.retiredPages.end(),
                    [&](const auto& rp){
                        // Strict <= : the fence value tagged at publish time is the frame during 
                        // which the old page was still live. Once all monitors have
                        // completed that frame, it is safe to free.
                        return rp.retireFence <= safeRetireFence;
                        // unique_ptr destructs automatically on removal
                    }), storage.retiredPages.end()
                );
            }
        }
    } // End of while (!shutdownSignal)
    
    std::wcout << "GPU Copy Thread shutting down." << std::endl;
}

void GpuRenderThread(int monitorId, int refreshRate) {
    // Our architecture is 1 GPU Render thread per Monitor. 
    std::wcout << "Render Thread (Monitor " << monitorId << ", " << refreshRate << "Hz) started." << std::endl;
    auto lastFpsTime = std::chrono::high_resolution_clock::now();
    int frameCount = 0;
	
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
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&threadRes.commandAllocators[i]) ));
    }
    // Create the Command List (Only 1 needed, we reset it repeatedly)
    ThrowIfFailed(gpu.device->CreateCommandList( 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        threadRes.commandAllocators[0].Get(), nullptr, // Pipeline state set later
        IID_PPV_ARGS(&threadRes.commandList) ));

    // Command lists are created in the recording state, but our loop expects them closed initially.
    threadRes.commandList->Close();

    uint64_t lastRenderedFrame = -1;
    const auto frameDuration = std::chrono::milliseconds(1000 / refreshRate);

    // Create synchronization objects
    ThrowIfFailed(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&threadRes.fence)));
    UINT64 currentFenceValue = 0; // TODO: Is it OK to start separate render threads with same 0 value ?
    threadRes.fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    while (!shutdownSignal && !pauseRenderThreads) {
        auto frameStart = std::chrono::high_resolution_clock::now();
        
		// Reset Thread Allocator & List.We must do this outside the window loop,
		// since we send the command list of all windows in one go to the GPU, to render all windows on this monitor.
        auto& allocator = threadRes.commandAllocators[threadRes.allocatorIndex];
        allocator->Reset();
        threadRes.commandList->Reset(allocator.Get(), nullptr); // Pass 'nullptr' for the PSO here so we don't enforce a state yet.

        bool didRender = false;

		// Do not move outside the while loop since, main thread could have created new windows or moved some.
        uint16_t* windowList = publishedWindowIndexes.load(std::memory_order_acquire);
        uint16_t windowCount = publishedWindowCount.load(std::memory_order_acquire);

        for (uint16_t wi = 0; wi < windowCount; ++wi) {
            SingleUIWindow& window = allWindows[windowList[wi]];

            // Check if user have migrated this windows to a different monitor. This is marked by UI thread.
            uint32_t state = window.migrationState.load(std::memory_order_acquire);
            if (state == 1 && window.currentMonitorIndex == monitorId){
                std::wcout << "Source thread releasing window\n";
                // Wait GPU idle
                UINT64 waitValue = gpu.screens[window.currentMonitorIndex].renderFenceValue;
                auto& screen = gpu.screens[window.currentMonitorIndex];
                if (screen.renderFence->GetCompletedValue() < waitValue){
                    screen.renderFence->SetEventOnCompletion(waitValue, screen.renderFenceEvent);
                    WaitForSingleObject(screen.renderFenceEvent, INFINITE);
                }
                window.isMigrating = true;
                gpu.CleanupWindowResources(window.dx);
                window.migrationState.store(2, std::memory_order_release);
                continue;
            }

            // Check if any other render thread released this thread for migration and we need to acquired it.
            else if (state == 2 && window.requestedMonitorIndex == monitorId) {
                uint32_t expected = 2;
                if (!window.migrationState.compare_exchange_strong(expected, 3, std::memory_order_acq_rel))
                    continue;

                int newMonitor = window.requestedMonitorIndex;
                std::wcout << "Destination thread acquiring window\n";

                gpu.InitD3DPerWindow( window.dx, window.hWnd, gpu.screens[newMonitor].commandQueue.Get() );
                window.currentMonitorIndex = newMonitor;
                window.isMigrating = false;
                window.migrationState.store(0, std::memory_order_release);
                std::wcout << "Migration complete\n";
                continue;
            }

            if (window.currentMonitorIndex != monitorId) continue; // Skip the windows not on this monitor.
            // The Safety Switch: If migrating, pretend this window doesn't exist for now.
            if (window.isMigrating) continue;
            if (window.isResizing) continue;
            if (!window.dx.swapChain) continue;
            
            // TODO: Ideally, it should be handled in WM_MOVE or nearby. Following is simply safeguard for bugs elsewhere.
            // Check if the window is physically on this monitor, but chemically bound to another queue
            if (window.dx.swapChain && window.dx.creatorQueue != threadRes.commandQueue.Get()) {
                std::wcout << "Monitor Mismatch detected! Recreating SwapChain for new Queue." << std::endl;
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
                winRes.frameIndex, gpu.rtvDescriptorSize);
            CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle( winRes.rtvHeap->GetCPUDescriptorHandleForHeapStart(),
                winRes.frameIndex, gpu.rtvDescriptorSize);
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
                uint64_t fenceToWaitFor = gpu.copyFenceValue.load(std::memory_order_acquire);// Cross-Queue Sync.
                //if (fenceToWaitFor > 0) { threadRes.commandQueue->Wait(gpu.copyFence.Get(), fenceToWaitFor); }
                //Above is commented out because render thread now no longer need to wait for copyFence,
                //because, now render thread operate over READ ONLY page list.
                gpu.PopulateCommandList(threadRes.commandList.Get(), winRes, tabRes, tab.geometry);// Renders geometry.
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
            
            for (uint16_t wi = 0; wi < windowCount; ++wi) {
                SingleUIWindow& window = allWindows[windowList[wi]];

                if (window.currentMonitorIndex != monitorId) continue;
                if (window.isResizing) continue;
                if (window.isMigrating) continue;
                HRESULT hr = window.dx.swapChain->Present(1, 0);
                if (FAILED(hr)) { std::cerr << "Present failed: " << hr << std::endl; }
				// Do NOT Handle Fences here. It will be handled after all windows are presented.

                // Update THIS window's specific buffer index. Update the frame index immediately so the NEXT loop uses the correct buffer.
                window.dx.frameIndex = window.dx.swapChain->GetCurrentBackBufferIndex();
            }

            // SIGNAL the fence for the current frame
            currentFenceValue = gpu.renderFenceValue.fetch_add(1);
            threadRes.commandQueue->Signal(threadRes.fence.Get(), currentFenceValue);
            //tabRes.lastRenderFenceValue.store(currentFenceValue, std::memory_order_release);
            // Mirror into the globally-visible per-monitor fence so PruneOneRetiredPage can see it.
            threadRes.commandQueue->Signal(gpu.screens[monitorId].renderFence.Get(), currentFenceValue);
            gpu.screens[monitorId].renderFenceValue = currentFenceValue; // Submitted value. Not necessarily executed.

            // WAIT: Throttle CPU (Double Buffering Logic). We need to reuse the Command Allocator for the *next* frame.
            // If we have 2 allocators (Double Buffering), we must ensure the GPU is finished with Frame (Current - 1).
            if (currentFenceValue >= FRAMES_PER_RENDERTARGETS) {
                const UINT64 fenceValueToWaitFor = currentFenceValue - FRAMES_PER_RENDERTARGETS + 1;

                // If the GPU hasn't reached that point yet, we sleep the CPU thread.
                if (threadRes.fence->GetCompletedValue() < fenceValueToWaitFor) {
                    threadRes.fence->SetEventOnCompletion(fenceValueToWaitFor, threadRes.fenceEvent);
                    WaitForSingleObject(threadRes.fenceEvent, INFINITE);
                }
            }
            // Update the Thread's allocator index (0 -> 1 -> 0 -> 1). This is separate from the window's back buffer index!
            threadRes.allocatorIndex = (threadRes.allocatorIndex + 1) % FRAMES_PER_RENDERTARGETS;
        } else { // Idle handling
            // If I have no windows, I just wait (maybe for a VSync or sleep). Maybe we are running without a monitor !
            // This fulfills "waiting for some windows to be dragged into it" ?
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }

#ifdef _DEBUG
        // Update FPS counter (Debug build only)
        // FPS calculation variables - add these as global or class members
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
    const UINT64 fenceValueToWaitFor = currentFenceValue;

    // If the GPU hasn't reached that point yet, we sleep the CPU thread.
    if (threadRes.fence->GetCompletedValue() < fenceValueToWaitFor) {
        threadRes.fence->SetEventOnCompletion(fenceValueToWaitFor, threadRes.fenceEvent);
        //At this cleanup stage, do not wait INFINITE. Otherwise we may get stuck waiting forever.
        DWORD waitResult = WaitForSingleObject(threadRes.fenceEvent, 5000);  // 5 sec timeout
        if (waitResult != WAIT_OBJECT_0) {
            std::cerr << "Render thread cleanup Fence wait timed out!\n" << std::endl;
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
    std::wcout << "Render Thread (Monitor " << monitorId << ") shutting down.\n" << std::endl;
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
    ThrowIfFailed(dx.swapChain->ResizeBuffers( FRAMES_PER_RENDERTARGETS, // Resize swap-chain buffers
        newWidth, newHeight, desc.BufferDesc.Format, desc.Flags));
    dx.frameIndex = dx.swapChain->GetCurrentBackBufferIndex();

    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(dx.rtvHeap->GetCPUDescriptorHandleForHeapStart());// Recreate RTVs
    for (UINT i = 0; i < FRAMES_PER_RENDERTARGETS; i++) {
        ThrowIfFailed(dx.swapChain->GetBuffer(i, IID_PPV_ARGS(&dx.renderTargets[i])));
        device->CreateRenderTargetView(dx.renderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.Offset(1, gpu.rtvDescriptorSize);
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
    D3D12_CLEAR_VALUE clearValue{ .Format = gpu.rttFormat, .Color = {0.0f, 0.2f, 0.4f, 1.0f} };

    for (UINT i = 0; i < FRAMES_PER_RENDERTARGETS; i++) {
        auto texDesc = CD3DX12_RESOURCE_DESC::Tex2D(gpu.rttFormat,
            newWidth, newHeight, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        ThrowIfFailed(device->CreateCommittedResource(&heapProps,
            D3D12_HEAP_FLAG_NONE, &texDesc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            &clearValue, IID_PPV_ARGS(&dx.renderTextures[i])));
        device->CreateRenderTargetView(dx.renderTextures[i].Get(), nullptr, rttRtvHandle);// RTV
        rttRtvHandle.Offset(1, gpu.rtvDescriptorSize);

        // SRV
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = gpu.rttFormat;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(dx.renderTextures[i].Get(), &srvDesc, rttSrvHandle);
        rttSrvHandle.Offset(1, gpu.cbvSrvUavDescriptorSize);
    }

    dx.WindowWidth = newWidth; // Update stored dimensions
    dx.WindowHeight = newHeight;

    // Update viewport
    dx.viewport = CD3DX12_VIEWPORT(0.0f, 0.0f, static_cast<float>(newWidth), static_cast<float>(newHeight));
    dx.scissorRect = CD3DX12_RECT(0, 0, newWidth, newHeight);
}
