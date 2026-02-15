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
#include <random>
#include <png.h>
#include <shared_mutex>
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

#include "UserInputProcessing.h"
#include "Input_UI_Network_File.h"

#include <windows.h>
#include <windowsx.h> // For some macros like GET_X_LPARAM, GET_Y_LPARAM etc.

/* We have moved to statically compiling the .h/.c files of dependencies. 
Hence we don't need to compile them and generate .lib file and link them separately.
*/

// Global Shared Objects
extern राम cpu;
extern शंकर gpu;
std::atomic<bool> shutdownSignal = false;

int primaryMonitorIndex = 0;

extern std::random_device rd;
std::shared_mutex monitorMutex; //Where there is topology change, pause windows move / resize.

// Render Thread Management.
std::vector<std::thread> renderThreads;
std::atomic<bool> pauseRenderThreads = false;

// Forward declarations of thread functions
void NetworkInputThread();
void FileInputThread();
void विश्वकर्मा(uint64_t); //Main Logic Thread. The ringmaster ! :-)
void GpuCopyThread();
void GpuRenderThread(int monitorId, int refreshRate);

std::wstring GetExecutablePath() {
    wchar_t buffer[MAX_PATH];
    GetModuleFileNameW(NULL, buffer, MAX_PATH);
    std::wstring::size_type pos = std::wstring(buffer).find_last_of(L"\\/");

    if (pos == std::wstring::npos) return buffer; //Guard against pos == npos → overflow. ? Explain !
    return std::wstring(buffer).substr(0, pos);
}

// Defined in ImageHandling.cpp
void LoadPngImage(const char* filename, unsigned char** image_data, int* width, int* height);
void LoadPngImageFromMemory(const void* data, size_t size, unsigned char** image_data, int* width, int* height);
// Load directly from Windows Resource
void LoadPngFromResource(int resourceID, unsigned char** image_data, int* width, int* height) {
    HRSRC hRes = FindResource(NULL, MAKEINTRESOURCE(resourceID), RT_RCDATA);
    if (!hRes) {
        std::cerr << "Error: Could not find resource ID " << resourceID << std::endl;
        return;
    }

    HGLOBAL hMem = LoadResource(NULL, hRes);
    if (!hMem) return;

    void* data = LockResource(hMem);
    DWORD size = SizeofResource(NULL, hRes);

    LoadPngImageFromMemory(data, size, image_data, width, height);

    // Note: Resources are freed automatically when the module unloads, 
    // strictly speaking FreeResource is a no-op on 32-bit/64-bit Windows.
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
    if (gpu.currentMonitorCount >= MV_MAX_MONITORS) {
        std::wcout << L"Max monitors exceeded (" << MV_MAX_MONITORS << L"). Skipping additional monitor." << std::endl;
        return FALSE; // Stop enumeration if limit reached
    }
    OneMonitorController* currentScreen = &gpu.screens[gpu.currentMonitorCount];

    currentScreen->hMonitor = hMonitor;// Store monitor handle
    currentScreen->isVirtualMonitor = false;
    currentScreen->dpiX = 96;// Default DPI to 96 to prevent divide-by-zero later if GetDpiForMonitor fails
    currentScreen->dpiY = 96;

    MONITORINFOEXW monitorInfo = {};// Get monitor info
    monitorInfo.cbSize = sizeof(MONITORINFOEXW);

    // CRITICAL CHECK: If this fails, we have a "Ghost Monitor". Remove it and skip.
    if (!GetMonitorInfoW(hMonitor, &monitorInfo)) {
        // No increment happened yet, so just skip.
        std::wcout << L"GetMonitorInfoW returned nothing. Skipping this monitor." << std::endl;
        return TRUE; // Continue enumeration hoping for valid monitors
    }

    currentScreen->monitorName = std::wstring(monitorInfo.szDevice);
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

    // Debug output
    std::wcout << L"Monitor " << gpu.currentMonitorCount << L":" << std::endl;
    std::wcout << L"Device: " << currentScreen->monitorName << std::endl;
    std::wcout << L"Name: " << currentScreen->friendlyName << std::endl;
    std::wcout << L"Resolution: " << currentScreen->screenPixelWidth << L"x" << currentScreen->screenPixelHeight << std::endl;
    std::wcout << L"Physical: " << currentScreen->screenPhysicalWidth << L"x" << currentScreen->screenPhysicalHeight << L" mm" << std::endl;
    std::wcout << L"DPI: " << currentScreen->dpiX << L"x" << currentScreen->dpiY << std::endl;
    std::wcout << L"Scale: " << static_cast<int>(currentScreen->scaleFactor * 100) << L"%" << std::endl;
    std::wcout << L"Work Area: " << currentScreen->WindowWidth << L"x" << currentScreen->WindowHeight << std::endl;
    std::wcout << L"Primary: " << (currentScreen->isPrimary ? L"Yes" : L"No") << std::endl;
    std::wcout << L"Refresh: " << currentScreen->refreshRate << L" Hz" << std::endl;
    std::wcout << L"Color Depth: " << currentScreen->colorDepth << L" bits" << std::endl;
    std::wcout << std::endl;

    // Default initialization if physical calc fails. Fallback: 1 inch = 25.4 mm
    if (currentScreen->screenPhysicalWidth == 0) {
        currentScreen->screenPhysicalWidth = static_cast<int>((currentScreen->screenPixelWidth / (float)currentScreen->dpiX) * 25.4f);
        currentScreen->screenPhysicalHeight = static_cast<int>((currentScreen->screenPixelHeight / (float)currentScreen->dpiY) * 25.4f);
    }

    // Calculate drawable area dimensions (work area)
    currentScreen->WindowWidth = currentScreen->workAreaRect.right - currentScreen->workAreaRect.left;
    currentScreen->WindowHeight = currentScreen->workAreaRect.bottom - currentScreen->workAreaRect.top;
    currentScreen->isScreenInitalized = true;// Mark as initialized
    
    gpu.currentMonitorCount++; // Increment count after successful initialization
    return TRUE; // Continue enumeration
}

