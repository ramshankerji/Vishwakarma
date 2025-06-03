// HelloWindowsDesktop.cpp : The beginning of Vishwakarma Desktop Application.
// compile with: /D_UNICODE /DUNICODE /DWIN32 /D_WINDOWS /c

#include "preCompiledHeadersWindows.h"

#include <stdlib.h>
#include <string.h>
#include <tchar.h>
#include "ft2build.h"
#include FT_FREETYPE_H
#include <iostream>
#include <vishwakarma-2D.h>
#include <random>
#include <png.h>
#include <vishwakarmaMainUI.h>
#include "resource.h"
#include <shellscalingapi.h> // For PROCESS_PER_MONITOR_DPI_AWARE.

//DirectX 12 headers. Best Place to learn DirectX12 is original microsoft documentation.
// https://learn.microsoft.com/en-us/windows/win32/direct3d12/direct3d-12-graphics
// You need a good dose of prior C++ knowledge and Computer Fundamentals before learning DirectX12.
// Expect to read at least 2 times before you start grasping it !

#include <windows.h>
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

// Struct for vertex data
struct Vertex {
    XMFLOAT3 position;
    XMFLOAT4 color;
};

// D3D12 objects
const UINT FrameCount = 2;
ComPtr<ID3D12Device> device;
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
D3D12_VERTEX_BUFFER_VIEW vertexBufferView;

// App variables
const int WindowWidth = 800;
const int WindowHeight = 600;

void InitD3D(HWND hwnd);
void PopulateCommandList();
void WaitForPreviousFrame();
void CleanupD3D();



#pragma comment(lib, "libpng16_staticd.lib")
#pragma comment(lib, "zlibd.lib")
#pragma comment(lib, "freetype.lib")

//DirectX12 Libraries.
#pragma comment(lib, "d3d12.lib") //%WindowsSdkDir\Lib%WindowsSDKVersion%\\um\arch
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxguid.lib")


std::wstring GetExecutablePath() {
    wchar_t buffer[MAX_PATH];
    GetModuleFileNameW(NULL, buffer, MAX_PATH);
    std::wstring::size_type pos = std::wstring(buffer).find_last_of(L"\\/");

    return std::wstring(buffer).substr(0, pos);
}

