// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.

/*This is the application's orchestrator. It consumes commands, updates the scene database, 
identifies dirty objects, and generates work for the GPU threads.
This thread is also responsible for engineering calculations, consistency of Data etc.
*/

#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <algorithm>
#include <cstring>
#include <string>
#include <random> // Required for std::uniform_int_distribution
#include <unordered_map>
#include <memory>
#include <bit> // std::bit_cast for the property-edit value payload.
#include "MemoryManagerCPU.h"
#include "विश्वकर्मा.h"
#include "डेटा.h"
#include "डेटा-सामान्य-3D.h"
#include "PropertyPane.h"
#include "MemoryManagerGPU-DirectX12.h"
#include "ExtensionCommunications.h"

राम cpu;
शंकर gpu;

// Global Variables.
extern std::atomic<bool> shutdownSignal; // Externs for communication
std::atomic<uint64_t> g_nextPyramidId = 1;

// UI action queue (produced by UI thread, consumed by engineering threads)
// TODO: WARNING: We must not have mutex contention on this queue. Get rid of this soon.
// If the UI thread is producing actions at a very high rate, it can cause performance issues.
std::mutex g_actionQueueMutex;
std::deque<UIActionEntry> g_actionQueue;

// Helper for engineering thread to pop all pending actions (thread-safe)
static void PopAllUIActions(std::vector<UIActionEntry>& out) {
    std::lock_guard<std::mutex> lk(g_actionQueueMutex);
    while (!g_actionQueue.empty()) {
        out.push_back(g_actionQueue.front());
        g_actionQueue.pop_front();
    }
}
// Engineering thread registry (threads created dynamically)
struct EngineeringThreadRecord {
    uint64_t tabID;
    std::thread thread;
};

static std::mutex g_engineThreadsMutex;
static std::vector<EngineeringThreadRecord> g_engineeringThreads;

void AddEngineeringThread(uint64_t tabID, std::thread&& t) {
    std::lock_guard<std::mutex> lk(g_engineThreadsMutex);
    g_engineeringThreads.push_back({ tabID, std::move(t) });
}

void JoinReleasedEngineeringThreads() {
    std::vector<std::thread> threadsToJoin;
    {
        std::lock_guard<std::mutex> lk(g_engineThreadsMutex);
        auto it = g_engineeringThreads.begin();
        while (it != g_engineeringThreads.end()) {
            if (it->tabID < MV_MAX_TABS &&
                allTabs[it->tabID].engineeringReleased.load(std::memory_order_acquire)) {
                threadsToJoin.emplace_back(std::move(it->thread));
                it = g_engineeringThreads.erase(it);
            } else {
                ++it;
            }
        }
    }

    for (auto& t : threadsToJoin) {
        if (t.joinable()) t.join();
    }
}

void JoinAllEngineeringThreads() {
    std::lock_guard<std::mutex> lk(g_engineThreadsMutex);
    for (auto & et : g_engineeringThreads) {
        if (et.thread.joinable()) et.thread.join();
    }
    g_engineeringThreads.clear();
}


/* Different tabs represent different files opened in the software.
Just like different website links open in different Internet browser tab. Tab No. 0 Show the opening screen.
i.e.Not associated with any particular opened file. 1 DATASET = 1 TAB visible to user / to website. */
uint8_t noOfOpenedDataset = 0;

/* Initially we started with std:vector. , but latter changed to static array to simplify software design.
std::vector Grows exponentially. 1.5x for GCC/Clang, 2x for MSVC.
activeTabIndexesA/B are used to quickly iterate over active tabs without checking all slots in allTabs array.
We will maintain this list in sorted order for better cache performance.
Tab Lifecycle : creation → activation → rendering → deactivation → deferred destruction.
Static slot-based tab registry. Double-buffered index lists are published atomically.
Only UI thread modifies structure (creation/deletion). 
Engineering threads and Render threads only read published snapshot and modify runtime fields.
Tab Lifecycle : creation → activation → rendering → deactivation → deferred destruction.
*/
DATASETTAB allTabs[MV_MAX_TABS]; //They are all the dataset tabs opened in the application.
uint16_t activeTabIndexesA[MV_MAX_TABS], activeTabIndexesB[MV_MAX_TABS]; // double buffered index list
std::atomic<uint16_t*> publishedTabIndexes;
std::atomic<uint16_t>  publishedTabCount;

SingleUIWindow allWindows[MV_MAX_WINDOWS];
uint16_t activeWindowIndexesA[MV_MAX_WINDOWS], activeWindowIndexesB[MV_MAX_WINDOWS];
std::atomic<uint16_t*> publishedWindowIndexes;
std::atomic<uint16_t>  publishedWindowCount;

std::atomic<int32_t> g_uiActionSourceTabIndex{ -1 };

static int SceneTopUIHeightPxForWindow(const SingleUIWindow& window, int windowHeight) {
    if (window.windowKind == WINDOW_KIND_VIEW) return 0; // Extracted views render content only.
    int topUITotalHeightPx = 0;
    const int monitorId = window.currentMonitorIndex;
    if (monitorId >= 0 && monitorId < gpu.currentMonitorCount) {
        const UITopRibbonLayout& layout = gpu.screens[monitorId].topRibbonLayout;
        if (layout.isValid && layout.topUITotalHeightPx > 0.0f) {
            topUITotalHeightPx = static_cast<int>(std::round(layout.topUITotalHeightPx));
        }
        else {
            const float dpiY = gpu.screens[monitorId].physicalDpiY > 0
                ? static_cast<float>(gpu.screens[monitorId].physicalDpiY)
                : 96.0f;
            const float pixelsPerMMy = dpiY / 25.4f;
            topUITotalHeightPx = static_cast<int>(std::round((UI_TAB_BAR_HEIGHT_MM + UI_DIVIDER_GAP_PX +
                UI_ACTION_GROUP_LABEL_HEIGHT_MM + UI_DIVIDER_GAP_PX +
                UI_ACTION_GROUP_HEIGHT_MM + UI_DIVIDER_GAP_PX +
                UI_ACTION_GROUP_LABEL_HEIGHT_MM + UI_DIVIDER_GAP_PX +
                UI_INTERNAL_TAB_BAR_HEIGHT_MM) * pixelsPerMMy)) + 7;
        }
    }

    return std::clamp(topUITotalHeightPx, 0, windowHeight);
}

bool GetVisibleSceneViewportForTab(const DATASETTAB& tab, int& widthPx, int& heightPx, int& topPx) {
    widthPx = 0;
    heightPx = 0;
    topPx = 0;

    // When the input view is an extracted sub-tab, measure its dedicated window (content-only);
    // otherwise measure the tab-host window showing the inline view.
    const int inputSlot = InputViewSlot(tab);
    const int16_t viewWindowSlot = inputSlot >= 0
        ? tab.subTabHostWindowSlots[inputSlot].load(std::memory_order_acquire)
        : static_cast<int16_t>(-1);

    uint16_t* windowList = publishedWindowIndexes.load(std::memory_order_acquire);
    const uint16_t windowCount = publishedWindowCount.load(std::memory_order_acquire);
    for (uint16_t i = 0; i < windowCount; ++i) {
        const SingleUIWindow& window = allWindows[windowList[i]];
        if (viewWindowSlot >= 0) {
            if (windowList[i] != static_cast<uint16_t>(viewWindowSlot)) continue;
        } else {
            if (window.windowKind != WINDOW_KIND_TABHOST) continue;
            if (window.activeTabIndex != static_cast<int>(tab.tabID)) continue;
        }

        const int windowWidth = window.dx.WindowWidth > 0 ? window.dx.WindowWidth : window.currentWidth;
        const int windowHeight = window.dx.WindowHeight > 0 ? window.dx.WindowHeight : window.currentHeight;
        if (windowWidth <= 0 || windowHeight <= 0) continue;

        topPx = SceneTopUIHeightPxForWindow(window, windowHeight);
        widthPx = windowWidth;
        heightPx = windowHeight - topPx;
        return heightPx > 0;
    }

    return false;
}

// True when client-space x falls inside the right icon bar / properties pane overlay of the window
// hosting this tab. Backup guard for scene interaction (see propertiesPane.md §6); the WndProc side
// is the primary guard, this covers events already queued when the pane opens.
static bool IsOverRightOverlay(const DATASETTAB& tab, int x) {
    // Extracted view windows have no right overlay: input targeting one is never over it.
    const int inputSlot = InputViewSlot(tab);
    if (inputSlot >= 0 &&
        tab.subTabHostWindowSlots[inputSlot].load(std::memory_order_acquire) >= 0) {
        return false;
    }

    uint16_t* windowList = publishedWindowIndexes.load(std::memory_order_acquire);
    const uint16_t windowCount = publishedWindowCount.load(std::memory_order_acquire);
    for (uint16_t i = 0; i < windowCount; ++i) {
        const SingleUIWindow& window = allWindows[windowList[i]];
        if (window.windowKind != WINDOW_KIND_TABHOST) continue;
        if (window.activeTabIndex != static_cast<int>(tab.tabID)) continue;

        const int windowWidth = window.dx.WindowWidth > 0 ? window.dx.WindowWidth : window.currentWidth;
        const uint32_t overlayWidth = window.rightOverlayWidthPx.load(std::memory_order_acquire);
        if (overlayWidth == 0 || windowWidth <= 0) return false;
        return x >= windowWidth - static_cast<int>(overlayWidth);
    }
    return false;
}

/*Each tab will be hosted in exactly 1 windows.
However some of the views of the tab can be extracted to other windows.
Each tab gets its own engineering thread, capable of doing background processing, receiving network data, file I/O etc.
However engineering threads do not directly talk to GPU. They submit the screen visible changes to the GPU Copy thread.
More importantly, engineering thread are responsible for maintaining data consistency,
tracking which objects are visible in which views, what are the dirty objects to be cleaned up from GPU memory etc.
*/

// Latter move this to विश्वकर्मा.h
//Remember these global codes outside any function run even before main() starts.
std::random_device rd; //Universal random number generator seed. Non-Deterministic. Obtained from OS.
std::mt19937 gen(rd()); //rd(): Calls the device we made above to get a single random number.
//std::mt19937: A specific algorithm famous for being very fast and having high statistical quality.
// Period of 2^{19937}-1. All subsequent random numbers are generated from this seeded mt19937 object.

static void CopyAsciiName(char* target, size_t capacity, const char* value) {
    if (!target || capacity == 0) return;
    std::memset(target, 0, capacity);
    if (!value) return;
    const size_t copyLength = (std::min)(std::strlen(value), capacity - 1);
    if (copyLength > 0) std::memcpy(target, value, copyLength);
}

static void InitializeLogicalMeta(META_DATA* object, VishwakarmaStorage::ObjectType objectType,
    uint64_t parentMemoryId) {
    if (!object) return;
    object->dataType = static_cast<uint16_t>(VishwakarmaStorage::ToNumber(objectType));
    object->schemaVersion = VishwakarmaStorage::kLogicalElementSchemaVersion;
    object->memoryIDParent = parentMemoryId;
}

static uint32_t CountLogicalObjectsOfType(DATASETTAB* targetTab, VishwakarmaStorage::ObjectType objectType) {
    if (!targetTab || !targetTab->storageObjectsMutex) return 0;

    std::lock_guard<std::mutex> lock(*targetTab->storageObjectsMutex);
    uint32_t count = 0;
    for (const StoredLogicalObject& entry : targetTab->storageLogicalObjects) {
        if (entry.objectType == objectType) ++count;
    }
    return count;
}

static void ExpandDataTreeNode(DATASETTAB* targetTab, uint64_t memoryId) {
    if (!targetTab || memoryId == 0) return;
    if (!targetTab->storageObjectsMutex) targetTab->storageObjectsMutex = std::make_unique<std::mutex>();

    std::lock_guard<std::mutex> lock(*targetTab->storageObjectsMutex);
    auto& expanded = targetTab->expandedDataTreeNodeIds;
    if (std::find(expanded.begin(), expanded.end(), memoryId) == expanded.end()) {
        expanded.push_back(memoryId);
    }
}