void FetchAllMonitorDetails() // Main function to fetch all monitor details
{
    std::unique_lock<std::shared_mutex> lock(monitorMutex); // Blocks everyone
    gpu.currentMonitorCount = 0; // Reset monitor count and clear all screen data
    //memset(gpu.screens, 0, sizeof(gpu.screens)); // "Clear" the array. TODO latter without memset.
    std::wcout << L"Enumerating monitors..." << std::endl; // Try to enumerate real monitors
    EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, 0);

    /* Default values I expect. This was used during initial development.
    Latter we started relying on Windows API results. This can also be used for headless runs.
        gpu.screens[i].isScreenInitalized = false;
        gpu.screens[i].screenPixelWidth = 800;
        gpu.screens[i].screenPixelHeight = 600;
        gpu.screens[i].screenPhysicalWidth = 0;
        gpu.screens[i].screenPhysicalHeight = 0;
        gpu.screens[i].hMonitor = NULL;
        gpu.screens[i].deviceName.clear();
        gpu.screens[i].friendlyName.clear();
        ZeroMemory(&gpu.screens[i].monitorRect, sizeof(RECT));
        ZeroMemory(&gpu.screens[i].workAreaRect, sizeof(RECT));
        gpu.screens[i].dpiX = 96;
        gpu.screens[i].dpiY = 96;
        gpu.screens[i].scaleFactor = 1.0;
        gpu.screens[i].isPrimary = false;
        gpu.screens[i].orientation = DMDO_DEFAULT;
        gpu.screens[i].refreshRate = 60;
        gpu.screens[i].colorDepth = 32;
        gpu.screens[i].WindowWidth = 800;
        gpu.screens[i].WindowHeight = 600;
    */
    std::wcout << L"Total monitors found =" << gpu.currentMonitorCount << std::endl;
    // HEADLESS CHECK: If the API returned success but found 0 monitors, OR if the API failed...
    if (gpu.currentMonitorCount == 0) {
        std::wcout << L"Headless mode detected (No monitors found). Creating Virtual Display." << std::endl;

        // Force-add one virtual monitor so the rest of the app doesn't crash
        if (gpu.currentMonitorCount >= MV_MAX_MONITORS) {
            std::wcout << L"Cannot add virtual monitor: Max exceeded." << std::endl;
            return;
        }
        OneMonitorController * s = &gpu.screens[gpu.currentMonitorCount];

        s->isScreenInitalized = true;
        s->monitorName = L"HEADLESS_DISPLAY";
        s->friendlyName = L"Virtual Adapter";

        // Default to a safe resolution (e.g., SVGA or FullHD)
        s->screenPixelWidth = 1024;
        s->screenPixelHeight = 768;

        // If GetSystemMetrics fails (returns 0), we stick to the hardcoded 1024x768
        int sysW = GetSystemMetrics(SM_CXSCREEN);
        int sysH = GetSystemMetrics(SM_CYSCREEN);
        if (sysW > 0) s->screenPixelWidth = sysW;
        if (sysH > 0) s->screenPixelHeight = sysH;

        s->WindowWidth = s->screenPixelWidth;
        s->WindowHeight = s->screenPixelHeight;

        // Critical: Mark as primary so the index 0 logic works
        s->isPrimary = true;
        s->dpiX = 96;
        s->dpiY = 96;
        s->scaleFactor = 1.0;
        s->refreshRate = 60; // Virtual 60Hz
        s->isVirtualMonitor = true;
        gpu.currentMonitorCount++;
    }
}

// Helper to find a monitor in the OLD list by name
int FindMonitorIndexByName(const std::vector<OneMonitorController>& list, const std::wstring& name) {
    for (int i = 0; i < list.size(); ++i) { if (list[i].monitorName == name) return i; }
    return -1;
}
int FindMonitorIndexByName(const OneMonitorController* list, int count, const std::wstring& name) {
    for (int i = 0; i < count; ++i) { if (list[i].monitorName == name) return i; }
    return -1;
}

DATASETTAB* GetActiveTabFromHwnd(HWND hWnd) {
    uint16_t* windowList = publishedWindowIndexes.load(std::memory_order_acquire);
    uint16_t windowCount = publishedWindowCount.load(std::memory_order_acquire);
    
    for (uint16_t i = 0; i < windowCount; ++i) {
        SingleUIWindow & w = allWindows[windowList[i]];
        if (w.hWnd == hWnd) {
            int tabIndex = w.activeTabIndex;
            if (tabIndex < 0) return nullptr;
            return &allTabs[tabIndex];
        }
    }
    return nullptr;
}

