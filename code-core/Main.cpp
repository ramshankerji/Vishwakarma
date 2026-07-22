// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.

// compile with: /D_UNICODE /DUNICODE /DWIN32 /D_WINDOWS /c

#include "preCompiledHeadersWindows.h"

// Standard Library we depend upon.
#include <stdlib.h>
#include <string.h>
#include <tchar.h>
#include <chrono>
#include <cmath>
#include <iomanip>  // for std::setprecision
#include <iostream>
#include <thread>
#include <random>
#include <filesystem>
#include <png.h>
#include <shared_mutex>
#include "resource.h"
#include <shellscalingapi.h> // For PROCESS_PER_MONITOR_DPI_AWARE.
#include <commdlg.h>

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
#include "RenderCompositor-DirectX12.h"
#include "MemoryManagerGPU-DirectX12.h"
#include "UserInterface-DirectX12.h"
#include "DataTreeView.h"
#include "DataStorage.h"

#include "UserInputProcessing.h"
#include "Input_UI_Network_File.h"
#include "SoftwareUpdate.h"
#include "PrinterController.h"
#include "ExtensionCommunications.h"
#include "AccountManager.h"
#include "ImprovementData.h"
#include "ApplicationTab.h"

#include <windows.h>
#include <windowsx.h> // For some macros like GET_X_LPARAM, GET_Y_LPARAM etc.

#pragma comment(lib, "Comdlg32.lib")

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

extern void PrintHResult(int);

// Forward declarations of thread functions
void NetworkInputThread();
void FileInputThread();
void विश्वकर्मा(uint64_t); //Main Logic Thread. The ringmaster ! :-)
void GpuCopyThread();
void GpuRenderThread(int monitorId, int refreshRate);
void AddEngineeringThread(uint64_t tabID, std::thread&& t);
void JoinReleasedEngineeringThreads();
void JoinAllEngineeringThreads();

void PushSystemTodoToTab(DATASETTAB* tab, ACTION_TYPE actionType, int x = 0,
    int y = 0, int delta = 0, uint64_t objectId = 0, uint64_t auxValue = 0) {
    if (!tab || !tab->todoCPUQueue) return;
    // The Application Tab has no engineering thread, so its queues are never drained: pushing
    // here would grow memory without bound (tabs.md Decision 3). Single choke point for all
    // ribbon / system todos, including the ones raised from the compositor.
    if (ApplicationTab::IsApplicationTab(tab->tabID)) return;

    ACTION_DETAILS request{};
    request.actionType = actionType;
    request.source = INPUT_SOURCE::SYSTEM;
    request.x = x;
    request.y = y;
    request.delta = delta;
    request.objectId = objectId;
    request.auxValue = auxValue;
    request.timestamp = GetTickCount64();
    tab->todoCPUQueue->push(request);
}