void LoadPngImage(const char* filename, unsigned char** image_data, int* width, int* height)
{
    #pragma warning(disable: 4996)
    FILE* fp = fopen(filename, "rb");
    if (!fp) abort();

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) abort();

    png_infop info = png_create_info_struct(png);
    if (!info) abort();

    if (setjmp(png_jmpbuf(png))) abort();

    png_init_io(png, fp);

    png_read_info(png, info);

    *width = png_get_image_width(png, info);
    *height = png_get_image_height(png, info);
    png_byte color_type = png_get_color_type(png, info);
    png_byte bit_depth = png_get_bit_depth(png, info);

    if (bit_depth == 16)
        png_set_strip_16(png);

    if (color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png);

    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(png);

    if (png_get_valid(png, info, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png);

    if (color_type == PNG_COLOR_TYPE_RGB ||
        color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_filler(png, 0xFF, PNG_FILLER_AFTER);

    if (color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);

    png_read_update_info(png, info);

    *image_data = (unsigned char*)malloc(png_get_rowbytes(png, info) * (*height));
    png_bytep* row_pointers = (png_bytep*)malloc(sizeof(png_bytep) * (*height));
    for (int y = 0; y < *height; y++) {
        row_pointers[y] = (*image_data) + y * png_get_rowbytes(png, info);
    }

    png_read_image(png, row_pointers);

    fclose(fp);
    free(row_pointers);
    png_destroy_read_struct(&png, &info, NULL);
    
}

void DisplayImage(HDC hdc, const unsigned char* image_data, int width, int height)
{
    BITMAPINFO bmi = { 0 };
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // top-down DIB
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    StretchDIBits(hdc, 0, 0, width, height, 0, 0, width, height, image_data, &bmi, DIB_RGB_COLORS, SRCCOPY);
}

// Global variables

// The main window class name.
static TCHAR szWindowClass[] = _T("DesktopApp");

// The string that appears in the application's title bar.
static TCHAR szTitle[] = _T("Vishwakarma 0 :-) ");

// Stored instance handle for use in Win32 API calls such as FindResource
HINSTANCE hInst;

// Forward declarations of functions included in this code module:
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
// Forward declaration of the RenderText function
void RenderText(HDC hdc, FT_Face face, const char* text, int x, int y);


int WINAPI WinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPSTR     lpCmdLine,
    _In_ int       nCmdShow
)
{
    // Enable per Monitor DPI Awareness. Works Windows 8.1 and latter only.
    SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);

    //Create Windows Class.
    WNDCLASSEX wcex;

    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.cbClsExtra = 0;
    wcex.cbWndExtra = 0;
    wcex.hInstance = hInstance;
    wcex.hIcon = LoadIcon(wcex.hInstance, IDI_APPLICATION);
    wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszMenuName = NULL;
    wcex.lpszClassName = szWindowClass;
    wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APP_ICON));
    //wcex.hIconSm = LoadIcon(wcex.hInstance, IDI_APPLICATION);
    wcex.hIconSm = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APP_ICON));

    if (!RegisterClassEx(&wcex))
    {
        MessageBox(NULL,
            _T("Call to RegisterClassEx failed!"),
            _T("Windows Desktop Guided Tour"),
            NULL);

        return 1;
    }

    // Define the window style without WS_CAPTION, but include WS_THICKFRAME and WS_SYSMENU
    DWORD windowStyle = WS_OVERLAPPED | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU;

    // Store instance handle in our global variable
    hInst = hInstance;

    HWND hWnd = CreateWindowEx(
        WS_EX_OVERLAPPEDWINDOW,  //An optional extended window style.
        szWindowClass,           // Window class: The name of the application
        szTitle,       // The text that appears in the title bar
        // The type of window to create
        WS_OVERLAPPEDWINDOW | WS_MINIMIZEBOX | WS_MAXIMIZEBOX,
        // Size and position
        CW_USEDEFAULT, CW_USEDEFAULT, // Initial position (x, y)
        1280, 720, //Corresponds to 720p screen resolution. We expect at least this much. :)
        NULL,      // The parent of this window
        NULL,      // This application does not have a menu bar, we create our own Menu.
        hInstance, // Instance handle, the first parameter from WinMain
        NULL       // Additional application data, not used in this application
    );

    if (!hWnd)
    {
        MessageBox(NULL,
            _T("Call to CreateWindow failed!"),
            _T("Windows Desktop Guided Tour"),
            NULL);

        return 1;
    }

    // The parameters to ShowWindow explained:
    // hWnd: the value returned from CreateWindow
    // nCmdShow: the fourth parameter from WinMain
    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    // Initialize D3D12
    InitD3D(hWnd);


    FT_Library ft;
    FT_Face face;

    if (FT_Init_FreeType(&ft)) {
        std::cerr << "Could not initialize FreeType library" << std::endl;
        return -1;
    }

    if (FT_New_Face(ft, "C:\\Windows\\Fonts\\arial.ttf", 0, &face)) {
        std::cerr << "Could not open font" << std::endl;
        return -1;
    }

    FT_Set_Pixel_Sizes(face, 0, 48); // Set font size to 48 pixels high

    // Main message loop:
    MSG msg = {};

    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) { //Does not block. Returns immediately.
            //We can not use alternate GetMessage() since that one block waiting for windows.
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        else {
            // Render frame
            PopulateCommandList();

            // Execute command list
            ID3D12CommandList* ppCommandLists[] = { commandList.Get() };
            commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

            // Present
            swapChain->Present(1, 0);

            // Wait for GPU
            WaitForPreviousFrame();
        }
    }

    //Cleanup Freetype library.
    //FT_Done_Face(face);
    //FT_Done_FreeType(ft);

    // Wait for GPU to finish all commands
    WaitForPreviousFrame();

    // Clean up D3D resources
    CleanupD3D();

    return (int)msg.wParam;
}

