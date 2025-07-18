﻿// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.

// compile with: /D_UNICODE /DUNICODE /DWIN32 /D_WINDOWS /c

#include "preCompiledHeadersWindows.h"

#include <stdlib.h>
#include <string.h>
#include <tchar.h>
//#include "ft2build.h"
//#include FT_FREETYPE_H
#include <iostream>
#include <vishwakarma-2D.h>
#include <random>
#include <png.h>
#include "resource.h"
#include <shellscalingapi.h> // For PROCESS_PER_MONITOR_DPI_AWARE.

#include "विश्वकर्मा.h"
#include "जीपीयू-नियंत्रक.h"

#include <windows.h>

/* We have moved to statically compiling the .h/.c files of dependencies. 
Hence we don't need to compile them and generate .lib file and link them separately.
*/


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

void GetMaxScreenResolution(int& maxWidth, int& maxHeight) {
    maxWidth = GetSystemMetrics(SM_CXSCREEN);
    maxHeight = GetSystemMetrics(SM_CYSCREEN);
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
// void RenderText(HDC hdc, FT_Face face, const char* text, int x, int y);

int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPSTR lpCmdLine, _In_ int nCmdShow)
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

    int screenWidth = 100, screenHeight = 100; //Smallest resize we will allow !
    GetMaxScreenResolution(screenWidth, screenHeight);

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
        screenWidth / 2, screenHeight / 2, // Window size divided by 2 when user press un-maximize button. 
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

    // By default we always initialize application in maximized state.
    // Intentionally we don't remember last closed size and slowdown startup time retrieving that value.
    ShowWindow(hWnd, SW_MAXIMIZE); // hWnd: the value returned from CreateWindow
    //ShowWindow(hWnd, nCmdShow); // nCmdShow: the fourth parameter from WinMain
    UpdateWindow(hWnd);

    // Initialize D3D12
    InitD3D(hWnd);

    /*
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
    */

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
            ID3D12CommandList* ppCommandLists[] = { screen[0].commandList.Get() };
            screen[0].commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

            // Present
            screen[0].swapChain->Present(1, 0);

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

// PURPOSE:  Processes messages for the main window.
// This is the function which runs whenever something changes from Operating System and we are expected to update ourselves.
// Even the user input such as keyboard presses, mouse clicks, open/close are notified to this function.
// Remember this is not the function which keeps running every frame, that is a different infinite loop in WinMain function.
// Question: What happens to WinMain when this function runs? Does that one pause?
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    PAINTSTRUCT ps;
    HDC hdc;
    TCHAR greeting[] = _T("Hello, Vishwakarma!");

    //static FT_Face face;
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
        MessageBoxW(hWnd, L"Middle button clicked", L"Mouse Click", MB_OK);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int zDelta = GET_WHEEL_DELTA_WPARAM(wParam);
        if (zDelta > 0) {
            MessageBoxW(hWnd, L"Mouse wheel scrolled up", L"Mouse Scroll", MB_OK);
        }
        else {
            MessageBoxW(hWnd, L"Mouse wheel scrolled down", L"Mouse Scroll", MB_OK);
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