namespace {
constexpr uint32_t ACTION_ENGINEERING_CLOSE = 0xE0000001u;
constexpr uint32_t ACTION_ENGINEERING_CREATE = 0xE0000002u;
uint16_t nextTabSlot = 2; // Slot 0 is the Application Tab, slot 1 the initial engineering tab.
bool closeQueuedTabs[MV_MAX_TABS] = {};

void PublishTabList(uint16_t* nextList, uint16_t nextCount) {
    publishedTabIndexes.store(nextList, std::memory_order_release);
    publishedTabCount.store(nextCount, std::memory_order_release);
}

HWND FirstActiveWindowHandle() {
    uint16_t* windowList = publishedWindowIndexes.load(std::memory_order_acquire);
    uint16_t windowCount = publishedWindowCount.load(std::memory_order_acquire);
    if (windowCount == 0) return nullptr;
    return allWindows[windowList[0]].hWnd;
}

DATASETTAB* GetActiveTabForUIAction() {
    uint16_t* tabList = publishedTabIndexes.load(std::memory_order_acquire);
    uint16_t tabCount = publishedTabCount.load(std::memory_order_acquire);

    // Prefer the tab of the window the user last clicked in (set by RenderUIOverlay), so ribbon
    // commands issued from an extracted tab window act on that window's tab.
    const int32_t sourceTab = g_uiActionSourceTabIndex.load(std::memory_order_acquire);
    if (sourceTab >= 0 && sourceTab < MV_MAX_TABS) {
        // Engineering commands raised while the Application Tab is in front target nothing; the
        // callers' existing null checks turn them into no-ops (tabs.md Guard inventory).
        if (ApplicationTab::IsApplicationTab(static_cast<uint64_t>(sourceTab))) return nullptr;
        for (uint16_t i = 0; i < tabCount; ++i) {
            if (tabList[i] == sourceTab) return &allTabs[sourceTab];
        }
    }

    uint16_t* windowList = publishedWindowIndexes.load(std::memory_order_acquire);
    uint16_t windowCount = publishedWindowCount.load(std::memory_order_acquire);
    for (uint16_t i = 0; i < windowCount; ++i) {
        int tabID = allWindows[windowList[i]].activeTabIndex;
        if (tabID < 0 || tabID >= MV_MAX_TABS) continue;
        if (ApplicationTab::IsApplicationTab(static_cast<uint64_t>(tabID))) continue;
        return &allTabs[tabID];
    }

    for (uint16_t i = 0; i < tabCount; ++i) {
        if (!ApplicationTab::IsApplicationTab(tabList[i])) return &allTabs[tabList[i]];
    }
    return nullptr;
}

std::wstring FileNameFromPath(const std::wstring& path) {
    try {
        std::wstring fileName = std::filesystem::path(path).filename().wstring();
        return fileName.empty() ? L"Untitled.yyy" : fileName;
    } catch (...) {
        return L"Untitled.yyy";
    }
}

std::wstring ShowSaveYyyDialog(const DATASETTAB& tab) {
    wchar_t fileName[MAX_PATH] = {};
    std::wstring initialName = tab.storageFilePath.empty()
        ? (tab.fileName.empty() ? L"Untitled.yyy" : tab.fileName)
        : tab.storageFilePath;

    if (std::filesystem::path(initialName).extension().empty()) {
        initialName += L".yyy";
    }
    wcsncpy_s(fileName, MAX_PATH, initialName.c_str(), _TRUNCATE);

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = FirstActiveWindowHandle();
    ofn.lpstrFilter = L"Vishwakarma files (*.yyy)\0*.yyy\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"yyy";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!GetSaveFileNameW(&ofn)) return {};
    return fileName;
}

std::wstring ShowOpenYyyDialog() {
    wchar_t fileName[MAX_PATH] = {};

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = FirstActiveWindowHandle();
    ofn.lpstrFilter = L"Vishwakarma files (*.yyy)\0*.yyy\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"yyy";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!GetOpenFileNameW(&ofn)) return {};
    return fileName;
}

DATASETTAB* CreateEngineeringTab(const std::wstring& displayName = L"",
    const std::wstring& storageFilePath = L"", bool autoGenerateRandomGeometry = true,
    uint16_t hostWindowSlot = 0) {
    uint16_t tabCount = publishedTabCount.load(std::memory_order_acquire);
    if (tabCount >= MV_MAX_TABS || nextTabSlot >= MV_MAX_TABS) return nullptr;
    if (hostWindowSlot >= MV_MAX_WINDOWS || !allWindows[hostWindowSlot].hWnd ||
        allWindows[hostWindowSlot].windowKind != WINDOW_KIND_TABHOST) {
        hostWindowSlot = 0; // Fall back to the main window when the requested host is gone.
    }

    uint16_t* currentList = publishedTabIndexes.load(std::memory_order_acquire);
    uint16_t* nextList = (currentList == activeTabIndexesA) ? activeTabIndexesB : activeTabIndexesA;
    for (uint16_t i = 0; i < tabCount; ++i) nextList[i] = currentList[i];

    uint16_t tabID = nextTabSlot++;
    DATASETTAB& tab = allTabs[tabID];
    tab.tabID = tabID;
    tab.tabNo = tabID;
    tab.fileName = displayName.empty() ? L"Untitled " + std::to_wstring(tabID + 1) : displayName;
    tab.storageFilePath = storageFilePath;
    tab.mode = storageFilePath.empty() ? 0 : 1;
    tab.autoGenerateRandomGeometry = autoGenerateRandomGeometry;
    tab.allIDsInThisTab.clear();
    tab.dataTreeView.isVisible.store(true, std::memory_order_release);
    tab.dataTreeView.everythingExpanded.store(true, std::memory_order_release);
    DataTreeView::ResetScroll(tab.dataTreeView);
    if (!tab.storageObjectsMutex) tab.storageObjectsMutex = std::make_unique<std::mutex>();
    {
        std::lock_guard<std::mutex> lock(*tab.storageObjectsMutex);
        tab.storageLogicalObjects.clear();
        tab.storageObjects3D.clear();
        tab.expandedDataTreeNodeIds.clear();
        CloseAllInternalSubTabsLocked(tab);
        tab.defaultScene3DMemoryId = 0;
        tab.activeScene3DMemoryId = 0;
    }
    tab.closeRequested.store(false, std::memory_order_release);
    tab.engineeringReleased.store(false, std::memory_order_release);
    closeQueuedTabs[tabID] = false;
    gpu.InitD3DPerTab(tab.dx);
    if (tab.cad2d) InitCad2DTabResources(*tab.cad2d);

    nextList[tabCount] = tabID;
    tab.hostWindowSlot.store(static_cast<int16_t>(hostWindowSlot), std::memory_order_release);
    PublishTabList(nextList, tabCount + 1);

    allWindows[hostWindowSlot].activeTabIndex = tabID; // Only the hosting window switches to it.

    std::thread t(विश्वकर्मा, (uint64_t)tabID);
    AddEngineeringThread(tabID, std::move(t));
    return &tab;
}

void RequestCloseEngineeringTab(uint16_t tabID) {
    uint16_t tabCount = publishedTabCount.load(std::memory_order_acquire);
    // The Application Tab is never closable, and it is always in the published list hosted by
    // window 0 - so closing the LAST engineering tab is legal now: the replacement logic below
    // lands the main window on tab 0, Chrome-style (tabs.md Decision 8).
    if (ApplicationTab::IsApplicationTab(tabID)) return;
    if (tabID >= MV_MAX_TABS) return;
    if (closeQueuedTabs[tabID]) return;

    uint16_t* currentList = publishedTabIndexes.load(std::memory_order_acquire);
    uint16_t* nextList = (currentList == activeTabIndexesA) ? activeTabIndexesB : activeTabIndexesA;
    uint16_t nextCount = 0;
    int closedPosition = -1;

    for (uint16_t i = 0; i < tabCount; ++i) {
        if (currentList[i] == tabID) {
            closedPosition = i;
            continue;
        }
        nextList[nextCount++] = currentList[i];
    }
    if (closedPosition < 0 || nextCount == tabCount) return;
    closeQueuedTabs[tabID] = true;

    uint16_t replacementTab = nextList[(std::min)(static_cast<uint16_t>(closedPosition), static_cast<uint16_t>(nextCount - 1))];

    // Prefer a replacement hosted by the same window as the closing tab, so that window's band
    // does not jump to a tab it doesn't even show.
    const int16_t hostSlot = allTabs[tabID].hostWindowSlot.load(std::memory_order_acquire);
    int sameWindowReplacement = -1;
    for (uint16_t i = 0; i < nextCount; ++i) {
        if (allTabs[nextList[i]].hostWindowSlot.load(std::memory_order_acquire) == hostSlot) {
            sameWindowReplacement = nextList[i];
            break;
        }
    }

    uint16_t* windowList = publishedWindowIndexes.load(std::memory_order_acquire);
    uint16_t windowCount = publishedWindowCount.load(std::memory_order_acquire);
    for (uint16_t i = 0; i < windowCount; ++i) {
        SingleUIWindow& window = allWindows[windowList[i]];
        if (window.activeTabIndex == tabID) {
            window.activeTabIndex = sameWindowReplacement >= 0 ? sameWindowReplacement : replacementTab;
        }
    }

    PublishTabList(nextList, nextCount);

    if (sameWindowReplacement < 0) {
        if (hostSlot > 0 && hostSlot < MV_MAX_WINDOWS) {
            // The closing tab was the only one in a secondary tab window: close that window too.
            CloseSecondaryWindow(static_cast<uint16_t>(hostSlot));
        } else {
            // Main window ran out of hosted tabs: pull the replacement tab back into it.
            allTabs[replacementTab].hostWindowSlot.store(0, std::memory_order_release);
            allWindows[0].activeTabIndex = replacementTab;
        }
    }

    ACTION_DETAILS closeRequest{};
    closeRequest.actionType = ACTION_TYPE::CLOSE_TAB;
    closeRequest.source = INPUT_SOURCE::SYSTEM;
    closeRequest.x = tabID;
    closeRequest.timestamp = GetTickCount64();
    allTabs[tabID].todoCPUQueue->push(closeRequest);
}

void CleanupReleasedTabs() {
    JoinReleasedEngineeringThreads();

    for (uint16_t tabID = 0; tabID < nextTabSlot; ++tabID) {
        DATASETTAB& tab = allTabs[tabID];
        if (tab.closeRequested.load(std::memory_order_acquire) &&
            tab.engineeringReleased.load(std::memory_order_acquire)) {
            // GPU teardown is handed to the copy thread (fence-gated): it owns tab.dx matrix
            // writes, the RCU geometry pages and the cad2d records, and one of its iterations
            // may still be walking the pre-close published tab list. Releasing here would race.
            tab.gpuReleaseState.store(1, std::memory_order_release);
            gPendingTabGpuReleases.fetch_add(1, std::memory_order_release);
            toCopyThreadCV.notify_one();
            tab.closeRequested.store(false, std::memory_order_release);
            tab.engineeringReleased.store(false, std::memory_order_release);
            closeQueuedTabs[tabID] = false;
        }
    }
}

void SaveActiveTabToStorage() {
    DATASETTAB* tab = GetActiveTabForUIAction();
    if (!tab) return;

    std::wstring path = tab->storageFilePath;
    if (path.empty()) path = ShowSaveYyyDialog(*tab);
    if (path.empty()) return;

    std::string error;
    if (!DataStorage::Instance().SaveTabToYyy(*tab, path, &error)) {
        MessageBoxA(FirstActiveWindowHandle(), error.c_str(), "Save failed", MB_OK | MB_ICONERROR);
        return;
    }

    tab->storageFilePath = path;
    tab->fileName = FileNameFromPath(path);
    tab->mode = 1;
}

void OpenStorageFileInNewTab() {
    std::wstring path = ShowOpenYyyDialog();
    if (path.empty()) return;

    DATASETTAB* tab = CreateEngineeringTab(FileNameFromPath(path), path, false);
    if (!tab) return;

    std::string error;
    if (!DataStorage::Instance().LoadYyyIntoTab(*tab, path, &error)) {
        MessageBoxA(FirstActiveWindowHandle(), error.c_str(), "Open failed", MB_OK | MB_ICONERROR);
    }
}

void ProcessPendingUIActions() {
    std::vector<UIActionEntry> actions;
    {
        std::lock_guard<std::mutex> lk(g_actionQueueMutex);
        while (!g_actionQueue.empty()) {
            actions.push_back(g_actionQueue.front());
            g_actionQueue.pop_front();
        }
    }

    for (const UIActionEntry& action : actions) {
        if (action.id == ACTION_ENGINEERING_CREATE) {
            // p1 = window slot whose '+' button was clicked; the new tab is hosted there.
            CreateEngineeringTab(L"", L"", true,
                action.p1 < MV_MAX_WINDOWS ? static_cast<uint16_t>(action.p1) : 0);
        } else if (action.id == ACTION_ENGINEERING_CLOSE) {
            RequestCloseEngineeringTab(static_cast<uint16_t>(action.p1));
        } else if (action.id == DataTreeView::kToggleEverythingUIAction) {
            if (action.p1 < MV_MAX_TABS) {
                PushSystemTodoToTab(&allTabs[static_cast<uint16_t>(action.p1)], ACTION_TYPE::DATA_TREE_TOGGLE_EVERYTHING);
            }
        } else if (action.id == DataTreeView::kToggleNodeUIAction) {
            if (action.p1 < MV_MAX_TABS) {
                PushSystemTodoToTab(&allTabs[static_cast<uint16_t>(action.p1)],
                    ACTION_TYPE::DATA_TREE_TOGGLE_NODE, 0, 0, 0, action.p2);
            }
        } else if (action.id == DataTreeView::kSetActiveBranchUIAction) {
            if (action.p1 < MV_MAX_TABS) {
                PushSystemTodoToTab(&allTabs[static_cast<uint16_t>(action.p1)],
                    ACTION_TYPE::DATA_TREE_SET_ACTIVE_BRANCH, 0, 0, 0, action.p2);
            }
        } else if (action.id == InternalSubTabs::kOpenUIAction) {
            if (action.p1 < MV_MAX_TABS) {
                PushSystemTodoToTab(&allTabs[static_cast<uint16_t>(action.p1)],
                    ACTION_TYPE::OPEN_INTERNAL_SUB_TAB, 0, 0, 0, action.p2);
            }
        } else if (action.id == InternalSubTabs::kActivateUIAction) {
            if (ApplicationTab::IsApplicationTab(action.p1)) {
                // No engineering thread to service this: activate here (tabs.md Decision 9).
                ApplicationTab::ActivateApplicationTabView(action.p2);
            } else if (action.p1 < MV_MAX_TABS) {
                DATASETTAB& actionTab = allTabs[static_cast<uint16_t>(action.p1)];
                const int subTabSlot = FindPublishedSubTabSlot(actionTab, action.p2);
                const int16_t viewWindowSlot = subTabSlot >= 0
                    ? actionTab.subTabHostWindowSlots[subTabSlot].load(std::memory_order_acquire)
                    : static_cast<int16_t>(-1);
                if (subTabSlot >= 0 && viewWindowSlot >= 0) {
                    // An extracted sub-tab is not shown inline: activating its band button
                    // focuses the dedicated window instead (applies to all container types).
                    if (allWindows[viewWindowSlot].hWnd) SetForegroundWindow(allWindows[viewWindowSlot].hWnd);
                } else {
                    PushSystemTodoToTab(&actionTab, ACTION_TYPE::ACTIVATE_INTERNAL_SUB_TAB, 0, 0, 0, action.p2);
                }
            }
        } else if (action.id == InternalSubTabs::kCloseUIAction) {
            if (action.p1 < MV_MAX_TABS) {
                PushSystemTodoToTab(&allTabs[static_cast<uint16_t>(action.p1)],
                    ACTION_TYPE::CLOSE_INTERNAL_SUB_TAB, 0, 0, 0, action.p2);
            }
        } else if (action.id == InternalSubTabs::kExtractUIAction) {
            if (action.p1 < MV_MAX_TABS) {
                ExtractViewToNewWindow(static_cast<uint16_t>(action.p1), action.p2);
            }
        } else if (action.id == kExtractTabUIAction) {
            if (action.p1 < MV_MAX_TABS) {
                ExtractTabToNewWindow(static_cast<uint16_t>(action.p1));
            }
        } else if (action.id == kCloseViewWindowUIAction) {
            if (action.p1 < MV_MAX_TABS && action.p2 < MV_MAX_SUBTABS) {
                CloseViewWindowFor(static_cast<uint16_t>(action.p1), static_cast<uint16_t>(action.p2));
            }
        } else if (action.id == static_cast<uint32_t>(Commands::PROJECT_SAVE)) {
            SaveActiveTabToStorage();
        } else if (action.id == static_cast<uint32_t>(Commands::PROJECT_OPEN)) {
            OpenStorageFileInNewTab();
        } else if (action.id == static_cast<uint32_t>(Commands::PROJECT_PRINT)) {
            PrintActiveTab();
        } else if (action.id == static_cast<uint32_t>(Commands::PROJECT_CLOSE)) {
            // Same path as the tab band's 'x': a no-op while only one tab is left.
            if (DATASETTAB* tab = GetActiveTabForUIAction()) {
                RequestCloseEngineeringTab(static_cast<uint16_t>(tab->tabNo));
            }
        } else if (action.id == static_cast<uint32_t>(Commands::FOLDER_VISIBILITY)) {
            PushSystemTodoToTab(GetActiveTabForUIAction(), ACTION_TYPE::DATA_TREE_TOGGLE_VISIBILITY);
        } else if (action.id == static_cast<uint32_t>(Commands::CREATE_FOLDER)) {
            PushSystemTodoToTab(GetActiveTabForUIAction(), ACTION_TYPE::CREATE_LOGICAL_OBJECT,
                static_cast<int>(VishwakarmaStorage::ToNumber(VishwakarmaStorage::ObjectType::Folder)));
        } else if (action.id == static_cast<uint32_t>(Commands::CREATE_PAGE2D)) {
            PushSystemTodoToTab(GetActiveTabForUIAction(), ACTION_TYPE::CREATE_LOGICAL_OBJECT,
                static_cast<int>(VishwakarmaStorage::ToNumber(VishwakarmaStorage::ObjectType::Page2D)));
        } else if (action.id == static_cast<uint32_t>(Commands::CREATE_SCENE3D)) {
            PushSystemTodoToTab(GetActiveTabForUIAction(), ACTION_TYPE::CREATE_LOGICAL_OBJECT,
                static_cast<int>(VishwakarmaStorage::ToNumber(VishwakarmaStorage::ObjectType::Scene3D)));
        } else if (action.id == static_cast<uint32_t>(Commands::CREATE_CUBOID)) {
            PushSystemTodoToTab(GetActiveTabForUIAction(), ACTION_TYPE::BEGIN_PRIMITIVE_CREATION3D,
                static_cast<int>(VishwakarmaStorage::ToNumber(VishwakarmaStorage::ObjectType::Cuboid)));
        } else if (action.id == static_cast<uint32_t>(Commands::CREATE_CYLINDER)) {
            PushSystemTodoToTab(GetActiveTabForUIAction(), ACTION_TYPE::BEGIN_PRIMITIVE_CREATION3D,
                static_cast<int>(VishwakarmaStorage::ToNumber(VishwakarmaStorage::ObjectType::Cylinder)));
        } else if (action.id == static_cast<uint32_t>(Commands::CREATE_SPHERE)) {
            PushSystemTodoToTab(GetActiveTabForUIAction(), ACTION_TYPE::BEGIN_PRIMITIVE_CREATION3D,
                static_cast<int>(VishwakarmaStorage::ToNumber(VishwakarmaStorage::ObjectType::Sphere)));
        } else if (action.id == static_cast<uint32_t>(Commands::CREATE_PYRAMID)) {
            PushSystemTodoToTab(GetActiveTabForUIAction(), ACTION_TYPE::BEGIN_PRIMITIVE_CREATION3D,
                static_cast<int>(VishwakarmaStorage::ToNumber(VishwakarmaStorage::ObjectType::Pyramid)));
        } else if (action.id == static_cast<uint32_t>(Commands::CREATE_CONE)) {
            PushSystemTodoToTab(GetActiveTabForUIAction(), ACTION_TYPE::BEGIN_PRIMITIVE_CREATION3D,
                static_cast<int>(VishwakarmaStorage::ToNumber(VishwakarmaStorage::ObjectType::Cone)));
        } else if (action.id == static_cast<uint32_t>(Commands::CREATE_TORUS)) {
            PushSystemTodoToTab(GetActiveTabForUIAction(), ACTION_TYPE::BEGIN_PRIMITIVE_CREATION3D,
                static_cast<int>(VishwakarmaStorage::ToNumber(VishwakarmaStorage::ObjectType::Torus)));
        } else if (action.id == static_cast<uint32_t>(Commands::CREATE_ELLIPSOID)) {
            PushSystemTodoToTab(GetActiveTabForUIAction(), ACTION_TYPE::BEGIN_PRIMITIVE_CREATION3D,
                static_cast<int>(VishwakarmaStorage::ToNumber(VishwakarmaStorage::ObjectType::Ellipsoid)));
        } else if (action.id == static_cast<uint32_t>(Commands::CREATE_ELBOW)) {
            PushSystemTodoToTab(GetActiveTabForUIAction(), ACTION_TYPE::BEGIN_PRIMITIVE_CREATION3D,
                static_cast<int>(VishwakarmaStorage::ToNumber(VishwakarmaStorage::ObjectType::Elbow)));
        } else if (action.id == static_cast<uint32_t>(Commands::CREATE_T)) {
            PushSystemTodoToTab(GetActiveTabForUIAction(), ACTION_TYPE::BEGIN_PRIMITIVE_CREATION3D,
                static_cast<int>(VishwakarmaStorage::ToNumber(VishwakarmaStorage::ObjectType::Tee)));
        } else if (action.id == static_cast<uint32_t>(Commands::CREATE_FLANGE)) {
            PushSystemTodoToTab(GetActiveTabForUIAction(), ACTION_TYPE::BEGIN_PRIMITIVE_CREATION3D,
                static_cast<int>(VishwakarmaStorage::ToNumber(VishwakarmaStorage::ObjectType::Flange)));
        } else if (action.id == static_cast<uint32_t>(Commands::CREATE_LINE_MEMBER)) {
            PushSystemTodoToTab(GetActiveTabForUIAction(), ACTION_TYPE::BEGIN_PRIMITIVE_CREATION3D,
                static_cast<int>(VishwakarmaStorage::ToNumber(VishwakarmaStorage::ObjectType::LineMember)));
        } else if (action.id == static_cast<uint32_t>(Commands::CREATE_LINE)) {
            PushSystemTodoToTab(GetActiveTabForUIAction(), ACTION_TYPE::BEGIN_LINE_CREATION2D);
        } else if (action.id == static_cast<uint32_t>(Commands::CREATE_POLYLINE)) {
            PushSystemTodoToTab(GetActiveTabForUIAction(), ACTION_TYPE::BEGIN_POLYLINE_CREATION2D);
        } else if (action.id == static_cast<uint32_t>(Commands::CREATE_POLYGON)) {
            PushSystemTodoToTab(GetActiveTabForUIAction(), ACTION_TYPE::BEGIN_POLYGON_CREATION2D);
        } else if (action.id == static_cast<uint32_t>(Commands::CREATE_CIRCLE)) {
            PushSystemTodoToTab(GetActiveTabForUIAction(), ACTION_TYPE::BEGIN_CIRCLE_CREATION2D);
        } else if (action.id == static_cast<uint32_t>(Commands::CREATE_ELLIPSE)) {
            PushSystemTodoToTab(GetActiveTabForUIAction(), ACTION_TYPE::BEGIN_ELLIPSE_CREATION2D);
        } else if (action.id == static_cast<uint32_t>(Commands::CREATE_ARC)) {
            PushSystemTodoToTab(GetActiveTabForUIAction(), ACTION_TYPE::BEGIN_ARC_CREATION2D);
        } else if (action.id == static_cast<uint32_t>(Commands::CREATE_TEXT)) {
            PushSystemTodoToTab(GetActiveTabForUIAction(), ACTION_TYPE::BEGIN_TEXT_CREATION2D);
        } else if (action.id == static_cast<uint32_t>(Commands::EDIT_COPY)) {
            PushSystemTodoToTab(GetActiveTabForUIAction(), ACTION_TYPE::BEGIN_TRANSFORM2D,
                static_cast<int>(Cad2DTransformKind::Copy));
        } else if (action.id == static_cast<uint32_t>(Commands::EDIT_OFFSET)) {
            PushSystemTodoToTab(GetActiveTabForUIAction(), ACTION_TYPE::BEGIN_TRANSFORM2D,
                static_cast<int>(Cad2DTransformKind::Offset));
        } else if (action.id == static_cast<uint32_t>(Commands::EDIT_MIRROR)) {
            PushSystemTodoToTab(GetActiveTabForUIAction(), ACTION_TYPE::BEGIN_TRANSFORM2D,
                static_cast<int>(Cad2DTransformKind::Mirror));
        } else if (action.id == static_cast<uint32_t>(Commands::EDIT_ROTATE)) {
            PushSystemTodoToTab(GetActiveTabForUIAction(), ACTION_TYPE::BEGIN_TRANSFORM2D,
                static_cast<int>(Cad2DTransformKind::Rotate));
        } else if (action.id == static_cast<uint32_t>(Commands::EDIT_MOVE)) {
            PushSystemTodoToTab(GetActiveTabForUIAction(), ACTION_TYPE::BEGIN_TRANSFORM2D,
                static_cast<int>(Cad2DTransformKind::Move));
        } else if (action.id == static_cast<uint32_t>(Commands::CREATE_ASSET2D)) {
            PushSystemTodoToTab(GetActiveTabForUIAction(), ACTION_TYPE::CREATE_ASSET2D_FROM_SELECTION);
        } else if (action.id == static_cast<uint32_t>(Commands::INSERT_ASSET2D)) {
            PushSystemTodoToTab(GetActiveTabForUIAction(), ACTION_TYPE::BEGIN_ASSET_INSERT2D);
        } else if (action.id == static_cast<uint32_t>(Commands::ZOOM_MAX)) {
            PushSystemTodoToTab(GetActiveTabForUIAction(), ACTION_TYPE::ZOOM_MAX_EXTENTS);
        } else if (action.id == static_cast<uint32_t>(Commands::ZOOM_FOCUS)) {
            PushSystemTodoToTab(GetActiveTabForUIAction(), ACTION_TYPE::ZOOM_FOCUS_SELECTED);
        } else if (action.id == static_cast<uint32_t>(Commands::ZOOM_WINDOW)) {
            PushSystemTodoToTab(GetActiveTabForUIAction(), ACTION_TYPE::ZOOM_WINDOW_BEGIN);
        } else if (action.id == static_cast<uint32_t>(Commands::TOGGLE_AUTO_RANDOM)) {
            PushSystemTodoToTab(GetActiveTabForUIAction(), ACTION_TYPE::TOGGLE_AUTO_RANDOM_GEOMETRY);
        } else if (action.id == static_cast<uint32_t>(Commands::IMPORT_STD)) {
            ExtensionCommunications::QueueImportStdCommand(GetActiveTabForUIAction());
        } else if (action.id == static_cast<uint32_t>(Commands::IMPORT_DXF)) {
            ExtensionCommunications::QueueImportDxfCommand(GetActiveTabForUIAction());
        } else if (action.id == static_cast<uint32_t>(Commands::SOFTWARE_UPDATE_CHECK)) {
            RequestImmediateSoftwareUpdateCheck(); // Wakes the update thread; no-op for dev builds.
        } else if (action.id == kPropertyCommitUIAction) {
            // p1 = (tabIndex << 8) | fieldIndex, p2 = objectMemoryId, p3 = double value bits.
            const uint32_t tabIndex = static_cast<uint32_t>(action.p1 >> 8);
            const uint8_t fieldIndex = static_cast<uint8_t>(action.p1 & 0xFFu);
            if (tabIndex < MV_MAX_TABS) {
                // fieldIndex is forwarded as-is; the engineering thread bounds-checks it against the
                // resolved type's fieldCount (the object type is unknown until resolution there).
                PushSystemTodoToTab(&allTabs[tabIndex], ACTION_TYPE::MODIFY_OBJECT_PROPERTY,
                    static_cast<int>(fieldIndex), 0, 0, action.p2, action.p3);
            }
        }
    }

    CleanupReleasedTabs();
#ifdef _DEBUG
    ApplicationTab::DebugVerifyQueuesEmpty(); // Proves no path pushes to the thread-less tab 0.
#endif
}
}

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