void UpdateWindowMonitorAffinity(SingleUIWindow& window, int newMonitorIndex) {
    /*Each window is backed by 1 swap chain. Each swap chain is permanently tied to a commandQueue.
    We have 1 commandQueue per monitor. So when the window move from 1 monitor to another, 
    we must purge existing swap chain, and create a new one associated with destination monitor's commandQueue.
    Associating a new swap chain to an existing windows is well supported and fast enough. */
    
    window.isMigrating = true;// SWITCH OFF RENDERING. The threads will now skip this window in their loops.

    // We must ensure the GPU is done with the old SwapChain before destroying it.
    int oldIndex = window.currentMonitorIndex;
    if (oldIndex >= 0) {
        // You need to pass the *Window's* specific resources which hold the fence? 
        // Actually, use the Monitor's Queue directly for a hard flush.
        ID3D12CommandQueue* oldQueue = gpu.screens[oldIndex].commandQueue.Get();

        // Simple flush to ensure safety
        ComPtr<ID3D12Fence> flushFence;
        gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&flushFence));
        oldQueue->Signal(flushFence.Get(), 1);
        HANDLE hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (flushFence->GetCompletedValue() < 1) {
            flushFence->SetEventOnCompletion(1, hEvent);
            WaitForSingleObject(hEvent, INFINITE);
        }
        CloseHandle(hEvent);
    }

    gpu.CleanupWindowResources(window.dx);// Now it is safe to destroy the old SwapChain.

    // Update the monitor index so the destination monitor's render thread picks it up.
    window.currentMonitorIndex = newMonitorIndex;

    // Create the NEW SwapChain on the NEW Monitor's Queue.
    ID3D12CommandQueue* destinationCommandQueue = gpu.screens[newMonitorIndex].commandQueue.Get();
    gpu.InitD3DPerWindow(window.dx, window.hWnd, destinationCommandQueue);
    window.isMigrating = false;// SWITCH ON RENDERING

    std::wcout << "Window migrated to Monitor " << newMonitorIndex << std::endl;
}

void RestartRenderThreads() { // The "Safe" Restart Function. Runs for initial render thread creation as well.
	// We want to preserve CommandQueues because they are tied to the physical monitor and Swap Chains.
	// However actual render threads are lightweight, so we can just restart them.
	// Adding a monitor is a RARE event, so a brief pause in rendering is acceptable.
    // Either the user intentionally added / removed a monitor manually, so he expects windows to move around.
    // Or maybe electricity went out and external display connected to laptop turned off. These are rare event.

	pauseRenderThreads = true; // Pause Logic. Causes render threads (if running) to exit their loops safely.
    for (auto& t : renderThreads) { if (t.joinable()) t.join(); }
    renderThreads.clear();

    std::wcout << "RestartRenderThreads called. Analyzing..." << std::endl;
    // Capture OLD topology to salvage queues. TODO: Remove garbage values in case monitor count decreased.
    OneMonitorController oldScreens[MV_MAX_MONITORS];
	for (int i = 0; i < gpu.currentMonitorCount; i++) {
        oldScreens[i] = gpu.screens[i];
    }
	int oldMonitorCount = gpu.currentMonitorCount;
    gpu.currentMonitorCount = 0; // Global monitor count. It can be 0 when no monitors are found (headless mode)

    // Enumerate NEW topology. This populates gpu.screens with FRESH data, no queues yet.
    // Never exceeds MAX_MONITORS count. If it does, extra monitors are ignored with a warning.
	EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, 0); 
    // Handle Headless/Virtual Monitor logic if needed (copy from your FetchAllMonitorDetails)
    //if (gpu.screens.empty()) { /* ... Insert headless logic here ... */ }

    // MERGE: Create Queues OR Salvage them from oldScreens
	bool matchFound = false;
    for (int i = 0; i < gpu.currentMonitorCount; i++) {
        matchFound = false;
		for (int j = 0; j < oldMonitorCount; j++) {
            if (oldScreens[j].monitorName == gpu.screens[i].monitorName) {
                gpu.screens[i].commandQueue = oldScreens[j].commandQueue;
                std::wcout << L"Preserved Queue for: " << gpu.screens[i].monitorName << std::endl;
				matchFound = true;
                break;
            }
        }
        if (matchFound == false) {
            // NEW MONITOR (or first run). Create a new Queue.
            D3D12_COMMAND_QUEUE_DESC queueDesc = {};
            queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            gpu.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&gpu.screens[i].commandQueue));
            std::wcout << L"Created New Queue for: " << gpu.screens[i].monitorName << std::endl;
        }
    }

    uint16_t * windowList = publishedWindowIndexes.load(std::memory_order_acquire);
    uint16_t windowCount = publishedWindowCount.load(std::memory_order_acquire);
    for (uint16_t wi = 0; wi < windowCount; ++wi){
        SingleUIWindow & window = allWindows[windowList[wi]];

        // Ask Windows: "Where is this window physically right now?"
        HMONITOR hWinMonitor = MonitorFromWindow(window.hWnd, MONITOR_DEFAULTTONEAREST);
        // Find the index in our NEW gpu.screens list
        int newMonitorIdx = 0;
        for (int i = 0; i < gpu.currentMonitorCount; ++i) {
            if (gpu.screens[i].hMonitor == hWinMonitor) {
                newMonitorIdx = i;
                break;
            }
        }

        // CHECK: Do we need to migrate this window? We migrate if:
        // a) The monitor index changed (moved logically)
        // b) The underlying CommandQueue changed (e.g. moved to a totally new monitor)
        ID3D12CommandQueue* newQueue = gpu.screens[newMonitorIdx].commandQueue.Get();

        // We can't easily check 'oldQueue' since we don't store it in Window, 
        // but we can check if the monitor index changed.
        // OPTIMIZATION: If the monitor index is the same, and we preserved the queue,
        // we do NOTHING. The window stays happy.
        bool needsMigration = (window.currentMonitorIndex != newMonitorIdx);

        // Edge case: If the monitor index stayed the same (e.g. 0 -> 0), but it's actually
        // a different physical monitor (old 0 unplugged, new 0 took its place), 
        // the 'commandQueue' pointer check handles it.
        // Since we don't store the old queue in Window, we rely on the index check +
        // the fact that we preserved queues by DeviceName.
        if (needsMigration) {
            std::wcout << "Migrating Window to Monitor " << newMonitorIdx << std::endl;
            gpu.CleanupWindowResources(window.dx);// Cleanup OLD resources (Swap Chain, RTVs)

            // Initialize NEW resources with the NEW Queue
            gpu.InitD3DPerWindow(window.dx, window.hWnd, newQueue);
            window.currentMonitorIndex = newMonitorIdx;
        }
    }
    
	pauseRenderThreads = false; //When this is true, render threads terminate their loops.
    for (int i = 0; i < gpu.currentMonitorCount; i++) { // Spawn Threads (One per Monitor)
        int refreshRate = gpu.screens[i].refreshRate;
        renderThreads.emplace_back(GpuRenderThread, i, refreshRate);
        std::wcout << "Spawned Render Thread for Monitor " << i << " (" << refreshRate << "Hz)" << std::endl;
    }
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
    std::wcerr.clear();
    std::cerr.clear();
    std::wcin.clear();
    std::cin.clear();
    SetConsoleTitleA("Vishwakarma Debug Console");
}

