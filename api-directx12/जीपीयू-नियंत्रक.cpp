// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.

#include <random>
#include <ctime>
#include <iostream>
#include <thread>
#include <chrono>
#include <map>
#include <list>
#include <unordered_map>

#include "डेटा.h"
#include "जीपीयू-नियंत्रक.h"

void InitD3D(HWND hwnd);
void PopulateCommandList();
void WaitForPreviousFrame();
void CleanupD3D();
void GenerateVertexData(Vertex** vertexData, UINT* vertexCount, UINT* vertexBufferSize);
void GenerateIndexData(UINT16** indexData, UINT* indexCount, UINT* indexBufferSize);

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

OneMonitorController screen[4];
ComPtr<ID3D12Device> device;
bool isGPUEngineInitialized = false; //TODO: To be implemented.

// Global variables for dynamic pyramid generation
static std::vector<Vertex> dynamicVertices;
static std::vector<UINT16> dynamicIndices;
static UINT pyramidCount = 0;
// Add this near your other global variables
static std::chrono::steady_clock::time_point lastPyramidAddTime;

void GenerateVertexData(Vertex** vertexData, UINT* vertexCount, UINT* vertexBufferSize) {
    *vertexData = dynamicVertices.data();
    *vertexCount = dynamicVertices.size();
    *vertexBufferSize = *vertexCount * sizeof(Vertex);
}

void GenerateIndexData(UINT16** indexData, UINT* indexCount, UINT* indexBufferSize) {
    *indexData = dynamicIndices.data();
    *indexCount = dynamicIndices.size();
    *indexBufferSize = *indexCount * sizeof(UINT16);
}