static void ToggleDataTreeNode(DATASETTAB* targetTab, uint64_t memoryId) {
    if (!targetTab || memoryId == 0) return;
    if (!targetTab->storageObjectsMutex) targetTab->storageObjectsMutex = std::make_unique<std::mutex>();

    std::lock_guard<std::mutex> lock(*targetTab->storageObjectsMutex);
    auto& expanded = targetTab->expandedDataTreeNodeIds;
    auto it = std::find(expanded.begin(), expanded.end(), memoryId);
    if (it == expanded.end()) {
        expanded.push_back(memoryId);
    } else {
        expanded.erase(it);
    }
}

static void SetActiveDataTreeBranch(DATASETTAB* targetTab, uint64_t memoryId) {
    if (!targetTab || memoryId == 0) return;
    if (!targetTab->storageObjectsMutex) targetTab->storageObjectsMutex = std::make_unique<std::mutex>();

    std::lock_guard<std::mutex> lock(*targetTab->storageObjectsMutex);
    for (const StoredLogicalObject& entry : targetTab->storageLogicalObjects) {
        if (entry.objectType == VishwakarmaStorage::ObjectType::Scene3D &&
            entry.object && entry.object->memoryID == memoryId) {
            targetTab->activeScene3DMemoryId = memoryId;
            return;
        }
    }
}

static const char* LogicalObjectNameForInternalSubTab(
    VishwakarmaStorage::ObjectType objectType, const META_DATA* object) {
    if (!object) return "";

    switch (objectType) {
    case VishwakarmaStorage::ObjectType::Page2D:
        return static_cast<const PAGE2D*>(object)->name;
    case VishwakarmaStorage::ObjectType::Scene3D:
        return static_cast<const SCENE3D*>(object)->name;
    default:
        return "";
    }
}

int FindPublishedSubTabSlot(const DATASETTAB& tab, uint64_t containerMemoryId) {
    if (containerMemoryId == 0) return -1;
    uint16_t* list = tab.publishedSubTabIndexes.load(std::memory_order_acquire);
    const uint16_t count = tab.publishedSubTabCount.load(std::memory_order_acquire);
    if (!list) return -1;
    for (uint16_t i = 0; i < count; ++i) {
        if (tab.subTabs[list[i]].containerMemoryId == containerMemoryId) return list[i];
    }
    return -1;
}

int InputViewSlot(const DATASETTAB& tab) {
    const int32_t forced = tab.inputViewSubTabSlot.load(std::memory_order_acquire);
    if (forced >= 0 && forced < MV_MAX_SUBTABS &&
        tab.subTabStates[forced].load(std::memory_order_acquire) == SUBTAB_OPEN) {
        return forced;
    }
    // Unlocked read of activeInternalSubTabMemoryId is benign: worst case one stale resolution.
    return FindPublishedSubTabSlot(tab, tab.activeInternalSubTabMemoryId);
}

uint64_t InputViewContainerId(const DATASETTAB& tab) {
    const int slot = InputViewSlot(tab);
    return slot >= 0 ? tab.subTabs[slot].containerMemoryId : 0;
}

// Camera the engineering thread's scene input math applies to: the input view's per-view camera
// when it is a Scene3D, else the tab-level fallback camera (content shown without any sub-tab).
static CameraState& ActiveSceneCamera(DATASETTAB& tab) {
    const int slot = InputViewSlot(tab);
    if (slot >= 0 && tab.subTabs[slot].containerType == VishwakarmaStorage::ObjectType::Scene3D) {
        return tab.subTabs[slot].camera;
    }
    return tab.camera;
}

// Swaps to the other double-buffered index list and publishes it atomically.
static void PublishSubTabList(DATASETTAB& tab, const uint16_t* entries, uint16_t count) {
    uint16_t* currentList = tab.publishedSubTabIndexes.load(std::memory_order_acquire);
    uint16_t* nextList = (currentList == tab.subTabIndexesA) ? tab.subTabIndexesB : tab.subTabIndexesA;
    for (uint16_t i = 0; i < count; ++i) nextList[i] = entries[i];
    tab.publishedSubTabIndexes.store(nextList, std::memory_order_release);
    tab.publishedSubTabCount.store(count, std::memory_order_release);
}

// Marks a slot for delayed release. Frames submitted up to the recorded fence may still reference
// this view's GPU assets; CleanupReleasedSubTabs frees the slot once every monitor passed it.
static void RetireSubTabSlot(DATASETTAB& tab, uint16_t slot) {
    tab.subTabReleaseFenceValues[slot] = gpu.renderFenceValue.load(std::memory_order_acquire);
    tab.subTabStates[slot].store(SUBTAB_PENDING_GPU_RELEASE, std::memory_order_release);
    const int16_t hostWindow = tab.subTabHostWindowSlots[slot].load(std::memory_order_acquire);
    if (hostWindow >= 0) {
        // The view lives in its own extracted window; ask the UI thread to close that window.
        PushUIAction(kCloseViewWindowUIAction, tab.tabID, slot);
    }
}

void CloseAllInternalSubTabsLocked(DATASETTAB& tab) {
    uint16_t* list = tab.publishedSubTabIndexes.load(std::memory_order_acquire);
    const uint16_t count = tab.publishedSubTabCount.load(std::memory_order_acquire);
    PublishSubTabList(tab, nullptr, 0);
    for (uint16_t i = 0; list && i < count; ++i) RetireSubTabSlot(tab, list[i]);
    tab.activeInternalSubTabMemoryId = 0;
}

// Delayed slot release: PENDING_GPU_RELEASE -> FREE once every monitor's render fence passed the
// value recorded at close time. Idle monitors that never reached that value count as drained once
// they completed everything they actually submitted.
static bool AllMonitorRenderFencesPassed(uint64_t fenceValue) {
    for (int i = 0; i < gpu.currentMonitorCount; ++i) {
        OneMonitorController& screen = gpu.screens[i];
        if (!screen.renderFence) continue;
        const uint64_t target = (std::min)(fenceValue, screen.renderFenceValue);
        if (screen.renderFence->GetCompletedValue() < target) return false;
    }
    return true;
}

static void CleanupReleasedSubTabs(DATASETTAB* targetTab) {
    if (!targetTab || !targetTab->storageObjectsMutex) return;
    for (uint16_t slot = 0; slot < MV_MAX_SUBTABS; ++slot) {
        if (targetTab->subTabStates[slot].load(std::memory_order_acquire) != SUBTAB_PENDING_GPU_RELEASE) continue;
        if (!AllMonitorRenderFencesPassed(targetTab->subTabReleaseFenceValues[slot])) continue;

        std::lock_guard<std::mutex> lock(*targetTab->storageObjectsMutex);
        targetTab->subTabs[slot] = InternalSubTab{}; // Release title string memory.
        targetTab->subTabHostWindowSlots[slot].store(-1, std::memory_order_release);
        targetTab->subTabStates[slot].store(SUBTAB_FREE, std::memory_order_release);
    }
}

static void OpenInternalSubTab(DATASETTAB* targetTab, uint64_t memoryId) {
    if (!targetTab || memoryId == 0) return;
    if (!targetTab->storageObjectsMutex) targetTab->storageObjectsMutex = std::make_unique<std::mutex>();

    std::lock_guard<std::mutex> lock(*targetTab->storageObjectsMutex);
    for (const StoredLogicalObject& entry : targetTab->storageLogicalObjects) {
        if (!entry.object || entry.object->memoryID != memoryId) continue;
        if (entry.objectType != VishwakarmaStorage::ObjectType::Scene3D &&
            entry.objectType != VishwakarmaStorage::ObjectType::Page2D) {
            return;
        }

        // A container (in particular Page2D) is only ever open in 1 sub-tab: reuse it if found.
        if (FindPublishedSubTabSlot(*targetTab, memoryId) < 0) {
            int freeSlot = -1;
            for (uint16_t slot = 0; slot < MV_MAX_SUBTABS; ++slot) {
                if (targetTab->subTabStates[slot].load(std::memory_order_acquire) == SUBTAB_FREE) {
                    freeSlot = slot;
                    break;
                }
            }
            if (freeSlot < 0) return; // All MV_MAX_SUBTABS slots occupied (open or draining).

            const char* objectName = LogicalObjectNameForInternalSubTab(entry.objectType, entry.object);
            InternalSubTab& subTab = targetTab->subTabs[freeSlot];
            subTab.containerType = entry.objectType;
            subTab.containerMemoryId = memoryId;
            subTab.title = objectName && objectName[0] != '\0'
                ? objectName
                : VishwakarmaStorage::ObjectTypeDisplayName(entry.objectType);
            subTab.camera.Initialize(); // Fresh per-view 3D camera for this slot.
            if (targetTab->cad2d) {
                targetTab->cad2d->views[freeSlot].Reset(); // Fresh per-view Page2D pan/zoom.
            }
            targetTab->subTabHostWindowSlots[freeSlot].store(-1, std::memory_order_release);
            targetTab->subTabStates[freeSlot].store(SUBTAB_OPEN, std::memory_order_release);

            uint16_t* currentList = targetTab->publishedSubTabIndexes.load(std::memory_order_acquire);
            const uint16_t count = targetTab->publishedSubTabCount.load(std::memory_order_acquire);
            uint16_t entries[MV_MAX_SUBTABS];
            for (uint16_t i = 0; i < count; ++i) entries[i] = currentList[i];
            entries[count] = static_cast<uint16_t>(freeSlot);
            PublishSubTabList(*targetTab, entries, count + 1);
        }

        targetTab->activeInternalSubTabMemoryId = memoryId;
        if (entry.objectType == VishwakarmaStorage::ObjectType::Scene3D) {
            targetTab->activeScene3DMemoryId = memoryId;
        }
        return;
    }
}

static void ActivateInternalSubTab(DATASETTAB* targetTab, uint64_t memoryId) {
    if (!targetTab || memoryId == 0 || !targetTab->storageObjectsMutex) return;

    std::lock_guard<std::mutex> lock(*targetTab->storageObjectsMutex);
    const int slot = FindPublishedSubTabSlot(*targetTab, memoryId);
    if (slot < 0) return;

    targetTab->activeInternalSubTabMemoryId = memoryId;
    if (targetTab->subTabs[slot].containerType == VishwakarmaStorage::ObjectType::Scene3D) {
        targetTab->activeScene3DMemoryId = memoryId;
    }
}

static void CloseInternalSubTab(DATASETTAB* targetTab, uint64_t memoryId) {
    if (!targetTab || memoryId == 0 || !targetTab->storageObjectsMutex) return;

    std::lock_guard<std::mutex> lock(*targetTab->storageObjectsMutex);
    uint16_t* currentList = targetTab->publishedSubTabIndexes.load(std::memory_order_acquire);
    const uint16_t count = targetTab->publishedSubTabCount.load(std::memory_order_acquire);
    if (!currentList) return;

    uint16_t entries[MV_MAX_SUBTABS];
    uint16_t nextCount = 0;
    int closedPosition = -1;
    uint16_t closedSlot = 0;
    for (uint16_t i = 0; i < count; ++i) {
        if (targetTab->subTabs[currentList[i]].containerMemoryId == memoryId) {
            closedPosition = i;
            closedSlot = currentList[i];
            continue;
        }
        entries[nextCount++] = currentList[i];
    }
    if (closedPosition < 0) return;

    const bool closedActive = targetTab->activeInternalSubTabMemoryId == memoryId;
    PublishSubTabList(*targetTab, entries, nextCount);
    RetireSubTabSlot(*targetTab, closedSlot);

    if (!closedActive) return;
    if (nextCount == 0) {
        targetTab->activeInternalSubTabMemoryId = 0;
        return;
    }

    const uint16_t replacementSlot = entries[(std::min)(
        static_cast<uint16_t>(closedPosition), static_cast<uint16_t>(nextCount - 1))];
    const InternalSubTab& replacement = targetTab->subTabs[replacementSlot];
    targetTab->activeInternalSubTabMemoryId = replacement.containerMemoryId;
    if (replacement.containerType == VishwakarmaStorage::ObjectType::Scene3D) {
        targetTab->activeScene3DMemoryId = replacement.containerMemoryId;
    }
}