//  FUNCTION: WndProc(HWND, UINT, WPARAM, LPARAM)
//
//  PURPOSE:  Processes messages for the main window.
//
//  WM_PAINT    - Paint the main window
//  WM_DESTROY  - post a quit message and return
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    PAINTSTRUCT ps;
    HDC hdc;
    TCHAR greeting[] = _T("Hello, Vishwakarma!");

    static FT_Face face;
    static const char* uiText001 = "Vishwakarma!";
    static const char* uiText002 = "Name";
    static const char* uiText003 = "Level 21";
    static const char* uiText004 = "Create New";
    static const char* uiText005 = "2D Drawing";
    static const char* uiText006 = "2D P&ID";
    static const char* uiText007 = "2D SLD";
    static const char* uiText008 = "3D Pipes";
    static const char* uiText009 = "3D Structure";
    static const char* uiText010 = "Pressure Vessel";
    static const char* uiText011 = "Heat Exchanger";
    static const char* uiText016 = "3D Gen. Eq.";
    static const char* uiText017 = "Filter";
    static const char* uiText018 = "Air Cooler";
    static const char* uiText019 = "Compressor";
    static const char* uiText020 = "Pump";

    static const char* uiText012 = "Recently Opened files";
    static const char* uiText013 = "Extensions";
    static const char* uiText014 = "Training";
    static const char* uiText015 = "Medals";

    static bool initialized = false;

    static unsigned char* image_data = NULL;
    static int width, height;
    std::wstring exePath = GetExecutablePath();
    int size_needed = WideCharToMultiByte(
        CP_UTF8, 0, &exePath[0], static_cast<int>(exePath.size()), NULL, 0, NULL, NULL);
    std::string str(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &exePath[0], static_cast<int>(exePath.size()), &str[0], size_needed, NULL, NULL);

    std::string fullPath = str + "\\logo.png";

    switch (message)
    {
     /*
    case WM_NCCALCSIZE: //Override the WM_NCCALCSIZE message to extend the client area into the title bar space.
        if (wParam == TRUE) {
            // Extend the client area to cover the title bar
            NCCALCSIZE_PARAMS* pncsp = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
            pncsp->rgrc[0].top -= GetSystemMetrics(SM_CYCAPTION);
            return 0;
        },,,,
        break;
        */
    case WM_CREATE:
        
        std::cout << "Full path to logo.png: " << fullPath << std::endl;

        LoadPngImage(fullPath.c_str(), &image_data, &width, &height);
        //LoadPngImage("C:\\RAM\\CODE\\Vishwakarma\\api-direct3d12\\x64\\Debug\\logo.png", &image_data, &width, &height);
        break;

    case WM_LBUTTONDOWN: {
        int x = LOWORD(lParam);
        int y = HIWORD(lParam);
        //MessageBox(hWnd, L"Left button clicked", L"Mouse Click", MB_OK);
        return 0;
    }
    case WM_RBUTTONDOWN: {
        int x = LOWORD(lParam);
        int y = HIWORD(lParam);
        //MessageBox(hWnd, L"Right button clicked", L"Mouse Click", MB_OK);
        return 0;
    }
    case WM_MBUTTONDOWN: {
        MessageBox(hWnd, L"Middle button clicked", L"Mouse Click", MB_OK);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int zDelta = GET_WHEEL_DELTA_WPARAM(wParam);
        if (zDelta > 0) {
            MessageBox(hWnd, L"Mouse wheel scrolled up", L"Mouse Scroll", MB_OK);
        }
        else {
            MessageBox(hWnd, L"Mouse wheel scrolled down", L"Mouse Scroll", MB_OK);
        }
        return 0;
    }
    case WM_PAINT:
    {
        // We're not using GDI for rendering anymore - DirectX12 handles all rendering
        // Just validate the paint message to prevent Windows from continuously sending WM_PAINT
        // TODO: Figure out why can't we remove BeginPaint & EndPaint commands also.

        hdc = BeginPaint(hWnd, &ps);
        /*

        // Initialize FreeType face only once
        if (!initialized) {
            FT_Library ft;
            if (FT_Init_FreeType(&ft)) {
                std::cerr << "Could not initialize FreeType library" << std::endl;
                return -1;
            }

            if (FT_New_Face(ft, "C:\\Windows\\Fonts\\arial.ttf", 0, &face)) {
                std::cerr << "Could not open font" << std::endl;
                return -1;
            }

            FT_Set_Pixel_Sizes(face, 0, 24); // Set font size to 48 pixels high
            initialized = true;
        }

        // Get the dimensions of the client area
        RECT rect;
        GetClientRect(hWnd, &rect);

        // Calculate the position to start drawing the text to center it
        int x = (rect.right - rect.left) / 2;
        int y = (rect.bottom - rect.top) / 2;

        //RenderText(hdc, face, uiText001, 10, 20);
        
        if (1) // Homepage of the application.
        {
            
            RenderText(hdc, face, uiText002, 10, 60);
            RenderText(hdc, face, uiText003, 10, 80);
            RenderText(hdc, face, uiText015, 10, 120);

            RenderText(hdc, face, uiText004, 200, 80);

            RenderText(hdc, face, uiText005, 200, 120);
            RenderText(hdc, face, uiText006, 400, 120);
            RenderText(hdc, face, uiText007, 600, 120);

            RenderText(hdc, face, uiText008, 200, 220);
            RenderText(hdc, face, uiText009, 400, 220);
            RenderText(hdc, face, uiText016, 600, 220);
            RenderText(hdc, face, uiText010, 200, 320);
            RenderText(hdc, face, uiText011, 400, 320);
            RenderText(hdc, face, uiText017, 600, 320);
            RenderText(hdc, face, uiText018, 200, 420);
            RenderText(hdc, face, uiText019, 400, 420);
            RenderText(hdc, face, uiText020, 600, 420);

            RenderText(hdc, face, uiText012, 200, 520);

            RenderText(hdc, face, uiText013, 1000, 60);
            RenderText(hdc, face, uiText014, 1000, 320);
            
        }

        if (0) //2D Drafting module
        {
            
            RenderText(hdc, face, "I am 2D Drafting Module", 200, 80);

            RenderText(hdc, face, "Line", 200, 320);
            Draw2DLine(hdc, 150, 250, 250, 150);

            RenderText(hdc, face, "Polyline", 400, 320);
            Draw2DLine(hdc, 350, 250, 400, 200);
            Draw2DLine(hdc, 400, 200, 450, 250);
            Draw2DLine(hdc, 450, 250, 500, 150);

            RenderText(hdc, face, "Triangle", 600, 320);
            Draw2DLine(hdc, 650, 250, 700, 150);
            Draw2DLine(hdc, 700, 150, 750, 250);
            Draw2DLine(hdc, 750, 250, 650, 250);

            RenderText(hdc, face, "Rectangle", 200, 620);
            Draw2DLine(hdc, 150, 550, 150, 450);
            Draw2DLine(hdc, 150, 450, 300, 450);
            Draw2DLine(hdc, 300, 450, 300, 550);
            Draw2DLine(hdc, 300, 550, 150, 550);

            RenderText(hdc, face, "Circle", 400, 620);

            RenderText(hdc, face, "Text", 600, 620);
            RenderText(hdc, face, "EIL :-)", 600, 520);
            
        }

        
        // Create a random device and a random number generator
        std::random_device rd;  // Obtain a random number from hardware
        std::mt19937 gen(rd()); // Seed the generator

        // Define the distribution range
        std::uniform_int_distribution<> distr(100, 500);
        
        if (image_data)
        {
            //DisplayImage(hdc, image_data, width, height);
        }
        */
        EndPaint(hWnd, &ps);
        
        break;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
        break;
    }

    return 0;
}

