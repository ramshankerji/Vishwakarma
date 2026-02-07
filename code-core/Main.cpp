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

int g_monitorCount = 0; // Global monitor count
int primaryMonitorIndex = 0;

extern std::vector<DATASETTAB> allTabs; //Defined in विश्वकर्मा.cpp
extern std::vector<SingleUIWindow> allUIWindows; //Defined in विश्वकर्मा.cpp
extern std::random_device rd;

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

// Defined in ImageHandling.cpp
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
    gpu.screens.emplace_back();// Create a new slot in the vector
    // Get a pointer to this new slot (the back of the vector)
    OneMonitorController* currentScreen = &gpu.screens.back();
    // Set defaults immediately in case API calls fail
    currentScreen->hMonitor = hMonitor;// Store monitor handle
    currentScreen->isVirtualMonitor = false;
    currentScreen->dpiX = 96;// Default DPI to 96 to prevent divide-by-zero later if GetDpiForMonitor fails
    currentScreen->dpiY = 96;

    MONITORINFOEXW monitorInfo = {};// Get monitor info
    monitorInfo.cbSize = sizeof(MONITORINFOEXW);

    // CRITICAL CHECK: If this fails, we have a "Ghost Monitor". Remove it and skip.
    if (!GetMonitorInfoW(hMonitor, &monitorInfo)) {
        gpu.screens.pop_back(); // Undo the emplace_back
        return TRUE; // Continue enumeration hoping for valid monitors
    }

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

    // Default initialization if physical calc fails
    if (currentScreen->screenPhysicalWidth == 0) {
        // Fallback: 1 inch = 25.4 mm
        currentScreen->screenPhysicalWidth = static_cast<int>((currentScreen->screenPixelWidth / (float)currentScreen->dpiX) * 25.4f);
        currentScreen->screenPhysicalHeight = static_cast<int>((currentScreen->screenPixelHeight / (float)currentScreen->dpiY) * 25.4f);
    }

    // Calculate drawable area dimensions (work area)
    currentScreen->WindowWidth = currentScreen->workAreaRect.right - currentScreen->workAreaRect.left;
    currentScreen->WindowHeight = currentScreen->workAreaRect.bottom - currentScreen->workAreaRect.top;
    currentScreen->isScreenInitalized = true;// Mark as initialized

    // Optional Logging
    std::wcout << L"Monitor Found: " << currentScreen->deviceName << L" ("
        << currentScreen->screenPixelWidth << L"x" << currentScreen->screenPixelHeight << L")" << std::endl;

    return TRUE; // Continue enumeration
}

void FetchAllMonitorDetails() // Main function to fetch all monitor details
{
    g_monitorCount = 0; // Reset monitor count and clear all screen data
    gpu.screens.clear();// Clear the vector completely.
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

    // HEADLESS CHECK: If the API returned success but found 0 monitors, OR if the API failed...
    if (gpu.screens.empty()) {
        std::wcout << L"Headless mode detected (No monitors found). Creating Virtual Display." << std::endl;

        // Force-add one virtual monitor so the rest of the app doesn't crash
        gpu.screens.emplace_back();
        OneMonitorController* s = &gpu.screens.back();

        s->isScreenInitalized = true;
        s->deviceName = L"HEADLESS_DISPLAY";
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
    }

    // Update the global count to match the actual vector size
    g_monitorCount = static_cast<int>(gpu.screens.size());

    std::wcout << L"Found " << g_monitorCount << L" monitor(s)" << std::endl;
}

// Helper to find a monitor in the OLD list by name
int FindMonitorIndexByName(const std::vector<OneMonitorController>& list, const std::wstring& name) {
    for (int i = 0; i < list.size(); ++i) { if (list[i].deviceName == name) return i; }
    return -1;
}

// Helper to find our internal Window Struct from the OS Window Handle
SingleUIWindow* GetWindowFromHwnd(HWND hWnd) {
    for (auto& window : allUIWindows) { if (window.hWnd == hWnd) { return &window; } }
    return nullptr;
}