// An extracted view no longer renders inline: when it was the inline-active sub-tab, hand the
// inline band over to the first still-inline open sub-tab (or none).
static void HandleSubTabExtracted(DATASETTAB* targetTab, uint64_t memoryId) {
    if (!targetTab || memoryId == 0 || !targetTab->storageObjectsMutex) return;

    std::lock_guard<std::mutex> lock(*targetTab->storageObjectsMutex);
    if (targetTab->activeInternalSubTabMemoryId != memoryId) return;

    targetTab->activeInternalSubTabMemoryId = 0;
    uint16_t* list = targetTab->publishedSubTabIndexes.load(std::memory_order_acquire);
    const uint16_t count = targetTab->publishedSubTabCount.load(std::memory_order_acquire);
    for (uint16_t i = 0; list && i < count; ++i) {
        const InternalSubTab& candidate = targetTab->subTabs[list[i]];
        if (candidate.containerMemoryId == memoryId) continue;
        if (targetTab->subTabHostWindowSlots[list[i]].load(std::memory_order_acquire) >= 0) continue;
        targetTab->activeInternalSubTabMemoryId = candidate.containerMemoryId;
        if (candidate.containerType == VishwakarmaStorage::ObjectType::Scene3D) {
            targetTab->activeScene3DMemoryId = candidate.containerMemoryId;
        }
        break;
    }
}

static META_DATA* CreateLogicalElement(DATASETTAB* targetTab, VishwakarmaStorage::ObjectType objectType,
    uint64_t parentMemoryId, const char* requestedName = nullptr) {
    if (!targetTab || !VishwakarmaStorage::IsLogicalObjectType(objectType)) return nullptr;

    const uint32_t sequence = CountLogicalObjectsOfType(targetTab, objectType) + 1;
    std::string generatedName = VishwakarmaStorage::ObjectTypeDisplayName(objectType);
    generatedName += " ";
    generatedName += std::to_string(sequence);
    const char* name = requestedName ? requestedName : generatedName.c_str();

    META_DATA* object = nullptr;
    switch (objectType) {
    case VishwakarmaStorage::ObjectType::Folder: {
        FOLDER* folder = new (targetTab->tabNo) FOLDER();
        CopyAsciiName(folder->name, sizeof(folder->name), name);
        CopyAsciiName(folder->shortCode, sizeof(folder->shortCode), "F");
        object = folder;
        break;
    }
    case VishwakarmaStorage::ObjectType::Page2D: {
        PAGE2D* page = new (targetTab->tabNo) PAGE2D();
        CopyAsciiName(page->name, sizeof(page->name), name);
        object = page;
        break;
    }
    case VishwakarmaStorage::ObjectType::Scene3D: {
        SCENE3D* scene = new (targetTab->tabNo) SCENE3D();
        CopyAsciiName(scene->name, sizeof(scene->name), name);
        object = scene;
        break;
    }
    default:
        return nullptr;
    }

    InitializeLogicalMeta(object, objectType, parentMemoryId);

    if (!targetTab->storageObjectsMutex) targetTab->storageObjectsMutex = std::make_unique<std::mutex>();
    {
        std::lock_guard<std::mutex> lock(*targetTab->storageObjectsMutex);
        targetTab->storageLogicalObjects.push_back({ objectType, object->memoryID, object });
        if (objectType == VishwakarmaStorage::ObjectType::Scene3D) {
            if (targetTab->defaultScene3DMemoryId == 0) {
                targetTab->defaultScene3DMemoryId = object->memoryID;
            }
            if (targetTab->activeScene3DMemoryId == 0) {
                targetTab->activeScene3DMemoryId = object->memoryID;
            }
        }
    }

    targetTab->allIDsInThisTab.push_back(object->memoryID);
    if (objectType == VishwakarmaStorage::ObjectType::Scene3D) {
        ExpandDataTreeNode(targetTab, object->memoryID);
    }
    return object;
}

static uint64_t FindActiveScene3D(DATASETTAB* targetTab) {
    if (!targetTab) return 0;
    if (!targetTab->storageObjectsMutex) targetTab->storageObjectsMutex = std::make_unique<std::mutex>();

    std::lock_guard<std::mutex> lock(*targetTab->storageObjectsMutex);
    for (const StoredLogicalObject& entry : targetTab->storageLogicalObjects) {
        if (entry.objectType == VishwakarmaStorage::ObjectType::Scene3D && entry.object &&
            entry.object->memoryID == targetTab->activeScene3DMemoryId) {
            return targetTab->activeScene3DMemoryId;
        }
    }

    for (const StoredLogicalObject& entry : targetTab->storageLogicalObjects) {
        if (entry.objectType == VishwakarmaStorage::ObjectType::Scene3D && entry.object) {
            if (targetTab->defaultScene3DMemoryId == 0) {
                targetTab->defaultScene3DMemoryId = entry.object->memoryID;
            }
            targetTab->activeScene3DMemoryId = entry.object->memoryID;
            auto& expanded = targetTab->expandedDataTreeNodeIds;
            if (std::find(expanded.begin(), expanded.end(), entry.object->memoryID) == expanded.end()) {
                expanded.push_back(entry.object->memoryID);
            }
            return entry.object->memoryID;
        }
    }
    return 0;
}

static uint64_t FindFirstLogicalObject(DATASETTAB* targetTab, VishwakarmaStorage::ObjectType objectType) {
    if (!targetTab || !targetTab->storageObjectsMutex) return 0;

    std::lock_guard<std::mutex> lock(*targetTab->storageObjectsMutex);
    for (const StoredLogicalObject& entry : targetTab->storageLogicalObjects) {
        if (entry.objectType == objectType && entry.object) {
            return entry.object->memoryID;
        }
    }
    return 0;
}

static void OpenInitialLogicalContainerSubTabs(DATASETTAB* targetTab) {
    if (!targetTab) return;

    const uint64_t page2DMemoryId =
        FindFirstLogicalObject(targetTab, VishwakarmaStorage::ObjectType::Page2D);
    const uint64_t scene3DMemoryId =
        FindFirstLogicalObject(targetTab, VishwakarmaStorage::ObjectType::Scene3D);

    if (page2DMemoryId != 0) {
        SetActiveDataTreeBranch(targetTab, page2DMemoryId);
        OpenInternalSubTab(targetTab, page2DMemoryId);
    }
    if (scene3DMemoryId != 0) {
        SetActiveDataTreeBranch(targetTab, scene3DMemoryId);
        OpenInternalSubTab(targetTab, scene3DMemoryId);
    }
}

static void EnsureDefaultLogicalHierarchy(DATASETTAB* targetTab) {
    if (!targetTab) return;
    if (!targetTab->storageObjectsMutex) targetTab->storageObjectsMutex = std::make_unique<std::mutex>();

    bool hasAnyLogicalObject = false;
    bool hasScene3D = false;
    {
        std::lock_guard<std::mutex> lock(*targetTab->storageObjectsMutex);
        hasAnyLogicalObject = !targetTab->storageLogicalObjects.empty();
        for (const StoredLogicalObject& entry : targetTab->storageLogicalObjects) {
            if (entry.objectType == VishwakarmaStorage::ObjectType::Scene3D) {
                hasScene3D = true;
                if (targetTab->defaultScene3DMemoryId == 0 && entry.object) {
                    targetTab->defaultScene3DMemoryId = entry.object->memoryID;
                }
                if (targetTab->activeScene3DMemoryId == 0 && entry.object) {
                    targetTab->activeScene3DMemoryId = entry.object->memoryID;
                }
                break;
            }
        }
    }

    if (!hasAnyLogicalObject) {
        CreateLogicalElement(targetTab, VishwakarmaStorage::ObjectType::Scene3D, 0, "Scene3D");
        CreateLogicalElement(targetTab, VishwakarmaStorage::ObjectType::Page2D, 0, "Page2D");
        CreateLogicalElement(targetTab, VishwakarmaStorage::ObjectType::Folder, 0, "Folder");
    } else if (!hasScene3D) {
        CreateLogicalElement(targetTab, VishwakarmaStorage::ObjectType::Scene3D, 0, "Scene3D");
    }
}

static uint64_t EnsureActiveScene3D(DATASETTAB* targetTab) {
    uint64_t sceneMemoryId = FindActiveScene3D(targetTab);
    if (sceneMemoryId != 0) return sceneMemoryId;

    SCENE3D* scene = static_cast<SCENE3D*>(
        CreateLogicalElement(targetTab, VishwakarmaStorage::ObjectType::Scene3D, 0, "Scene3D"));
    return scene ? scene->memoryID : 0;
}

static void RegisterGeneratedGeometryElement(DATASETTAB* targetTab, VishwakarmaStorage::ObjectType objectType,
    META_DATA* object, GeometryData&& geometry) {
    if (!targetTab || !object) return;

    if (object->memoryIDParent == 0) {
        object->memoryIDParent = EnsureActiveScene3D(targetTab);
    }
    object->dataType = static_cast<uint16_t>(VishwakarmaStorage::ToNumber(objectType));
    object->schemaVersion = VishwakarmaStorage::kGeometry3DMvpSchemaVersion;

    {
        std::lock_guard<std::mutex> lock(toCopyThreadMutex);
        commandToCopyThreadQueue.push({ CommandToCopyThreadType::ADD, geometry, object->memoryID,
            targetTab->tabID, object->memoryIDParent });
    }

    if (!targetTab->storageObjectsMutex) targetTab->storageObjectsMutex = std::make_unique<std::mutex>();
    {
        std::lock_guard<std::mutex> lock(*targetTab->storageObjectsMutex);
        targetTab->storageObjects3D.push_back({ objectType, object->memoryID, object });
    }

    targetTab->allIDsInThisTab.push_back(object->memoryID);
    toCopyThreadCV.notify_one();
}

// Applies one committed property edit: validate against live values (authoritative gate), store the
// field, bump dataVersion, regenerate geometry and push a MODIFY to the copy thread. The two mutexes
// are taken strictly one after the other, never nested (matching AppendObjectToTab /
// RegisterGeneratedGeometryElement). See propertiesPane.md §5.
static void ModifyObjectProperty(DATASETTAB* myTab, uint64_t objectId, uint8_t fieldIndex, double value) {
    if (!myTab || !myTab->storageObjectsMutex) return;

    // The engineering thread is the sole writer of storageObjects3D, so the lookup needs no lock.
    META_DATA* object = nullptr;
    VishwakarmaStorage::ObjectType objectType = VishwakarmaStorage::ObjectType::Unknown;
    for (const StoredGeometryObject3D& stored : myTab->storageObjects3D) {
        if (stored.memoryId == objectId) {
            object = stored.object;
            objectType = stored.objectType;
            break;
        }
    }
    if (!object) return;

    const PropertyTypeDescriptor* table = FindPropertyTable(objectType);
    if (!table || fieldIndex >= table->fieldCount) return;

    // Re-run the MVP validator against live values. The UI pre-validated, so this only fires on
    // races or bugs; on rejection we simply drop the commit.
    float values[16] = {};
    const uint8_t count = table->fieldCount;
    for (uint8_t i = 0; i < count; ++i) values[i] = table->fields[i].get(object);
    const float newValue = static_cast<float>(value);
    if (!ValidatePropertyEdit(*table, values, count, fieldIndex, newValue)) return;

    {
        // Hold storageObjectsMutex only for the store, so the render thread (which takes it every
        // frame) is not stalled by geometry generation.
        std::lock_guard<std::mutex> lock(*myTab->storageObjectsMutex);
        table->fields[fieldIndex].set(object, newValue);
        object->dataVersion++;
    }

    GeometryData geo; // Regenerate with no lock held.
    if (!GeometryForObject(objectType, object, geo)) return;

    {
        std::lock_guard<std::mutex> lock(toCopyThreadMutex);
        commandToCopyThreadQueue.push({ CommandToCopyThreadType::MODIFY, std::move(geo), object->memoryID,
            myTab->tabID, object->memoryIDParent });
    }
    toCopyThreadCV.notify_one();
}

