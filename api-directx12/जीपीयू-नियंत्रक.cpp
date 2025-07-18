// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.

#include "जीपीयू-नियंत्रक.h"
#include <random>
#include <ctime>

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

OneMonitorController screen[4];
ComPtr<ID3D12Device> device;


// Global variables for dynamic triangle generation
static std::vector<Vertex> dynamicVertices;
static std::vector<UINT16> dynamicIndices;
static UINT triangleCount = 0;

void GenerateVertexData(Vertex** vertexData, UINT* vertexCount, UINT* vertexBufferSize) {
    // Initialize random number generator
    static std::mt19937 rng(static_cast<unsigned>(std::time(nullptr)));
    std::uniform_real_distribution<float> posDist(-0.9f, 0.9f);  // Position range
    std::uniform_real_distribution<float> sizeDist(0.02f, 0.1f); // Size range (max 0.1 as requested)
    std::uniform_real_distribution<float> colorDist(0.0f, 1.0f); // Color range
    std::uniform_int_distribution<int> countDist(10, 100);       // Triangle count range

    // Generate random number of triangles (10-100)
    triangleCount = countDist(rng);

    // Clear previous data
    dynamicVertices.clear();
    dynamicVertices.reserve(triangleCount * 3);

    // Generate triangles
    for (UINT i = 0; i < triangleCount; ++i) {
        // Random center position for the triangle
        float centerX = posDist(rng);
        float centerY = posDist(rng);

        // Random size for the triangle
        float triangleSize = sizeDist(rng);

        // Random colors for each vertex
        XMFLOAT4 color1(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
        XMFLOAT4 color2(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);
        XMFLOAT4 color3(colorDist(rng), colorDist(rng), colorDist(rng), 1.0f);

        // Generate triangle vertices relative to center
        // Top vertex
        dynamicVertices.push_back({
            XMFLOAT3(centerX, centerY + triangleSize * 0.5f, 0.0f),
            color1
            });

        // Bottom right vertex
        dynamicVertices.push_back({
            XMFLOAT3(centerX + triangleSize * 0.433f, centerY - triangleSize * 0.25f, 0.0f),
            color2
            });

        // Bottom left vertex
        dynamicVertices.push_back({
            XMFLOAT3(centerX - triangleSize * 0.433f, centerY - triangleSize * 0.25f, 0.0f),
            color3
            });
    }

    *vertexData = dynamicVertices.data();
    *vertexCount = triangleCount * 3;
    *vertexBufferSize = dynamicVertices.size() * sizeof(Vertex);
}

void GenerateIndexData(UINT16** indexData, UINT* indexCount, UINT* indexBufferSize) {
    // Clear previous data
    dynamicIndices.clear();
    dynamicIndices.reserve(triangleCount * 3);

    // Generate indices for each triangle
    for (UINT i = 0; i < triangleCount; ++i) {
        UINT16 baseIndex = i * 3;
        dynamicIndices.push_back(baseIndex);     // First vertex
        dynamicIndices.push_back(baseIndex + 1); // Second vertex
        dynamicIndices.push_back(baseIndex + 2); // Third vertex
    }

    *indexData = dynamicIndices.data();
    *indexCount = triangleCount * 3;
    *indexBufferSize = dynamicIndices.size() * sizeof(UINT16);
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

    // Create command queue
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&screen[i].commandQueue));

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
    factory->CreateSwapChainForHwnd(
        screen[i].commandQueue.Get(),
        hwnd,
        &swapChainDesc,
        nullptr,
        nullptr,
        &tempSwapChain
    );

    tempSwapChain.As(&screen[i].swapChain);
    screen[i].frameIndex = screen[i].swapChain->GetCurrentBackBufferIndex();

    // Create descriptor heap for render target views
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = FrameCount;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&screen[i].rtvHeap));

    screen[i].rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // Create render target views
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(screen[i].rtvHeap->GetCPUDescriptorHandleForHeapStart());
    for (UINT j = 0; j < FrameCount; j++) {
        screen[i].swapChain->GetBuffer(j, IID_PPV_ARGS(&screen[i].renderTargets[j]));
        device->CreateRenderTargetView(screen[i].renderTargets[j].Get(), nullptr, rtvHandle);
        rtvHandle.Offset(1, screen[i].rtvDescriptorSize);
    }

    // Create command allocator
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&screen[i].commandAllocator));

    // Create empty root signature
    CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc;
    rootSignatureDesc.Init(0, nullptr, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
        IID_PPV_ARGS(&screen[i].rootSignature));

    // Create the shader
    ComPtr<ID3DBlob> vertexShader;
    ComPtr<ID3DBlob> pixelShader;

