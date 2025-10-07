// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.

// compile with: /D_UNICODE /DUNICODE /DWIN32 /D_WINDOWS /c

#include "preCompiledHeadersWindows.h"

// Standard Library we depend upon.
#include <stdlib.h>
#include <string.h>
#include <tchar.h>
#include <chrono>
#include <iomanip>  // for std::setprecision
#include <iostream>
#include <thread>
#include <vector>
#include <random>
#include <png.h>
#include "resource.h"
#include <shellscalingapi.h> // For PROCESS_PER_MONITOR_DPI_AWARE.

// External Library we depend upon.
//#include "ft2build.h"
//#include FT_FREETYPE_H

// Our own codes.
#include "विश्वकर्मा.h"
#include "डेटा.h"
#include "डेटा-सामान्य-2D.h"
#include "डेटा-सामान्य-3D.h"
#include "डेटा-संरचना.h"
#include "डेटा-पाइप.h"
#include "डेटा-बिजली.h"
#include "डेटा-उपकरण.h"
#include "डेटा-स्थिर-मशीन.h"
#include "डेटा-गतिशील-मशीन.h"
#include "MemoryManagerCPU.h"
#include "MemoryManagerGPU-DirectX12.h"

#include "Input_UI_Network_File.h"

#include <windows.h>

/* We have moved to statically compiling the .h/.c files of dependencies. 
Hence we don't need to compile them and generate .lib file and link them separately.
*/
//राम cpuRAMManager;//already defined in विश्वकर्मा.cpp

// --- Global Shared Objects ---
std::atomic<bool> shutdownSignal = false;

extern ThreadSafeQueueCPU todoCPUQueue;
//extern ThreadSafeQueueGPU g_gpuCommandQueue;

// Fences (simulated)
std::mutex g_logicFenceMutex;
std::condition_variable g_logicFenceCV;
uint64_t g_logicFrameCount = -1;

// Copy Thread "Fence"
std::mutex g_copyFenceMutex;
std::condition_variable g_copyFenceCV;
uint64_t g_copyFrameCount = -1; // -1 indicates not yet signaled

int g_monitorCount = 0; // Global monitor count
int primaryMonitorIndex = 0;

// --- Forward declarations of thread functions ---
void UserInputThread();
void NetworkInputThread();
void FileInputThread();
void विश्वकर्मा(); //Main Logic Thread. The ringmaster ! :-)
void GpuCopyThread();
void GpuRenderThread(int monitorId, int refreshRate);

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