static XMFLOAT3 AddPoint(const XMFLOAT3& a, const XMFLOAT3& b) {
    return { a.x + b.x, a.y + b.y, a.z + b.z };
}

static XMFLOAT3 OffsetTo(const XMFLOAT3& from, const XMFLOAT3& to) {
    return { to.x - from.x, to.y - from.y, to.z - from.z };
}

static XMFLOAT3 MidPoint(const XMFLOAT3& a, const XMFLOAT3& b) {
    return { (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f };
}

static XMFLOAT3 AveragePoint(const std::vector<XMFLOAT3>& points) {
    if (points.empty()) return {};

    XMFLOAT3 sum{};
    for (const XMFLOAT3& point : points) {
        sum = AddPoint(sum, point);
    }
    const float scale = 1.0f / static_cast<float>(points.size());
    return { sum.x * scale, sum.y * scale, sum.z * scale };
}

static void TranslatePoint(XMFLOAT3& point, const XMFLOAT3& offset) {
    point.x += offset.x;
    point.y += offset.y;
    point.z += offset.z;
}

static void TranslatePoints(std::vector<XMFLOAT3>& points, const XMFLOAT3& offset) {
    for (XMFLOAT3& point : points) {
        TranslatePoint(point, offset);
    }
}

static bool Scene3DPlacementPointFromInput(DATASETTAB& tab, const ACTION_DETAILS& input, XMFLOAT3& outPoint) {
    int viewportWidth = 0, viewportHeight = 0, viewportTop = 0;
    if (!GetVisibleSceneViewportForTab(tab, viewportWidth, viewportHeight, viewportTop)) {
        return false;
    }
    if (input.x < 0 || input.x >= viewportWidth ||
        input.y < viewportTop || input.y >= viewportTop + viewportHeight) {
        return false;
    }

    const float mouseX = std::clamp(static_cast<float>(input.x), 0.0f, static_cast<float>(viewportWidth));
    const float mouseY = std::clamp(static_cast<float>(input.y - viewportTop), 0.0f, static_cast<float>(viewportHeight));
    const float ndcX = mouseX / static_cast<float>(viewportWidth) * 2.0f - 1.0f;
    const float ndcY = 1.0f - mouseY / static_cast<float>(viewportHeight) * 2.0f;
    const float aspect = static_cast<float>(viewportWidth) / static_cast<float>(viewportHeight);
    const CameraState& cam = ActiveSceneCamera(tab);
    const float tanHalfFov = std::tan(cam.fov * 0.5f);

    DirectX::XMVECTOR eye = DirectX::XMLoadFloat3(&cam.position);
    DirectX::XMVECTOR target = DirectX::XMLoadFloat3(&cam.target);
    DirectX::XMVECTOR worldUp = DirectX::XMLoadFloat3(&cam.up);
    DirectX::XMVECTOR forward = DirectX::XMVector3Normalize(DirectX::XMVectorSubtract(target, eye));
    DirectX::XMVECTOR right = DirectX::XMVector3Cross(worldUp, forward);
    if (DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(right)) <= 0.000001f) {
        return false;
    }
    right = DirectX::XMVector3Normalize(right);
    DirectX::XMVECTOR viewUp = DirectX::XMVector3Cross(forward, right);
    DirectX::XMVECTOR ray = DirectX::XMVectorAdd(forward,
        DirectX::XMVectorAdd(
            DirectX::XMVectorScale(right, ndcX * tanHalfFov * aspect),
            DirectX::XMVectorScale(viewUp, ndcY * tanHalfFov)));
    ray = DirectX::XMVector3Normalize(ray);

    const float distance = DirectX::XMVectorGetX(DirectX::XMVector3Length(DirectX::XMVectorSubtract(target, eye)));
    const float denom = DirectX::XMVectorGetX(DirectX::XMVector3Dot(ray, forward));
    if (distance <= 0.0001f || std::abs(denom) <= 0.000001f) {
        return false;
    }

    DirectX::XMStoreFloat3(&outPoint, DirectX::XMVectorAdd(eye, DirectX::XMVectorScale(ray, distance / denom)));
    return true;
}

// --- 3D click-selection glue (see website/content/software/selection.md) ------------------------
// Ask the render thread to run a GPU pick at the given client pixel. The result arrives ~1 frame
// later via SelectionState and is applied by ApplyPickResult below.
static void RequestScenePick(DATASETTAB& tab, int clientX, int clientY, PickPurpose purpose) {
    tab.selection.pickX.store(clientX, std::memory_order_relaxed);
    tab.selection.pickY.store(clientY, std::memory_order_relaxed);
    tab.selection.pickPurpose.store(static_cast<uint32_t>(purpose), std::memory_order_relaxed);
    tab.selection.pickRequested.store(true, std::memory_order_release);
}

// Recenter the orbit/look target on a world point, preserving view direction and distance (the
// image translates so the point glides to the orbit center; no rotation). Used for both the
// selected object's CG and the scrolled-to surface point.
static void CenterCameraOnPoint(CameraState& cam, const DirectX::XMFLOAT3& p) {
    DirectX::XMVECTOR eye = DirectX::XMLoadFloat3(&cam.position);
    DirectX::XMVECTOR target = DirectX::XMLoadFloat3(&cam.target);
    DirectX::XMVECTOR offset = DirectX::XMVectorSubtract(eye, target); // Preserve direction+distance.
    DirectX::XMVECTOR newTarget = DirectX::XMLoadFloat3(&p);
    DirectX::XMStoreFloat3(&cam.target, newTarget);
    DirectX::XMStoreFloat3(&cam.position, DirectX::XMVectorAdd(newTarget, offset));
}

static void ApplyPickResult(DATASETTAB& tab, bool hit, uint64_t objectId,
    const DirectX::XMFLOAT3& cg, const DirectX::XMFLOAT3& surface, uint32_t purposeRaw) {
    const PickPurpose purpose = static_cast<PickPurpose>(purposeRaw);
    CameraState& cam = ActiveSceneCamera(tab);
    if (purpose == PickPurpose::Select) {
        {
            std::lock_guard<std::mutex> lock(tab.selection.selectedMutex);
            tab.selection.selectedObjectIds.clear();
            if (objectId != 0) tab.selection.selectedObjectIds.push_back(objectId); // Single-select.
        }
        if (objectId != 0) CenterCameraOnPoint(cam, cg); // Focus on the selected object's CG.
    } else if (purpose == PickPurpose::Recenter && hit) {
        // Only recenter when the surface is meaningfully off the current pivot, so a stationary
        // cursor doesn't jitter the view every scroll notch. Tunable UX; see selection.md.
        DirectX::XMVECTOR target = DirectX::XMLoadFloat3(&cam.target);
        DirectX::XMVECTOR eye = DirectX::XMLoadFloat3(&cam.position);
        DirectX::XMVECTOR surf = DirectX::XMLoadFloat3(&surface);
        const float dist = DirectX::XMVectorGetX(
            DirectX::XMVector3Length(DirectX::XMVectorSubtract(eye, target)));
        const float moved = DirectX::XMVectorGetX(
            DirectX::XMVector3Length(DirectX::XMVectorSubtract(surf, target)));
        if (moved > 0.05f * (std::max)(dist, 1.0f)) CenterCameraOnPoint(cam, surface);
    }
}

// Zoom Max / Zoom Focus for the 3D scene: dolly the camera along its existing view direction so
// the objects fit the visible frustum. The look target and view direction stay fixed; only the
// distance between the camera and its target/projection plane changes. selectedOnly limits the
// fit to the current selection (falls back to all objects when nothing is selected).
static void ZoomSceneToExtents(DATASETTAB* myTab, bool selectedOnly) {
    if (!myTab) return;

    std::vector<uint64_t> selected;
    if (selectedOnly) {
        std::lock_guard<std::mutex> lock(myTab->selection.selectedMutex);
        selected = myTab->selection.selectedObjectIds;
    }
    const bool filterBySelection = selectedOnly && !selected.empty(); // Empty selection = fit all.

    CameraState& cam = ActiveSceneCamera(*myTab);
    DirectX::XMVECTOR target = DirectX::XMLoadFloat3(&cam.target);
    DirectX::XMVECTOR eye = DirectX::XMLoadFloat3(&cam.position);
    DirectX::XMVECTOR worldUp = DirectX::XMLoadFloat3(&cam.up);
    DirectX::XMVECTOR forward = DirectX::XMVectorSubtract(target, eye);
    if (DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(forward)) <= 0.000001f) return;
    forward = DirectX::XMVector3Normalize(forward);
    DirectX::XMVECTOR right = DirectX::XMVector3Cross(worldUp, forward);
    if (DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(right)) <= 0.000001f) return;
    right = DirectX::XMVector3Normalize(right);
    DirectX::XMVECTOR viewUp = DirectX::XMVector3Cross(forward, right);

    float aspect = cam.aspect;
    int viewportWidth = 0, viewportHeight = 0, viewportTop = 0;
    if (GetVisibleSceneViewportForTab(*myTab, viewportWidth, viewportHeight, viewportTop) &&
        viewportWidth > 0 && viewportHeight > 0) {
        aspect = static_cast<float>(viewportWidth) / static_cast<float>(viewportHeight);
    }
    const float margin = 0.95f; // Keep a small breathing border around the extents.
    const float tanHalfFovY = std::tan(cam.fov * 0.5f) * margin;
    const float tanHalfFovX = tanHalfFovY * aspect;
    if (tanHalfFovY <= 0.0001f || tanHalfFovX <= 0.0001f) return;

    // The engineering thread is the sole writer of storageObjects3D, so iteration needs no lock.
    float requiredDistance = 0.0f;
    bool hasPoints = false;
    GeometryData geometry;
    for (const StoredGeometryObject3D& stored : myTab->storageObjects3D) {
        if (!stored.object) continue;
        if (filterBySelection &&
            std::find(selected.begin(), selected.end(), stored.memoryId) == selected.end()) continue;
        if (!GeometryForObject(stored.objectType, stored.object, geometry)) continue;
        for (const Vertex& vertex : geometry.vertices) {
            DirectX::XMVECTOR offset = DirectX::XMVectorSubtract(
                DirectX::XMLoadFloat3(&vertex.position), target);
            const float alongForward = DirectX::XMVectorGetX(DirectX::XMVector3Dot(offset, forward));
            const float alongRight = DirectX::XMVectorGetX(DirectX::XMVector3Dot(offset, right));
            const float alongUp = DirectX::XMVectorGetX(DirectX::XMVector3Dot(offset, viewUp));
            // With the camera at distance d behind the target, the point's depth is d + alongForward
            // and it is inside the frustum when |lateral| <= depth * tanHalfFov. Solve for minimum d.
            requiredDistance = (std::max)(requiredDistance, std::abs(alongRight) / tanHalfFovX - alongForward);
            requiredDistance = (std::max)(requiredDistance, std::abs(alongUp) / tanHalfFovY - alongForward);
            requiredDistance = (std::max)(requiredDistance, cam.nearZ - alongForward);
            hasPoints = true;
        }
    }
    if (!hasPoints) return;

    // Same distance clamping as the mouse-wheel zoom.
    const float newDistance = std::clamp(requiredDistance, 1.0f, cam.farZ - 10.0f);
    DirectX::XMStoreFloat3(&cam.position,
        DirectX::XMVectorSubtract(target, DirectX::XMVectorScale(forward, newDistance)));
    myTab->selection.lastNavInteractionMs.store(GetTickCount64(), std::memory_order_release);
}