void AddRandomPyramid() {
    if (pyramidCount >= MaxPyramids) return; // Safety check to not exceed our pre-allocated buffer size

    // Initialize random number generator (persistent)
    static std::mt19937 rng(static_cast<unsigned>(std::time(nullptr)));
    std::uniform_real_distribution<float> posDist(-5.0f, 5.0f);
    std::uniform_real_distribution<float> sizeDist(0.2f, 1.0f);
    std::uniform_real_distribution<float> colorDist(0.0f, 1.0f);

    // Random properties for the new pyramid
    float centerX = posDist(rng);
    float centerY = posDist(rng);
    float centerZ = posDist(rng);
    float pyramidSize = sizeDist(rng);
    XMFLOAT4 color1(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
    XMFLOAT4 color2(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
    XMFLOAT4 color3(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
    XMFLOAT4 color4(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);

    // --- Append new vertex data ---
    dynamicVertices.push_back({ XMFLOAT3(centerX - pyramidSize * 0.5f, centerY - pyramidSize * 0.5f, centerZ + pyramidSize * 0.5f), color1 });
    dynamicVertices.push_back({ XMFLOAT3(centerX + pyramidSize * 0.5f, centerY - pyramidSize * 0.5f, centerZ + pyramidSize * 0.5f), color2 });
    dynamicVertices.push_back({ XMFLOAT3(centerX, centerY - pyramidSize * 0.5f, centerZ - pyramidSize * 0.5f), color3 });
    dynamicVertices.push_back({ XMFLOAT3(centerX, centerY + pyramidSize * 0.8f, centerZ), color4 });

    // --- Append new index data ---
    UINT16 baseIndex = pyramidCount * 4;
    // Base triangle
    dynamicIndices.push_back(baseIndex + 0); dynamicIndices.push_back(baseIndex + 1); dynamicIndices.push_back(baseIndex + 2);
    // Side triangles
    dynamicIndices.push_back(baseIndex + 0); dynamicIndices.push_back(baseIndex + 1); dynamicIndices.push_back(baseIndex + 3);
    dynamicIndices.push_back(baseIndex + 1); dynamicIndices.push_back(baseIndex + 2); dynamicIndices.push_back(baseIndex + 3);
    dynamicIndices.push_back(baseIndex + 2); dynamicIndices.push_back(baseIndex + 0); dynamicIndices.push_back(baseIndex + 3);

    // Increment the total pyramid count
    pyramidCount++;
}

void InitD3D(HWND hwnd) {
    UINT dxgiFactoryFlags = 0;
    int i = 0; // Latter to be iterated over number of screens.

#if defined(_DEBUG)
    // Enable debug layer in debug mode
    ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
        debugController->EnableDebugLayer();
        dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
    }
#endif

    // Create DXGI factory
    ComPtr<IDXGIFactory4> factory;
    CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&factory));

    // Create device
    ComPtr<IDXGIAdapter1> hardwareAdapter;
    for (UINT adapterIndex = 0; SUCCEEDED(factory->EnumAdapters1(adapterIndex, &hardwareAdapter)); ++adapterIndex) {
        if (SUCCEEDED(D3D12CreateDevice(hardwareAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
            break;
        }
    }

    // Create command queue. TODO: Ideally it should be per GPU, not per Monitor.
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&screen[i].commandQueue));

    // Create command allocator. Here we are creating just 1. We will create more in future.
    // TODO: Create a separate COPY queue for async load of data to GPU memory.
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&screen[i].commandAllocator));
    // CreateCommandList is not created latter, because that one needs complete pipelineState defined.

    // Create swap chain
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = FrameCount;
    swapChainDesc.Width = screen[i].WindowWidth;
    swapChainDesc.Height = screen[i].WindowHeight;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;

    ComPtr<IDXGISwapChain1> tempSwapChain;
    factory->CreateSwapChainForHwnd( screen[i].commandQueue.Get(),
        hwnd, &swapChainDesc, nullptr, nullptr, &tempSwapChain );

    tempSwapChain.As(&screen[i].swapChain);
    screen[i].frameIndex = screen[i].swapChain->GetCurrentBackBufferIndex();

    // Create descriptor heap for render target views
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = FrameCount; //One RTV per frame buffer for multi-buffering support
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&screen[i].rtvHeap));

    screen[i].rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // Create depth stencil descriptor heap
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&screen[i].dsvHeap));

    // Create depth stencil buffer
    D3D12_CLEAR_VALUE depthOptimizedClearValue = {};
    depthOptimizedClearValue.Format = DXGI_FORMAT_D32_FLOAT;
    depthOptimizedClearValue.DepthStencil.Depth = 1.0f;
    depthOptimizedClearValue.DepthStencil.Stencil = 0;

    auto depthHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto depthResourceDesc = CD3DX12_RESOURCE_DESC::Tex2D(
        DXGI_FORMAT_D32_FLOAT, screen[i].WindowWidth, screen[i].WindowHeight,
        1, 0, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);

    device->CreateCommittedResource(
        &depthHeapProps,             D3D12_HEAP_FLAG_NONE,
        &depthResourceDesc,          D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &depthOptimizedClearValue,   IID_PPV_ARGS(&screen[i].depthStencilBuffer)
    );
    //Observe that depthStencilBuffer has been created on D3D12_HEAP_TYPE_DEFAULT, i.e. main GPU memory
    //dsvHeap is created on D3D12_DESCRIPTOR_HEAP_TYPE_DSV. 
    //All DESCRIPTOR HEAPs are stored on Small Fast gpu memory. It is normal D3D12 design.

    device->CreateDepthStencilView(screen[i].depthStencilBuffer.Get(), nullptr,
        screen[i].dsvHeap->GetCPUDescriptorHandleForHeapStart());

    // Create render target views
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(screen[i].rtvHeap->GetCPUDescriptorHandleForHeapStart());
    for (UINT j = 0; j < FrameCount; j++) {
        screen[i].swapChain->GetBuffer(j, IID_PPV_ARGS(&screen[i].renderTargets[j]));
        device->CreateRenderTargetView(screen[i].renderTargets[j].Get(), nullptr, rtvHandle);
        rtvHandle.Offset(1, screen[i].rtvDescriptorSize);
    }

    // Create root signature with constant buffer
    D3D12_FEATURE_DATA_ROOT_SIGNATURE featureData = {};
    featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
    //Check if the GPU supports Root Signature version 1.1 (newer, more efficient) or fall back to 1.0.
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &featureData, sizeof(featureData)))) {
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
    device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
        IID_PPV_ARGS(&screen[i].rootSignature));

    /* Note that The root signature is like a function declaration. It defines the interface but doesn't 
    contain actual data. Now that we have declared the data layout, time to prepare for movement
    of the data from CPU RAM to GPU RAM. This way we update the constant every frame without
    changing the root signature. */

    // Create constant buffer descriptor heap
    D3D12_DESCRIPTOR_HEAP_DESC cbvHeapDesc = {};
    cbvHeapDesc.NumDescriptors = 1;
    cbvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    cbvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    device->CreateDescriptorHeap(&cbvHeapDesc, IID_PPV_ARGS(&screen[i].cbvHeap));

    // Create constant buffer
    /* Our HLSL constant buffer contains: float4x4 worldViewProjection; 64 bytes (4x4 floats = 16*4=64)
    float4x4 world; 64 bytes. Total: 128 bytes, but padded to 256 bytes for D3D12 alignment requirements */
    auto cbHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto cbResourceDesc = CD3DX12_RESOURCE_DESC::Buffer(256); // Constant buffers must be 256-byte aligned

    device->CreateCommittedResource( &cbHeapProps, D3D12_HEAP_FLAG_NONE,
        &cbResourceDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&screen[i].constantBuffer));

    // Create constant buffer view
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
    cbvDesc.BufferLocation = screen[i].constantBuffer->GetGPUVirtualAddress();
    cbvDesc.SizeInBytes = 256;
    device->CreateConstantBufferView(&cbvDesc, screen[i].cbvHeap->GetCPUDescriptorHandleForHeapStart());

    // Map constant buffer i.e. Map Constant Buffer for CPU Access
    CD3DX12_RANGE readRange(0, 0); //CPU won't read from this buffer (write-only optimization)
    screen[i].constantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&screen[i].cbvDataBegin));

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
        };

        PSInput VSMain(float3 position : POSITION, float4 color : COLOR) {
            PSInput result;
            result.position = mul(float4(position, 1.0f), worldViewProjection);
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
        };

        float4 PSMain(PSInput input) : SV_TARGET {
            return input.color;
        }
    )";

    D3DCompile(vertexShaderCode, strlen(vertexShaderCode), nullptr, nullptr, nullptr,
        "VSMain", "vs_5_0", compileFlags, 0, &vertexShader, nullptr);
    D3DCompile(pixelShaderCode, strlen(pixelShaderCode), nullptr, nullptr, nullptr,
        "PSMain", "ps_5_0", compileFlags, 0, &pixelShader, nullptr);

    /* Define the vertex input layout
    Position: 3 floats (12 bytes) starting at offset 0
    Color: 4 floats (16 bytes) starting at offset 12 */
    D3D12_INPUT_ELEMENT_DESC inputElementDescs[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    // Create the pipeline state object with depth testing enabled
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
    psoDesc.pRootSignature = screen[i].rootSignature.Get();
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
    device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&screen[i].pipelineState));
    vertexShader.Reset(); //Release memory once we have created pipelineState.
    pixelShader.Reset();  //Release memory suggested by Claude code review.
    signature.Reset();
    if (error) error.Reset();

    // Create the command list. Note that this is default pipelineState for the command list.
    // It can be changed inside command list also by calling ID3D12GraphicsCommandList::SetPipelineState.
    // CommandList is : List of various Commands including repeated calls of many CommandBundles.
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, screen[i].commandAllocator.Get(), 
        screen[i].pipelineState.Get(), IID_PPV_ARGS(&screen[i].commandList));
    screen[i].commandList->Close();

    // Now we will now pre-allocate large buffers that can be updated every frame.

    // --- Create Vertex Buffer Resources (Pre-allocation) ---
    auto defaultHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto uploadHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

    // Create the main vertex buffer on the default heap (GPU-only access).
    auto vbResourceDesc = CD3DX12_RESOURCE_DESC::Buffer(MaxVertexBufferSize);
    device->CreateCommittedResource( &defaultHeapProps, D3D12_HEAP_FLAG_NONE,
        &vbResourceDesc, D3D12_RESOURCE_STATE_COMMON, // Start in a common state
        nullptr, IID_PPV_ARGS(&screen[i].vertexBuffer));

    // Create an upload heap for the vertex buffer.
    device->CreateCommittedResource( &uploadHeapProps, D3D12_HEAP_FLAG_NONE,
        &vbResourceDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&screen[i].vertexBufferUpload));

    // --- Create Index Buffer Resources (Pre-allocation) ---
    // Create the main index buffer on the default heap.
    auto ibResourceDesc = CD3DX12_RESOURCE_DESC::Buffer(MaxIndexBufferSize);
    device->CreateCommittedResource( &defaultHeapProps, D3D12_HEAP_FLAG_NONE,
        &ibResourceDesc, D3D12_RESOURCE_STATE_COMMON, // Start in a common state
        nullptr, IID_PPV_ARGS(&screen[i].indexBuffer));

    // Create an upload heap for the index buffer.
    device->CreateCommittedResource( &uploadHeapProps, D3D12_HEAP_FLAG_NONE,
        &ibResourceDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&screen[i].indexBufferUpload));

    // Persistently map the upload buffers. We won't unmap them until cleanup.
    // This is efficient as we avoid map/unmap calls every frame.
    CD3DX12_RANGE readRange2(0, 0); // We do not intend to read from this resource on the CPU.
    screen[i].vertexBufferUpload->Map(0, &readRange2, reinterpret_cast<void**>(&screen[i].pVertexDataBegin));
    screen[i].indexBufferUpload->Map(0, &readRange2, reinterpret_cast<void**>(&screen[i].pIndexDataBegin));

    // Initialize the buffer views with default (but valid) values.
    // The sizes will be updated each frame in PopulateCommandList.
    screen[i].vertexBufferView.BufferLocation = screen[i].vertexBuffer->GetGPUVirtualAddress();
    screen[i].vertexBufferView.StrideInBytes = sizeof(Vertex);
    screen[i].vertexBufferView.SizeInBytes = 0; // Will be updated per frame

    screen[i].indexBufferView.BufferLocation = screen[i].indexBuffer->GetGPUVirtualAddress();
    screen[i].indexBufferView.Format = DXGI_FORMAT_R16_UINT;
    screen[i].indexBufferView.SizeInBytes = 0; // Will be updated per frame

    // Create synchronization objects
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&screen[i].fence));
    screen[i].fenceValue = 1;
    screen[i].fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    // Upto this point, setting of Graphics Engine is complete. Now we generate the actual 
    // vertex & index data which graphics engine will use to render things on screen.
    // Initial set of vertexes are generated here. Latter on more vertexes / indexes shall be updated each frame.

    // Generate the initial 10 pyramids
    dynamicVertices.reserve(MaxVertexBufferSize);
    dynamicIndices.reserve(MaxIndexBufferSize);
    for (int k = 0; k < 10; ++k) { AddRandomPyramid(); }
    lastPyramidAddTime = std::chrono::steady_clock::now();// Initialize the timer

    // Create synchronization objects
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&screen[i].fence));

    auto initialVertexBarrier = CD3DX12_RESOURCE_BARRIER::Transition( screen[i].vertexBuffer.Get(),
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);

    auto initialIndexBarrier = CD3DX12_RESOURCE_BARRIER::Transition( screen[i].indexBuffer.Get(),
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_INDEX_BUFFER);

    // Execute these transitions during initialization
    screen[i].commandList->ResourceBarrier(1, &initialVertexBarrier);
    screen[i].commandList->ResourceBarrier(1, &initialIndexBarrier);

    // Wait for initialization to complete
    WaitForPreviousFrame();
}