// Callback function for EnumDisplayMonitors. Notice: This function is NOT async. Just runs inline.
// But windows API design needs a separate function to be defined, which is called for each monitor.
BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData)
{
    if (g_monitorCount >= 4) {
        // We support maximum 4 monitors
        return FALSE; // Stop enumeration
    }

    OneMonitorController* currentScreen = &gpu.screen[g_monitorCount];
    currentScreen->hMonitor = hMonitor; // Store monitor handle
    MONITORINFOEXW monitorInfo = {};// Get monitor info
    monitorInfo.cbSize = sizeof(MONITORINFOEXW);

    if (GetMonitorInfoW(hMonitor, &monitorInfo)) {
        currentScreen->deviceName = std::wstring(monitorInfo.szDevice);
        currentScreen->monitorRect = monitorInfo.rcMonitor;// Store monitor rectangle (full screen)
        currentScreen->workAreaRect = monitorInfo.rcWork;// Store work area (screen minus taskbar/docked toolbars)
        // Calculate pixel dimensions
        currentScreen->screenPixelWidth = monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left;
        currentScreen->screenPixelHeight = monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top;
        // Check if this is the primary monitor
        currentScreen->isPrimary = (monitorInfo.dwFlags & MONITORINFOF_PRIMARY) != 0;

        UINT dpiX, dpiY;// Get DPI information
        if (SUCCEEDED(GetDpiForMonitor(hMonitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) {
            currentScreen->dpiX = static_cast<int>(dpiX);
            currentScreen->dpiY = static_cast<int>(dpiY);
            currentScreen->scaleFactor = static_cast<double>(dpiX) / 96.0; // 96 DPI = 100% scale
        }
        DISPLAY_DEVICEW displayDevice = {};// Get physical dimensions and additional display properties
        displayDevice.cb = sizeof(DISPLAY_DEVICEW);

        // Find the display device that matches this monitor
        for (DWORD deviceNum = 0; EnumDisplayDevicesW(NULL, deviceNum, &displayDevice, 0); deviceNum++) {
            if (wcscmp(displayDevice.DeviceName, monitorInfo.szDevice) == 0) {
                currentScreen->friendlyName = std::wstring(displayDevice.DeviceString);
                DEVMODEW devMode = {};// Get current display mode for additional info
                devMode.dmSize = sizeof(DEVMODEW);

                if (EnumDisplaySettingsW(displayDevice.DeviceName, ENUM_CURRENT_SETTINGS, &devMode)) {
                    currentScreen->refreshRate = devMode.dmDisplayFrequency;
                    currentScreen->colorDepth = devMode.dmBitsPerPel;
                    currentScreen->orientation = devMode.dmDisplayOrientation;

                    // Try to get physical dimensions from device mode. Note: These might be 0 if not available from driver
                    if (devMode.dmFields & DM_PAPERSIZE) {
                        // Physical dimensions in 0.1mm units (if available)
                        currentScreen->screenPhysicalWidth = devMode.dmPaperWidth / 10;
                        currentScreen->screenPhysicalHeight = devMode.dmPaperLength / 10;
                    }
                }
                break;
            }
        }

        // If physical dimensions not available from device mode, try to calculate from DPI
        if (currentScreen->screenPhysicalWidth == 0 || currentScreen->screenPhysicalHeight == 0) {
            // Calculate physical size from DPI (approximate), 1 inch = 25.4 mm
            double inchesWidth = static_cast<double>(currentScreen->screenPixelWidth) / currentScreen->dpiX;
            double inchesHeight = static_cast<double>(currentScreen->screenPixelHeight) / currentScreen->dpiY;
            currentScreen->screenPhysicalWidth = static_cast<int>(inchesWidth * 25.4);
            currentScreen->screenPhysicalHeight = static_cast<int>(inchesHeight * 25.4);
        }

        // Calculate drawable area dimensions (work area)
        currentScreen->WindowWidth = currentScreen->workAreaRect.right - currentScreen->workAreaRect.left;
        currentScreen->WindowHeight = currentScreen->workAreaRect.bottom - currentScreen->workAreaRect.top;

        currentScreen->isScreenInitalized = true;// Mark as initialized

        // Debug output
        std::wcout << L"Monitor " << g_monitorCount << L":" << std::endl;
        std::wcout << L"  Device: " << currentScreen->deviceName << std::endl;
        std::wcout << L"  Name: " << currentScreen->friendlyName << std::endl;
        std::wcout << L"  Resolution: " << currentScreen->screenPixelWidth << L"x" << currentScreen->screenPixelHeight << std::endl;
        std::wcout << L"  Physical: " << currentScreen->screenPhysicalWidth << L"x" << currentScreen->screenPhysicalHeight << L" mm" << std::endl;
        std::wcout << L"  DPI: " << currentScreen->dpiX << L"x" << currentScreen->dpiY << std::endl;
        std::wcout << L"  Scale: " << static_cast<int>(currentScreen->scaleFactor * 100) << L"%" << std::endl;
        std::wcout << L"  Work Area: " << currentScreen->WindowWidth << L"x" << currentScreen->WindowHeight << std::endl;
        std::wcout << L"  Primary: " << (currentScreen->isPrimary ? L"Yes" : L"No") << std::endl;
        std::wcout << L"  Refresh: " << currentScreen->refreshRate << L" Hz" << std::endl;
        std::wcout << L"  Color Depth: " << currentScreen->colorDepth << L" bits" << std::endl;
        std::wcout << std::endl;
    }

    g_monitorCount++;
    return TRUE; // Continue enumeration
}

void FetchAllMonitorDetails() // Main function to fetch all monitor details
{
    g_monitorCount = 0; // Reset monitor count and clear all screen data

    // Clear all screen structures
    for (int i = 0; i < 4; i++) {
        // Reset monitor-specific fields but preserve D3D objects if they exist
        gpu.screen[i].isScreenInitalized = false;
        gpu.screen[i].screenPixelWidth = 800;
        gpu.screen[i].screenPixelHeight = 600;
        gpu.screen[i].screenPhysicalWidth = 0;
        gpu.screen[i].screenPhysicalHeight = 0;
        gpu.screen[i].hMonitor = NULL;
        gpu.screen[i].deviceName.clear();
        gpu.screen[i].friendlyName.clear();
        ZeroMemory(&gpu.screen[i].monitorRect, sizeof(RECT));
        ZeroMemory(&gpu.screen[i].workAreaRect, sizeof(RECT));
        gpu.screen[i].dpiX = 96;
        gpu.screen[i].dpiY = 96;
        gpu.screen[i].scaleFactor = 1.0;
        gpu.screen[i].isPrimary = false;
        gpu.screen[i].orientation = DMDO_DEFAULT;
        gpu.screen[i].refreshRate = 60;
        gpu.screen[i].colorDepth = 32;
        gpu.screen[i].WindowWidth = 800;
        gpu.screen[i].WindowHeight = 600;
    }

    std::wcout << L"Enumerating monitors..." << std::endl;

    // Enumerate all monitors
    if (!EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, 0)) {
        std::wcerr << L"Failed to enumerate monitors!" << std::endl;
        // Set up default single monitor if enumeration fails
        g_monitorCount = 1;
        gpu.screen[0].isScreenInitalized = true;
        gpu.screen[0].screenPixelWidth = GetSystemMetrics(SM_CXSCREEN);
        gpu.screen[0].screenPixelHeight = GetSystemMetrics(SM_CYSCREEN);
        gpu.screen[0].WindowWidth = gpu.screen[0].screenPixelWidth;
        gpu.screen[0].WindowHeight = gpu.screen[0].screenPixelHeight;
        gpu.screen[0].isPrimary = true;
    }

    std::wcout << L"Found " << g_monitorCount << L" monitor(s)" << std::endl;
}

void AllocateConsoleWindow() {
    AllocConsole();// Allocate a console for this GUI application
    FILE* pCout;// Redirect stdout, stdin, stderr to console
    FILE* pCin;
    FILE* pCerr;
    freopen_s(&pCout, "CONOUT$", "w", stdout);
    freopen_s(&pCin, "CONIN$", "r", stdin);
    freopen_s(&pCerr, "CONOUT$", "w", stderr);
    // Make cout, wcout, cin, wcin, wcerr, cerr, wclog and clog point to console as well
    std::ios::sync_with_stdio(true);
    std::wcout.clear();
    std::cout.clear();
    std::wcerr.clear();
    std::cerr.clear();
    std::wcin.clear();
    std::cin.clear();
    SetConsoleTitleA("Vishwakarma Debug Console");
}

static TCHAR szWindowClass[] = _T("DesktopApp"); // The main window class name.
static TCHAR szTitle[] = _T("Vishwakarma 0 :-) "); // The string that appears in the application's title bar.
HINSTANCE hInst;// Stored instance handle for use in Win32 API calls such as FindResource

// Forward declarations of functions included in this code module:
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
// Forward declaration of the RenderText function
// void RenderText(HDC hdc, FT_Face face, const char* text, int x, int y);

int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPSTR lpCmdLine, _In_ int nCmdShow)
{
#ifdef _DEBUG
    AllocateConsoleWindow();// Only allocate console in debug builds
#endif

    // Enable per Monitor DPI Awareness. Requires Windows 10 version 1703+.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    FetchAllMonitorDetails();

    // Use the monitor details for window creation, i.e. to create window on primary monitor:
    for (int i = 0; i < g_monitorCount; i++) {
        if (gpu.screen[i].isPrimary) {
            primaryMonitorIndex = i;
            break;
        }
    }

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
        gpu.screen[primaryMonitorIndex].WindowWidth / 2,  // Half the work area width
        gpu.screen[primaryMonitorIndex].WindowHeight / 2, // Window size divided by 2 when user press un-maximize button. 
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

    std::cout << "Starting application..." << std::endl;

    gpu.InitD3D(hWnd);// Initialize D3D12. We need this before we can start GPU Copy thread and Render thread.
    // Create and launch all threads
    std::vector<std::thread> threads;
    threads.emplace_back(UserInputThread);
    threads.emplace_back(NetworkInputThread);
    threads.emplace_back(FileInputThread);

    threads.emplace_back(विश्वकर्मा); //Main logic thread. The ringmaster of the application.
    threads.emplace_back(GpuCopyThread);
    threads.emplace_back(GpuRenderThread, 0, 60);  // Monitor 1 at 60Hz
    //threads.emplace_back(GpuRenderThread, 1, 144); // Monitor 2 at 144Hz    

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
        } else {
            //WaitMessage(); // blocks until new Windows message arrives
            gpu.PopulateCommandList();// Render frame

            // Execute command list
            ID3D12CommandList* ppCommandLists[] = { gpu.screen[0].commandList.Get() };
            gpu.screen[0].commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

            /*  The first parameter 1 enables VSync!This, tells the GPU to wait for the monitor's vertical blank interval before presenting the frame
                Synchronize frame presentation with the display's refresh rate. It Throttle application to match the monitor's Hz
                This is more energy efficient way. We are engineering application, not some 1st person shooter video game maximizing fps !
                Without VSync, it was going 650fps(FullHD) on Laptop GPU with 25 Pyramid only geometry.
             */
            gpu.screen[0].swapChain->Present(1, 0); //Present. TODO: Multi Monitor window handling to be developed.
            gpu.WaitForPreviousFrame(); // Wait for GPU

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
        }
    }

    // Let's try to gracefully shutdown all the threads we started.
    // // Signal all threads to stop
    shutdownSignal = true; // UI Input thread, Network Input Thread & File Handling thread listen to this.
    todoCPUQueue.shutdownQueue();
    //g_gpuCommandQueue.shutdownQueue();
    g_logicFenceCV.notify_all();
    g_copyFenceCV.notify_all();

    // Wait for all threads to finish
    for (auto& t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }
    // Clean up resources before exiting the application.
    
    //Cleanup Freetype library.
    //FT_Done_Face(face);
    //FT_Done_FreeType(ft);
    
    gpu.WaitForPreviousFrame(); // Wait for GPU to finish all commands
    gpu.CleanupD3D();// Clean up D3D resources

    std::cout << "Application finished cleanly." << std::endl;

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
    case WM_DISPLAYCHANGE: //When user adds or disconnects a new monitor.
    case WM_DPICHANGED:    //When user manually changes screen resolution through windows settings.
        std::wcout << L"Display configuration changed. Reinitializing..." << std::endl;
        gpu.WaitForPreviousFrame();// Wait for GPU to finish all operations
        gpu.CleanupD3D();// Clean up existing swap chains and D3D resources
        FetchAllMonitorDetails();// Fetch updated monitor details
        gpu.InitD3D(hWnd);// Reinitialize D3D with new monitor configuration
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
        break;
    }

    return 0;
}