DATASETTAB* GetActiveTabFromHwnd(HWND hWnd) {
    SingleUIWindow* window = nullptr;
    for (auto& w : allUIWindows) {
        if (w.hWnd == hWnd) {
            window = &w;
            break;
        }
    }
    if (!window || window->activeTabIndex < 0 || window->activeTabIndex >= window->tabIds.size()) {
        return nullptr;
    }
    uint64_t tabID = window->tabIds[window->activeTabIndex];
    for (auto& t : allTabs) {
        if (t.tabID == tabID) {
            return &t;
        }
    }
    return nullptr;
}

void HandleTopologyChange() {
    std::cout << "Topology Change Detected. Analyzing..." << std::endl;
    std::vector<OneMonitorController> oldScreens = std::move(gpu.screens);// Capture the OLD topology
    FetchAllMonitorDetails(); //Scan for NEW topology (populates gpu.screens)This now fills a fresh gpu.screens vector

    for (auto& newScreen : gpu.screens) { // MERGE: Transfer existing CommandQueues to the new list
        int oldIdx = FindMonitorIndexByName(oldScreens, newScreen.deviceName);

        if (oldIdx != -1) {
            // MATCH FOUND! This monitor existed before.
            // Steal the CommandQueue so we don't have to recreate it (and break Swap Chains).
            newScreen.commandQueue = oldScreens[oldIdx].commandQueue;
            newScreen.hasActiveThread = oldScreens[oldIdx].hasActiveThread;
            // Mark the old one as "handled" so we don't double-close threads later
            oldScreens[oldIdx].hasActiveThread = false;
            std::wcout << L"Monitor persisted: " << newScreen.deviceName << std::endl;
        }
        else {
            // NEW MONITOR! Create a new Command Queue for it.
            D3D12_COMMAND_QUEUE_DESC queueDesc = {};
            queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            gpu.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&newScreen.commandQueue));
            std::wcout << L"New Monitor detected: " << newScreen.deviceName << std::endl;
        }
    }

    // 4. CLEANUP: Stop threads for monitors that are GONE
    // (In your architecture, you might need a way to signal specific threads to stop)
    // For simplicity here, if the thread index doesn't match, we might need to restart threads
    // but we SAVED the queues, so Swap Chains are safe!

    // 5. RE-MAP Windows
    for (auto& window : allUIWindows) {
        HMONITOR hWinMonitor = MonitorFromWindow(window.hWnd, MONITOR_DEFAULTTONEAREST);

        // Find the new index in gpu.screens
        int newMonitorIdx = 0;
        for (int i = 0; i < gpu.screens.size(); ++i) {
            if (gpu.screens[i].hMonitor == hWinMonitor) {
                newMonitorIdx = i;
                break;
            }
        }

        // Did the window move logically?
        if (window.currentMonitorIndex != newMonitorIdx) {
            // If the CommandQueue pointer CHANGED (i.e., it moved to a different physical monitor),
            // we MUST recreate the Swap Chain.
            // If it's the SAME monitor (just index changed), the queue pointer is identical, so no visual glitch!

            ID3D12CommandQueue* newQueue = gpu.screens[newMonitorIdx].commandQueue.Get();
            ID3D12CommandQueue* oldQueue = nullptr; // Retrieve from window.dx if stored, or infer

            // We can check if the underlying device changed
            // Since we don't store the Queue in Window, we just Re-Init to be safe.
            // But this is fast because it's only for the affected window.
            gpu.CleanupWindowResources(window.dx);
            gpu.InitD3DPerWindow(window.dx, window.hWnd, newQueue);

            window.currentMonitorIndex = newMonitorIdx;
        }
    }

    // THREAD MANAGEMENT. Simplest Robust Approach:
    // Since threads are lightweight, just restart the RENDER LOOP threads.
    // Because we kept the CommandQueues alive, this is instant and invisible to the user.

    pauseRenderThreads = true;
    for (auto& t : renderThreads) { if (t.joinable()) t.join(); }
    renderThreads.clear();
    pauseRenderThreads = false;

    for (int i = 0; i < g_monitorCount; i++) {
        renderThreads.emplace_back(GpuRenderThread, i, gpu.screens[i].refreshRate);
    }
}