void PopulateCommandList() {
    // Reset allocator and command list
    int i = 0; // Latter to be iterated over number of screens.
    screen[i].commandAllocator->Reset();
    screen[i].commandList->Reset(screen[i].commandAllocator.Get(), screen[i].pipelineState.Get());

    // Check timer and add a new pyramid every second ---
    auto currentTime = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(currentTime - lastPyramidAddTime).count() >= 1) {
        AddRandomPyramid();
        lastPyramidAddTime = currentTime; // Reset the timer
    }

    // --- Generate and Upload Geometry Every Frame (using the persistent vectors) ---

    // 1. Get the current state of the vertex and index data.
    Vertex* pyramidVertices;
    UINT vertexCount;
    UINT vertexBufferSize;
    GenerateVertexData(&pyramidVertices, &vertexCount, &vertexBufferSize);

    UINT16* pyramidIndices;
    UINT indexCount;
    UINT indexBufferSize;
    GenerateIndexData(&pyramidIndices, &indexCount, &indexBufferSize);

    // Early exit if no data to render
    if (vertexCount == 0 || indexCount == 0) {
        screen[i].commandList->Close();
        return;
    }

    memcpy(screen[i].pVertexDataBegin, pyramidVertices, vertexBufferSize);// Copy vertex data to upload buffer
    memcpy(screen[i].pIndexDataBegin, pyramidIndices, indexBufferSize);// Copy index data to upload buffer  
    // Update buffer view sizes with current data
    screen[i].vertexBufferView.SizeInBytes = vertexBufferSize;
    screen[i].indexBufferView.SizeInBytes = indexBufferSize;
    
    // Add resource barriers and copy commands to transfer from upload to default heap
    // Transition vertex buffer to copy destination
    auto vertexBarrierToCopy = CD3DX12_RESOURCE_BARRIER::Transition( screen[i].vertexBuffer.Get(),
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_COPY_DEST);
    screen[i].commandList->ResourceBarrier(1, &vertexBarrierToCopy);

    // Transition index buffer to copy destination  
    auto indexBarrierToCopy = CD3DX12_RESOURCE_BARRIER::Transition( screen[i].indexBuffer.Get(),
        D3D12_RESOURCE_STATE_INDEX_BUFFER, D3D12_RESOURCE_STATE_COPY_DEST);
    screen[i].commandList->ResourceBarrier(1, &indexBarrierToCopy);

    // Copy vertex data from upload heap to default heap
    screen[i].commandList->CopyBufferRegion( screen[i].vertexBuffer.Get(), 0,
        screen[i].vertexBufferUpload.Get(), 0, vertexBufferSize);

    // Copy index data from upload heap to default heap
    screen[i].commandList->CopyBufferRegion( screen[i].indexBuffer.Get(), 0,
        screen[i].indexBufferUpload.Get(), 0, indexBufferSize);

    // Transition buffers back to their usage states
    auto vertexBarrierToUse = CD3DX12_RESOURCE_BARRIER::Transition( screen[i].vertexBuffer.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    screen[i].commandList->ResourceBarrier(1, &vertexBarrierToUse);

    auto indexBarrierToUse = CD3DX12_RESOURCE_BARRIER::Transition( screen[i].indexBuffer.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER);
    screen[i].commandList->ResourceBarrier(1, &indexBarrierToUse);

    // Update buffer view sizes with current data
    screen[i].vertexBufferView.SizeInBytes = vertexBufferSize;
    screen[i].indexBufferView.SizeInBytes = indexBufferSize;

    // Update constant buffer with transformation matrices
    static float rotationAngle = 0.0f;
    rotationAngle += 0.02f; // Rotate over time

    // Create view matrix (camera looking at scene from distance)
    DirectX::XMVECTOR eyePosition = DirectX::XMVectorSet(0.0f, 2.0f, -10.0f, 1.0f);
    DirectX::XMVECTOR focusPoint = DirectX::XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
    DirectX::XMVECTOR upDirection = DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    DirectX::XMMATRIX viewMatrix = DirectX::XMMatrixLookAtLH(eyePosition, focusPoint, upDirection);

    // Create projection matrix
    float aspectRatio = static_cast<float>(screen[i].WindowWidth) / static_cast<float>(screen[i].WindowHeight);
    DirectX::XMMATRIX projectionMatrix = DirectX::XMMatrixPerspectiveFovLH(DirectX::XM_PIDIV4, aspectRatio, 0.1f, 100.0f);

    // Create world matrix with rotation
    DirectX::XMMATRIX worldMatrix = DirectX::XMMatrixRotationY(rotationAngle);

    // Combine matrices
    DirectX::XMMATRIX worldViewProjectionMatrix = worldMatrix * viewMatrix * projectionMatrix;

    // Update constant buffer
    ConstantBuffer constantBufferData;
    DirectX::XMStoreFloat4x4(&constantBufferData.worldViewProjection, DirectX::XMMatrixTranspose(worldViewProjectionMatrix));
    DirectX::XMStoreFloat4x4(&constantBufferData.world, DirectX::XMMatrixTranspose(worldMatrix));

    memcpy(screen[i].cbvDataBegin, &constantBufferData, sizeof(constantBufferData));

    // Set necessary state
    screen[i].commandList->SetGraphicsRootSignature(screen[i].rootSignature.Get());

    // Set descriptor heaps
    ID3D12DescriptorHeap* ppHeaps[] = { screen[i].cbvHeap.Get() };
    screen[i].commandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);

    // Set root descriptor table
    screen[i].commandList->SetGraphicsRootDescriptorTable(0, screen[i].cbvHeap->GetGPUDescriptorHandleForHeapStart());

    // 1) Create named variables (l‑values)
    CD3DX12_VIEWPORT viewport(0.0f, 0.0f,
        static_cast<float>(screen[i].WindowWidth),
        static_cast<float>(screen[i].WindowHeight)
    );

    CD3DX12_RECT scissorRect(0, 0, screen[i].WindowWidth, screen[i].WindowHeight);

    // 2) Now you can take their addresses and call the methods
    screen[i].commandList->RSSetViewports(1, &viewport);
    screen[i].commandList->RSSetScissorRects(1, &scissorRect);

    // Indicate that the back buffer will be used as a render target
    auto barrier1 = CD3DX12_RESOURCE_BARRIER::Transition(screen[i].renderTargets[screen[i].frameIndex].Get(),
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    screen[i].commandList->ResourceBarrier(1, &barrier1); // Pass address of barrier1

    // Record commands
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(screen[i].rtvHeap->GetCPUDescriptorHandleForHeapStart(),
        screen[i].frameIndex, screen[i].rtvDescriptorSize);
    CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle(screen[i].dsvHeap->GetCPUDescriptorHandleForHeapStart());
    screen[i].commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

    // Clear render target and depth stencil
    const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f }; // Example color, adjust as needed
    screen[i].commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
    screen[i].commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    // Draw pyramids
    screen[i].commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    screen[i].commandList->IASetVertexBuffers(0, 1, &screen[i].vertexBufferView);
    screen[i].commandList->IASetIndexBuffer(&screen[i].indexBufferView);
    screen[i].commandList->DrawIndexedInstanced(indexCount, 1, 0, 0, 0);

    // Indicate that the back buffer will now be used to present
    auto barrier2 = CD3DX12_RESOURCE_BARRIER::Transition(screen[i].renderTargets[screen[i].frameIndex].Get(), 
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    screen[i].commandList->ResourceBarrier(1, &barrier2); // Pass address of barrier2

    // Close command list
    screen[i].commandList->Close();
}