// Zoom Window for the 3D scene: the two clicked pixels define a rectangle on the screen. The view
// direction stays fixed; the look target glides to the rectangle center on the focal plane and the
// camera dollies in so the rectangle fills the viewport.
static void ZoomSceneToWindow(DATASETTAB& tab, int x0, int y0, int x1, int y1) {
    int viewportWidth = 0, viewportHeight = 0, viewportTop = 0;
    if (!GetVisibleSceneViewportForTab(tab, viewportWidth, viewportHeight, viewportTop)) return;
    if (viewportWidth <= 0 || viewportHeight <= 0) return;

    const float rectWidth = static_cast<float>(std::abs(x1 - x0));
    const float rectHeight = static_cast<float>(std::abs(y1 - y0));
    if (rectWidth < 3.0f && rectHeight < 3.0f) return; // Degenerate window; ignore.

    ACTION_DETAILS centerPixel{};
    centerPixel.x = (x0 + x1) / 2;
    centerPixel.y = (y0 + y1) / 2;
    XMFLOAT3 focus{};
    if (!Scene3DPlacementPointFromInput(tab, centerPixel, focus)) return;

    CameraState& cam = ActiveSceneCamera(tab);
    DirectX::XMVECTOR eye = DirectX::XMLoadFloat3(&cam.position);
    DirectX::XMVECTOR target = DirectX::XMLoadFloat3(&cam.target);
    DirectX::XMVECTOR forward = DirectX::XMVectorSubtract(target, eye);
    const float distance = DirectX::XMVectorGetX(DirectX::XMVector3Length(forward));
    if (distance <= 0.0001f) return;
    forward = DirectX::XMVector3Normalize(forward);

    // Dollying to scale * distance shrinks the visible focal-plane extents by the same factor, so
    // the larger rectangle/viewport ratio keeps the whole clicked window visible.
    const float scale = (std::max)(rectWidth / static_cast<float>(viewportWidth),
        rectHeight / static_cast<float>(viewportHeight));
    const float newDistance = std::clamp(distance * scale, 1.0f, cam.farZ - 10.0f);

    DirectX::XMVECTOR newTarget = DirectX::XMLoadFloat3(&focus);
    DirectX::XMStoreFloat3(&cam.target, newTarget);
    DirectX::XMStoreFloat3(&cam.position,
        DirectX::XMVectorSubtract(newTarget, DirectX::XMVectorScale(forward, newDistance)));
    tab.selection.lastNavInteractionMs.store(GetTickCount64(), std::memory_order_release);
}

static bool CreatePrimitiveGeometryElement(DATASETTAB* targetTab, VishwakarmaStorage::ObjectType objectType,
    const XMFLOAT3& placementPoint) {
    if (!targetTab || !VishwakarmaStorage::IsGeometry3DObjectType(objectType)) return false;

    GeometryData geometry;
    META_DATA* object = nullptr;

    switch (objectType) {
    case VishwakarmaStorage::ObjectType::Pyramid: {
        PYRAMID* shape = new (targetTab->tabNo) PYRAMID();
        shape->Randomize();
        TranslatePoints(shape->vertices, OffsetTo(AveragePoint(shape->vertices), placementPoint));
        geometry = shape->GetGeometry();
        object = shape;
        break;
    }
    case VishwakarmaStorage::ObjectType::Cuboid: {
        CUBOID* shape = new (targetTab->tabNo) CUBOID();
        shape->Randomize();
        TranslatePoints(shape->vertices, OffsetTo(AveragePoint(shape->vertices), placementPoint));
        geometry = shape->GetGeometry();
        object = shape;
        break;
    }
    case VishwakarmaStorage::ObjectType::Cone: {
        CONE* shape = new (targetTab->tabNo) CONE();
        shape->Randomize();
        const XMFLOAT3 offset = OffsetTo(MidPoint(shape->apex, shape->baseCenter), placementPoint);
        TranslatePoint(shape->apex, offset);
        TranslatePoint(shape->baseCenter, offset);
        geometry = shape->GetGeometry();
        object = shape;
        break;
    }
    case VishwakarmaStorage::ObjectType::Cylinder: {
        CYLINDER* shape = new (targetTab->tabNo) CYLINDER();
        shape->Randomize();
        const XMFLOAT3 offset = OffsetTo(MidPoint(shape->p1, shape->p2), placementPoint);
        TranslatePoint(shape->p1, offset);
        TranslatePoint(shape->p2, offset);
        geometry = shape->GetGeometry();
        object = shape;
        break;
    }
    case VishwakarmaStorage::ObjectType::Parallelepiped: {
        PARALLELEPIPED* shape = new (targetTab->tabNo) PARALLELEPIPED();
        shape->Randomize();
        TranslatePoints(shape->vertices, OffsetTo(AveragePoint(shape->vertices), placementPoint));
        geometry = shape->GetGeometry();
        object = shape;
        break;
    }
    case VishwakarmaStorage::ObjectType::Sphere: {
        SPHERE* shape = new (targetTab->tabNo) SPHERE();
        shape->Randomize();
        shape->center = placementPoint;
        geometry = shape->GetGeometry();
        object = shape;
        break;
    }
    case VishwakarmaStorage::ObjectType::FrustumOfPyramid: {
        FRUSTUM_OF_PYRAMID* shape = new (targetTab->tabNo) FRUSTUM_OF_PYRAMID();
        shape->Randomize();
        TranslatePoints(shape->vertices, OffsetTo(AveragePoint(shape->vertices), placementPoint));
        geometry = shape->GetGeometry();
        object = shape;
        break;
    }
    case VishwakarmaStorage::ObjectType::FrustumOfCone: {
        FRUSTUM_OF_CONE* shape = new (targetTab->tabNo) FRUSTUM_OF_CONE();
        shape->Randomize();
        const XMFLOAT3 offset = OffsetTo(MidPoint(shape->bottomCenter, shape->topCenter), placementPoint);
        TranslatePoint(shape->bottomCenter, offset);
        TranslatePoint(shape->topCenter, offset);
        geometry = shape->GetGeometry();
        object = shape;
        break;
    }
    case VishwakarmaStorage::ObjectType::Pipe: {
        PIPE* shape = new (targetTab->tabNo) PIPE();
        shape->Randomize();
        const XMFLOAT3 offset = OffsetTo(MidPoint(shape->center1, shape->center2), placementPoint);
        TranslatePoint(shape->center1, offset);
        TranslatePoint(shape->center2, offset);
        geometry = shape->GetGeometry();
        object = shape;
        break;
    }
    case VishwakarmaStorage::ObjectType::Torus: {
        TORUS* shape = new (targetTab->tabNo) TORUS();
        shape->Randomize();
        shape->center = placementPoint;
        geometry = shape->GetGeometry();
        object = shape;
        break;
    }
    case VishwakarmaStorage::ObjectType::Ellipsoid: {
        ELLIPSOID* shape = new (targetTab->tabNo) ELLIPSOID();
        shape->Randomize();
        shape->center = placementPoint;
        geometry = shape->GetGeometry();
        object = shape;
        break;
    }
    default:
        return false;
    }

    RegisterGeneratedGeometryElement(targetTab, objectType, object, std::move(geometry));
    return true;
}

// Materializes a validated STAAD import (nodes as spheres, members as pipes).
// Runs on the engineering thread — the only writer of model data. The IPC and
// validation live in ExtensionCommunications.cpp.
static void ImportStdFileIntoTab(DATASETTAB* myTab, uint64_t payloadId) {
    std::string error;
    std::unique_ptr<ExtensionCommunications::ImportedStructuralModel> model(
        ExtensionCommunications::RunQueuedStdImport(payloadId, error));
    if (!model) {
        std::cout << "[std-importer] " << error << "\n";
        MessageBoxA(nullptr, error.c_str(), "STAAD import failed", MB_OK | MB_ICONERROR);
        return;
    }

    const uint64_t sceneMemoryId = EnsureActiveScene3D(myTab);
    if (sceneMemoryId != 0) OpenInternalSubTab(myTab, sceneMemoryId);

    constexpr float kNodeRadius = 0.12f;            // Meters; import coordinates are SI.
    constexpr float kMemberOutsideDiameter = 0.25f; // Placeholder until profiles are mapped.
    constexpr float kMemberInsideDiameter = 0.10f;
    const XMHALF4 nodeColor(0.85f, 0.25f, 0.15f, 1.0f);
    const XMHALF4 memberColor(0.35f, 0.55f, 0.85f, 1.0f);

    std::unordered_map<uint32_t, XMFLOAT3> nodePositions;
    nodePositions.reserve(model->nodes.size());

    for (const auto& node : model->nodes) {
        SPHERE* shape = new (myTab->tabNo) SPHERE();
        shape->center = { node.x, node.y, node.z };
        shape->radius = kNodeRadius;
        shape->color = nodeColor;
        nodePositions.emplace(node.id, shape->center);
        RegisterGeneratedGeometryElement(myTab, SPHERE::storageObjectType, shape, shape->GetGeometry());
    }

    size_t createdMembers = 0;
    for (const auto& member : model->members) {
        const auto start = nodePositions.find(member.startNodeId);
        const auto end = nodePositions.find(member.endNodeId);
        if (start == nodePositions.end() || end == nodePositions.end()) continue;
        const float dx = end->second.x - start->second.x;
        const float dy = end->second.y - start->second.y;
        const float dz = end->second.z - start->second.z;
        if (dx * dx + dy * dy + dz * dz < 1e-8f) continue; // Zero-length member: no axis.

        PIPE* shape = new (myTab->tabNo) PIPE();
        shape->center1 = start->second;
        shape->center2 = end->second;
        shape->outsideDiameter = kMemberOutsideDiameter;
        shape->insideDiameter = kMemberInsideDiameter;
        shape->colorOuter = memberColor;
        shape->colorInner = memberColor;
        shape->colorCap = memberColor;
        RegisterGeneratedGeometryElement(myTab, PIPE::storageObjectType, shape, shape->GetGeometry());
        ++createdMembers;
    }

    std::cout << "[std-importer] Created " << model->nodes.size() << " node spheres and "
              << createdMembers << " member pipes." << std::endl; // Flush: rare event, aids diagnosis.
}

// Materializes a validated DXF import into the currently open Page2D through
// the same copy-thread queue interactive 2D creation uses. Import policy: the
// content goes into the *active* Page2D sub-tab only; abort when none is open
// (the UI pre-checks too, but the state can change while the action is queued).
static void ImportDxfFileIntoTab(DATASETTAB* myTab, uint64_t payloadId) {
    uint64_t pageMemoryId = 0;
    if (Cad2DIsActivePage2D(*myTab)) {
        // The active sub-tab is a Page2D, so the lookup returns exactly it.
        pageMemoryId = Cad2DFindTargetPage2DMemoryId(*myTab);
    }
    if (pageMemoryId == 0) {
        ExtensionCommunications::ReleaseQueuedImportPath(payloadId);
        const char* message = "DXF import aborted: no Page2D is currently open.";
        std::cout << "[dxf-importer] " << message << std::endl;
        MessageBoxA(nullptr, message, "DXF import", MB_OK | MB_ICONINFORMATION);
        return;
    }

    std::string error;
    std::unique_ptr<ExtensionCommunications::ImportedPage2DContent> content(
        ExtensionCommunications::RunQueuedDxfImport(payloadId, error));
    if (!content) {
        std::cout << "[dxf-importer] " << error << "\n";
        MessageBoxA(nullptr, error.c_str(), "DXF import failed", MB_OK | MB_ICONERROR);
        return;
    }

    for (const auto& line : content->lines) {
        Cad2DLineRecordCPU record{};
        record.containerMemoryId = pageMemoryId;
        record.x1 = line.x1;
        record.y1 = line.y1;
        record.x2 = line.x2;
        record.y2 = line.y2;
        record.lineWeight = 1.0f;
        record.lineWeightMode = Cad2DLineWeightMode::ScreenPixel;
        record.colorABGR = 0xFF000000u;
        record.schemaVersion = VishwakarmaStorage::kGeometry2DLineSchemaVersion;
        EnqueueCad2DLine(myTab->tabID, pageMemoryId, record);
    }

    for (const auto& text : content->texts) {
        Cad2DTextRecordCPU record{};
        record.containerMemoryId = pageMemoryId;
        record.x = text.x;
        record.y = text.y;
        record.textHeightCU = text.heightCU;
        record.rotationRadians = text.rotationRadians;
        record.colorABGR = 0xFF000000u;
        record.font = 0; // Imported text always renders with the embedded MSDF font.
        record.justification = static_cast<Cad2DTextJustification>(text.justification);
        record.text = text.textUtf8;
        record.schemaVersion = VishwakarmaStorage::kGeometry2DTextSchemaVersion;
        EnqueueCad2DText(myTab->tabID, pageMemoryId, std::move(record));
    }

    for (const auto& polygon : content->polygons) {
        Cad2DPolygonRecordCPU record{};
        record.containerMemoryId = pageMemoryId;
        record.lineSegmentCount = polygon.segmentCount;
        record.centerX = polygon.centerX;
        record.centerY = polygon.centerY;
        record.radius = polygon.radius;
        record.rotationDegrees = polygon.rotationDegrees;
        record.lineWeight = 1.0f;
        record.lineWeightMode = Cad2DLineWeightMode::ScreenPixel;
        record.colorABGR = 0xFF000000u;
        record.schemaVersion = VishwakarmaStorage::kGeometry2DPolygonSchemaVersion;
        EnqueueCad2DPolygon(myTab->tabID, pageMemoryId, record);
    }

    std::cout << "[dxf-importer] Created " << content->lines.size() << " lines, "
              << content->texts.size() << " texts and " << content->polygons.size()
              << " polygons in the open Page2D." << std::endl; // Flush: rare event, aids diagnosis.
}