static const wchar_t szWindowClass[] = L"विश्वकर्मा"; // The main window class name.
static const wchar_t* szTitle = L"विश्वकर्मा 0 :-)"; // The string that appears in the application's title bar.

HINSTANCE hInst;// Stored instance handle for use in Win32 API calls such as FindResource
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);// Forward declarations of functions included in this code module.

// Ask to AI: Whats the difference between WinMain and wWinMain for Windows Desktop C++ DirectX12 application?
// WinMain: This was legacy name. New name is wWinMain with Unicode support.
int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow)
{
    #ifdef _DEBUG
        AllocateConsoleWindow();// Only allocate console in debug builds
    #endif

    // Enable per Monitor DPI Awareness. Requires Windows 10 version 1703+.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    FetchAllMonitorDetails();

    // Use the monitor details for window creation, i.e. to create window on primary monitor:
    for (int i = 0; i < gpu.currentMonitorCount; i++) {
        if (gpu.screens[i].isPrimary) {
            primaryMonitorIndex = i;
            break;
        }
    }

	//Create Windows Class. The primary purpose if to link to Window Procedure (WndProc) to handle messages.
	//For simplicity we will have only 1 type of window in this application.
    WNDCLASSEXW wcex;

    wcex.cbSize = sizeof(WNDCLASSEXW);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
	wcex.lpfnWndProc = WndProc; //This is the root of all Windows message handling for our application.
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

    if (!RegisterClassExW(&wcex))
    {
        MessageBox(NULL,
            _T("Call to RegisterClassEx failed!"),
            _T("Something bad happened. Failed to register Windows Class."),
            NULL);

        return 1;
    }

    // Define the window style without WS_CAPTION, but include WS_THICKFRAME and WS_SYSMENU
    DWORD windowStyle = WS_OVERLAPPED | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU;
    hInst = hInstance;// Store instance handle in our global variable

    // Initialize D3D12. We need this before we can start GPU Copy thread and Render thread.
    // Window Creation and InitD3DGlobal (Happens ONCE) 
    gpu.InitD3DDeviceOnly();// Initialize Global D3D Device - DO NOT CALL THIS AGAIN
    
    // Now that the Device exists, create a Command Queue for every monitor found.
    for (auto& screen : gpu.screens) {
        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        gpu.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&screen.commandQueue));
    }

    // SETUP TABS (The Data) CRITICAL: Resize first to prevent pointer invalidation when threads start!
    publishedTabIndexes.store(activeTabIndexesA, std::memory_order_release);
    publishedTabCount.store(0, std::memory_order_release);

    publishedWindowIndexes.store(activeWindowIndexesA, std::memory_order_release);
    publishedWindowCount.store(0, std::memory_order_release);

    for (int i = 0; i < 3; ++i)
    {
        DATASETTAB& tab = allTabs[i];
        tab.tabID = i;
        tab.tabNo = i;
        gpu.InitD3DPerTab(tab.dx);
        // Optional: Give them names
        // allTabs[i].fileName = L"Untitled-" + std::to_wstring(i);
        // We can set random colors or names here to distinguish them
        // allTabs[i].color = ...
    }

    // Publish tab list
    activeTabIndexesA[0] = 0;
    activeTabIndexesA[1] = 1;
    activeTabIndexesA[2] = 2;

    publishedTabIndexes.store(activeTabIndexesA, std::memory_order_release);
    publishedTabCount.store(3, std::memory_order_release);

    // SETUP WINDOW (The View)
    uint16_t windowSlot = 0;
    SingleUIWindow& mainWindow = allWindows[windowSlot];
    mainWindow.activeTabIndex = 0;
    mainWindow.currentMonitorIndex = primaryMonitorIndex;

    mainWindow.hWnd = CreateWindowExW(
        WS_EX_OVERLAPPEDWINDOW,  //An optional extended window style.
        szWindowClass,           // Window class: The name of the application
        szTitle,       // The text that appears in the title bar
        WS_OVERLAPPEDWINDOW | WS_MINIMIZEBOX | WS_MAXIMIZEBOX,// The type of window to create
        CW_USEDEFAULT, CW_USEDEFAULT, // Size and position. Initial position (x, y)
        gpu.screens[primaryMonitorIndex].WindowWidth / 2,  // Half the work area width
        gpu.screens[primaryMonitorIndex].WindowHeight / 2, // Window size divided by 2 when user press un-maximize button. 
        NULL,      // The parent of this window
        NULL,      // This application does not have a menu bar, we create our own Menu.
        hInstance, // Instance handle, the first parameter from wWinMain
        NULL       // Additional application data, not used in this application
    );

    if (!mainWindow.hWnd)
    {
        MessageBox(NULL,
            _T("Call to CreateWindow failed!"),
            _T("Something bad happened. Failed to create a new Window."),
            NULL);
        return 1;
    }

    // Initialize D3D for this Window. Access via reference from vector to ensure we modify the stored instance
    gpu.InitD3DPerWindow(mainWindow.dx, mainWindow.hWnd, gpu.screens[primaryMonitorIndex].commandQueue.Get());
    // Publish window list// Register window in global list (Used by Render Threads)
    activeWindowIndexesA[0] = windowSlot;
    publishedWindowIndexes.store(activeWindowIndexesA, std::memory_order_release);
    publishedWindowCount.store(1, std::memory_order_release);
    
    std::wcout << "Starting application..." << std::endl;

    // By default we always initialize application in maximized state.
    // Intentionally we don't remember last closed size and slowdown startup time retrieving that value.
    ShowWindow(mainWindow.hWnd, SW_MAXIMIZE); // hWnd: the value returned from CreateWindow
    UpdateWindow(mainWindow.hWnd);

    // Create and launch all threads
    std::vector<std::thread> threads;
    threads.emplace_back(NetworkInputThread);
    threads.emplace_back(FileInputThread);
    threads.emplace_back(GpuCopyThread);

    // LAUNCH 3 ENGINEERING THREADS (One per Tab). Main logic thread. The ringmaster of the application.
	// TODO: 3 Initial threads is during development. Final application will have dynamic thread management.
    for (int i = 0; i < 3; ++i) {
        threads.emplace_back(विश्वकर्मा, (uint64_t)i);// Pass the index 'i' to the thread function
    }

    //threads.emplace_back(GpuRenderThread, 0, 60);  // Monitor 1 at 60Hz
    //threads.emplace_back(GpuRenderThread, 1, 144); // Monitor 2 at 144Hz
    RestartRenderThreads();// Initial Render Thread Launch (Not a monitor topology change, just startup)

    MSG msg = {};// Main message loop:

    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) { //Does not block. Returns immediately.
            //We can not use alternate GetMessage() since that one block waiting for windows.
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        } else {
            //WaitMessage(); // blocks until new Windows message arrives
            // Just sleep briefly to avoid burning CPU if no messages.
            // The Render Threads are responsible for the heartbeat of the app.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    // Cleanup
    std::wcout << "Message Loop exited.\n";
    pauseRenderThreads = true;
    for (auto& t : renderThreads) { if (t.joinable()) t.join(); }
    std::wcout << "All render threads exited.\n";

    // Let's try to gracefully shutdown all the threads we started. Signal all threads to stop
    shutdownSignal = true; // UI Input thread, Network Input Thread & File Handling thread listen to this.
    toCopyThreadCV.notify_all(); // This one is to wake up the sleepy GPU Copy thread to shutdown.

    // Wait for all threads to finish
    std::wcout << "Thread Count: " << threads.size() <<"\n";
    for (auto& t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }
    // Clean up resources before exiting the application.
    // Cleanup Windows
    uint16_t* winList = publishedWindowIndexes.load(std::memory_order_acquire);
    uint16_t winCount = publishedWindowCount.load(std::memory_order_acquire);
    for (uint16_t i = 0; i < winCount; ++i) gpu.CleanupWindowResources(allWindows[winList[i]].dx);
    // Cleanup Tabs (Geometry)
    uint16_t* tabList = publishedTabIndexes.load(std::memory_order_acquire);
    uint16_t tabCount = publishedTabCount.load(std::memory_order_acquire);
    for (uint16_t i = 0; i < tabCount; ++i) gpu.CleanupTabResources(allTabs[tabList[i]].dx);

    gpu.CleanupD3DGlobal();// Global Cleanup
    
    //Cleanup Freetype library.
    //FT_Done_Face(face);
    //FT_Done_FreeType(ft);
    
    //gpu.WaitForPreviousFrame(); // Wait for GPU to finish all commands. No need. All render threads have exited by now.

    std::wcout << "Application finished cleanly." << std::endl;

    return (int)msg.wParam;
}