void WaitForPreviousFrame() {
    // Signal and increment the fence value
    int i = 0; // Latter to be iterated over number of screens.
    const UINT64 currentFenceValue = screen[i].fenceValue;
    //Tells the GPU command queue to "signal" (mark) the fence with the current fence value when it 
    //finishes executing all previously submitted commands.
    screen[i].commandQueue->Signal(screen[i].fence.Get(), currentFenceValue);
    screen[i].fenceValue++;

    // Wait until the previous frame is finished
    if (screen[i].fence->GetCompletedValue() < currentFenceValue) {
        screen[i].fence->SetEventOnCompletion(currentFenceValue, screen[i].fenceEvent);
        //This is the one halting CPU thread till fence completes.
        WaitForSingleObject(screen[i].fenceEvent, INFINITE); 
    }

    // Update the frame index
    screen[i].frameIndex = screen[i].swapChain->GetCurrentBackBufferIndex();
}

void CleanupD3D() {
    // Wait for the GPU to be done with all resources
    int i = 0; // Latter to be iterated over number of screens.
    WaitForPreviousFrame();

    // Unmap all persistently mapped buffers before releasing them.
    if (screen[i].pVertexDataBegin) {
        screen[i].vertexBufferUpload->Unmap(0, nullptr);
        screen[i].pVertexDataBegin = nullptr;
    }
    if (screen[i].pIndexDataBegin) {
        screen[i].indexBufferUpload->Unmap(0, nullptr);
        screen[i].pIndexDataBegin = nullptr;
    }
    if (screen[i].cbvDataBegin) {
        screen[i].constantBuffer->Unmap(0, nullptr);
        screen[i].cbvDataBegin = nullptr;
    }

    CloseHandle(screen[i].fenceEvent);
    // Reset all ComPtr objects
    screen[i] = OneMonitorController{}; // Reset to default state
    
}

