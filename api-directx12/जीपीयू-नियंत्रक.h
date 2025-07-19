// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.

#pragma once

//DirectX 12 headers. Best Place to learn DirectX12 is original microsoft documentation.
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
const UINT FrameCount = 2; //Initially we are going with double buffering. TODO: Move to triple buffering in future.

//extern ComPtr<ID3D12Device> device;

struct OneMonitorController {
    // System Fetched information.
    bool isScreenInitalized = false;
    int screenPixelWidth = 800;
    int screenPixelHeight = 600;
    int screenPhysicalWidth = 0; // in mm
    int screenPhysicalHeight = 0; // in mm

    //Current ViewPort ( Rendering area ) size.
    int WindowWidth = 800;
    int WindowHeight = 600;

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
    UINT64 fenceValue;

    // Pipeline objects
    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> pipelineState;
    ComPtr<ID3D12Resource> vertexBuffer;
    ComPtr<ID3D12Resource> indexBuffer;
    D3D12_VERTEX_BUFFER_VIEW vertexBufferView;
    D3D12_INDEX_BUFFER_VIEW indexBufferView;
    ComPtr<ID3D12Resource> depthStencilBuffer;
    ComPtr<ID3D12DescriptorHeap> dsvHeap;
    ComPtr<ID3D12Resource> constantBuffer;
    ComPtr<ID3D12DescriptorHeap> cbvHeap;

    UINT8* cbvDataBegin;
};

// 4 is the maximum number of simultaneous screen we are ever going to support. DO NOT CHANGE EVER.
extern OneMonitorController screen[4]; 

void InitD3D(HWND hwnd);
void PopulateCommandList();
void WaitForPreviousFrame();
void CleanupD3D();