// Helper to find a monitor in the OLD list by name
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

void AllocateConsoleWindow() {
    // Launched from a terminal with stdout captured (pipe / file): keep the inherited
    // handles so the diagnostics land in the captor instead of a fresh console window.
    const DWORD stdoutType = GetFileType(GetStdHandle(STD_OUTPUT_HANDLE));
    if (stdoutType == FILE_TYPE_PIPE || stdoutType == FILE_TYPE_DISK) {
        std::ios::sync_with_stdio(true);
        return;
    }
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

static const wchar_t* szTitle = L"विश्वकर्मा 0 :-)"; // The string that appears in the application's title bar.
std::wstring exePath;
unsigned char* imgData = nullptr;

HINSTANCE hInst;// Stored instance handle for use in Win32 API calls such as FindResource
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);// Forward declarations of functions included in this code module.

float GetTopRibbonHeightPxForWindow(const SingleUIWindow* window);

// Ask to AI: Whats the difference between WinMain and wWinMain for Windows Desktop C++ DirectX12 application?
// WinMain: This was legacy name. New name is wWinMain with Unicode support.
int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow)
{
    // Headless command-line modes: the every-6-days scheduled task launches us with --background-update
    // and "Apps & features" launches us with --uninstall. Both run without any window and exit
    // before graphics start up, so none of the copy / render / engineering threads are created.
    if (lpCmdLine) {
        if (wcsstr(lpCmdLine, L"--background-update")) return RunBackgroundUpdate();
        if (wcsstr(lpCmdLine, L"--uninstall")) { RunUninstall(); return 0; }
    }

    #ifdef _DEBUG
        AllocateConsoleWindow();// Only allocate console in debug builds
    #endif

    // If a newer verified setup was staged by the update thread, apply it and exit:
    // the installer replaces our exe and relaunches the new version.
    if (SoftwareUpdateOnAppLaunch()) return 0;
    StartSoftwareUpdateThread();

    // Installation identity (created by the installer; re-created here when missing) and
    // the per-launch ephemeral session key pair.
    AccountManager::InitializeOnLaunch();

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
    wcex.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
	wcex.lpfnWndProc = WndProc; //This is the root of all Windows message handling for our application.
    wcex.cbClsExtra = 0;
    wcex.cbWndExtra = 0;
    wcex.hInstance = hInstance;
    wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszMenuName = NULL;
    wcex.lpszClassName = szWindowClass;
    wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APP_ICON));
    //wcex.hIconSm = LoadIcon(wcex.hInstance, IDI_APPLICATION);
    wcex.hIconSm = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APP_ICON));

    exePath = GetExecutablePath();

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
        // ThrowIfFailed: a silently null queue surfaces much later as an inscrutable
        // "Device interface cannot be NULL" from CreateSwapChainForHwnd. Fail at the source.
        ThrowIfFailed(gpu.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&screen.commandQueue)));
    }

    // SETUP TABS (The Data) CRITICAL: Resize first to prevent pointer invalidation when threads start!
    publishedTabIndexes.store(activeTabIndexesA, std::memory_order_release);
    publishedTabCount.store(0, std::memory_order_release);

    publishedWindowIndexes.store(activeWindowIndexesA, std::memory_order_release);
    publishedWindowCount.store(0, std::memory_order_release);

    // Slot 0 is the un-closable Application Tab (no engineering thread); slot 1 is the single
    // engineering tab the application starts with, which keeps CPU memory group 0 reserved
    // for the Application Tab (tabs.md). Further tabs come from the '+' button.
    for (int i = 0; i < 2; ++i)
    {
        DATASETTAB& tab = allTabs[i];
        if (i == 0) {
            ApplicationTab::InitializeApplicationTab(tab);
        } else {
            tab.tabID = i;
            tab.tabNo = i;
            tab.fileName = L"Untitled 0";
        }
        // Tab 0 gets GPU state too: empty but valid keeps every downstream path (matrix tables,
        // compositor, copy-thread retirement) untouched.
        gpu.InitD3DPerTab(tab.dx);
        if (tab.cad2d) InitCad2DTabResources(*tab.cad2d);
        // We can set random colors here later to distinguish them further.
        // allTabs[i].color = ...
    }

    // Publish tab list
    activeTabIndexesA[0] = 0;
    activeTabIndexesA[1] = 1;

    publishedTabIndexes.store(activeTabIndexesA, std::memory_order_release);
    publishedTabCount.store(2, std::memory_order_release);

    // SETUP WINDOW (The View)
    uint16_t windowSlot = 0;
    SingleUIWindow& mainWindow = allWindows[windowSlot];
    // The application opens on the engineering tab, not the Application Tab: the user's work is
    // what they came for, and tab 0 is one click away in the band.
    mainWindow.activeTabIndex = 1;
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
    threads.emplace_back(ImprovementDataThread); // Usage statistics collection (5 min interval).

    InitUIResources(gpu.uiResources, gpu.device.Get()); //Prepare and upload UI resources (e.g. fonts, icons) to GPU.
    //Above function depends on GpuCopyThread, hence it can't be done earlier.
    InitSkyGradientResources(gpu.device.Get()); //Scene3D background pipeline, read by every render thread.
    std::wcout << L"Hello...." << std::endl;
    // LAUNCH THE ENGINEERING THREAD of the one initial tab. Main logic thread, the ringmaster of
    // the application: one per engineering tab, created with the tab and joined when it closes.
    // Slot 0 (the Application Tab) deliberately gets no thread: the UI thread owns it.
    {
        std::thread t(विश्वकर्मा, (uint64_t)1);// Pass the tab index to the thread function
        AddEngineeringThread(1, std::move(t));
    }

    //threads.emplace_back(GpuRenderThread, 0, 60);  // Monitor 1 at 60Hz
    //threads.emplace_back(GpuRenderThread, 1, 144); // Monitor 2 at 144Hz
    RestartRenderThreads();// Initial Render Thread Launch (Not a monitor topology change, just startup)
    // Graphics startup complete: topology-change messages (WM_DISPLAYCHANGE / WM_DPICHANGED) may
    // now trigger RestartRenderThreads. Written and read on this (UI) thread only.
    gpu.isGPUEngineInitialized = true;
    // Icon atlases are built (RestartRenderThreads above), so the splash's rounded background can be
    // tessellated. Set here, not inside RestartRenderThreads, which also runs on monitor topology
    // changes and would otherwise re-show the splash every time a monitor is plugged in.
    g_splashOverlayStartTick.store(GetTickCount64(), std::memory_order_relaxed);

    MSG msg = {};// Main message loop:

    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) { //Does not block. Returns immediately.
            //We can not use alternate GetMessage() since that one block waiting for windows.
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            ProcessPendingUIActions();
        } else {
            ProcessPendingUIActions();
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
    // Join engineering threads created dynamically
    JoinAllEngineeringThreads();
    // Clean up resources before exiting the application.
    // Cleanup Windows
    uint16_t* winList = publishedWindowIndexes.load(std::memory_order_acquire);
    uint16_t winCount = publishedWindowCount.load(std::memory_order_acquire);
    for (uint16_t i = 0; i < winCount; ++i) gpu.CleanupWindowResources(allWindows[winList[i]].dx);
    // Cleanup Tabs (Geometry)
    uint16_t* tabList = publishedTabIndexes.load(std::memory_order_acquire);
    uint16_t tabCount = publishedTabCount.load(std::memory_order_acquire);
    for (uint16_t i = 0; i < tabCount; ++i) {
        DATASETTAB& tab = allTabs[tabList[i]];
        gpu.CleanupTabResources(tab.dx);
        if (tab.cad2d) CleanupCad2DTabResources(*tab.cad2d);
    }

    CleanupUIResources(gpu.uiResources);
    gpu.skyGradientPSO.Reset();
    gpu.skyGradientRootSignature.Reset();
    gpu.CleanupD3DGlobal();// Global Cleanup
    
    //Cleanup Freetype library.
    //FT_Done_Face(face);
    //FT_Done_FreeType(ft);
    
    //gpu.WaitForPreviousFrame(); // Wait for GPU to finish all commands. No need. All render threads have exited by now.

    std::wcout << "Application finished cleanly." << std::endl;

    return (int)msg.wParam;
}