/////////////////////////////////////////////////////////////////////////////////
// Initial draft of thread based program to handle GPU commands

// --- Externs for communication ---
extern std::atomic<bool> shutdownSignal;
extern ThreadSafeQueue<GpuCommand> g_gpuCommandQueue;

// Logic Thread "Fence"
extern std::mutex g_logicFenceMutex;
extern std::condition_variable g_logicFenceCV;
extern uint64_t g_logicFrameCount;

// Copy Thread "Fence"
extern std::mutex g_copyFenceMutex;
extern std::condition_variable g_copyFenceCV;
extern uint64_t g_copyFrameCount;

// Render Packet from Main Logic
extern std::mutex g_renderPacketMutex;
extern RenderPacket g_renderPacket;

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

std::optional<GpuResourceInfo> शंकर::Allocate(size_t size) {
    if (m_nextFreeOffset + size > m_vram_capacity) {
        std::cerr << "VRAM MANAGER: Out of memory!" << std::endl;
        // Here, the Main Logic thread would be signaled to reduce LOD.
        return std::nullopt;
    }
    GpuResourceInfo info{ m_nextFreeOffset, size };
    m_nextFreeOffset += size; // Simple bump allocator
    return info;
}

शंकर gpuRAMManager;

// --- GPU Thread Functions ---