//RenderText is GDI. Not DirectX12. It is to be phased out as soon as we have UI working in DirectX12.
void RenderText(HDC hdc, FT_Face face, const char* text, int x, int y) {
    FT_GlyphSlot g = face->glyph;

    for (const char* p = text; *p; p++) {
        if (FT_Load_Char(face, *p, FT_LOAD_RENDER)) {
            continue; // Ignore errors
        }

        // Draw the character here
        for (int row = 0; row < g->bitmap.rows; ++row) {
            for (int col = 0; col < g->bitmap.width; ++col) {
                if (g->bitmap.buffer[row * g->bitmap.width + col]) {
                    SetPixel(hdc, x + col + g->bitmap_left, y + row - g->bitmap_top, RGB(0, 0, 0));
                }
            }
        }

        x += g->advance.x >> 6; // Advance to the next character
    }
}

/*
IID_PPV_ARGS is a MACRO used in DirectX (and COM programming in general) to help safely and correctly 
retrieve interface pointers during object creation or querying. It helps reduce repetative typing of codes.
COM interfaces are identified by unique GUIDs. Than GUID pointer is converted to appropriate pointer type.

Ex: IID_PPV_ARGS(&device) expands to following:
IID iid = __uuidof(ID3D12Device);
void** ppv = reinterpret_cast<void**>(&device);
*/