// Helper to sync modifiers for the current window (called on every input message)
static void SyncModifiersForWindow(SingleUIWindow* window) {
    if (!window) return;
    window->uiInput.shiftDown = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    window->uiInput.ctrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    window->uiInput.altDown = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
}

static void UpdateUIMousePosition(SingleUIWindow* window, LPARAM lParam) {
    if (!window) return;
    window->uiInput.mouseX = static_cast<float>(GET_X_LPARAM(lParam));
    window->uiInput.mouseY = static_cast<float>(GET_Y_LPARAM(lParam));
}

float GetTopRibbonHeightPxForWindow(const SingleUIWindow* window) {
    if (!window || window->windowKind == WINDOW_KIND_VIEW || // Extracted views have no ribbon.
        window->currentMonitorIndex < 0 || window->currentMonitorIndex >= gpu.currentMonitorCount) {
        return 0.0f;
    }

    const OneMonitorController& monitor = gpu.screens[window->currentMonitorIndex];
    if (monitor.topRibbonLayout.isValid && monitor.topRibbonLayout.topUITotalHeightPx > 0.0f) {
        return monitor.topRibbonLayout.topUITotalHeightPx;
    }

    const float dpiY = (monitor.physicalDpiY > 0) ? (float)monitor.physicalDpiY : (float)monitor.dpiY;
    const float pixelsPerMMy = dpiY / 25.4f;
    return std::round((UI_TAB_BAR_HEIGHT_MM + UI_DIVIDER_GAP_PX +
        UI_ACTION_GROUP_LABEL_HEIGHT_MM + UI_DIVIDER_GAP_PX +
        UI_ACTION_GROUP_HEIGHT_MM + UI_DIVIDER_GAP_PX +
        UI_ACTION_GROUP_LABEL_HEIGHT_MM + UI_DIVIDER_GAP_PX +
        UI_INTERNAL_TAB_BAR_HEIGHT_MM) * pixelsPerMMy) + 7.0f;
}