// Logic: Just update the integer ID. Destroys NOTHING.
// This function is called in 2 cases: User moves window to another monitor, or monitor disconnected/added.
void UpdateWindowsMonitorAffinity() {
    for (auto& window : allUIWindows) {
        int newIndex = 0; // Fallback to primary GetMonitorIndexForWindow(window.hWnd);

        HMONITOR hMonitor = MonitorFromWindow(window.hWnd, MONITOR_DEFAULTTOPRIMARY);
        for (int i = 0; i < g_monitorCount; i++) {
            if (gpu.screens[i].hMonitor == hMonitor) {
                newIndex = i;
            }
        }

        // Optional optimization: Only log if it actually changed
        if (window.currentMonitorIndex != newIndex) {
            std::cout << "Window moved to Monitor " << newIndex << std::endl;
            window.currentMonitorIndex = newIndex;
        }
    }
}

void HandleWindowMove(SingleUIWindow& window, int newMonitorIndex) {

    // SWITCH OFF RENDERING. The threads will now skip this window in their loops.
    window.isMigrating = true;

    // WAIT FOR OLD QUEUE (The only technical constraint)
    // We must ensure the GPU is done with the old SwapChain before destroying it.
    int oldIndex = window.currentMonitorIndex;
    if (oldIndex >= 0) {
        // Using your existing helper from MemoryManagerGPU-DirectX12.h
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

    // RECREATE RESOURCES (The "Surgery"). Now it is safe to destroy the old SwapChain.
    gpu.CleanupWindowResources(window.dx);

    // Update the monitor index so the NEW thread picks it up.
    window.currentMonitorIndex = newMonitorIndex;

    // Create the NEW SwapChain on the NEW Monitor's Queue.
    ID3D12CommandQueue* newQueue = gpu.screens[newMonitorIndex].commandQueue.Get();
    gpu.InitD3DPerWindow(window.dx, window.hWnd, newQueue);
    window.isMigrating = false;// SWITCH ON RENDERING

    std::cout << "Window migrated to Monitor " << newMonitorIndex << std::endl;
}

// The "Safe" Restart Function
void RestartRenderThreads(bool isTopologyChange) {
    pauseRenderThreads = true; // Pause Logic
    for (auto& t : renderThreads) { if (t.joinable()) t.join(); }
    renderThreads.clear();

    if (isTopologyChange) {
        std::cout << "Display Topology Changed. Analyzing..." << std::endl;
        // Capture OLD topology to salvage queues
        std::vector<OneMonitorController> oldScreens = std::move(gpu.screens);
        gpu.screens.clear(); // Clear global list to rebuild
        // Enumerate NEW topology. This populates gpu.screens with FRESH data, no queues yet.
        EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, 0);
        g_monitorCount = static_cast<int>(gpu.screens.size());
        // Handle Headless/Virtual Monitor logic if needed (copy from your FetchAllMonitorDetails)
        if (gpu.screens.empty()) { /* ... Insert headless logic here ... */ }

        // MERGE: Create Queues OR Salvage them from oldScreens
        for (auto& newScreen : gpu.screens) {
            // Try to find this monitor in the old list (Match by Device Name)
            auto it = std::find_if(oldScreens.begin(), oldScreens.end(),
                [&](const OneMonitorController& old) { return old.deviceName == newScreen.deviceName; });

            if (it != oldScreens.end() && it->commandQueue) {
                // FOUND! Reuse the existing Command Queue.
                // This keeps the Swap Chain alive for windows on this monitor.
                newScreen.commandQueue = it->commandQueue;
                // std::wcout << L"Preserved Queue for: " << newScreen.deviceName << std::endl;
            }
            else {
                // NEW MONITOR (or first run). Create a new Queue.
                D3D12_COMMAND_QUEUE_DESC queueDesc = {};
                queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
                gpu.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&newScreen.commandQueue));
                // std::wcout << L"Created New Queue for: " << newScreen.deviceName << std::endl;
            }
        }

        for (auto& window : allUIWindows) {// RE-MAP WINDOWS
            // Ask Windows: "Where is this window physically right now?"
            HMONITOR hWinMonitor = MonitorFromWindow(window.hWnd, MONITOR_DEFAULTTONEAREST);
            // Find the index in our NEW gpu.screens list
            int newMonitorIdx = 0;
            for (int i = 0; i < g_monitorCount; ++i) {
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
                std::cout << "Migrating Window to Monitor " << newMonitorIdx << std::endl;
                gpu.CleanupWindowResources(window.dx);// Cleanup OLD resources (Swap Chain, RTVs)

                // Initialize NEW resources with the NEW Queue
                gpu.InitD3DPerWindow(window.dx, window.hWnd, newQueue);
                window.currentMonitorIndex = newMonitorIdx;
            }
        }
    }
    else {
        // Not a topology change (Just a restart). Just update affinities without touching D3D resources.
        UpdateWindowsMonitorAffinity();
    }

	pauseRenderThreads = false; //When this is true, render threads terminate their loops.

    // Spawn Threads (One per Monitor)
    for (int i = 0; i < g_monitorCount; i++) {
        int refreshRate = gpu.screens[i].refreshRate;
        renderThreads.emplace_back(GpuRenderThread, i, refreshRate);
        std::cout << "Spawned Render Thread for Monitor " << i << " (" << refreshRate << "Hz)" << std::endl;
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
    std::cout.clear();
    std::wcerr.clear();
    std::cerr.clear();
    std::wcin.clear();
    std::cin.clear();
    SetConsoleTitleA("Vishwakarma Debug Console");
}