void GpuCopyThreadFunc() {
    std::cout << "GPU Copy Thread started." << std::endl;
    uint64_t lastProcessedFrame = -1;

    while (!shutdownSignal) {
        // 1. Wait for the Main Logic thread to finish a frame.
        {
            std::unique_lock<std::mutex> lock(g_logicFenceMutex);
            g_logicFenceCV.wait(lock, [&]{ return g_logicFrameCount > lastProcessedFrame || shutdownSignal; });
            if (shutdownSignal) break;
            lastProcessedFrame = g_logicFrameCount;
        }
        
        // 2. Process all pending GPU commands for this frame.
        GpuCommand cmd;
        while (g_gpuCommandQueue.try_pop(cmd)) {
            std::visit([&](auto&& arg) {
                using T = std::decay_t<decltype(arg)>;
                if constexpr (std::is_same_v<T, GpuUploadCmd>) {
                    // An object's data needs to be uploaded.
                    auto newResourceOpt = gpuRAMManager.Allocate(arg.data.size());
                    if (newResourceOpt) {
                        GpuResourceInfo newResource = *newResourceOpt;
                        // In DX12, this is where you'd record and execute the copy command list.
                        //std::cout << "COPY THREAD: Uploaded " << newResource.size << " bytes for object " << arg.objectId << " to VRAM offset " << newResource.vramOffset << std::endl;

                        // Check if this is an update to an existing resource.
                        if (gpuRAMManager.resourceMap.count(arg.objectId)) {
                            // It is an update. The old resource must be freed, but not yet!
                            // A render thread might still be using it for the current frame.
                            GpuResourceInfo oldResource = gpuRAMManager.resourceMap[arg.objectId];
                            gpuRAMManager.deferredFreeQueue.push_back({lastProcessedFrame, oldResource});
                        }
                        // Atomically update the map to point to the new resource.
                        gpuRAMManager.resourceMap[arg.objectId] = newResource;
                    }
                } else if constexpr (std::is_same_v<T, GpuFreeCmd>) {
                    // An object was deleted. Free its VRAM resource (defer it).
                     if (gpuRAMManager.resourceMap.count(arg.objectId)) {
                         gpuRAMManager.deferredFreeQueue.push_back({lastProcessedFrame, arg.resourceToFree});
                         gpuRAMManager.resourceMap.erase(arg.objectId);
                     }
                }
            }, cmd);
        }

        // 3. Signal Fence: Let the Render Thread(s) know the data for this frame is in VRAM.
        {
            std::lock_guard<std::mutex> lock(g_copyFenceMutex);
            g_copyFrameCount = lastProcessedFrame;
        }
        g_copyFenceCV.notify_all();
    }
    g_copyFenceCV.notify_all(); // Wake up threads for shutdown
    std::cout << "GPU Copy Thread shutting down." << std::endl;
}