static bool IsClientPointOverTopRibbon(const SingleUIWindow* window, const POINT& pt) {
    const float ribbonHeight = GetTopRibbonHeightPxForWindow(window);
    return ribbonHeight > 0.0f && pt.y >= 0 && (float)pt.y < ribbonHeight;
}

// Interacting inside a window's content area retargets the tab's input view: extracted view
// windows route scene/page input (camera, 2D tools) to their own sub-tab slot, the tab-host
// window routes it back to the inline-active sub-tab. Ribbon / band clicks (above the top UI of
// a tab-host window) leave the routing unchanged, so ribbon tools keep acting on the view the
// user last worked in.
static void RouteSceneInputToWindowView(SingleUIWindow* window, DATASETTAB* tab, int clientY) {
    if (!window || !tab) return;
    if (window->windowKind == WINDOW_KIND_VIEW) {
        const uint16_t slot = window->viewSubTabSlot;
        if (slot >= MV_MAX_SUBTABS) return;
        if (tab->subTabStates[slot].load(std::memory_order_acquire) != SUBTAB_OPEN) return;
        tab->inputViewSubTabSlot.store(slot, std::memory_order_release);
    } else if (static_cast<float>(clientY) >= GetTopRibbonHeightPxForWindow(window)) {
        tab->inputViewSubTabSlot.store(-1, std::memory_order_release);
    }
}

