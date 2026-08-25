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
#include "डेटा-पाइप.h"
#include "डेटा-संरचना.h"
#include "PropertyPane.h"
#include "ExtensionCommunications.h"
#include "GPUPlatformSelector.h"

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
    object->memoryIDContainer = parentMemoryId;
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

// Camera the engineering thread's scene input math applies to: the camera of the Viewport driving
// the input SubTab when that SubTab is a Scene3D, else the tab-level fallback camera (content shown
// without any sub-tab). The camera lives in the Viewport now, not the SubTab (10M plan Step 6).
static CameraState& ActiveSceneCamera(DATASETTAB& tab) {
    const int slot = InputViewSlot(tab);
    if (slot >= 0 && tab.subTabs[slot].containerType == VishwakarmaStorage::ObjectType::Scene3D) {
        return tab.viewports[slot].camera;
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
    std::vector<uint16_t> freedSlots;
    for (uint16_t slot = 0; slot < MV_MAX_SUBTABS; ++slot) {
        if (targetTab->subTabStates[slot].load(std::memory_order_acquire) != SUBTAB_PENDING_GPU_RELEASE) continue;
        if (!AllMonitorRenderFencesPassed(targetTab->subTabReleaseFenceValues[slot])) continue;

        {
            std::lock_guard<std::mutex> lock(*targetTab->storageObjectsMutex);
            targetTab->subTabs[slot] = InternalSubTab{}; // Release title string memory.
            targetTab->subTabHostWindowSlots[slot].store(-1, std::memory_order_release);
            targetTab->subTabStates[slot].store(SUBTAB_FREE, std::memory_order_release);
        }
        freedSlots.push_back(slot);
    }

    /* Hand each freed slot's VisibilityMask bit back clean (graphics.md, 10M plan Step 5). Slots
    are recycled, so without this the next sub-tab to land in one would silently inherit hides
    authored for a view that no longer exists, and the user would have no way to see why objects
    are missing. The copy thread touches only the objects actually hiding that bit.

    Deliberately HERE and not in RetireSubTabSlot, for two reasons. Correctness: this is the
    fence-gated FREE transition, so frames still drawing the closing view keep their hides until
    they retire, instead of having objects pop back mid-flight. And locking: RetireSubTabSlot runs
    under storageObjectsMutex (CloseAllInternalSubTabsLocked), so enqueueing there would nest that
    mutex inside toCopyThreadMutex - against the never-nested discipline the geometry producers
    follow, and a way to stall every render thread (they take storageObjectsMutex each frame in
    ResolveWindowViewTarget) behind a copy-thread drain. Both locks above are released by now. */
    if (freedSlots.empty()) return;
    {
        std::lock_guard<std::mutex> lock(toCopyThreadMutex);
        for (uint16_t slot : freedSlots) {
            const uint32_t bit = SubTabVisibilityBit(slot);
            if (bit == kNoSubTabBit) continue; // Defensive: every slot has a bit at MV_MAX_SUBTABS 64.
            CommandToCopyThread command;
            command.type = CommandToCopyThreadType::CLEAR_SUBTAB_HIDES;
            command.tabID = targetTab->tabID;
            command.visibilityBits = 1ull << bit;
            commandToCopyThreadQueue.push(std::move(command));
        }
    }
    toCopyThreadCV.notify_one();
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
            // The container SET the renderers iterate (10M plan Step 6). Opening a container seeds
            // it with exactly that container; the set exists so more can be added later without
            // the renderers - or the VisibilityMask - having to learn anything new.
            subTab.containers.Clear();
            subTab.containers.Add(memoryId);
            subTab.title = objectName && objectName[0] != '\0'
                ? objectName
                : VishwakarmaStorage::ObjectTypeDisplayName(entry.objectType);
            // Bind a Viewport to this SubTab and give it fresh view state (10M plan Step 6). The
            // mapping is 1:1 today; subTabSlot is what makes that a stored fact rather than an
            // assumption baked into every reader.
            Viewport& viewport = targetTab->viewports[freeSlot];
            viewport.subTabSlot = static_cast<int16_t>(freeSlot);
            viewport.camera.Initialize(); // Fresh 3D camera.
            viewport.page2DView.Reset();  // Fresh Page2D pan/zoom.
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

/* Compose a Scene3D container INTO the active SubTab's container set (graphics.md, 10M plan Step 6).
No geometry is copied: the set is a list of container IDs the renderers iterate, so the dragged
Scene3D's pages are simply drawn alongside the home container's. The active SubTab is the drop target
because the same-window drag drops onto the inline viewport, which shows the active SubTab. Mutating
under storageObjectsMutex matches how the set is seeded at open time; render threads read it lock-free
next frame (ResolveWindowViewTarget copies it by value). */
static void AddContainerToActiveSubTab(DATASETTAB* targetTab, uint64_t containerId) {
    if (!targetTab || containerId == 0 || !targetTab->storageObjectsMutex) return;

    std::lock_guard<std::mutex> lock(*targetTab->storageObjectsMutex);
    const int slot = FindPublishedSubTabSlot(*targetTab, targetTab->activeInternalSubTabMemoryId);
    if (slot < 0) return;
    InternalSubTab& subTab = targetTab->subTabs[slot];
    // Composition is Scene3D-only: a mixed-type SubTab has ambiguous renderer/interaction semantics.
    if (subTab.containerType != VishwakarmaStorage::ObjectType::Scene3D) return;
    // The dragged id must be an existing Scene3D in this tab (not a Page2D, folder or logical node).
    bool isScene3D = false;
    for (const StoredLogicalObject& entry : targetTab->storageLogicalObjects) {
        if (entry.object && entry.memoryId == containerId &&
            entry.objectType == VishwakarmaStorage::ObjectType::Scene3D) { isScene3D = true; break; }
    }
    if (!isScene3D) return;
    subTab.containers.Add(containerId); // Dedupes the home + duplicates, caps at 8.
}

// Remove a composed container from the active SubTab's set. The home/primary container defines the
// SubTab and is never removable.
static void RemoveContainerFromActiveSubTab(DATASETTAB* targetTab, uint64_t containerId) {
    if (!targetTab || containerId == 0 || !targetTab->storageObjectsMutex) return;

    std::lock_guard<std::mutex> lock(*targetTab->storageObjectsMutex);
    const int slot = FindPublishedSubTabSlot(*targetTab, targetTab->activeInternalSubTabMemoryId);
    if (slot < 0) return;
    InternalSubTab& subTab = targetTab->subTabs[slot];
    if (containerId == subTab.containerMemoryId) return; // Cannot drop the home container.
    subTab.containers.Remove(containerId);
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

/* Accumulator for BULK geometry creation. Registering one object at a time takes toCopyThreadMutex
and storageObjectsMutex once per object, and - more importantly - lets the copy thread drain between
every push, so it sees batches of a handful of commands and clones a whole 4 MB page to add each
handful. Collecting objects here and handing them over in one locked burst per queue turns that into
one clone per burst. Holding toCopyThreadMutex across the whole burst is the part that actually
groups them: the copy thread's drain loop takes the same mutex, so it cannot interleave.

Only the bulk paths use this; passing nullptr keeps the original immediate behaviour. */
struct GeneratedGeometryBatch {
    std::vector<CommandToCopyThread> copyCommands;
    std::vector<StoredGeometryObject3D> storedObjects;
    std::vector<uint64_t> memoryIds;
};

static void RegisterGeneratedGeometryElement(DATASETTAB* targetTab, VishwakarmaStorage::ObjectType objectType,
    META_DATA* object, GeometryData&& geometry, GeneratedGeometryBatch* batch = nullptr) {
    if (!targetTab || !object) return;

    if (object->memoryIDContainer == 0) {
        object->memoryIDContainer = EnsureActiveScene3D(targetTab);
    }
    object->dataType = static_cast<uint16_t>(VishwakarmaStorage::ToNumber(objectType));
    // Derived from the type, not hardcoded: this used to stamp kGeometry3DMvpSchemaVersion on every
    // 3D object, which labelled a freshly drawn LINE_MEMBER one version behind the format its own
    // payload used. Same function the load and save paths call.
    object->schemaVersion = VishwakarmaStorage::DefaultSchemaVersionForObjectType(objectType);

    /* Compose the object's local -> world matrix, exactly as the load / property-edit path does
    inside GeometryForObject. Every interactive creation path funnels through here, and all of them
    build their GeometryData by calling the type's GetGeometry() DIRECTLY - about 35 call sites
    across CreatePrimitiveGeometryElement, addRandomGeometryElement and the importers - so this is
    the one place that can give them all a correct matrix.

    graphics.md already flagged those call sites as bypassing GeometryForObject, and called it
    harmless "while new objects are unplaced". That stopped being true the moment a type emitted
    vertices in a LOCAL frame: a generator's raw GeometryData carries an IDENTITY matrix, so a
    freshly created SPHERE drew as a unit sphere at the world origin until something moved it. It
    also fixes the originally predicted defect - an object created WITH a placement now shows it
    immediately instead of only after a reload. */
    DirectX::XMStoreFloat4x4(&geometry.worldMatrix, WorldMatrixForObject(objectType, object));

    if (batch) { // Deferred: the caller hands everything over via FlushGeneratedGeometryBatch.
        batch->copyCommands.push_back({ CommandToCopyThreadType::ADD, std::move(geometry),
            object->memoryID, targetTab->tabID, object->memoryIDContainer });
        batch->storedObjects.push_back({ objectType, object->memoryID, object });
        batch->memoryIds.push_back(object->memoryID);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(toCopyThreadMutex);
        // Moved, not copied: geometry is an rvalue reference we own and nothing reads it after
        // this, so copying it deep-copied both vertex and index vectors per object.
        commandToCopyThreadQueue.push({ CommandToCopyThreadType::ADD, std::move(geometry),
            object->memoryID, targetTab->tabID, object->memoryIDContainer });
    }

    if (!targetTab->storageObjectsMutex) targetTab->storageObjectsMutex = std::make_unique<std::mutex>();
    {
        std::lock_guard<std::mutex> lock(*targetTab->storageObjectsMutex);
#ifdef _DEBUG
        // The sorted-by-memoryId invariant the id resolver binary searches on (विश्वकर्मा.h).
        // Breaking it returns the wrong object rather than failing, so it has to be said out loud.
        if (!targetTab->storageObjects3D.empty() &&
            targetTab->storageObjects3D.back().memoryId >= object->memoryID) {
            std::cout << "[3d][warn] storageObjects3D out of memoryId order: appending "
                      << object->memoryID << " after "
                      << targetTab->storageObjects3D.back().memoryId
                      << " - binary-search lookups are now invalid." << std::endl;
        }
#endif
        targetTab->storageObjects3D.push_back({ objectType, object->memoryID, object });
    }

    targetTab->allIDsInThisTab.push_back(object->memoryID);
    toCopyThreadCV.notify_one();
}

// Hand an accumulated batch over: one lock acquisition per queue for the whole burst, one notify.
// Same two-mutexes-never-nested discipline as the immediate path above.
static void FlushGeneratedGeometryBatch(DATASETTAB* targetTab, GeneratedGeometryBatch& batch) {
    if (!targetTab || batch.copyCommands.empty()) return;

    {
        std::lock_guard<std::mutex> lock(toCopyThreadMutex);
        for (CommandToCopyThread& command : batch.copyCommands) {
            commandToCopyThreadQueue.push(std::move(command));
        }
    }

    if (!targetTab->storageObjectsMutex) targetTab->storageObjectsMutex = std::make_unique<std::mutex>();
    {
        // Held briefly and once: the render thread takes this mutex every frame in
        // ResolveWindowViewTarget, so per-object locking here stalls rendering during an import.
        std::lock_guard<std::mutex> lock(*targetTab->storageObjectsMutex);
        targetTab->storageObjects3D.insert(targetTab->storageObjects3D.end(),
            batch.storedObjects.begin(), batch.storedObjects.end());
    }

    targetTab->allIDsInThisTab.insert(targetTab->allIDsInThisTab.end(),
        batch.memoryIds.begin(), batch.memoryIds.end());

    batch.copyCommands.clear();
    batch.storedObjects.clear();
    batch.memoryIds.clear();
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
    // Both the validation snapshot and the incoming value are in the same space the pane showed -
    // WORLD for point components. Every rule is placement-invariant, so the verdict is unchanged.
    double values[16] = {};
    const uint8_t count = table->fieldCount;
    ReadPropertyValuesForDisplay(*table, object, values);
    if (!ValidatePropertyEdit(*table, values, count, fieldIndex, value)) return;

    {
        // Hold storageObjectsMutex only for the store, so the render thread (which takes it every
        // frame) is not stalled by geometry generation.
        std::lock_guard<std::mutex> lock(*myTab->storageObjectsMutex);
        ApplyPropertyValueFromDisplay(*table, object, fieldIndex, value);
        object->dataVersion++;
    }

    GeometryData geo; // Regenerate with no lock held.
    if (!GeometryForObject(objectType, object, geo)) return;

    {
        std::lock_guard<std::mutex> lock(toCopyThreadMutex);
        commandToCopyThreadQueue.push({ CommandToCopyThreadType::MODIFY, std::move(geo), object->memoryID,
            myTab->tabID, object->memoryIDContainer });
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
        std::lock_guard<std::mutex> lock(tab.selection.selectedMutex);
        tab.selection.selectedObjectIds.clear();
        if (objectId != 0) tab.selection.selectedObjectIds.push_back(objectId); // Single-select.
        /* Selecting deliberately does NOT move the camera. It used to recenter the orbit target on
        the picked object's CG, which meant every click re-framed the whole scene - fine for the
        first pick, disorienting for the tenth. Picking is now a pure selection change; the view is
        moved only by explicit navigation (orbit / wheel) or the Zoom commands. `cg` stays in the
        signature because the pick resolve already computes it and the scroll-to-surface path below
        is the other consumer of the same result. */
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
        /* Vertices come out of the generator in AUTHORED space, so a placed object has to be
        carried to world space before it can be framed - otherwise zoom-to-fit would point the
        camera at where the object was drawn rather than where it is displayed. The vertex shader
        applies exactly this matrix, so doing it here keeps the fit consistent with what is on
        screen. Identity for anything that has never been moved, which is the common case. */
        const DirectX::XMMATRIX world = DirectX::XMLoadFloat4x4(&geometry.worldMatrix);
        auto framePoint = [&](DirectX::FXMVECTOR localPosition) {
            const DirectX::XMVECTOR worldPosition =
                DirectX::XMVector3Transform(localPosition, world);
            DirectX::XMVECTOR offset = DirectX::XMVectorSubtract(worldPosition, target);
            const float alongForward = DirectX::XMVectorGetX(DirectX::XMVector3Dot(offset, forward));
            const float alongRight = DirectX::XMVectorGetX(DirectX::XMVector3Dot(offset, right));
            const float alongUp = DirectX::XMVectorGetX(DirectX::XMVector3Dot(offset, viewUp));
            // With the camera at distance d behind the target, the point's depth is d + alongForward
            // and it is inside the frustum when |lateral| <= depth * tanHalfFov. Solve for minimum d.
            requiredDistance = (std::max)(requiredDistance, std::abs(alongRight) / tanHalfFovX - alongForward);
            requiredDistance = (std::max)(requiredDistance, std::abs(alongUp) / tanHalfFovY - alongForward);
            requiredDistance = (std::max)(requiredDistance, cam.nearZ - alongForward);
            hasPoints = true;
        };

        /* An INSTANCED object has NO vertices to walk - its shape lives in the global primitive
        library - so framing it needs the eight corners of that shape's canonical AABB instead.
        Without this branch a sphere-only scene contributes nothing at all and zoom-to-fit becomes a
        silent no-op. Note this is a SEPARATE code path from the registry's world-centre shadow,
        which has the same gap for the same reason and is fixed independently on the copy thread. */
        if (geometry.libraryShapeId >= 0) {
            const PrimitiveLibraryEntry& entry =
                gpu.primitiveLibrary.table.At(geometry.libraryShapeId, kPrimitiveFixedLod);
            for (int corner = 0; corner < 8; ++corner) {
                framePoint(DirectX::XMVectorSet(
                    (corner & 1) ? entry.maxX : entry.minX,
                    (corner & 2) ? entry.maxY : entry.minY,
                    (corner & 4) ? entry.maxZ : entry.minZ, 1.0f));
            }
            continue;
        }
        for (const Vertex& vertex : geometry.vertices) {
            framePoint(DirectX::XMLoadFloat3(&vertex.position));
        }
    }
    if (!hasPoints) return;

    // Same distance clamping as the mouse-wheel zoom.
    const float newDistance = std::clamp(requiredDistance, 1.0f, cam.farZ - 10.0f);
    DirectX::XMStoreFloat3(&cam.position,
        DirectX::XMVectorSubtract(target, DirectX::XMVectorScale(forward, newDistance)));
    myTab->selection.lastNavInteractionMs.store(GetTickCount64(), std::memory_order_release);
}

/* Per-object hide / show inside the input view's Scene3D (graphics.md, 10M plan Step 5).

The engineering thread decides WHICH objects change; the copy thread owns the membership word and
does the bit arithmetic. All that crosses between them is one SET_VISIBILITY per affected object,
and each of those costs one aligned 8-byte write - no geometry page cloned, no argument buffer
rebuilt, no snapshot published. That is the entire point: hiding half of a ten-million-object scene
must not touch geometry at all.

The bit is the sub-tab SLOT, so a hide applies to the view the user is looking at rather than to
every view of the same Scene3D.

Objects are filtered through SubTabDrawsContainer, i.e. by the sub-tab's whole container SET, so a
composed container hides along with the home one - matching what the draw and pick paths already
walk. Filtering by the home containerMemoryId instead (which this did until the set existed) let an
object in a composed container be selected and then silently refuse to hide.

Each action touches only the objects it names - "Hide Selected" does not silently un-hide everything
else - so the three compose the way a user expects. */
/* Does the sub-tab in `subTabSlot` draw objects parented to `containerMemoryId`?

Every producer acting on "what the user is looking at" must ask THIS, not compare against the
sub-tab's home containerMemoryId. Step 6 turned a sub-tab's content from one container into a SET
(drag a Scene3D onto the view and it is composed in by reference), and the draw and pick paths were
updated while the move and hide producers were not - so an object in a composed container could be
selected and would then silently refuse to move or hide (graphics.md, 10M plan Step 6).

The empty-set fallback to the home container mirrors ResolveWindowViewTarget exactly: a sub-tab
whose set was never populated still behaves as it always did. */
static bool SubTabDrawsContainer(const DATASETTAB& tab, int subTabSlot, uint64_t containerMemoryId) {
    if (containerMemoryId == 0 || subTabSlot < 0 || subTabSlot >= MV_MAX_SUBTABS) return false;
    const InternalSubTab& subTab = tab.subTabs[subTabSlot];
    if (subTab.containers.Empty()) return containerMemoryId == subTab.containerMemoryId;
    return subTab.containers.Contains(containerMemoryId);
}

enum class SceneVisibilityAction { HideSelected, HideUnselected, ShowAll };

static void ApplySceneVisibilityAction(DATASETTAB* myTab, SceneVisibilityAction action) {
    if (!myTab) return;
    const int viewSlot = InputViewSlot(*myTab);
    if (viewSlot < 0) return;
    if (myTab->subTabs[viewSlot].containerType != VishwakarmaStorage::ObjectType::Scene3D) return;
    const uint32_t bit = SubTabVisibilityBit(viewSlot);
    if (bit == kNoSubTabBit) return; // Defensive: every slot has a bit at MV_MAX_SUBTABS 64.
    if (myTab->subTabs[viewSlot].containerMemoryId == 0) return;

    std::vector<uint64_t> selected;
    if (action != SceneVisibilityAction::ShowAll) {
        std::lock_guard<std::mutex> lock(myTab->selection.selectedMutex);
        selected = myTab->selection.selectedObjectIds;
    }
    // Hiding nothing is not the same as hiding everything: with an empty selection both hide
    // actions are no-ops rather than blanking the view or hiding the whole model.
    if (action != SceneVisibilityAction::ShowAll && selected.empty()) return;

    // The engineering thread is the sole writer of storageObjects3D, so iteration needs no lock.
    std::vector<CommandToCopyThread> commands;
    for (const StoredGeometryObject3D& stored : myTab->storageObjects3D) {
        if (!stored.object) continue;
        // The whole container SET the sub-tab draws, so a composed container hides too.
        if (!SubTabDrawsContainer(*myTab, viewSlot, stored.object->memoryIDContainer)) continue;
        const bool isSelected =
            std::find(selected.begin(), selected.end(), stored.memoryId) != selected.end();
        if (action == SceneVisibilityAction::HideSelected && !isSelected) continue;
        if (action == SceneVisibilityAction::HideUnselected && isSelected) continue;

        CommandToCopyThread command;
        command.type = CommandToCopyThreadType::SET_VISIBILITY;
        command.id = stored.memoryId;
        command.tabID = myTab->tabID;
        command.containerMemoryId = stored.object->memoryIDContainer;
        command.visibilityBits = 1ull << bit;
        command.visibilityVisible = action == SceneVisibilityAction::ShowAll;
        commands.push_back(std::move(command));
    }
    if (commands.empty()) return;

    {   // One lock for the whole burst, like FlushGeneratedGeometryBatch: the copy thread's drain
        // takes this same mutex, so holding it across the push is what keeps them in one batch.
        std::lock_guard<std::mutex> lock(toCopyThreadMutex);
        for (CommandToCopyThread& command : commands) {
            commandToCopyThreadQueue.push(std::move(command));
        }
    }
    toCopyThreadCV.notify_one();
}

/* THE MOVE PRODUCER (graphics.md, 10M plan Step 4 - the last missing piece of the Phase 5 hot-drag
item). Translates the current selection by writing each object's rigid PLACEMENT and emitting a
TRANSFORM-ONLY MODIFY: a GeometryData carrying a world matrix but no vertices and no indices, which
is exactly what IsTransformOnlyEdit recognises.

What that buys, and why the whole placement design exists: the copy thread answers such a command
with one fresh 64-byte instance record plus one naturally-aligned 4-byte redirect flip. No geometry
page is cloned, no argument buffer rebuilt, no snapshot published - so moving a thousand objects
scattered over a thousand pages costs ~68 KB of writes instead of ~4 GB of page cloning. Regenerating
geometry instead (which is what a MODIFY carrying vertices does) would clone every page touched.

The delta ACCUMULATES into the existing placement rather than replacing it, so repeated moves
compose and an object that was already placed is translated from where it actually is.

Locking follows the property-edit path exactly: mutate under storageObjectsMutex (render threads read
these objects every frame), release it, then push under toCopyThreadMutex. The two are never nested -
taking the copy-thread mutex while holding the storage mutex would stall every render thread behind a
copy-thread drain. */
// Returns how many objects were actually moved, so a caller (the debug key) can report the truth
// rather than assuming the call did something - an empty selection makes this a silent no-op.
static size_t TranslateSelectedSceneObjects(DATASETTAB* myTab, const XMFLOAT3& delta) {
    if (!myTab) return 0;
    const int viewSlot = InputViewSlot(*myTab);
    if (viewSlot < 0) return 0;
    if (myTab->subTabs[viewSlot].containerType != VishwakarmaStorage::ObjectType::Scene3D) return 0;
    if (myTab->subTabs[viewSlot].containerMemoryId == 0) return 0;

    std::vector<uint64_t> selected;
    {
        std::lock_guard<std::mutex> lock(myTab->selection.selectedMutex);
        selected = myTab->selection.selectedObjectIds;
    }
    if (selected.empty()) return 0; // Moving nothing is a no-op, not "move everything".

    // The engineering thread is the sole writer of storageObjects3D, so iteration needs no lock;
    // only the placement WRITE below does.
    std::vector<CommandToCopyThread> commands;
    for (const StoredGeometryObject3D& stored : myTab->storageObjects3D) {
        if (!stored.object) continue;
        // The whole container SET the sub-tab draws, so a composed container moves too.
        if (!SubTabDrawsContainer(*myTab, viewSlot, stored.object->memoryIDContainer)) continue;
        if (std::find(selected.begin(), selected.end(), stored.memoryId) == selected.end()) continue;

        Placement3D* placement = PlacementForObject(stored.objectType, stored.object);
        if (!placement) continue; // Type carries no placement; nothing to move.

        GeometryData transformOnly; // No vertices, no indices: THE transform-only encoding.
        transformOnly.id = stored.memoryId;
        {
            std::lock_guard<std::mutex> lock(*myTab->storageObjectsMutex);
            placement->origin.x += delta.x;
            placement->origin.y += delta.y;
            placement->origin.z += delta.z;
            stored.object->dataVersion++;
            /* WorldMatrixForObject, NOT placement->ToMatrix(). The placement is only the
            authored -> world half; composing it alone silently drops the type's local -> authored
            half, which is identity for every generator that bakes world coordinates into its
            vertices - and is NOT identity for a canonical-frame type. Building it from the
            placement alone redrew a moved SPHERE as a unit sphere at the placement origin, having
            discarded its radius and centre. Same composition as the geometry path, by construction:
            both call this one function. */
            XMStoreFloat4x4(&transformOnly.worldMatrix,
                WorldMatrixForObject(stored.objectType, stored.object));
        }

        CommandToCopyThread command;
        command.type = CommandToCopyThreadType::MODIFY;
        command.geometry = std::move(transformOnly);
        command.id = stored.memoryId;
        command.tabID = myTab->tabID;
        command.containerMemoryId = stored.object->memoryIDContainer;
        commands.push_back(std::move(command));
    }
    if (commands.empty()) return 0;

    const size_t moved = commands.size();
    {   // One lock for the whole burst so the copy thread drains them as a single batch.
        std::lock_guard<std::mutex> lock(toCopyThreadMutex);
        for (CommandToCopyThread& command : commands) {
            commandToCopyThreadQueue.push(std::move(command));
        }
    }
    toCopyThreadCV.notify_one();
    return moved;
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
        shape->center = placementPoint; // The centre IS the stored anchor now - nothing to average.
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
    case VishwakarmaStorage::ObjectType::Elbow: {
        ELBOW* shape = new (targetTab->tabNo) ELBOW();
        shape->Randomize();
        shape->center = placementPoint;
        geometry = shape->GetGeometry();
        object = shape;
        break;
    }
    case VishwakarmaStorage::ObjectType::Tee: {
        TEE* shape = new (targetTab->tabNo) TEE();
        shape->Randomize();
        const XMFLOAT3 offset = OffsetTo(MidPoint(shape->center1, shape->center2), placementPoint);
        TranslatePoint(shape->center1, offset);
        TranslatePoint(shape->center2, offset);
        geometry = shape->GetGeometry();
        object = shape;
        break;
    }
    case VishwakarmaStorage::ObjectType::Flange: {
        FLANGE* shape = new (targetTab->tabNo) FLANGE();
        shape->Randomize();
        const XMFLOAT3 offset = OffsetTo(MidPoint(shape->center1, shape->center2), placementPoint);
        TranslatePoint(shape->center1, offset);
        TranslatePoint(shape->center2, offset);
        geometry = shape->GetGeometry();
        object = shape;
        break;
    }
    case VishwakarmaStorage::ObjectType::LineMember: {
        LINE_MEMBER* shape = new (targetTab->tabNo) LINE_MEMBER();
        shape->Randomize();
        const XMFLOAT3 offset = OffsetTo(MidPoint(shape->point1, shape->point2), placementPoint);
        TranslatePoint(shape->point1, offset);
        TranslatePoint(shape->point2, offset);
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

// Materializes a validated STAAD import: nodes as spheres, profile-mapped members as
// LINE_MEMBERs, remaining members as placeholder pipes. Runs on the engineering
// thread — the only writer of model data. The IPC and validation live in
// ExtensionCommunications.cpp.
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
    constexpr float kMemberOutsideDiameter = 0.25f; // For members without a mapped profile.
    constexpr float kMemberInsideDiameter = 0.10f;

    // Designation -> catalog row for profile-mapped members. All STAAD-name mapping happens
    // worker-side (profile_mapping.py); this is only an exact lookup into the embedded
    // catalog. Duplicate designations across codes (JIS/KS mirrors) keep the first row —
    // identical geometry by design.
    std::unordered_map<std::string, const SteelProfileRecord*> profileByDesignation;
    profileByDesignation.reserve(kSteelProfileCount);
    for (uint32_t i = 0; i < kSteelProfileCount; ++i) {
        profileByDesignation.emplace(kSteelProfiles[i].designation, &kSteelProfiles[i]);
    }
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

    size_t createdLineMembers = 0, createdPipes = 0;
    for (const auto& member : model->members) {
        const auto start = nodePositions.find(member.startNodeId);
        const auto end = nodePositions.find(member.endNodeId);
        if (start == nodePositions.end() || end == nodePositions.end()) continue;
        const float dx = end->second.x - start->second.x;
        const float dy = end->second.y - start->second.y;
        const float dz = end->second.z - start->second.z;
        if (dx * dx + dy * dy + dz * dz < 1e-8f) continue; // Zero-length member: no axis.

        const SteelProfileRecord* profile = nullptr;
        if (!member.profileDesignation.empty()) {
            const auto found = profileByDesignation.find(member.profileDesignation);
            if (found != profileByDesignation.end()) profile = found->second;
        }
        if (profile) {
            LINE_MEMBER* shape = new (myTab->tabNo) LINE_MEMBER();
            shape->point1 = start->second;
            shape->point2 = end->second;
            shape->profileId = profile->id;
            shape->userParameter1 = static_cast<float>(member.userParameter1 * 1000.0); // Wire meters -> stored mm.
            shape->userParameter2 = static_cast<float>(member.userParameter2 * 1000.0);
            shape->colorMain = memberColor;
            shape->colorInner = memberColor;
            shape->colorCap = memberColor;
            RegisterGeneratedGeometryElement(myTab, LINE_MEMBER::storageObjectType, shape, shape->GetGeometry());
            ++createdLineMembers;
            continue;
        }

        PIPE* shape = new (myTab->tabNo) PIPE();
        shape->center1 = start->second;
        shape->center2 = end->second;
        shape->outsideDiameter = kMemberOutsideDiameter;
        shape->insideDiameter = kMemberInsideDiameter;
        shape->colorOuter = memberColor;
        shape->colorInner = memberColor;
        shape->colorCap = memberColor;
        RegisterGeneratedGeometryElement(myTab, PIPE::storageObjectType, shape, shape->GetGeometry());
        ++createdPipes;
    }

    std::cout << "[std-importer] Created " << model->nodes.size() << " node spheres, "
              << createdLineMembers << " profile members and "
              << createdPipes << " placeholder pipes." << std::endl; // Flush: rare event, aids diagnosis.
}

// Materializes a validated DXF import into the currently open Page2D through
// the same copy-thread queue interactive 2D creation uses. Import policy: the
// content goes into the *active* Page2D sub-tab only; abort when none is open
// (the UI pre-checks too, but the state can change while the action is queued).
// Owner for engineering-thread message boxes: with a null owner the box can open BEHIND the
// main window, invisibly blocking this thread (and every queued action after it).
static HWND FirstWindowHandleForEngineeringDialogs() {
    uint16_t* windowList = publishedWindowIndexes.load(std::memory_order_acquire);
    const uint16_t windowCount = publishedWindowCount.load(std::memory_order_acquire);
    return windowCount > 0 ? allWindows[windowList[0]].hWnd : nullptr;
}

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
        MessageBoxA(FirstWindowHandleForEngineeringDialogs(), message, "DXF import",
            MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
        return;
    }

    std::string error;
    std::unique_ptr<ExtensionCommunications::ImportedPage2DContent> content(
        ExtensionCommunications::RunQueuedDxfImport(payloadId, error));
    if (!content) {
        std::cout << "[dxf-importer] " << error << "\n";
        MessageBoxA(FirstWindowHandleForEngineeringDialogs(), error.c_str(), "DXF import failed",
            MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        return;
    }

#ifdef _DEBUG
    std::cout << "[dxf-import][dbg] received total="
              << (content->lines.size() + content->texts.size() + content->polygons.size() +
                  content->assetDefinitions.size() + content->assetInserts.size())
              << " (lines=" << content->lines.size() << ", texts=" << content->texts.size()
              << ", polygons=" << content->polygons.size()
              << ", assetDefs=" << content->assetDefinitions.size()
              << ", assetInserts=" << content->assetInserts.size()
              << ") -> container " << pageMemoryId << std::endl;
#endif

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

    // DXF blocks -> Asset2D. Each definition's master geometry (block frame) is stored hidden;
    // each insert stamps an instance onto the page with its scale / rotation baked into the
    // members: Cad2DInstantiateAsset maps member = insert + R(rot) * S(scale) * (master - base).
    std::unordered_map<uint32_t, uint64_t> definitionByKey;
    definitionByKey.reserve(content->assetDefinitions.size());
    for (const auto& definition : content->assetDefinitions) {
        std::vector<Cad2DLineRecordCPU> masterLines;
        std::vector<Cad2DTextRecordCPU> masterTexts;
        std::vector<Cad2DPolygonRecordCPU> masterPolygons;
        masterLines.reserve(definition.lines.size());
        masterTexts.reserve(definition.texts.size());
        masterPolygons.reserve(definition.polygons.size());
        for (const auto& line : definition.lines) {
            Cad2DLineRecordCPU record{};
            record.x1 = line.x1; record.y1 = line.y1; record.x2 = line.x2; record.y2 = line.y2;
            record.lineWeight = 1.0f;
            record.lineWeightMode = Cad2DLineWeightMode::ScreenPixel;
            record.colorABGR = 0xFF000000u;
            record.schemaVersion = VishwakarmaStorage::kGeometry2DLineSchemaVersion;
            masterLines.push_back(record);
        }
        for (const auto& text : definition.texts) {
            Cad2DTextRecordCPU record{};
            record.x = text.x; record.y = text.y;
            record.textHeightCU = text.heightCU;
            record.rotationRadians = text.rotationRadians;
            record.colorABGR = 0xFF000000u;
            record.font = 0;
            record.justification = static_cast<Cad2DTextJustification>(text.justification);
            record.text = text.textUtf8;
            record.schemaVersion = VishwakarmaStorage::kGeometry2DTextSchemaVersion;
            masterTexts.push_back(std::move(record));
        }
        for (const auto& polygon : definition.polygons) {
            Cad2DPolygonRecordCPU record{};
            record.lineSegmentCount = polygon.segmentCount;
            record.centerX = polygon.centerX; record.centerY = polygon.centerY;
            record.radius = polygon.radius;
            record.rotationDegrees = polygon.rotationDegrees;
            record.lineWeight = 1.0f;
            record.lineWeightMode = Cad2DLineWeightMode::ScreenPixel;
            record.colorABGR = 0xFF000000u;
            record.schemaVersion = VishwakarmaStorage::kGeometry2DPolygonSchemaVersion;
            masterPolygons.push_back(record);
        }
        const uint64_t definitionId = Cad2DCreateAssetDefinition(*myTab, definition.baseX,
            definition.baseY, masterLines, masterTexts, masterPolygons);
        if (definitionId != 0) definitionByKey[definition.key] = definitionId;
    }

    size_t placedInserts = 0;
    for (const auto& insert : content->assetInserts) {
        auto it = definitionByKey.find(insert.key);
        if (it == definitionByKey.end()) continue;
        if (Cad2DInstantiateAsset(*myTab, pageMemoryId, it->second, insert.x, insert.y,
                insert.scaleX, insert.scaleY, insert.rotationDegrees)) {
            ++placedInserts;
        }
    }

    std::cout << "[dxf-importer] Created " << content->lines.size() << " lines, "
              << content->texts.size() << " texts, " << content->polygons.size()
              << " polygons, " << definitionByKey.size() << " asset definitions and "
              << placedInserts << " inserts in the open Page2D." << std::endl; // Flush: rare event, aids diagnosis.

#ifdef _DEBUG
    // Queued after every element above: the copy thread reports counts + bounding box
    // once the whole import has actually been ingested into the CPU records.
    EnqueueCad2DIngestStatsReport(myTab->tabID, pageMemoryId);
#endif
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
         tab.cad2d->assetInsertMode.load(std::memory_order_acquire) ||
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

// Pass a batch to defer the hand-over (bulk creation); nullptr registers immediately as before.
inline void addRandomGeometryElement(DATASETTAB* targetTab, GeneratedGeometryBatch* batch = nullptr) {
	if (!targetTab) return; //Safety against NULL pointer dereference.
    GeometryData geometry;// These will hold the data of the randomly created shape.
    META_DATA* object = nullptr;
    VishwakarmaStorage::ObjectType objectType = VishwakarmaStorage::ObjectType::Unknown;

    // Randomly select a shape type (0-14 for the 15 shapes available).
    // We use the GetRNG() helper function already available in "डेटा-सामान्य-3D.h".
    std::uniform_int_distribution<int> shapeDist(0, 14);
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
    case 9: {
        TORUS* shape = new (targetTab->tabNo) TORUS();
        shape->Randomize();
        geometry = shape->GetGeometry();
        object = shape;
        objectType = TORUS::storageObjectType;
        break;
    }
    case 10: {
        ELLIPSOID* shape = new (targetTab->tabNo) ELLIPSOID();
        shape->Randomize();
        geometry = shape->GetGeometry();
        object = shape;
        objectType = ELLIPSOID::storageObjectType;
        break;
    }
    case 11: {
        ELBOW* shape = new (targetTab->tabNo) ELBOW();
        shape->Randomize();
        geometry = shape->GetGeometry();
        object = shape;
        objectType = ELBOW::storageObjectType;
        break;
    }
    case 12: {
        TEE* shape = new (targetTab->tabNo) TEE();
        shape->Randomize();
        geometry = shape->GetGeometry();
        object = shape;
        objectType = TEE::storageObjectType;
        break;
    }
    case 13: {
        FLANGE* shape = new (targetTab->tabNo) FLANGE();
        shape->Randomize();
        geometry = shape->GetGeometry();
        object = shape;
        objectType = FLANGE::storageObjectType;
        break;
    }
    case 14: {
        LINE_MEMBER* shape = new (targetTab->tabNo) LINE_MEMBER();
        shape->Randomize();
        geometry = shape->GetGeometry();
        object = shape;
        objectType = LINE_MEMBER::storageObjectType;
        break;
    }
    }
    RegisterGeneratedGeometryElement(targetTab, objectType, object, std::move(geometry), batch);
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
            // Every Viewport onto a Scene3D orbits its own camera independently.
            uint16_t* orbitList = myTab->publishedSubTabIndexes.load(std::memory_order_acquire);
            const uint16_t orbitCount = myTab->publishedSubTabCount.load(std::memory_order_acquire);
            for (uint16_t i = 0; orbitList && i < orbitCount; ++i) {
                const uint16_t slot = orbitList[i];
                if (myTab->subTabs[slot].containerType == VishwakarmaStorage::ObjectType::Scene3D) {
                    UpdateCameraOrbit(myTab->viewports[slot].camera);
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
        float dx, dy, vx, vy, vz;

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
                // Check if in render area (vs UI): compare input.x/y against the Viewport's rect.
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
                else if (input.x == 17) {myTab->isCtrlDown = true;} // CTRL (VK_CONTROL)
                break;
            case ACTION_TYPE::KEYUP:
                if (input.x == 18) { myTab->isAltDown = false;}// 18 is VK_MENU (ALT)
                else if (input.x == 16) { myTab->isShiftDown = false; } // SHIFT
                else if (input.x == 17) { myTab->isCtrlDown = false; } // CTRL (VK_CONTROL)
                break;

            case ACTION_TYPE::CHAR:
				//Temporary Debug Key: Toggle Auto Camera Rotation with "r" key.
                if (input.x == 82 || input.x == 114) // 'r' & "R"
                {
                    myTab->autoCameraRotation = !myTab->autoCameraRotation; 
                }
				if (input.x == 67 || input.x == 99) { cam.Initialize(); } // 'c' & "C". Reset camera.
                // Temporary Debug Key: bulk-generate geometry with "g", to exercise the copy
                // thread's import path at scale (graphics.md, 10M plan). Shift+G goes 10x bigger.
                // Everything lands in ONE queue push burst, so it drives the drain cap, the upload
                // ring and the per-chunk publish exactly the way a real model import does.
                if (input.x == 71 || input.x == 103) { // 'G' & 'g'
                    const int bulkCount = (input.x == 71) ? 100000 : 10000;
                    // Hand the copy thread 100 objects at a time. One-at-a-time registration let
                    // it drain ~5 commands per batch and clone a whole page for each handful:
                    // ~10 GB of clone traffic for 10k objects. At 100 per burst that is one clone
                    // per 100 objects instead of one per 5.
                    constexpr size_t kStressBurst = 100;
                    const auto bulkStart = std::chrono::steady_clock::now();
                    GeneratedGeometryBatch bulkBatch;
                    for (int i = 0; i < bulkCount; ++i) {
                        addRandomGeometryElement(myTab, &bulkBatch);
                        if (bulkBatch.copyCommands.size() >= kStressBurst) {
                            FlushGeneratedGeometryBatch(myTab, bulkBatch);
                        }
                    }
                    FlushGeneratedGeometryBatch(myTab, bulkBatch); // Partial tail burst.
                    const auto bulkMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - bulkStart).count();
                    std::cout << "[gpu][stress] queued " << bulkCount << " objects in "
                              << bulkMs << " ms (bursts of " << kStressBurst << ")" << std::endl;
                }
                /* Temporary Debug Key: "m" moves every object in the active Scene3D, as a
                TRANSFORM-ONLY edit - a world matrix with EMPTY vertex/index vectors, which is
                exactly what IsTransformOnlyEdit detects (graphics.md, 10M plan Step 4). Sibling of
                the "g" stress key above, and kept for the same reason: no shipping producer emits
                these yet (every generator bakes world positions into its vertices), so this is the
                only way to exercise the move fast path. Watch the heartbeat - `moves` must climb
                while `clones` and `cloneMB` stay perfectly flat. It is what caught Pass 1 and the
                append-candidate scan still dragging a page into the clone set. */
                if (input.x == 77 || input.x == 109) { // 'M' & 'm'
                    const uint64_t container = InputViewContainerId(*myTab);
                    std::vector<CommandToCopyThread> moves;
                    // Each press must send a DIFFERENT matrix, else passes 2+ rewrite the same
                    // transform and nothing visibly changes - which would make "clones stayed flat"
                    // indistinguishable from "the move path silently does nothing".
                    static int movePass = 0;
                    ++movePass;
                    if (container != 0) {
                        for (const StoredGeometryObject3D& stored : myTab->storageObjects3D) {
                            if (!stored.object || stored.object->memoryIDContainer != container) continue;
                            GeometryData transformOnly; // No vertices, no indices: THE encoding.
                            transformOnly.id = stored.memoryId;
                            DirectX::XMStoreFloat4x4(&transformOnly.worldMatrix,
                                DirectX::XMMatrixTranslation(0.0f, 0.0f, movePass * 4.0f));
                            CommandToCopyThread command;
                            command.type = CommandToCopyThreadType::MODIFY;
                            command.geometry = std::move(transformOnly);
                            command.id = stored.memoryId;
                            command.tabID = myTab->tabID;
                            command.containerMemoryId = container;
                            moves.push_back(std::move(command));
                        }
                        std::lock_guard<std::mutex> lock(toCopyThreadMutex);
                        for (CommandToCopyThread& command : moves) {
                            commandToCopyThreadQueue.push(std::move(command));
                        }
                    }
                    toCopyThreadCV.notify_one();
                    std::cout << "[gpu][stress] queued " << moves.size()
                              << " transform-only moves" << std::endl;
                }
                /* Temporary Debug Key: "n" re-meshes every object in the active Scene3D - a
                GEOMETRY-CHANGED MODIFY, which soft-deletes the object's placement record and
                appends a fresh one. That is the only thing in the app that manufactures holeBytes,
                so it is how the page-compaction threshold gets exercised (graphics.md,
                "Defragmentation logic"): press once to punch the holes, again to watch `compacted`
                tick while cloneMB grows by far less than a whole page. */
                if (input.x == 78 || input.x == 110) { // 'N' & 'n'
                    const uint64_t container = InputViewContainerId(*myTab);
                    std::vector<CommandToCopyThread> remesh;
                    /* Re-mesh HALF the objects, alternating which half each press. Re-meshing all
                    of them proves nothing: every object leaves its page, the page drains to
                    objectCount 0 and the empty-page GC drops it, so holes never coexist with
                    survivors. Alternating halves leaves each page ~50% holes AND still populated,
                    and the next press touches the survivors - which is what re-clones that page
                    and lets the threshold fire. (Holes are added to the CLONE in Pass 3, while the
                    compaction decision reads the OLD page in Pass 2, so the punch and the compact
                    can never be the same press.) */
                    static int remeshPass = 0;
                    const int phase = remeshPass++ % 2;
                    int objectIndex = 0;
                    if (container != 0) {
                        for (const StoredGeometryObject3D& stored : myTab->storageObjects3D) {
                            if (!stored.object || stored.object->memoryIDContainer != container) continue;
                            if ((objectIndex++ % 2) != phase) continue;
                            GeometryData geo;
                            if (!GeometryForObject(stored.objectType, stored.object, geo)) continue;
                            CommandToCopyThread command;
                            command.type = CommandToCopyThreadType::MODIFY;
                            command.geometry = std::move(geo);
                            command.id = stored.memoryId;
                            command.tabID = myTab->tabID;
                            command.containerMemoryId = container;
                            remesh.push_back(std::move(command));
                        }
                        std::lock_guard<std::mutex> lock(toCopyThreadMutex);
                        for (CommandToCopyThread& command : remesh) {
                            commandToCopyThreadQueue.push(std::move(command));
                        }
                    }
                    toCopyThreadCV.notify_one();
                    std::cout << "[gpu][stress] queued " << remesh.size()
                              << " geometry re-meshes" << std::endl;
                }
                // Temporary Debug Key: "k" toggles the GPU draw-command compaction path
                // (gUseComputeCull) against the legacy direct-ExecuteIndirect path, for A/B parity
                // checking (graphics.md, 10M plan Step 7 slice). Hide a few objects, then flip this
                // and confirm the image is identical - hidden objects are dropped pre-raster now.
                if (input.x == 75 || input.x == 107) { // 'K' & 'k'
                    gUseComputeCull = !gUseComputeCull;
                    std::cout << "[gpu][stress] compute cull "
                              << (gUseComputeCull ? "ON" : "OFF") << std::endl;
                }
                /* Temporary Debug Key: "l" pins every instanced object to the CPU-chosen LOD instead
                of picking one per frame from projected screen size (graphics.md, "Shared geometry
                and the primitive libraries", Step 2). Zoom in and out with this ON and OFF: pinned
                keeps one tessellation at every distance, unpinned should coarsen as objects shrink
                and refine as they grow, with the silhouette staying round throughout. */
                if (input.x == 76 || input.x == 108) { // 'L' & 'l'
                    gLodPinned = !gLodPinned;
                    std::cout << "[gpu][stress] instanced LOD "
                              << (gLodPinned ? "PINNED to CPU level" : "per-frame from screen size")
                              << std::endl;
                }
                /* Temporary Debug Key: "v" translates the SELECTION by +2 in Z through the real
                producer path - it writes each object's placement and emits a transform-only MODIFY
                (graphics.md, 10M plan Step 4). Unlike the older "m" key, which fabricates a raw
                world matrix for every object in the container without touching stored state, this
                goes through TranslateSelectedSceneObjects, so the move PERSISTS and survives a
                save/reload. Acceptance is the heartbeat: `moves` must climb while `clones` and
                `cloneMB` stay perfectly flat, and the selected objects must visibly rise. Repeated
                presses accumulate, which is what proves the delta composes onto the existing
                placement rather than replacing it. */
                /* Temporary Debug Key: "b" appends lines to the active Page2D on a deterministic
                grid, to measure and then to beat the Page2D rebuild (id.md §11, step 2a). Shift+B
                is the sheet - ten presses make the million-line drawing - and plain "b" is the
                measured event, the five appended lines §8 step 2 states its criterion in. Watch
                the [cad2d][perf] line the copy thread prints: today `cmds=5` sits next to
                `expanded=1000000`, and that ratio is the whole point of the step. */
                if (input.x == 66 || input.x == 98) { // 'B' & 'b'
                    Cad2DGenerateBulkLines(*myTab, (input.x == 66) ? 100000u : 5u);
                }
                /* Temporary Debug Key: "e" MODIFIES lines already on the active Page2D, which is
                what makes holes - every modify appends a new run and hides the old one (id.md §11,
                step 2e). Shift+E moves 1,000 of them; plain "e" moves EVERY line, which is the
                only way to take a page to 100% holes in one batch and so the only way to see
                compaction hand a whole page back. Acceptance is two numbers in the [cad2d][perf]
                line: `holes=` must climb and then fall back when `packed=` fires, and `pages=`
                must stay balanced (or retire more than it builds) instead of growing. */
                if (input.x == 69 || input.x == 101) { // 'E' & 'e'
                    Cad2DModifyBulkLines(*myTab, (input.x == 69) ? 1000u : 0u);
                }
                if (input.x == 86 || input.x == 118) { // 'V' & 'v'
                    // Report the COUNT, not the intent: with an empty selection this is a no-op,
                    // and a message that claims otherwise makes a missed selection look like a
                    // broken move path (which is exactly how it read the first time).
                    const size_t moved =
                        TranslateSelectedSceneObjects(myTab, XMFLOAT3{ 0.0f, 0.0f, 2.0f });
                    std::cout << "[gpu][stress] placement move: " << moved
                              << " object(s) translated by +2 Z" << std::endl;
                }
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
            } else if (nextWorkTODO.actionType == ACTION_TYPE::ADD_CONTAINER_TO_SUBTAB) {
                AddContainerToActiveSubTab(myTab, nextWorkTODO.objectId);
            } else if (nextWorkTODO.actionType == ACTION_TYPE::REMOVE_CONTAINER_FROM_SUBTAB) {
                RemoveContainerFromActiveSubTab(myTab, nextWorkTODO.objectId);
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
            } else if (nextWorkTODO.actionType == ACTION_TYPE::CREATE_ASSET2D_FROM_SELECTION) {
                Cad2DCreateAssetFromSelection(*myTab);
            } else if (nextWorkTODO.actionType == ACTION_TYPE::BEGIN_ASSET_INSERT2D) {
                CancelPrimitive3DPlacement(*myTab);
                Cad2DBeginAssetInsert(*myTab);
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
            } else if (nextWorkTODO.actionType == ACTION_TYPE::TOGGLE_AUTO_RANDOM_GEOMETRY) {
                myTab->autoGenerateRandomGeometry = !myTab->autoGenerateRandomGeometry;
            } else if (nextWorkTODO.actionType == ACTION_TYPE::HIDE_SELECTED_OBJECTS) {
                ApplySceneVisibilityAction(myTab, SceneVisibilityAction::HideSelected);
            } else if (nextWorkTODO.actionType == ACTION_TYPE::HIDE_UNSELECTED_OBJECTS) {
                ApplySceneVisibilityAction(myTab, SceneVisibilityAction::HideUnselected);
            } else if (nextWorkTODO.actionType == ACTION_TYPE::HIDE_RESET_OBJECTS) {
                ApplySceneVisibilityAction(myTab, SceneVisibilityAction::ShowAll);
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