void InitD3D(HWND hwnd) {
    UINT dxgiFactoryFlags = 0;

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
    device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&commandQueue));

    // Create swap chain
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = FrameCount;
    swapChainDesc.Width = WindowWidth;
    swapChainDesc.Height = WindowHeight;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;

    ComPtr<IDXGISwapChain1> tempSwapChain;
    factory->CreateSwapChainForHwnd(
        commandQueue.Get(),
        hwnd,
        &swapChainDesc,
        nullptr,
        nullptr,
        &tempSwapChain
    );

    tempSwapChain.As(&swapChain);
    frameIndex = swapChain->GetCurrentBackBufferIndex();

    // Create descriptor heap for render target views
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = FrameCount;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap));

    rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // Create render target views
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(rtvHeap->GetCPUDescriptorHandleForHeapStart());
    for (UINT i = 0; i < FrameCount; i++) {
        swapChain->GetBuffer(i, IID_PPV_ARGS(&renderTargets[i]));
        device->CreateRenderTargetView(renderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.Offset(1, rtvDescriptorSize);
    }

    // Create command allocator
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator));

    // Create empty root signature
    CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc;
    rootSignatureDesc.Init(0, nullptr, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&rootSignature));

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

    D3DCompile(vertexShaderCode, strlen(vertexShaderCode), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", compileFlags, 0, &vertexShader, nullptr);
    D3DCompile(pixelShaderCode, strlen(pixelShaderCode), nullptr, nullptr, nullptr, "PSMain", "ps_5_0", compileFlags, 0, &pixelShader, nullptr);

    // Define the vertex input layout
    D3D12_INPUT_ELEMENT_DESC inputElementDescs[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    // Create the pipeline state object
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
    psoDesc.pRootSignature = rootSignature.Get();
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
    device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pipelineState));

    // Create the command list. Note that this is default pipelineStete for the command list.
    // It can be changed inside command list also by calling ID3D12GraphicsCommandList::SetPipelineState.
    // CommandList is : List of various Commands including repeated calls of many CommandBundles.
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator.Get(), pipelineState.Get(), IID_PPV_ARGS(&commandList));
    commandList->Close();

    // Create vertex buffer
    Vertex triangleVertices[] = {
        { XMFLOAT3(0.0f, 0.5f, 0.0f), XMFLOAT4(1.0f, 0.0f, 0.0f, 1.0f) },    // Top vertex (red)
        { XMFLOAT3(0.5f, -0.5f, 0.0f), XMFLOAT4(0.0f, 1.0f, 0.0f, 1.0f) },   // Bottom right vertex (green)
        { XMFLOAT3(-0.5f, -0.5f, 0.0f), XMFLOAT4(0.0f, 0.0f, 1.0f, 1.0f) }   // Bottom left vertex (blue)
    };

    const UINT vertexBufferSize = sizeof(triangleVertices);

    // Create upload heap and copy vertex data
    ComPtr<ID3D12Resource> vertexBufferUpload;

    // Create default heap for vertex buffer
    // Define the heap properties for the default heap
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    // Define the resource description for the vertex buffer
    auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(vertexBufferSize);

    device->CreateCommittedResource(
        &heapProps,                   // Correct: Pass address of the local variable
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,                // Correct: Pass address of the local variable
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&vertexBuffer));

    // Create upload heap

    // Define the heap properties for the UPLOAD heap
    auto uploadHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

    // Define the resource description for the upload buffer (same size as the destination)
    auto uploadBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(vertexBufferSize);

    device->CreateCommittedResource(
        &uploadHeapProps,              // Correct: Pass address of the local variable
        D3D12_HEAP_FLAG_NONE,
        &uploadBufferDesc,             // Correct: Pass address of the local variable
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&vertexBufferUpload));

    // Copy data to upload heap
    D3D12_SUBRESOURCE_DATA vertexData = {};
    vertexData.pData = triangleVertices;
    vertexData.RowPitch = vertexBufferSize;
    vertexData.SlicePitch = vertexData.RowPitch;

    // Open command list and record copy commands
    commandAllocator->Reset();
    commandList->Reset(commandAllocator.Get(), pipelineState.Get());
    UpdateSubresources<1>(commandList.Get(), vertexBuffer.Get(), vertexBufferUpload.Get(), 0, 0, 1, &vertexData);
    
    //Following line error removed by Gemini.
    //commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(vertexBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER));
    // Define the resource barrier to transition the vertex buffer
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        vertexBuffer.Get(),                     // The resource to transition
        D3D12_RESOURCE_STATE_COPY_DEST,         // The state before the transition
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER // The state after the transition
    );

    // Record the barrier command in the command list
    commandList->ResourceBarrier(1, &barrier); // Correct: Pass the address of the local 'barrier' variable

    // Close command list. It mostly runs synchronously with little work deferred. Completes quickly. 
    // Close():  Transitions the command list from recording mode to execution-ready mode.
    // Validates Commands / Catch errors, Compress (driver-specific optimization), to Immutable (Read-Only).
    commandList->Close();
    // Exicute the command list.
    ID3D12CommandList* ppCommandLists[] = { commandList.Get() };
    commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

    // Create vertex buffer view
    vertexBufferView.BufferLocation = vertexBuffer->GetGPUVirtualAddress();
    vertexBufferView.StrideInBytes = sizeof(Vertex);
    vertexBufferView.SizeInBytes = vertexBufferSize;

    // Create synchronization objects
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    fenceValue = 1;
    fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    // Wait for initialization to complete
    WaitForPreviousFrame();
}