#if defined(_DEBUG)
    UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    UINT compileFlags = 0;
#endif

    // Simple shader code
    const char* vertexShaderCode = R"(
        struct PSInput {
            float4 position : SV_POSITION;
            float4 color : COLOR;
        };

        PSInput VSMain(float3 position : POSITION, float4 color : COLOR) {
            PSInput result;
            result.position = float4(position, 1.0f);
            result.color = color;
            return result;
        }
    )";

    const char* pixelShaderCode = R"(
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

    // Define the vertex input layout
    D3D12_INPUT_ELEMENT_DESC inputElementDescs[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    // Create the pipeline state object
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
    psoDesc.pRootSignature = screen[i].rootSignature.Get();
    psoDesc.VS = CD3DX12_SHADER_BYTECODE(vertexShader.Get());
    psoDesc.PS = CD3DX12_SHADER_BYTECODE(pixelShader.Get());
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;
    device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&screen[i].pipelineState));

    // Create the command list. Note that this is default pipelineState for the command list.
    // It can be changed inside command list also by calling ID3D12GraphicsCommandList::SetPipelineState.
    // CommandList is : List of various Commands including repeated calls of many CommandBundles.
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, screen[i].commandAllocator.Get(), 
        screen[i].pipelineState.Get(), IID_PPV_ARGS(&screen[i].commandList));
    screen[i].commandList->Close();

    // Generate vertex data
    Vertex* triangleVertices;
    UINT vertexCount;
    UINT vertexBufferSize;
    GenerateVertexData(&triangleVertices, &vertexCount, &vertexBufferSize);

    // Generate index data
    UINT16* triangleIndices;
    UINT indexCount;
    UINT indexBufferSize;
    GenerateIndexData(&triangleIndices, &indexCount, &indexBufferSize);

    // Create upload heap and copy vertex data
    ComPtr<ID3D12Resource> vertexBufferUpload;

    // Create default heap for vertex buffer
    // Define the heap properties for the default heap
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    // Define the resource description for the vertex buffer
    auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(vertexBufferSize);

    device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE,
        &resourceDesc, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&screen[i].vertexBuffer));

    // Create upload heap

    // Define the heap properties for the UPLOAD heap
    auto uploadHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

    // Define the resource description for the upload buffer (same size as the destination)
    auto uploadBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(vertexBufferSize);

    device->CreateCommittedResource(
        &uploadHeapProps, D3D12_HEAP_FLAG_NONE,
        &uploadBufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&vertexBufferUpload));

    // Copy data to upload heap
    D3D12_SUBRESOURCE_DATA vertexData = {};
    vertexData.pData = triangleVertices;
    vertexData.RowPitch = vertexBufferSize;
    vertexData.SlicePitch = vertexData.RowPitch;

    // Create index buffer
    ComPtr<ID3D12Resource> indexBufferUpload;

    // Create default heap for index buffer
    // Define the heap properties for the default heap
    auto indexHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    // Define the resource description for the index buffer
    auto indexResourceDesc = CD3DX12_RESOURCE_DESC::Buffer(indexBufferSize);

    device->CreateCommittedResource(
        &indexHeapProps, D3D12_HEAP_FLAG_NONE,
        &indexResourceDesc, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&screen[i].indexBuffer));

    // Create upload heap for index buffer
    // Define the heap properties for the UPLOAD heap
    auto indexUploadHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

    // Define the resource description for the upload buffer (same size as the destination)
    auto indexUploadBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(indexBufferSize);

    device->CreateCommittedResource(
        &indexUploadHeapProps, D3D12_HEAP_FLAG_NONE,
        &indexUploadBufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&indexBufferUpload));

    // Copy data to upload heap
    D3D12_SUBRESOURCE_DATA indexData = {};
    indexData.pData = triangleIndices;
    indexData.RowPitch = indexBufferSize;
    indexData.SlicePitch = indexData.RowPitch;

    // Open command list and record copy commands
    screen[i].commandAllocator->Reset();
    screen[i].commandList->Reset(screen[i].commandAllocator.Get(), screen[i].pipelineState.Get());
    UpdateSubresources<1>(screen[i].commandList.Get(), screen[i].vertexBuffer.Get(), 
        vertexBufferUpload.Get(), 0, 0, 1, &vertexData);

    UpdateSubresources<1>(screen[i].commandList.Get(), screen[i].indexBuffer.Get(), 
        indexBufferUpload.Get(), 0, 0, 1, &indexData);

    // Define the resource barrier to transition the vertex buffer
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        screen[i].vertexBuffer.Get(),                     // The resource to transition
        D3D12_RESOURCE_STATE_COPY_DEST,         // The state before the transition
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER // The state after the transition
    );

    // Define the resource barrier to transition the index buffer
    auto indexBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
        screen[i].indexBuffer.Get(),                     // The resource to transition
        D3D12_RESOURCE_STATE_COPY_DEST,         // The state before the transition
        D3D12_RESOURCE_STATE_INDEX_BUFFER // The state after the transition
    );

    // Record the barrier commands in the command list
    screen[i].commandList->ResourceBarrier(1, &barrier);
    screen[i].commandList->ResourceBarrier(1, &indexBarrier);
    
    // Close command list. It mostly runs synchronously with little work deferred. Completes quickly. 
    // Close():  Transitions the command list from recording mode to execution-ready mode.
    // Validates Commands / Catch errors, Compress (driver-specific optimization), to Immutable (Read-Only).
    screen[i].commandList->Close();
    // Execute the command list.
    ID3D12CommandList* ppCommandLists[] = { screen[i].commandList.Get() };
    screen[i].commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

    // Create vertex buffer view
    screen[i].vertexBufferView.BufferLocation = screen[i].vertexBuffer->GetGPUVirtualAddress();
    screen[i].vertexBufferView.StrideInBytes = sizeof(Vertex);
    screen[i].vertexBufferView.SizeInBytes = vertexBufferSize;

    // Create index buffer view
    screen[i].indexBufferView.BufferLocation = screen[i].indexBuffer->GetGPUVirtualAddress();
    screen[i].indexBufferView.Format = DXGI_FORMAT_R16_UINT;
    screen[i].indexBufferView.SizeInBytes = indexBufferSize;

    // Create synchronization objects
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&screen[i].fence));
    screen[i].fenceValue = 1;
    screen[i].fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    // Wait for initialization to complete
    WaitForPreviousFrame();
}