static void BeginPrimitive3DPlacement(DATASETTAB* targetTab, VishwakarmaStorage::ObjectType objectType) {
    if (!targetTab || !VishwakarmaStorage::IsGeometry3DObjectType(objectType)) return;

    Cad2DCancelCreation(*targetTab);
    const uint64_t sceneMemoryId = EnsureActiveScene3D(targetTab);
    if (sceneMemoryId != 0) {
        OpenInternalSubTab(targetTab, sceneMemoryId);
    }
    targetTab->activePrimitive3DPlacementType.store(
        VishwakarmaStorage::ToNumber(objectType), std::memory_order_release);
}

static void CancelPrimitive3DPlacement(DATASETTAB& tab) {
    tab.activePrimitive3DPlacementType.store(
        VishwakarmaStorage::ToNumber(VishwakarmaStorage::ObjectType::Unknown),
        std::memory_order_release);
}

static bool HandlePrimitive3DPlacementInput(DATASETTAB& tab, const ACTION_DETAILS& input) {
    const auto objectType = static_cast<VishwakarmaStorage::ObjectType>(
        tab.activePrimitive3DPlacementType.load(std::memory_order_acquire));
    if (objectType == VishwakarmaStorage::ObjectType::Unknown) return false;
    if (!VishwakarmaStorage::IsGeometry3DObjectType(objectType)) {
        CancelPrimitive3DPlacement(tab);
        return false;
    }

    if (input.actionType == ACTION_TYPE::KEYDOWN && input.x == VK_ESCAPE) {
        CancelPrimitive3DPlacement(tab);
        return true;
    }

    if (input.actionType != ACTION_TYPE::LBUTTONDOWN || tab.isAltDown) return false;

    XMFLOAT3 placementPoint{};
    if (Scene3DPlacementPointFromInput(tab, input, placementPoint)) {
        CreatePrimitiveGeometryElement(&tab, objectType, placementPoint);
    }
    return true;
}

// --- Zoom Window mode (Commands::ZOOM_WINDOW) ---------------------------------------------------
// Works like primitive placement: arming the mode makes the render thread trail the command icon
// next to the cursor, then two clicks define the rectangle to zoom onto. ESC cancels. Applies to
// whichever view is active: Scene3D camera or Page2D view.
static void CancelZoomWindowMode(DATASETTAB& tab) {
    tab.zoomWindowMode.store(false, std::memory_order_release);
    tab.zoomWindowHasFirstCorner = false;
}

static void BeginZoomWindowMode(DATASETTAB* targetTab) {
    if (!targetTab) return;
    CancelPrimitive3DPlacement(*targetTab);
    Cad2DCancelCreation(*targetTab);
    targetTab->zoomWindowHasFirstCorner = false;
    targetTab->zoomWindowMode.store(true, std::memory_order_release);
}

static bool HandleZoomWindowInput(DATASETTAB& tab, const ACTION_DETAILS& input) {
    if (!tab.zoomWindowMode.load(std::memory_order_acquire)) return false;

    // A tool started after us wins: primitive placement or any 2D creation mode cancels this mode.
    const bool anyCad2DCreationMode = tab.cad2d &&
        (tab.cad2d->lineCreationMode.load(std::memory_order_acquire) ||
         tab.cad2d->polylineCreationMode.load(std::memory_order_acquire) ||
         tab.cad2d->polygonCreationMode.load(std::memory_order_acquire) ||
         tab.cad2d->circleCreationMode.load(std::memory_order_acquire) ||
         tab.cad2d->ellipseCreationMode.load(std::memory_order_acquire) ||
         tab.cad2d->arcCreationMode.load(std::memory_order_acquire) ||
         tab.cad2d->textCreationMode.load(std::memory_order_acquire) ||
         tab.cad2d->transform2DKind.load(std::memory_order_acquire) != 0);
    if (anyCad2DCreationMode || static_cast<VishwakarmaStorage::ObjectType>(
            tab.activePrimitive3DPlacementType.load(std::memory_order_acquire)) !=
            VishwakarmaStorage::ObjectType::Unknown) {
        CancelZoomWindowMode(tab);
        return false;
    }

    if (input.actionType == ACTION_TYPE::KEYDOWN && input.x == VK_ESCAPE) {
        CancelZoomWindowMode(tab);
        return true;
    }
    if (input.actionType != ACTION_TYPE::LBUTTONDOWN || tab.isAltDown) return false;
    if (IsOverRightOverlay(tab, input.x)) return false;

    int viewportWidth = 0, viewportHeight = 0, viewportTop = 0;
    if (!GetVisibleSceneViewportForTab(tab, viewportWidth, viewportHeight, viewportTop)) return true;
    if (input.x < 0 || input.x >= viewportWidth ||
        input.y < viewportTop || input.y >= viewportTop + viewportHeight) {
        return true; // Click outside the scene area; keep waiting for a corner.
    }

    if (!tab.zoomWindowHasFirstCorner) {
        tab.zoomWindowFirstX = input.x;
        tab.zoomWindowFirstY = input.y;
        tab.zoomWindowHasFirstCorner = true;
        return true;
    }

    const int firstX = tab.zoomWindowFirstX, firstY = tab.zoomWindowFirstY;
    CancelZoomWindowMode(tab);
    if (Cad2DIsActivePage2D(tab)) {
        Cad2DZoomToWindow(tab, firstX, firstY, input.x, input.y);
    } else {
        ZoomSceneToWindow(tab, firstX, firstY, input.x, input.y);
    }
    return true;
}

inline void addRandomGeometryElement(DATASETTAB* targetTab) {
	if (!targetTab) return; //Safety against NULL pointer dereference.
    GeometryData geometry;// These will hold the data of the randomly created shape.
    META_DATA* object = nullptr;
    VishwakarmaStorage::ObjectType objectType = VishwakarmaStorage::ObjectType::Unknown;

    // Randomly select a shape type (0-7 for the 8 shapes available).
    // We use the GetRNG() helper function already available in "डेटा-सामान्य-3D.h".
    std::uniform_int_distribution<int> shapeDist(0, 8);
    int shapeType = shapeDist(GetRNG());

    // Note: Ensure your shape constructors (new PYRAMID()) use the correct memoryGroupNo if needed.
    // For now, assuming they use default or we will fix memory grouping later. TODO

    switch (shapeType) {// Create and randomize the chosen shape.
        /* Important information, even though we are creating new shapes using "new" keyword,
        The memory allocation is done on our custom memory manager. Which will clean itself,
        when the tab is closed or destroyed. This prevents memory leaks in long-running applications. 
        Currently, it seems to be leaking memory, but it is NOT !*/
    case 0: {
        PYRAMID* shape = new (targetTab->tabNo) PYRAMID();
        shape->Randomize();
        geometry = shape->GetGeometry();
        object = shape;
        objectType = PYRAMID::storageObjectType;
        break;
    }
    case 1: {
        CUBOID* shape = new (targetTab->tabNo) CUBOID();
        shape->Randomize();
        geometry = shape->GetGeometry();
        object = shape;
        objectType = CUBOID::storageObjectType;
        break;
    }
    case 2: {
        CONE* shape = new (targetTab->tabNo) CONE();
        shape->Randomize();
        geometry = shape->GetGeometry();
        object = shape;
        objectType = CONE::storageObjectType;
        break;
    }
    case 3: {
        CYLINDER* shape = new (targetTab->tabNo) CYLINDER();
        shape->Randomize();
        geometry = shape->GetGeometry();
        object = shape;
        objectType = CYLINDER::storageObjectType;
        break;
    }
    case 4: {
        PARALLELEPIPED* shape = new (targetTab->tabNo) PARALLELEPIPED();
        shape->Randomize();
        geometry = shape->GetGeometry();
        object = shape;
        objectType = PARALLELEPIPED::storageObjectType;
        break;
    }
    case 5: {
        SPHERE* shape = new (targetTab->tabNo) SPHERE();
        shape->Randomize();
        geometry = shape->GetGeometry();
        object = shape;
        objectType = SPHERE::storageObjectType;
        break;
    }
    case 6: {
        FRUSTUM_OF_PYRAMID* shape = new (targetTab->tabNo) FRUSTUM_OF_PYRAMID();
        shape->Randomize();
        geometry = shape->GetGeometry();
        object = shape;
        objectType = FRUSTUM_OF_PYRAMID::storageObjectType;
        break;
    }
    case 7: {
        FRUSTUM_OF_CONE* shape = new (targetTab->tabNo) FRUSTUM_OF_CONE();
        shape->Randomize();
        geometry = shape->GetGeometry();
        object = shape;
        objectType = FRUSTUM_OF_CONE::storageObjectType;
        break;
    }
    case 8: {
        PIPE* shape = new (targetTab->tabNo) PIPE();
        shape->Randomize();
        geometry = shape->GetGeometry();
        object = shape;
        objectType = PIPE::storageObjectType;
        break;
    }
    }
    RegisterGeneratedGeometryElement(targetTab, objectType, object, std::move(geometry));
}