static bool GetDataTreeLayoutForWindow(
    const SingleUIWindow* window, const DATASETTAB* tab, DataTreeView::LayoutMetrics& layout) {
    if (!window || !tab || window->windowKind == WINDOW_KIND_VIEW || // No data tree in view windows.
        !tab->dataTreeView.isVisible.load(std::memory_order_acquire) ||
        window->currentMonitorIndex < 0 || window->currentMonitorIndex >= gpu.currentMonitorCount) {
        return false;
    }

    RECT clientRect{};
    if (!GetClientRect(window->hWnd, &clientRect)) return false;

    const OneMonitorController& monitor = gpu.screens[window->currentMonitorIndex];
    const float dpiX = (monitor.physicalDpiX > 0) ? (float)monitor.physicalDpiX : (float)monitor.dpiX;
    const float dpiY = (monitor.physicalDpiY > 0) ? (float)monitor.physicalDpiY : (float)monitor.dpiY;
    const float ribbonHeight = GetTopRibbonHeightPxForWindow(window);

    DataTreeView::BuildRequest request;
    request.viewportTopPx = ribbonHeight;
    request.viewportHeightPx = (std::max)(0.0f,
        static_cast<float>(clientRect.bottom - clientRect.top) - ribbonHeight);
    request.pixelsPerMMX = dpiX / 25.4f;
    request.pixelsPerMMY = dpiY / 25.4f;
    layout = DataTreeView::CalculateLayout(request);
    return layout.width > 0.0f && layout.height > 0.0f;
}

static bool IsClientPointOverDataTree(
    const SingleUIWindow* window, const DATASETTAB* tab, const POINT& pt) {
    DataTreeView::LayoutMetrics layout;
    if (!GetDataTreeLayoutForWindow(window, tab, layout)) return false;
    return DataTreeView::ContainsPoint(layout, static_cast<float>(pt.x), static_cast<float>(pt.y));
}

static bool IsClientPointOverDataTreeScrollbar(
    const SingleUIWindow* window, const DATASETTAB* tab, const POINT& pt) {
    DataTreeView::LayoutMetrics layout;
    if (!GetDataTreeLayoutForWindow(window, tab, layout)) return false;
    return pt.x >= layout.scrollbarX && pt.x < layout.scrollbarX + layout.scrollbarWidth &&
        pt.y >= layout.scrollbarY && pt.y < layout.scrollbarY + layout.scrollbarHeight;
}