void PopulateCommandList() {
    // Reset allocator and command list
    commandAllocator->Reset();
    commandList->Reset(commandAllocator.Get(), pipelineState.Get());

    // Set necessary state
    commandList->SetGraphicsRootSignature(rootSignature.Get());
    
    //Following 2 error lines fixed by ChatGPT.
    //commandList->RSSetViewports(1, &CD3DX12_VIEWPORT(0.0f, 0.0f, static_cast<float>(WindowWidth), static_cast<float>(WindowHeight)));
    //commandList->RSSetScissorRects(1, &CD3DX12_RECT(0, 0, WindowWidth, WindowHeight));
    // 1) Create named variables (l‑values)
    CD3DX12_VIEWPORT viewport(0.0f, 0.0f,
        static_cast<float>(WindowWidth),
        static_cast<float>(WindowHeight)
    );

    CD3DX12_RECT scissorRect(0, 0, WindowWidth, WindowHeight);

    // 2) Now you can take their addresses and call the methods
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissorRect);


    // Indicate that the back buffer will be used as a render target
    // Correct: Create barrier1 variable
    auto barrier1 = CD3DX12_RESOURCE_BARRIER::Transition(renderTargets[frameIndex].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    commandList->ResourceBarrier(1, &barrier1); // Pass address of barrier1

    // Record commands
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(rtvHeap->GetCPUDescriptorHandleForHeapStart(), frameIndex, rtvDescriptorSize);
    commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    // Clear render target
    const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f }; // Example color, adjust as needed
    commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

    // Draw triangle
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->IASetVertexBuffers(0, 1, &vertexBufferView);
    commandList->DrawInstanced(3, 1, 0, 0);

    // Indicate that the back buffer will now be used to present
    // Correct: Create barrier2 variable
    auto barrier2 = CD3DX12_RESOURCE_BARRIER::Transition(renderTargets[frameIndex].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    commandList->ResourceBarrier(1, &barrier2); // Pass address of barrier2

    // Close command list
    commandList->Close();
}

void WaitForPreviousFrame() {
    // Signal and increment the fence value
    const UINT64 currentFenceValue = fenceValue;
    commandQueue->Signal(fence.Get(), currentFenceValue);
    fenceValue++;

    // Wait until the previous frame is finished
    if (fence->GetCompletedValue() < currentFenceValue) {
        fence->SetEventOnCompletion(currentFenceValue, fenceEvent);
        WaitForSingleObject(fenceEvent, INFINITE);
    }

    // Update the frame index
    frameIndex = swapChain->GetCurrentBackBufferIndex();
}

void CleanupD3D() {
    // Wait for the GPU to be done with all resources
    WaitForPreviousFrame();

    CloseHandle(fenceEvent);
}