void विश्वकर्मा(uint64_t tabID) { //Main logic/engineering thread. The ringmaster of the application.
    std::cout << "Main Logic Thread विश्वकर्मा started." << std::endl;
    if (tabID >= MV_MAX_TABS) return;

    DATASETTAB* myTab = &allTabs[tabID];
    myTab->engineeringReleased.store(false, std::memory_order_release);
    std::chrono::steady_clock::time_point lastPyramidAddTime;
    lastPyramidAddTime = std::chrono::steady_clock::now();// Initialize the timer

    if (myTab->autoGenerateRandomGeometry || myTab->storageFilePath.empty()) {
        EnsureDefaultLogicalHierarchy(myTab);
        OpenInitialLogicalContainerSubTabs(myTab);
    }

    // Generate initial random geometry only for unsaved/dev tabs. Loaded .yyy tabs keep their stored contents.
    if (myTab->autoGenerateRandomGeometry) {
        for (int k = 0; k < 10; ++k) addRandomGeometryElement(myTab);
    }

    uint64_t frameCounter = 0;

    while (!shutdownSignal) { // This is our primary application loop.
        auto frameStart = std::chrono::high_resolution_clock::now();
        
        if (myTab->closeRequested.load(std::memory_order_acquire)) break;

		// Automatic camera rotation for troubleshooting. Toggle using "r". To be removed later or made optional in UI.
        if (myTab->autoCameraRotation) {
            UpdateCameraOrbit(myTab->camera); // Fallback camera (content without any sub-tab).
            // Every open Scene3D view orbits its own camera independently.
            uint16_t* orbitList = myTab->publishedSubTabIndexes.load(std::memory_order_acquire);
            const uint16_t orbitCount = myTab->publishedSubTabCount.load(std::memory_order_acquire);
            for (uint16_t i = 0; orbitList && i < orbitCount; ++i) {
                InternalSubTab& subTab = myTab->subTabs[orbitList[i]];
                if (subTab.containerType == VishwakarmaStorage::ObjectType::Scene3D) {
                    UpdateCameraOrbit(subTab.camera);
                }
            }
        }
        
        // Check timer and add a new pyramid every second.
        auto currentTime = std::chrono::steady_clock::now();
        if (myTab->autoGenerateRandomGeometry &&
            std::chrono::duration_cast<std::chrono::seconds>(currentTime - lastPyramidAddTime).count() >= 1) {
            addRandomGeometryElement(myTab);
            ACTION_DETAILS createLine2D{};
            createLine2D.actionType = ACTION_TYPE::CREATE_LINE2D;
            createLine2D.source = INPUT_SOURCE::SYSTEM;
            createLine2D.timestamp = GetTickCount64();
            myTab->todoCPUQueue->push(createLine2D);
            lastPyramidAddTime = currentTime; // Reset the timer
            // Optional: Log to prove background work is happening
            // std::cout << "Tab " << tabIndex << " generated object." << std::endl;
        }

        // Process User Inputs First (Lightweight: Camera, Selection, Throttling)
        ACTION_DETAILS input;
        int inputCount = 0;  // For throttling detection
        auto inputStart = std::chrono::steady_clock::now();
		bool isOrbiting = false, isPanning = false; // Track if we are in orbit/panning based on mouse state and modifiers.
        float distance = 0.0;
        float dx, dy, dz, vx, vy, vz;

        while (myTab->userInputQueue->try_pop(input)) {
            inputCount++;
            // Throttle: Skip intermediate MOUSEMOVE if >200/sec (check timestamp/rate)
            if (input.actionType == ACTION_TYPE::MOUSEMOVE && inputCount > 200) { continue; }  // Simple rate limit

            if (HandleZoomWindowInput(*myTab, input)) { continue; }
            if (Cad2DHandleInput(*myTab, input)) { continue; }
            if (HandlePrimitive3DPlacementInput(*myTab, input)) { continue; }

            // Camera of the view this input targets (per-view for Scene3D sub-tabs).
            CameraState& cam = ActiveSceneCamera(*myTab);

            // Handle based on type
            switch (input.actionType) {
            case ACTION_TYPE::MOUSEMOVE:
                isPanning = myTab->mouseMiddleDown && myTab->isShiftDown;
                // Orbit if Middle Mouse is down, but NOT panning, OR if Alt+Left Click
                isOrbiting = (!isPanning && myTab->mouseMiddleDown) || (myTab->mouseLeftDown && myTab->isAltDown);
                if (isOrbiting || isPanning) {
                    myTab->selection.lastNavInteractionMs.store(GetTickCount64(), std::memory_order_release);
                }

                dx = float(input.x - myTab->lastMouseX);
                dy = float(input.y - myTab->lastMouseY);

                // Calculate Vector from Target to Camera (View Vector)
                vx = cam.position.x - cam.target.x;
                vy = cam.position.y - cam.target.y;
                vz = cam.position.z - cam.target.z;
                distance = std::sqrt(vx * vx + vy * vy + vz * vz);

                if (isPanning) {// PANNING IMPLEMENTATION
                    // Pan Speed should scale with distance (zooming out makes pan faster)
                    float panSpeed = distance * 0.001f;

                    // Calculate Forward View Vector (Normalized). We need the direction looking AT the target
                    float invDist = 1.0f / (distance + 0.0001f); // Avoid div by zero
                    float fx = -vx * invDist;
                    float fy = -vy * invDist;
                    float fz = -vz * invDist;
                    // Calculate Right Vector (Cross Product of Forward and World Up)
                    // Assuming World Up is Z+ (0, 0, 1) based on your Orbit Math
                    float rx = fy;      // (fy * 1) - (fz * 0)
                    float ry = -fx;     // (fz * 0) - (fx * 1)
                    float rz = 0.0f;    // (fx * 0) - (fy * 0)
                    float rLen = std::sqrt(rx * rx + ry * ry);// Normalize Right Vector
                    if (rLen > 0.0001f) { rx /= rLen; ry /= rLen; }

                    // Calculate Camera Up Vector (Cross Product of Right and Forward)
                    // This creates the "Screen Up" vector perpendicular to view
                    float ux = (ry * fz) - (rz * fy);
                    float uy = (rz * fx) - (rx * fz);
                    float uz = (rx * fy) - (ry * fx);

                    // Apply Movement. Move Left/Right: -dx along Right Vector
                    // Move Up/Down: +dy along Camera Up Vector (Screen space Y is usually inverted, check preference)
                    float moveX = (rx * dx * panSpeed) + (ux * dy * panSpeed);
                    float moveY = (ry * dx * panSpeed) + (uy * dy * panSpeed);
                    float moveZ = (rz * dx * panSpeed) + (uz * dy * panSpeed);

                    // Apply to BOTH Position and Target to maintain view direction
                    cam.position.x += moveX;
                    cam.position.y += moveY;
                    cam.position.z += moveZ;

                    cam.target.x += moveX;
                    cam.target.y += moveY;
                    cam.target.z += moveZ;
                }
                else if (isOrbiting) {// Orbit / Rotate around Focal Point (Target)
                    float sensitivity = 0.005f; // Adjust rotation speed here
                    dx = (input.x - myTab->lastMouseX) * sensitivity;
                    dy = (input.y - myTab->lastMouseY) * sensitivity;

                    // Calculate vector from Target to Camera (The Radius vector)
                    vx = cam.position.x - cam.target.x;
                    vy = cam.position.y - cam.target.y;
                    vz = cam.position.z - cam.target.z;

                    // Convert to Spherical Coordinates. radius (distance), theta (azimuth), phi (elevation)
                    float radius = std::sqrt(vx * vx + vy * vy + vz * vz);
                    float theta = std::atan2(vy, vx);     // Angle in XY plane
                    if (radius < 0.0001f) radius = 0.0001f;
                    float phi = std::acos(vz / radius);   // Angle from Z axis (Up)

                    theta -= dx;// Apply Mouse Delta. Note: Sign +/- depends on desired control inversion
                    phi -= dy;

                    // Clamp Phi (Elevation) to prevent camera flipping upside down
                    // Keep it between 1 degree and 179 degrees (0.01 to PI - 0.01)
                    float epsilon = 0.01f;
                    float pi = 3.1415926535f;
                    if (phi < epsilon) phi = epsilon;
                    if (phi > pi - epsilon) phi = pi - epsilon;
                    
                    float nx = radius * std::sin(phi) * std::cos(theta);// Convert back to Cartesian Coordinates
                    float ny = radius * std::sin(phi) * std::sin(theta);
                    float nz = radius * std::cos(phi);

                    // Update Camera Position relative to Target
                    cam.position.x = cam.target.x + nx;
                    cam.position.y = cam.target.y + ny;
                    cam.position.z = cam.target.z + nz;

                    // Flag to Copy Thread that camera changed (if your engine requires explicit dirty flags)
                    // std::lock_guard<std::mutex> lock(toCopyThreadMutex);
                    // commandToCopyThreadQueue.push({ CommandToCopyThreadType::UPDATE_CAMERA, ... });
                }
				// Camera Safety Check to ensure camera and target are not at the same, crashing view matrix calculation.
                vx = cam.position.x - cam.target.x;
                vy = cam.position.y - cam.target.y;
                vz = cam.position.z - cam.target.z;
                if (vx * vx + vy * vy + vz * vz < 0.000001f) {
                    cam.position.z += 0.001f;}// tiny nudge along camera up
                
                // Standard Mouse Update
                if (myTab->mouseLeftDown && !isOrbiting && !isPanning) {
                    // Drag logic (selection box etc.)
                    // Push to commandToCopyThreadQueue if view changes dirty geometry.
                }

                myTab->lastMouseX = input.x;
                myTab->lastMouseY = input.y;
                // Check if in render area (vs UI): Compare input.x/y to myTab->views[activeViewIndex].rect or contentRect.
                break;
            case ACTION_TYPE::MOUSEWHEEL:
            {
                // Wheel over the right icon bar / properties pane must not zoom the scene camera.
                if (IsOverRightOverlay(*myTab, input.x)) break;
                float wheelSteps = input.delta / (float)WHEEL_DELTA; // Since new mouse send lots of events ?

                // Calculate the vector from Target to Position
                float dx = cam.position.x - cam.target.x;
                float dy = cam.position.y - cam.target.y;
                float dz = cam.position.z - cam.target.z;
                distance = std::sqrt(dx * dx + dy * dy + dz * dz);// Calculate current distance from target

                // Determine Zoom Factor. Standard mouse wheel delta is 120. 
                // Delta > 0 (Wheel Forward) -> Zoom IN  (Factor < 1.0). Delta < 0 (Wheel Back)    -> Zoom OUT (Factor > 1.0)
				//float zoomFactor = (input.delta > 0) ? 0.9f : 1.1f; // Binary zoom. Not smooth.                
                float zoomFactor = std::pow(0.9f, wheelSteps);// Smooth zoom instead of binary zoom
                float newDistance = distance * zoomFactor;// Apply Zoom

                // Safety Clamping. Prevent getting stuck at 0 (locking the camera) or going too far
                if (newDistance < 1.0f) newDistance = 1.0f;
                if (newDistance > cam.farZ - 10.0f) newDistance = cam.farZ - 10.0f;

                // Keep the point under the cursor visually anchored while changing distance.
                if (distance > 0.002f) {
                    float scale = newDistance / distance;
                    bool appliedCursorZoom = false;
                    int viewportWidth = 0, viewportHeight = 0, viewportTop = 0;
                    if (GetVisibleSceneViewportForTab(*myTab, viewportWidth, viewportHeight, viewportTop)) {
                        const float mouseX = std::clamp((float)input.x, 0.0f, (float)viewportWidth);
                        const float mouseY = std::clamp((float)(input.y - viewportTop), 0.0f, (float)viewportHeight);
                        const float ndcX = mouseX / (float)viewportWidth * 2.0f - 1.0f;
                        const float ndcY = 1.0f - mouseY / (float)viewportHeight * 2.0f;
                        const float aspect = (float)viewportWidth / (float)viewportHeight;
                        const float tanHalfFov = std::tan(cam.fov * 0.5f);

                        DirectX::XMVECTOR eye = DirectX::XMLoadFloat3(&cam.position);
                        DirectX::XMVECTOR target = DirectX::XMLoadFloat3(&cam.target);
                        DirectX::XMVECTOR worldUp = DirectX::XMLoadFloat3(&cam.up);
                        DirectX::XMVECTOR forward = DirectX::XMVector3Normalize(DirectX::XMVectorSubtract(target, eye));
                        DirectX::XMVECTOR right = DirectX::XMVector3Cross(worldUp, forward);
                        if (DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(right)) > 0.000001f) {
                            right = DirectX::XMVector3Normalize(right);
                            DirectX::XMVECTOR viewUp = DirectX::XMVector3Cross(forward, right);
                            DirectX::XMVECTOR ray = DirectX::XMVectorAdd(forward,
                                DirectX::XMVectorAdd(
                                    DirectX::XMVectorScale(right, ndcX * tanHalfFov * aspect),
                                    DirectX::XMVectorScale(viewUp, ndcY * tanHalfFov)));
                            ray = DirectX::XMVector3Normalize(ray);

                            const float denom = DirectX::XMVectorGetX(DirectX::XMVector3Dot(ray, forward));
                            if (std::abs(denom) > 0.000001f) {
                                DirectX::XMVECTOR focusPoint =
                                    DirectX::XMVectorAdd(eye, DirectX::XMVectorScale(ray, distance / denom));
                                eye = DirectX::XMVectorAdd(focusPoint,
                                    DirectX::XMVectorScale(DirectX::XMVectorSubtract(eye, focusPoint), scale));
                                target = DirectX::XMVectorAdd(focusPoint,
                                    DirectX::XMVectorScale(DirectX::XMVectorSubtract(target, focusPoint), scale));
                                DirectX::XMStoreFloat3(&cam.position, eye);
                                DirectX::XMStoreFloat3(&cam.target, target);
                                appliedCursorZoom = true;
                            }
                        }
                    }
                    if (!appliedCursorZoom) {
                        cam.position.x = cam.target.x + (dx * scale);
                        cam.position.y = cam.target.y + (dy * scale);
                        cam.position.z = cam.target.z + (dz * scale);
                    }
                }

                //std::cout << "Zoom Updated. New Distance: " << newDistance << "\n";// Debug logging (Optional)
                myTab->selection.lastNavInteractionMs.store(GetTickCount64(), std::memory_order_release);
                // Recenter the orbit pivot on the nearest surface under the cursor (async GPU pick).
                RequestScenePick(*myTab, input.x, input.y, PickPurpose::Recenter);
                break;
            }
            case ACTION_TYPE::LBUTTONDOWN:
                myTab->mouseLeftDown = true;
                // Plain left click selects the object under the cursor (Alt+Left is orbit). Clicks over
                // the right icon bar / properties pane never touch the scene (propertiesPane.md §6).
                if (!myTab->isAltDown && !IsOverRightOverlay(*myTab, input.x)) {
                    RequestScenePick(*myTab, input.x, input.y, PickPurpose::Select);
                }
                break;
            case ACTION_TYPE::LBUTTONUP:
                myTab->mouseLeftDown = false;
                break;
            case ACTION_TYPE::MBUTTONDOWN:
                myTab->mouseMiddleDown = true;
                break;
            case ACTION_TYPE::MBUTTONUP:
                myTab->mouseMiddleDown = false;
                break;
            case ACTION_TYPE::KEYDOWN:
                if (input.x == 'P') {  // Example mapping
                    ACTION_DETAILS todo;
                    todo.actionType = ACTION_TYPE::CREATEPYRAMID;
                    // Fill other fields...
                    myTab->todoCPUQueue->push(todo);
                }
                else if (input.x == 18) {myTab->isAltDown = true;} // 18 is VK_MENU (ALT)
                else if (input.x == 16) {myTab->isShiftDown = true;} // SHIFT (VK_SHIFT)
                break;
            case ACTION_TYPE::KEYUP:
                if (input.x == 18) { myTab->isAltDown = false;}// 18 is VK_MENU (ALT)
                else if (input.x == 16) { myTab->isShiftDown = false; } // SHIFT
                break;

            case ACTION_TYPE::CHAR:
				//Temporary Debug Key: Toggle Auto Camera Rotation with "r" key.
                if (input.x == 82 || input.x == 114) // 'r' & "R"
                {
                    myTab->autoCameraRotation = !myTab->autoCameraRotation; 
                }
				if (input.x == 67 || input.x == 99) { cam.Initialize(); } // 'c' & "C". Reset camera.
                break;

            case ACTION_TYPE::CAPTURECHANGED:
            case ACTION_TYPE::INPUT:  // For device reset Reset all button states
                myTab->mouseLeftDown = myTab->mouseRightDown = myTab->mouseMiddleDown = false;
                myTab->isShiftDown = myTab->isAltDown = myTab->isCtrlDown = false; // Reset modifiers too
                break;
            }
        }
        // After loop: If inputCount high, log or adjust (e.g., sleep if bursty).

        // Apply any completed GPU pick result (selection highlight set + camera recentering).
        if (myTab->selection.resultReady.load(std::memory_order_acquire)) {
            bool hit; uint64_t objId; uint32_t purpose;
            DirectX::XMFLOAT3 cg, surf;
            {
                std::lock_guard<std::mutex> lock(myTab->selection.resultMutex);
                hit = myTab->selection.resultHit;
                objId = myTab->selection.resultObjectId;
                cg = myTab->selection.resultCG;
                surf = myTab->selection.resultSurface;
                purpose = myTab->selection.resultPurpose;
                myTab->selection.resultReady.store(false, std::memory_order_release);
            }
            ApplyPickResult(*myTab, hit, objId, cg, surf, purpose);
        }

        // Existing todoCPUQueue processing remains (for self-TODOs like CREATEPYRAMID).

        // Input Processing (Specific to this Tab). Previously todoCPUQueue was global. now it is Local. 
        // Process all pending inputs from User, Network, File threads
        ACTION_DETAILS nextWorkTODO;
        while (bool todo = myTab->todoCPUQueue->try_pop(nextWorkTODO)) {
            std::cout << "Input received. Action Type = " << static_cast<int>(nextWorkTODO.actionType) <<"\n";
            if (nextWorkTODO.actionType == ACTION_TYPE::CREATEPYRAMID) {
                //addRandomGeometryElement();
            } else if (nextWorkTODO.actionType == ACTION_TYPE::CLOSE_TAB) {
                myTab->closeRequested.store(true, std::memory_order_release);
                break;
            } else if (nextWorkTODO.actionType == ACTION_TYPE::DATA_TREE_TOGGLE_VISIBILITY) {
                DataTreeView::ToggleVisibility(myTab->dataTreeView);
            } else if (nextWorkTODO.actionType == ACTION_TYPE::DATA_TREE_TOGGLE_EVERYTHING) {
                DataTreeView::ToggleEverything(myTab->dataTreeView);
            } else if (nextWorkTODO.actionType == ACTION_TYPE::DATA_TREE_TOGGLE_NODE) {
                ToggleDataTreeNode(myTab, nextWorkTODO.objectId);
            } else if (nextWorkTODO.actionType == ACTION_TYPE::DATA_TREE_SET_ACTIVE_BRANCH) {
                SetActiveDataTreeBranch(myTab, nextWorkTODO.objectId);
            } else if (nextWorkTODO.actionType == ACTION_TYPE::OPEN_INTERNAL_SUB_TAB) {
                OpenInternalSubTab(myTab, nextWorkTODO.objectId);
            } else if (nextWorkTODO.actionType == ACTION_TYPE::ACTIVATE_INTERNAL_SUB_TAB) {
                ActivateInternalSubTab(myTab, nextWorkTODO.objectId);
            } else if (nextWorkTODO.actionType == ACTION_TYPE::CLOSE_INTERNAL_SUB_TAB) {
                CloseInternalSubTab(myTab, nextWorkTODO.objectId);
            } else if (nextWorkTODO.actionType == ACTION_TYPE::INTERNAL_SUB_TAB_EXTRACTED) {
                HandleSubTabExtracted(myTab, nextWorkTODO.objectId);
            } else if (nextWorkTODO.actionType == ACTION_TYPE::BEGIN_PRIMITIVE_CREATION3D) {
                BeginPrimitive3DPlacement(myTab, static_cast<VishwakarmaStorage::ObjectType>(nextWorkTODO.x));
            } else if (nextWorkTODO.actionType == ACTION_TYPE::BEGIN_LINE_CREATION2D) {
                CancelPrimitive3DPlacement(*myTab);
                Cad2DBeginLineCreation(*myTab);
            } else if (nextWorkTODO.actionType == ACTION_TYPE::BEGIN_POLYLINE_CREATION2D) {
                CancelPrimitive3DPlacement(*myTab);
                Cad2DBeginPolylineCreation(*myTab);
            } else if (nextWorkTODO.actionType == ACTION_TYPE::BEGIN_POLYGON_CREATION2D) {
                CancelPrimitive3DPlacement(*myTab);
                Cad2DBeginPolygonCreation(*myTab);
            } else if (nextWorkTODO.actionType == ACTION_TYPE::BEGIN_CIRCLE_CREATION2D) {
                CancelPrimitive3DPlacement(*myTab);
                Cad2DBeginCircleCreation(*myTab);
            } else if (nextWorkTODO.actionType == ACTION_TYPE::BEGIN_ELLIPSE_CREATION2D) {
                CancelPrimitive3DPlacement(*myTab);
                Cad2DBeginEllipseCreation(*myTab);
            } else if (nextWorkTODO.actionType == ACTION_TYPE::BEGIN_ARC_CREATION2D) {
                CancelPrimitive3DPlacement(*myTab);
                Cad2DBeginArcCreation(*myTab);
            } else if (nextWorkTODO.actionType == ACTION_TYPE::BEGIN_TEXT_CREATION2D) {
                CancelPrimitive3DPlacement(*myTab);
                Cad2DBeginTextCreation(*myTab);
            } else if (nextWorkTODO.actionType == ACTION_TYPE::BEGIN_TRANSFORM2D) {
                CancelPrimitive3DPlacement(*myTab);
                Cad2DBeginTransform2D(*myTab, static_cast<Cad2DTransformKind>(nextWorkTODO.x));
            } else if (nextWorkTODO.actionType == ACTION_TYPE::CREATE_LINE2D) {
                Cad2DAutoGenerateDemoContent(*myTab);
            } else if (nextWorkTODO.actionType == ACTION_TYPE::CREATE_LOGICAL_OBJECT) {
                const auto objectType = static_cast<VishwakarmaStorage::ObjectType>(nextWorkTODO.x);
                if (VishwakarmaStorage::IsLogicalObjectType(objectType)) {
                    CreateLogicalElement(myTab, objectType, 0);
                }
            } else if (nextWorkTODO.actionType == ACTION_TYPE::IMPORT_STD_FILE) {
                ImportStdFileIntoTab(myTab, nextWorkTODO.objectId);
            } else if (nextWorkTODO.actionType == ACTION_TYPE::IMPORT_DXF_FILE) {
                ImportDxfFileIntoTab(myTab, nextWorkTODO.objectId);
            } else if (nextWorkTODO.actionType == ACTION_TYPE::MODIFY_OBJECT_PROPERTY) {
                ModifyObjectProperty(myTab, nextWorkTODO.objectId, static_cast<uint8_t>(nextWorkTODO.x),
                    std::bit_cast<double>(nextWorkTODO.auxValue));
            } else if (nextWorkTODO.actionType == ACTION_TYPE::ZOOM_MAX_EXTENTS ||
                       nextWorkTODO.actionType == ACTION_TYPE::ZOOM_FOCUS_SELECTED) {
                const bool selectedOnly = nextWorkTODO.actionType == ACTION_TYPE::ZOOM_FOCUS_SELECTED;
                if (Cad2DIsActivePage2D(*myTab)) {
                    Cad2DZoomToExtents(*myTab, selectedOnly);
                } else {
                    ZoomSceneToExtents(myTab, selectedOnly);
                }
            } else if (nextWorkTODO.actionType == ACTION_TYPE::ZOOM_WINDOW_BEGIN) {
                BeginZoomWindowMode(myTab);
            }
        }

        CleanupReleasedSubTabs(myTab); // Delayed sub-tab slot release once GPU fences passed.

        std::this_thread::sleep_for(std::chrono::milliseconds(10));// Sleep to yield CPU
        frameCounter++;
    } // End of while (!shutdownSignal), i.e. our primary application loop for this particular tab.

    //g_logicFenceCV.notify_all(); // Wake up threads for shutdown
    myTab->allIDsInThisTab.clear();
    if (myTab->storageObjectsMutex) {
        std::lock_guard<std::mutex> lock(*myTab->storageObjectsMutex);
        myTab->storageLogicalObjects.clear();
        myTab->storageObjects3D.clear();
        myTab->expandedDataTreeNodeIds.clear();
        CloseAllInternalSubTabsLocked(*myTab);
        myTab->defaultScene3DMemoryId = 0;
        myTab->activeScene3DMemoryId = 0;
        CancelPrimitive3DPlacement(*myTab);
        CancelZoomWindowMode(*myTab);
    }
    DataTreeView::ResetScroll(myTab->dataTreeView);
    myTab->engineeringReleased.store(true, std::memory_order_release);
    std::cout << "Main Logic Thread shutting down.\n" << std::endl;
}