// Right icon bar + properties pane overlay hit test. Width is published by the render thread each
// frame (0 when nothing is drawn there). Primary click-through guard (propertiesPane.md §6).
static bool IsClientPointOverRightOverlay(const SingleUIWindow* window, const POINT& pt) {
    if (!window) return false;
    const uint32_t overlayWidth = window->rightOverlayWidthPx.load(std::memory_order_acquire);
    if (overlayWidth == 0) return false;

    RECT clientRect{};
    if (!GetClientRect(window->hWnd, &clientRect)) return false;
    const float ribbonHeight = GetTopRibbonHeightPxForWindow(window);
    return static_cast<float>(pt.y) >= ribbonHeight &&
        pt.x >= clientRect.right - static_cast<int>(overlayWidth);
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
    int size_needed = WideCharToMultiByte(
        CP_UTF8, 0, &exePath[0], static_cast<int>(exePath.size()), NULL, 0, NULL, NULL);
    std::string str(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &exePath[0], static_cast<int>(exePath.size()), &str[0], size_needed, NULL, NULL);
    int w, h;

    DATASETTAB* tab = GetActiveTabFromHwnd(hWnd);
    // Nothing may reach the Application Tab's userInputQueue - it has no consumer (tabs.md
    // Decision 3). Dropping the pointer here covers every push site below at once; the per-window
    // uiInput snapshot path is untouched, so all overlay UI keeps working on tab 0.
    if (tab && ApplicationTab::IsApplicationTab(tab->tabID)) tab = nullptr;
    ACTION_DETAILS ad;

    uint16_t* tabList = publishedTabIndexes.load(std::memory_order_acquire);
    uint16_t tabCount = publishedTabCount.load(std::memory_order_acquire);
    uint16_t* windowList = publishedWindowIndexes.load(std::memory_order_acquire);
    uint16_t windowCount = publishedWindowCount.load(std::memory_order_acquire);

    SingleUIWindow* currentWindow = nullptr;
    for (uint16_t i = 0; i < windowCount; ++i) {
        if (allWindows[windowList[i]].hWnd == hWnd) {
            currentWindow = &allWindows[windowList[i]];
            break;
        }
    }

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
        g_statKeyPresses.fetch_add(1, std::memory_order_relaxed); // Usage statistics.
        if (tab) {
            // A focused UI text field swallows shortcut keys so e.g. 'P' does not spawn a pyramid
            // while the user types a value (propertiesPane.md §4).
            const bool uiFieldFocused = currentWindow &&
                currentWindow->uiKeyboardCaptureCount.load(std::memory_order_acquire) != 0;
            if (!uiFieldFocused) {
                ad.actionType = ACTION_TYPE::KEYDOWN;
                ad.source = INPUT_SOURCE::KEYBOARD;
                ad.x = static_cast<int>(wParam); //Virtual key code
                ad.y = static_cast<int>(lParam); //Repeat count, scan code, flags
                ad.delta = 0;
                ad.timestamp = GetTickCount64();
                tab->userInputQueue->push(ad); //userInputQueue is threadsafe.
            }
            SyncModifiersForWindow(currentWindow);
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
            SyncModifiersForWindow(currentWindow);
        }
        return 0;

    case WM_CHAR: // This is different from WM_KEYDOWN / UP because it accounts for keyboard layout,
    // modifiers, etc. It gives you the actual character that should be input, rather than the physical key.
    {
        // Feed the character to the per-frame UI text buffer (consumed by the focused text field in
        // the render thread). The buffer is reset each frame by the render thread (propertiesPane.md §4).
        if (currentWindow) {
            const uint8_t count = currentWindow->uiInput.textInputCount;
            if (count < 32) {
                currentWindow->uiInput.textInputThisFrame[count] = static_cast<char32_t>(wParam);
                currentWindow->uiInput.textInputCount = static_cast<uint8_t>(count + 1);
            }
        }
        const bool uiFieldFocused = currentWindow &&
            currentWindow->uiKeyboardCaptureCount.load(std::memory_order_acquire) != 0;
        if (tab && !uiFieldFocused) { // Suppress text reaching the tab while a UI field has focus.
            ad.actionType = ACTION_TYPE::CHAR;
            ad.source = INPUT_SOURCE::KEYBOARD;
            ad.x = static_cast<int>(wParam); //Virtual key code
            ad.y = static_cast<int>(lParam); //Repeat count, scan code, flags
            ad.delta = 0;
            ad.timestamp = GetTickCount64();
            tab->userInputQueue->push(ad); //userInputQueue is threadsafe.
        }
        if (currentWindow) SyncModifiersForWindow(currentWindow);
        return 0;
    }

    case WM_SYSKEYDOWN:
        g_statKeyPresses.fetch_add(1, std::memory_order_relaxed); // Usage statistics.
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
        UpdateUIMousePosition(currentWindow, lParam);
        if (currentWindow && tab && GetCapture() == hWnd) {
            SyncModifiersForWindow(currentWindow);
            return 0;
        }
        if (tab) {
            ad.actionType = ACTION_TYPE::MOUSEMOVE;
            ad.source = INPUT_SOURCE::MOUSE;
            ad.x = GET_X_LPARAM(lParam); //Client X
            ad.y = GET_Y_LPARAM(lParam); //Client Y
            ad.delta = static_cast<int>(wParam); // Button/modifier flags (MK_LBUTTON, etc. )
            ad.timestamp = GetTickCount64();
            tab->userInputQueue->push(ad); //userInputQueue is threadsafe.
            SyncModifiersForWindow(currentWindow);
        }
        return 0;

    case WM_LBUTTONDBLCLK:
    case WM_LBUTTONDOWN:
    {
        const bool isDoubleClick = message == WM_LBUTTONDBLCLK;
        g_statLeftClicks.fetch_add(1, std::memory_order_relaxed); // Usage statistics.
        if (currentWindow) {
            UpdateUIMousePosition(currentWindow, lParam);
            currentWindow->uiInput.leftButtonDown = true;
            currentWindow->uiInput.leftButtonPressedThisFrame = true;
            currentWindow->uiInput.leftButtonDoubleClickedThisFrame = isDoubleClick;
        }
        if (currentWindow && tab) {
            POINT clientPoint = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (IsClientPointOverDataTreeScrollbar(currentWindow, tab, clientPoint)) {
                SetCapture(hWnd);
                SyncModifiersForWindow(currentWindow);
                return 0;
            }
        }
        if (currentWindow) {
            // Clicks on the right icon bar / properties pane are handled by the immediate-mode UI
            // (via uiInput set above); never forward them to the scene (propertiesPane.md §6).
            POINT overlayPoint = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (IsClientPointOverRightOverlay(currentWindow, overlayPoint)) {
                SyncModifiersForWindow(currentWindow);
                return 0;
            }
        }
        if (tab) {
            RouteSceneInputToWindowView(currentWindow, tab, GET_Y_LPARAM(lParam));
            ad.actionType = ACTION_TYPE::LBUTTONDOWN;
            ad.source = INPUT_SOURCE::MOUSE;
            ad.x = GET_X_LPARAM(lParam);
            ad.y = GET_Y_LPARAM(lParam);
            ad.delta = static_cast<int>(wParam); // Button/modifier flags (MK_LBUTTON, etc. )
            ad.timestamp = GetTickCount64();
            tab->userInputQueue->push(ad); //userInputQueue is threadsafe.
            SyncModifiersForWindow(currentWindow);
        }
        return 0;
    }

    case WM_LBUTTONUP:
    {
        const bool releasingDataTreeScrollbar = GetCapture() == hWnd;
        if (currentWindow) {
            UpdateUIMousePosition(currentWindow, lParam);
            currentWindow->uiInput.leftButtonDown = false;
            currentWindow->uiInput.leftButtonReleasedThisFrame = true;
        }
        if (releasingDataTreeScrollbar) {
            ReleaseCapture();
            SyncModifiersForWindow(currentWindow);
            return 0;
        }
        if (tab) {
            ad.actionType = ACTION_TYPE::LBUTTONUP;
            ad.source = INPUT_SOURCE::MOUSE;
            ad.x = GET_X_LPARAM(lParam);
            ad.y = GET_Y_LPARAM(lParam);
            ad.delta = static_cast<int>(wParam); // Button/modifier flags (MK_LBUTTON, etc. )
            ad.timestamp = GetTickCount64();
            tab->userInputQueue->push(ad); //userInputQueue is threadsafe.
            SyncModifiersForWindow(currentWindow);
        }
        return 0;
    }
    
    case WM_RBUTTONDOWN:
        g_statRightClicks.fetch_add(1, std::memory_order_relaxed); // Usage statistics.
        if (currentWindow) {
            UpdateUIMousePosition(currentWindow, lParam);
            currentWindow->uiInput.rightButtonDown = true;
            currentWindow->uiInput.rightButtonPressedThisFrame = true;
        }
        if (tab) {
            ad.actionType = ACTION_TYPE::RBUTTONDOWN;
            ad.source = INPUT_SOURCE::MOUSE;
            ad.x = GET_X_LPARAM(lParam);
            ad.y = GET_Y_LPARAM(lParam);
            ad.delta = static_cast<int>(wParam); // Button/modifier flags (MK_LBUTTON, etc. )
            ad.timestamp = GetTickCount64();
            tab->userInputQueue->push(ad); //userInputQueue is threadsafe.
            SyncModifiersForWindow(currentWindow);
        }
        return 0;
    
    case WM_RBUTTONUP:
        if (currentWindow) {
            UpdateUIMousePosition(currentWindow, lParam);
            currentWindow->uiInput.rightButtonDown = false;
        }
        if (tab) {
            ad.actionType = ACTION_TYPE::RBUTTONUP;
            ad.source = INPUT_SOURCE::MOUSE;
            ad.x = GET_X_LPARAM(lParam);
            ad.y = GET_Y_LPARAM(lParam);
            ad.delta = static_cast<int>(wParam); // Button/modifier flags (MK_LBUTTON, etc. )
            ad.timestamp = GetTickCount64();
            tab->userInputQueue->push(ad); //userInputQueue is threadsafe.
            SyncModifiersForWindow(currentWindow);
        }
        return 0;
    
    case WM_MBUTTONDOWN:
        g_statMiddleClicks.fetch_add(1, std::memory_order_relaxed); // Usage statistics.
        if (currentWindow) {
            UpdateUIMousePosition(currentWindow, lParam);
            currentWindow->uiInput.middleButtonDown = true;
            currentWindow->uiInput.middleButtonPressedThisFrame = true;
        }
        if (tab) {
            RouteSceneInputToWindowView(currentWindow, tab, GET_Y_LPARAM(lParam));
            ad.actionType = ACTION_TYPE::MBUTTONDOWN;
            ad.source = INPUT_SOURCE::MOUSE;
            ad.x = GET_X_LPARAM(lParam);
            ad.y = GET_Y_LPARAM(lParam);
            ad.delta = static_cast<int>(wParam); // Button/modifier flags (MK_LBUTTON, etc. )
            ad.timestamp = GetTickCount64();
            tab->userInputQueue->push(ad); //userInputQueue is threadsafe.
            SyncModifiersForWindow(currentWindow);
        }
        return 0;
    
    case WM_MBUTTONUP:
        if (currentWindow) {
            UpdateUIMousePosition(currentWindow, lParam);
            currentWindow->uiInput.middleButtonDown = false;
        }
        if (tab) {
            ad.actionType = ACTION_TYPE::MBUTTONUP;
            ad.source = INPUT_SOURCE::MOUSE;
            ad.x = GET_X_LPARAM(lParam);
            ad.y = GET_Y_LPARAM(lParam);
            ad.delta = static_cast<int>(wParam); // Button/modifier flags (MK_LBUTTON, etc. )
            ad.timestamp = GetTickCount64();
            tab->userInputQueue->push(ad); //userInputQueue is threadsafe.
            SyncModifiersForWindow(currentWindow);
        }
        return 0;
    
    case WM_MOUSEWHEEL:
    {
        POINT uiPoint = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hWnd, &uiPoint);
        const bool handledByTopRibbon = currentWindow && IsClientPointOverTopRibbon(currentWindow, uiPoint);
        const bool handledByDataTree =
            currentWindow && tab && IsClientPointOverDataTree(currentWindow, tab, uiPoint);
        // Wheel over the right icon bar / properties pane is swallowed (no scene zoom). The pane
        // does not scroll in the MVP, so we do not accumulate the delta (propertiesPane.md §6).
        const bool handledByRightOverlay =
            currentWindow && IsClientPointOverRightOverlay(currentWindow, uiPoint);

        if (currentWindow) {
            currentWindow->uiInput.mouseX = static_cast<float>(uiPoint.x);
            currentWindow->uiInput.mouseY = static_cast<float>(uiPoint.y);
            if (handledByTopRibbon || handledByDataTree) {
                currentWindow->uiInput.mouseWheelDelta += GET_WHEEL_DELTA_WPARAM(wParam);
                SyncModifiersForWindow(currentWindow);
            }
        }
        if (handledByTopRibbon || handledByDataTree || handledByRightOverlay) return 0;

        if (tab) {
            RouteSceneInputToWindowView(currentWindow, tab, uiPoint.y);
            ad.actionType = ACTION_TYPE::MOUSEWHEEL;
            ad.source = INPUT_SOURCE::MOUSE;
            ad.x = uiPoint.x;
            ad.y = uiPoint.y;
            ad.delta = GET_WHEEL_DELTA_WPARAM(wParam);
            ad.timestamp = GetTickCount64();
            tab->userInputQueue->push(ad); //userInputQueue is threadsafe.
            SyncModifiersForWindow(currentWindow);
        }
        return 0;
    }

    //case WM_NCLBUTTONDOWN: return 0; // If needed for title bars / boarders.
    //Currently removed because it was causing WM_CLOSE to not fire up even when clicking close button.
    //If we want to support dragging from the title bar, we can re - enable this and add logic to handle it.
    
    case WM_CAPTURECHANGED: // Notify all tabs to release captured mouse states (e.g. , if dragging )
        if (currentWindow) {
            currentWindow->uiInput.leftButtonDown = false;
            currentWindow->uiInput.leftButtonReleasedThisFrame = true;
        }
        for (uint16_t ti = 0; ti < tabCount; ++ti) {
            if (ApplicationTab::IsApplicationTab(tabList[ti])) continue; // Queue has no consumer.
            DATASETTAB & tab = allTabs[tabList[ti]];
            tab.dataTreeView.scrollbarDragging.store(false, std::memory_order_release);

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
        // Before startup completes, RestartRenderThreads would call BuildMonitorIconAtlas, which
        // waits INFINITE on the copy fence - and GpuCopyThread may not exist yet. That deadlock
        // leaves a zombie process holding the D3D12 device (subsequent launches then fail at
        // CreateCommandQueue). Startup runs RestartRenderThreads itself, so early messages are moot.
        if (!gpu.isGPUEngineInitialized) break;
        std::wcout << L"WM_DISPLAYCHANGE / WM_DPICHANGED received. Restarting render threads." << std::endl;
        /* This IS a topology change. Monitor details may have changed. But Geometry belonging to tabs persists!
        Monitor addition / removal is very rare event. 1 event each couple hours average is already conservative.
        Hence we can afford to briefly pause rendering and restart all threads to pick up the new topology.
        Even windows OS flickers when you add/remove monitor, so a brief pause in rendering is not a big deal.*/
		RestartRenderThreads(); // Preserves commandQueues and swapChains to the extent possible.
        break;

    case WM_ENTERSIZEMOVE:
        if (currentWindow) currentWindow->isInSizeMove = true;
        // The actual handling of monitor affinity will be done in WM_EXITSIZEMOVE to avoid excessive handling 
        // during active movement.
		break;

    case WM_MOVE:
		break; // We handle this in WM_EXITSIZEMOVE to avoid excessive handling during active movement.

		
        break;
    case WM_SIZE:
    {   
        // Handle resizing of the window. This can be triggered by user resizing or maximize/unmaxmime. 
        // We need to resize the swap chain buffers accordingly.
        // TODO: optimize for minimized state by pausing rendering and skipping buffer presentation.
		if (wParam == SIZE_MINIMIZED) return 0; // For now we just skip resizing logic.
        UINT newWidth = LOWORD(lParam);
        UINT newHeight = HIWORD(lParam);

        if (currentWindow) { // Simply keep storing latest value. Render thread will pick it up when it has time.
            currentWindow->nextRequestedWidth.store(newWidth, std::memory_order_relaxed);
            currentWindow->nextRequestedHeight.store(newHeight, std::memory_order_relaxed);

            // Border dragging is committed in WM_EXITSIZEMOVE. Maximize/restore does not necessarily
            // enter that modal sizing loop, so signal the render thread directly for those paths.
            if (!currentWindow->isInSizeMove &&
                (newWidth != currentWindow->currentWidth || newHeight != currentWindow->currentHeight)) {
                currentWindow->resizeState.store(1, std::memory_order_release);
            }
        }
        return 0;
    }

    case WM_EXITSIZEMOVE:{ // It occurs post - movement, not while the window is actively moving(use WM_MOVING for that).
        std::wcout << L"WM_EXITSIZEMOVE received. Checking Monitor affinity." << std::endl;
        SingleUIWindow* pWin = nullptr; // Get our internal window object
        uint16_t * windowList = publishedWindowIndexes.load(std::memory_order_acquire);
        uint16_t windowCount = publishedWindowCount.load(std::memory_order_acquire);
        for (uint16_t wi = 0; wi < windowCount; ++wi) {
            SingleUIWindow& w = allWindows[windowList[wi]];
            if (w.hWnd == hWnd) { pWin = &w; break; }
        }
        if (pWin) {
            pWin->isInSizeMove = false;

            // Drag-drop merge: dropping this window on another window's tab band merges it back
            // (view windows only onto their parent tab's window). The window is destroyed then.
            if (TryMergeWindowOnDrop(*pWin)) break;

            // Determine which monitor the window is now on
            HMONITOR hMonitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
            int newMonitorIdx = -1;
            for (int i = 0; i < gpu.currentMonitorCount; i++) {// Find our internal index for this monitor
                if (gpu.screens[i].hMonitor == hMonitor) { newMonitorIdx = i; break; }
            }

            // Trigger Migration if monitor changed
            if (newMonitorIdx != -1 && pWin->currentMonitorIndex != newMonitorIdx) {
                pWin->requestedMonitorIndex = newMonitorIdx;
                uint32_t expected = 0;
                pWin->migrationState.compare_exchange_strong(expected, 1, std::memory_order_release);
                std::wcout <<"Migration requested: "<< pWin->currentMonitorIndex<< " to " <<newMonitorIdx<< std::endl;
            }
            else std::wcout << L"No change. Currently on " << gpu.screens[pWin->currentMonitorIndex].friendlyName << std::endl;

            uint32_t reqW = pWin->nextRequestedWidth.load(std::memory_order_relaxed);
            uint32_t reqH = pWin->nextRequestedHeight.load(std::memory_order_relaxed);
            if (reqW != pWin->currentWidth || reqH != pWin->currentHeight) {
                pWin->resizeState.store(1, std::memory_order_release);  // signal render thread
                std::wcout << L"Resizing Requested: " << pWin->nextRequestedWidth << L" x "
                    << pWin->nextRequestedHeight << std::endl;
            }
        }
        break;
        }
    case WM_CLOSE: // This is called BEFORE WM_DESTROY is received. Importantly, once this is over hWnd is destroyed.
        // Secondary windows (extracted tabs / views) close individually: hosted content returns
        // to the main window / inline band, rendering elsewhere continues undisturbed.
        if (currentWindow && currentWindow != &allWindows[0]) {
            HandleSecondaryWindowClose(*currentWindow);
            return 0;
        }
        // Main window: full application shutdown below.
        // Initiate shutdown for render threads FIRST
        std::wcout << "WM_CLOSE: Pausing render threads..." << std::endl;
        pauseRenderThreads = true;

        // Join render threads to ensure they stop BEFORE window destruction
        // TODO : Warning : This will terminate ALL render threads. Revisit this code when multi window is implemented.
        for (auto& t : renderThreads) {
            if (t.joinable()) { t.join(); }
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
        // Only the main window quits the application; secondary windows die individually.
        if (hWnd == allWindows[0].hWnd) PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
        break;
    }

    return 0;
}