// PURPOSE:  Processes messages for the main window.
// This is the function which runs whenever something changes from Operating System and we are expected to update ourselves.
// Even the user input such as keyboard presses, mouse clicks, open/close are notified to this function.
// Remember this is not the function which keeps running every frame, that is a different infinite loop in wWinMain function.
// Question: What happens to wWinMain when this function runs? Does that one pause?
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    PAINTSTRUCT ps;
    HDC hdc;
    TCHAR greeting[] = _T("Hello, Vishwakarma!");

    static bool initialized = false;

    static unsigned char* image_data = NULL;
    static int width, height;
    std::wstring exePath = GetExecutablePath();
    int size_needed = WideCharToMultiByte(
        CP_UTF8, 0, &exePath[0], static_cast<int>(exePath.size()), NULL, 0, NULL, NULL);
    std::string str(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &exePath[0], static_cast<int>(exePath.size()), &str[0], size_needed, NULL, NULL);

    std::string fullPath = str + "\\logo.png";

    unsigned char* imgData = nullptr;
    int w, h;

    DATASETTAB* tab = GetActiveTabFromHwnd(hWnd);
    ACTION_DETAILS ad;

    uint16_t* tabList = publishedTabIndexes.load(std::memory_order_acquire);
    uint16_t tabCount = publishedTabCount.load(std::memory_order_acquire);
    uint16_t* windowList = publishedWindowIndexes.load(std::memory_order_acquire);
    uint16_t windowCount = publishedWindowCount.load(std::memory_order_acquire);

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
    
    // ******* LIFECYCLE messages ******
    
    
    case WM_KEYDOWN: 
        if (tab) {
            ad.actionType = ACTION_TYPE::KEYDOWN;
            ad.source = INPUT_SOURCE::KEYBOARD;
            ad.x = static_cast<int>(wParam); //Virtual key code
            ad.y = static_cast<int>(lParam); //Repeat count, scan code, flags
            ad.delta = 0;
            ad.timestamp = GetTickCount64();
            tab->userInputQueue->push(ad); //userInputQueue is threadsafe.
        }
        return 0;
    
    case WM_KEYUP:
        if (tab) {
            ad.actionType = ACTION_TYPE::KEYUP;
            ad.source = INPUT_SOURCE::KEYBOARD;
            ad.x = static_cast<int>(wParam); //Virtual key code
            ad.y = static_cast<int>(lParam); //Repeat count, scan code, flags
            ad.delta = 0;
            ad.timestamp = GetTickCount64();
            tab->userInputQueue->push(ad); //userInputQueue is threadsafe.
        }
        return 0;

    case WM_CHAR: // This is different from WM_KEYDOWN / UP because it accounts for keyboard layout,
    // modifiers, etc. It gives you the actual character that should be input, rather than the physical key.
        if (tab) {
            ad.actionType = ACTION_TYPE::CHAR;
            ad.source = INPUT_SOURCE::KEYBOARD;
            ad.x = static_cast<int>(wParam); //Virtual key code
            ad.y = static_cast<int>(lParam); //Repeat count, scan code, flags
            ad.delta = 0;
            ad.timestamp = GetTickCount64();
            tab->userInputQueue->push(ad); //userInputQueue is threadsafe.
        }
        return 0;

    case WM_SYSKEYDOWN:
        if (tab) {
            ad.actionType = ACTION_TYPE::KEYDOWN;
            ad.source = INPUT_SOURCE::KEYBOARD;
            ad.x = static_cast<int>(wParam); //Virtual key code
            ad.y = static_cast<int>(lParam); //Repeat count, scan code, flags
            ad.delta = 0;
            ad.timestamp = GetTickCount64();
            tab->userInputQueue->push(ad); //userInputQueue is threadsafe.
        }
        return 0;

    case WM_MOUSEMOVE:
        if (tab) {
            ad.actionType = ACTION_TYPE::MOUSEMOVE;
            ad.source = INPUT_SOURCE::MOUSE;
            ad.x = GET_X_LPARAM(lParam); //Client X
            ad.y = GET_Y_LPARAM(lParam); //Client Y
            ad.delta = static_cast<int>(wParam); // Button/modifier flags (MK_LBUTTON, etc. )
            ad.timestamp = GetTickCount64();
            tab->userInputQueue->push(ad); //userInputQueue is threadsafe.
        }
        return 0;

    case WM_LBUTTONDOWN:
        if (tab) {
            ad.actionType = ACTION_TYPE::LBUTTONDOWN;
            ad.source = INPUT_SOURCE::MOUSE;
            ad.x = GET_X_LPARAM(lParam);
            ad.y = GET_Y_LPARAM(lParam);
            ad.delta = static_cast<int>(wParam); // Button/modifier flags (MK_LBUTTON, etc. )
            ad.timestamp = GetTickCount64();
            tab->userInputQueue->push(ad); //userInputQueue is threadsafe.
        }
        return 0;
    
    case WM_LBUTTONUP:
        if (tab) {
            ad.actionType = ACTION_TYPE::LBUTTONUP;
            ad.source = INPUT_SOURCE::MOUSE;
            ad.x = GET_X_LPARAM(lParam);
            ad.y = GET_Y_LPARAM(lParam);
            ad.delta = static_cast<int>(wParam); // Button/modifier flags (MK_LBUTTON, etc. )
            ad.timestamp = GetTickCount64();
            tab->userInputQueue->push(ad); //userInputQueue is threadsafe.
        }
        return 0;
    
    case WM_RBUTTONDOWN:
        if (tab) {
            ad.actionType = ACTION_TYPE::RBUTTONDOWN;
            ad.source = INPUT_SOURCE::MOUSE;
            ad.x = GET_X_LPARAM(lParam);
            ad.y = GET_Y_LPARAM(lParam);
            ad.delta = static_cast<int>(wParam); // Button/modifier flags (MK_LBUTTON, etc. )
            ad.timestamp = GetTickCount64();
            tab->userInputQueue->push(ad); //userInputQueue is threadsafe.
        }
        return 0;
    
    case WM_RBUTTONUP:
        if (tab) {
            ad.actionType = ACTION_TYPE::RBUTTONUP;
            ad.source = INPUT_SOURCE::MOUSE;
            ad.x = GET_X_LPARAM(lParam);
            ad.y = GET_Y_LPARAM(lParam);
            ad.delta = static_cast<int>(wParam); // Button/modifier flags (MK_LBUTTON, etc. )
            ad.timestamp = GetTickCount64();
            tab->userInputQueue->push(ad); //userInputQueue is threadsafe.
        }
        return 0;
    
    case WM_MBUTTONDOWN:
        if (tab) {
            ad.actionType = ACTION_TYPE::MBUTTONDOWN;
            ad.source = INPUT_SOURCE::MOUSE;
            ad.x = GET_X_LPARAM(lParam);
            ad.y = GET_Y_LPARAM(lParam);
            ad.delta = static_cast<int>(wParam); // Button/modifier flags (MK_LBUTTON, etc. )
            ad.timestamp = GetTickCount64();
            tab->userInputQueue->push(ad); //userInputQueue is threadsafe.
        }
        return 0;
    
    case WM_MBUTTONUP:
        if (tab) {
            ad.actionType = ACTION_TYPE::MBUTTONUP;
            ad.source = INPUT_SOURCE::MOUSE;
            ad.x = GET_X_LPARAM(lParam);
            ad.y = GET_Y_LPARAM(lParam);
            ad.delta = static_cast<int>(wParam); // Button/modifier flags (MK_LBUTTON, etc. )
            ad.timestamp = GetTickCount64();
            tab->userInputQueue->push(ad); //userInputQueue is threadsafe.
        }
        return 0;
    
    case WM_MOUSEWHEEL:
        if (tab) {
            ad.actionType = ACTION_TYPE::MOUSEWHEEL;
            ad.source = INPUT_SOURCE::MOUSE;
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hWnd, &pt);
            ad.x = pt.x;
            ad.y = pt.y;
            ad.delta = GET_WHEEL_DELTA_WPARAM(wParam);
            ad.timestamp = GetTickCount64();
            tab->userInputQueue->push(ad); //userInputQueue is threadsafe.
        }
        return 0;

    //case WM_NCLBUTTONDOWN: return 0; // If needed for title bars / boarders.
    //Currently removed because it was causing WM_CLOSE to not fire up even when clicking close button.
    //If we want to support dragging from the title bar, we can re - enable this and add logic to handle it.
    
    case WM_CAPTURECHANGED: // Notify all tabs to release captured mouse states (e.g. , if dragging )
        for (uint16_t ti = 0; ti < tabCount; ++ti) {
            DATASETTAB & tab = allTabs[tabList[ti]];

            ad.actionType = ACTION_TYPE::CAPTURECHANGED;
            ad.source = INPUT_SOURCE::MOUSE;
            ad.x = 0;
            ad.y = 0;
            ad.delta = 0;
            ad.timestamp = GetTickCount64();
            tab.userInputQueue->push(ad); //userInputQueue is threadsafe.

        }
        return 0;
    case WM_INPUT_DEVICE_CHANGE: return 0; //TODO : Copy this case from Grok.

    // ******* LIFECYCLE messages ******
    case WM_CREATE:
        LoadPngFromResource(IDR_LOGO_PNG, &imgData, &w, &h); //imgData is NOT to be freed till application life.
        //TODO: It will be made global variable latter. To be used as placeholder for icons.
        break;

    case WM_PAINT:
    {
        // We're not using GDI for rendering anymore - DirectX12 handles all rendering
        // Just validate the paint message to prevent Windows from continuously sending WM_PAINT.
		// This is a mandatory quirk of the Windows message system.
        hdc = BeginPaint(hWnd, &ps);
        EndPaint(hWnd, &ps);
        break;
    }
    case WM_DISPLAYCHANGE: // Resolution change OR When user adds or disconnects a new monitor.
    case WM_DPICHANGED:    // Scale change, When user manually changes screen resolution through windows settings.
        std::wcout << L"WM_DISPLAYCHANGE / WM_DPICHANGED received. Restarting render threads." << std::endl;
        /* This IS a topology change. Monitor details may have changed. But Geometry belonging to tabs persists!
        Monitor addition / removal is very rare event. 1 event each couple hours average is already conservative.
        Hence we can afford to briefly pause rendering and restart all threads to pick up the new topology.
        Even windows OS flickers when you add/remove monitor, so a brief pause in rendering is not a big deal.*/
		RestartRenderThreads(); // Preserves commandQueues and swapChains to the extent possible.
        break;

    case WM_ENTERSIZEMOVE:
		// TODO: Set a boolean flag isMoving = true. 
        // The actual handling of monitor affinity will be done in WM_EXITSIZEMOVE to avoid excessive handling 
        // during active movement.
		break;

    case WM_MOVE:
		break; // We handle this in WM_EXITSIZEMOVE to avoid excessive handling during active movement.

		
        break;
    case WM_SIZE:
    {   
        //Handle resizing of the window. This can be triggered by user resizing or maximize/unmaximize. 
        // We need to resize the swap chain buffers accordingly.
        // TODO: optimize for minimized state by pausing rendering and skipping buffer presentation.
		// TODO: Setup a timer to resize evend during middle of resizing, 
        // but with a lower frequency to avoid excessive GPU work. This will make resizing smoother.
		if (wParam == SIZE_MINIMIZED) return 0; // For now we just skip resizing logic.
        UINT newWidth = LOWORD(lParam);
        UINT newHeight = HIWORD(lParam);

        uint16_t* windowList = publishedWindowIndexes.load(std::memory_order_acquire);
        uint16_t windowCount = publishedWindowCount.load(std::memory_order_acquire);

        for (uint16_t i = 0; i < windowCount; ++i) {
            SingleUIWindow& window = allWindows[windowList[i]];
            if (window.hWnd == hWnd) {
                window.nextRequestedWidth = newWidth;
                window.nextRequestedHeight = newHeight;
                // gpu.ResizeD3DWindow(window.dx, newWidth, newHeight); Called in WM_EXITSIZEMOVE.
                break;
            }
        }
        return 0;
    }

    case WM_EXITSIZEMOVE:{ // It occurs post - movement, not while the window is actively moving(use WM_MOVING for that).
        std::wcout << L"WM_EXITSIZEMOVE received. Checking Monitor affinity." << std::endl;
        SingleUIWindow* pWin = nullptr; // Get our internal window object
        uint16_t * windowList = publishedWindowIndexes.load(std::memory_order_acquire);
        uint16_t windowCount = publishedWindowCount.load(std::memory_order_acquire);
        for (uint16_t wi = 0; wi < windowCount; ++wi)
        {
            SingleUIWindow& w = allWindows[windowList[wi]];
            if (w.hWnd == hWnd) { pWin = &w; break; }
        }
        if (pWin) {
            // Determine which monitor the window is now on
            HMONITOR hMonitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
            int newMonitorIdx = -1;
            for (int i = 0; i < gpu.currentMonitorCount; i++) {// Find our internal index for this monitor
                if (gpu.screens[i].hMonitor == hMonitor) { newMonitorIdx = i; break; }
            }

            // Trigger Migration if monitor changed
            if (newMonitorIdx != -1 && pWin->currentMonitorIndex != newMonitorIdx) {
                // This function contains the logic: 
                // isMigrating=true -> Flush Old Queue -> Cleanup -> Recreate on New Queue -> isMigrating=false
                UpdateWindowMonitorAffinity(*pWin, newMonitorIdx);
            }
            else std::wcout << L"No change. Currently on " << gpu.screens[pWin->currentMonitorIndex].friendlyName << std::endl;

            if (pWin->nextRequestedWidth != pWin->currentWidth or pWin->nextRequestedHeight != pWin->currentHeight) {
				std::wcout << L"Resizing window buffers to : " << pWin->nextRequestedWidth << L" x " 
                    << pWin->nextRequestedHeight << std::endl;
                pWin->isResizing = true; // Switch OFF rendering.
                gpu.ResizeD3DWindow(pWin->dx, pWin->nextRequestedWidth, pWin->nextRequestedHeight);
                pWin->currentWidth = pWin->nextRequestedWidth;
                pWin->currentHeight = pWin->nextRequestedHeight;
				pWin->isResizing = false; // Switch ON rendering.
            }
            
        }
        break;
        }
    case WM_CLOSE: // This is called BEFORE WM_DESTROY is received. Importantly, once this is over hWnd is destroyed.
        // Initiate shutdown for render threads FIRST
        std::wcout << "WM_CLOSE: Pausing render threads..." << std::endl;
        pauseRenderThreads = true;

        // Join render threads to ensure they stop BEFORE window destruction
        // TODO : Warning : This will terminate ALL render theads. Revisit this code when multi window is implemented.
        for (auto& t : renderThreads) {
            if (t.joinable()) {
                t.join();
            }
        }
        std::wcout << "WM_CLOSE: All render threads joined." << std::endl;

        // Now safe to clean up window-specific DX resources (swap chain, RTVs, etc.)
        // This prevents any lingering GPU work on invalid resources
        for (uint16_t i = 0; i < windowCount; ++i) {
            SingleUIWindow& window = allWindows[windowList[i]];
            if (window.hWnd == hWnd) {  // Target this specific window
                gpu.CleanupWindowResources(window.dx);
                break; // We found it. No need to continue.
            }
        }
        DestroyWindow(hWnd);// Now destroy the window (sends WM_DESTROY)
        return 0;  // Don't call DefWindowProc (prevents default handling)
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