void RenderThreadFunc(int monitorId, int refreshRate) {
    std::cout << "Render Thread (Monitor " << monitorId << ", " << refreshRate << "Hz) started." << std::endl;
    uint64_t lastRenderedFrame = -1;
    const auto frameDuration = std::chrono::milliseconds(1000 / refreshRate);

    while (!shutdownSignal) {
        auto frameStart = std::chrono::high_resolution_clock::now();

        // 1. Wait for the GPU Copy thread to finish preparing a frame.
        {
            std::unique_lock<std::mutex> lock(g_copyFenceMutex);
            g_copyFenceCV.wait(lock, [&]{ return g_copyFrameCount > lastRenderedFrame || shutdownSignal; });
            if (shutdownSignal) break;
            lastRenderedFrame = g_copyFrameCount;
        }
        
        // 2. Get the render packet for the frame we are about to draw.
        RenderPacket currentPacket;
        {
            std::lock_guard<std::mutex> lock(g_renderPacketMutex);
            currentPacket = g_renderPacket;
        }
        
        // 3. Record Draw Calls
        // In DX12, you'd record a command list here.
        // We just print the actions.
        if (!currentPacket.visibleObjectIds.empty()) {
            //std::cout << "RENDER-" << monitorId << ": Frame " << currentPacket.frameNumber << " | Drawing " << currentPacket.visibleObjectIds.size() << " objects." << std::endl;
            for (uint64_t id : currentPacket.visibleObjectIds) {
                if (gpuRAMManager.resourceMap.count(id)) {
                    auto res = gpuRAMManager.resourceMap[id];
                    // "Drawing object <id> using VRAM at offset <res.vramOffset>"
                }
            }
        }
        
        // 4. Execute command list and Present swap chain
        // This is simulated by sleeping to match the monitor's refresh rate.
        std::this_thread::sleep_until(frameStart + frameDuration);

        // 5. After rendering, perform garbage collection on VRAM
        // In a real engine, the last COMPLETED GPU frame is tracked via fences.
        if (lastRenderedFrame > 2) {
            gpuRAMManager.ProcessDeferredFrees(lastRenderedFrame - 2);
        }
    }
    std::cout << "Render Thread (Monitor " << monitorId << ") shutting down." << std::endl;
}