static const wchar_t szWindowClass[] = L"विश्वकर्मा"; // The main window class name.
static const wchar_t szTitle[] = L"Vishwakarma 0 :-) "; // The string that appears in the application's title bar.

HINSTANCE hInst;// Stored instance handle for use in Win32 API calls such as FindResource

// Forward declarations of functions included in this code module:
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

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
    for (int i = 0; i < g_monitorCount; i++) {
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
    allTabs.resize(3);
    // Configure the tabs
    for (int i = 0; i < 3; ++i) {
        allTabs[i].tabID = i; // Assign ID
        allTabs[i].tabNo = i; // Memory Group
        gpu.InitD3DPerTab(allTabs[i].dx);// Initialize the Geometry Buffers for this tab!
        // Optional: Give them names
        // allTabs[i].fileName = L"Untitled-" + std::to_wstring(i);
        // We can set random colors or names here to distinguish them
        // allTabs[i].color = ...
    }

    // SETUP WINDOW (The View)
    SingleUIWindow mainWindow;
    mainWindow.tabIds = { 0, 1, 2 };// Assign all 3 tabs to this window
    mainWindow.activeTabIndex = 0; // Tab 0 is visible
    mainWindow.currentMonitorIndex = primaryMonitorIndex;

    mainWindow.hWnd = CreateWindowExW(
        WS_EX_OVERLAPPEDWINDOW,  //An optional extended window style.
        szWindowClass,           // Window class: The name of the application
        szTitle,       // The text that appears in the title bar
        // The type of window to create
        WS_OVERLAPPEDWINDOW | WS_MINIMIZEBOX | WS_MAXIMIZEBOX,
        // Size and position
        CW_USEDEFAULT, CW_USEDEFAULT, // Initial position (x, y)
        gpu.screens[primaryMonitorIndex].WindowWidth / 2,  // Half the work area width
        gpu.screens[primaryMonitorIndex].WindowHeight / 2, // Window size divided by 2 when user press un-maximize button. 
        NULL,      // The parent of this window
        NULL,      // This application does not have a menu bar, we create our own Menu.
        hInstance, // Instance handle, the first parameter from WinMain
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

    allUIWindows.push_back(mainWindow);// Register window in global list (Used by Render Threads)

    // Initialize D3D for this Window. Access via reference from vector to ensure we modify the stored instance
    gpu.InitD3DPerWindow(allUIWindows[0].dx, allUIWindows[0].hWnd, gpu.screens[0].commandQueue.Get());
    std::cout << "Starting application..." << std::endl;

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
    RestartRenderThreads(false);// Initial Render Thread Launch (Not a monitor topology change, just startup)

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
            UpdateWindowsMonitorAffinity();// Check for window movement (Cheap, Non-Destructive)

            //WaitMessage(); // blocks until new Windows message arrives
            // Just sleep briefly to avoid burning CPU if no messages.
            // The Render Threads are responsible for the heartbeat of the app.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    // Cleanup
    std::cout << "Message Loop exited.\n";
    pauseRenderThreads = true;
    for (auto& t : renderThreads) { if (t.joinable()) t.join(); }
    std::cout << "All render threads exited.\n";

    // Let's try to gracefully shutdown all the threads we started. Signal all threads to stop
    shutdownSignal = true; // UI Input thread, Network Input Thread & File Handling thread listen to this.
    toCopyThreadCV.notify_all(); // This one is to wake up the sleepy GPU Copy thread to shutdown.

    // Wait for all threads to finish
    std::cout << "Thread Count: " << threads.size() <<"\n";
    for (auto& t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }
    // Clean up resources before exiting the application.
    for (auto& window : allUIWindows) { gpu.CleanupWindowResources(window.dx); } // Cleanup Windows
    for (auto& tab : allTabs) { gpu.CleanupTabResources(tab.dx); } // Cleanup Tabs (Geometry)
    gpu.CleanupD3DGlobal();// Global Cleanup
    
    //Cleanup Freetype library.
    //FT_Done_Face(face);
    //FT_Done_FreeType(ft);
    
    //gpu.WaitForPreviousFrame(); // Wait for GPU to finish all commands. No need. All render threads have exited by now.

    std::cout << "Application finished cleanly." << std::endl;

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

    unsigned char* imgData = nullptr;
    int w, h;

    DATASETTAB* tab = GetActiveTabFromHwnd(hWnd);
    ACTION_DETAILS ad;

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
            ad.actionType = ACTION_TYPE::KEYDOWN;
            ad.source = INPUT_SOURCE::KEYBOARD;
            ad.x = static_cast<int>(wParam); //Virtual key code
            ad.y = static_cast<int>(lParam); //Repeat count, scan code, flags
            ad.delta = 0;
            ad.timestamp = GetTickCount64();
            tab->userInputQueue->push(ad); //userInputQueue is threadsafe.
        }
        return 0;

    case WM_CHAR: // What is this? How is this different from WM_KEYDOWN / UP?
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
            ad.x = GET_X_LPARAM(lParam);
            ad.y = GET_Y_LPARAM(lParam);
            ad.delta = static_cast<int>(wParam); // Button/modifier flags (MK_LBUTTON, etc. )
            ad.timestamp = GetTickCount64();
            tab->userInputQueue->push(ad); //userInputQueue is threadsafe.
        }
        return 0;

    case WM_LBUTTONDOWN:
        if (tab) {
            ad.actionType = ACTION_TYPE::LBUTTONDOWN;
            ad.source = INPUT_SOURCE::MOUSE;
            ad.x = LOWORD(wParam); //Client X
            ad.y = HIWORD(lParam); //Client Y
            ad.delta = static_cast<int>(wParam); // Button/modifier flags (MK_LBUTTON, etc. )
            ad.timestamp = GetTickCount64();
            tab->userInputQueue->push(ad); //userInputQueue is threadsafe.
        }
        return 0;
    
    case WM_LBUTTONUP:
        if (tab) {
            ad.actionType = ACTION_TYPE::LBUTTONUP;
            ad.source = INPUT_SOURCE::MOUSE;
            ad.x = LOWORD(wParam); //Client X
            ad.y = HIWORD(lParam); //Client Y
            ad.delta = static_cast<int>(wParam); // Button/modifier flags (MK_LBUTTON, etc. )
            ad.timestamp = GetTickCount64();
            tab->userInputQueue->push(ad); //userInputQueue is threadsafe.
        }
        return 0;
    
    case WM_RBUTTONDOWN:
        if (tab) {
            ad.actionType = ACTION_TYPE::RBUTTONDOWN;
            ad.source = INPUT_SOURCE::MOUSE;
            ad.x = LOWORD(wParam); //Client X
            ad.y = HIWORD(lParam); //Client Y
            ad.delta = static_cast<int>(wParam); // Button/modifier flags (MK_LBUTTON, etc. )
            ad.timestamp = GetTickCount64();
            tab->userInputQueue->push(ad); //userInputQueue is threadsafe.
        }
        return 0;
    
    case WM_RBUTTONUP:
        if (tab) {
            ad.actionType = ACTION_TYPE::RBUTTONUP;
            ad.source = INPUT_SOURCE::MOUSE;
            ad.x = LOWORD(wParam); //Client X
            ad.y = HIWORD(lParam); //Client Y
            ad.delta = static_cast<int>(wParam); // Button/modifier flags (MK_LBUTTON, etc. )
            ad.timestamp = GetTickCount64();
            tab->userInputQueue->push(ad); //userInputQueue is threadsafe.
        }
        return 0;
    
    case WM_MBUTTONDOWN:
        if (tab) {
            ad.actionType = ACTION_TYPE::MBUTTONDOWN;
            ad.source = INPUT_SOURCE::MOUSE;
            ad.x = LOWORD(wParam); //Client X
            ad.y = HIWORD(lParam); //Client Y
            ad.delta = static_cast<int>(wParam); // Button/modifier flags (MK_LBUTTON, etc. )
            ad.timestamp = GetTickCount64();
            tab->userInputQueue->push(ad); //userInputQueue is threadsafe.
        }
        return 0;
    
    case WM_MBUTTONUP:
        if (tab) {
            ad.actionType = ACTION_TYPE::MBUTTONUP;
            ad.source = INPUT_SOURCE::MOUSE;
            ad.x = LOWORD(wParam); //Client X
            ad.y = HIWORD(lParam); //Client Y
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
    
    case WM_CAPTURECHANGED: // Notify all tabs to release captured mouse states (e.g. , if draggin )
        for (auto& tab : allTabs) {
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
        //std::cout << "Full path to logo.png: " << fullPath << std::endl;//No longer loading from disc.
        //LoadPngImage(fullPath.c_str(), &image_data, &width, &height); 
        LoadPngFromResource(IDR_LOGO_PNG, &imgData, &w, &h);
        break;

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
    case WM_DISPLAYCHANGE: // Resolution change OR When user adds or disconnects a new monitor.
    case WM_DPICHANGED:    // Scale change, When user manually changes screen resolution through windows settings.
        std::wcout << L"Display configuration changed. Reinitializing..." << std::endl;
        // This IS a topology change. We need to reset swap chains. But Geometry belonging to tabs persists!
		RestartRenderThreads(true); //True = Topology Change
        break;

    case WM_MOVE: {
        std::wcout << L"WM_MOVE received. Reinitializing..." << std::endl;
        SingleUIWindow* pWin = GetWindowFromHwnd(hWnd);// Get our internal window object
        if (pWin) {
            // Determine which monitor the window is now on
            HMONITOR hMonitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
            // Find our internal index for this monitor
            int newMonitorIdx = -1;
            for (int i = 0; i < gpu.screens.size(); i++) {
                if (gpu.screens[i].hMonitor == hMonitor) {
                    newMonitorIdx = i;
                    break;
                }
            }

            // Trigger Migration if monitor changed
            if (newMonitorIdx != -1 && pWin->currentMonitorIndex != newMonitorIdx) {
                // This function contains the logic: 
                // isMigrating=true -> Flush Old Queue -> Cleanup -> Recreate on New Queue -> isMigrating=false
                HandleWindowMove(*pWin, newMonitorIdx);
            }
            else {
                // Optional: If same monitor, just update position rects if you track them
                // UpdateRects(*pWin); 
            }
        }
        break;
        }
    case WM_CLOSE: // This is called BEFORE WM_DESTROY is received. Importantly, once this is over hWnd is destroyed.
        // Initiate shutdown for render threads FIRST
        std::cout << "WM_CLOSE: Pausing render threads..." << std::endl;
        pauseRenderThreads = true;

        // Join render threads to ensure they stop BEFORE window destruction
        // TODO : Warning : This will terminate ALL render theads. Revisit this code when multi window is implemented.
        for (auto& t : renderThreads) {
            if (t.joinable()) {
                t.join();
            }
        }
        std::cout << "WM_CLOSE: All render threads joined." << std::endl;

        // Now safe to clean up window-specific DX resources (swap chain, RTVs, etc.)
        // This prevents any lingering GPU work on invalid resources
        for (auto& window : allUIWindows) {
            if (window.hWnd == hWnd) {  // Target this specific window
                gpu.CleanupWindowResources(window.dx);
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