void PopulateCommandList() {
    // Reset allocator and command list
    int i = 0; // Latter to be iterated over number of screens.
    screen[i].commandAllocator->Reset();
    screen[i].commandList->Reset(screen[i].commandAllocator.Get(), screen[i].pipelineState.Get());

    // Set necessary state
    screen[i].commandList->SetGraphicsRootSignature(screen[i].rootSignature.Get());

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
    screen[i].commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    // Clear render target
    const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f }; // Example color, adjust as needed
    screen[i].commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

    // Draw triangles
    screen[i].commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    screen[i].commandList->IASetVertexBuffers(0, 1, &screen[i].vertexBufferView);
    //screen[i].commandList->DrawInstanced(3, 1, 0, 0);
    screen[i].commandList->IASetIndexBuffer(&screen[i].indexBufferView);
    screen[i].commandList->DrawIndexedInstanced(triangleCount * 3, 1, 0, 0, 0);

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
    screen[i].commandQueue->Signal(screen[i].fence.Get(), currentFenceValue);
    screen[i].fenceValue++;

    // Wait until the previous frame is finished
    if (screen[i].fence->GetCompletedValue() < currentFenceValue) {
        screen[i].fence->SetEventOnCompletion(currentFenceValue, screen[i].fenceEvent);
        WaitForSingleObject(screen[i].fenceEvent, INFINITE);
    }

    // Update the frame index
    screen[i].frameIndex = screen[i].swapChain->GetCurrentBackBufferIndex();
}

void CleanupD3D() {
    // Wait for the GPU to be done with all resources
    int i = 0; // Latter to be iterated over number of screens.
    WaitForPreviousFrame();

    CloseHandle(screen[i].fenceEvent);
}