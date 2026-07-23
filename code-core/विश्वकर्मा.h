// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.
#pragma once

#include <cstdint>
#include <vector>
#include <mutex>
#include <string>
#include "MemoryManagerCPU.h"
#include "GPUPlatformSelector.h"
#include "RenderPage2D-DirectX12.h"
#include "UserInputProcessing.h"
#include "CommonNamedNumbers.h"
#include "DataTreeView.h"

#pragma once //It prevents multiple inclusions of the same header file.

class ThreadSafeQueueCPU {
	//This class supports safer transfer of user inputs from main thread to engineering thread.
	//This also helps tabs maintain their own internal engineering todo queue. See DATASETTAB structure.
public:
    void push(ACTION_DETAILS value) {
        std::lock_guard<std::mutex> lock(mutex);
        fifoQueue.push(std::move(value));
        cond.notify_one();
    }

    // Non-blocking pop
    bool try_pop(ACTION_DETAILS& value) {
        std::lock_guard<std::mutex> lock(mutex);
        if (fifoQueue.empty()) {
            return false;
        }
        value = std::move(fifoQueue.front());
        fifoQueue.pop();
        return true;
    }

    // Shuts down the queue, waking up any waiting threads
    void shutdownQueue() {
        std::lock_guard<std::mutex> lock(mutex);
        shutdown = true;
        cond.notify_all();
    }

    ThreadSafeQueueCPU() = default; //default constructor
    ThreadSafeQueueCPU(ThreadSafeQueueCPU&&) noexcept = default; // Disable copy. Otherwise it can't reside in std::vector.
    ThreadSafeQueueCPU& operator=(ThreadSafeQueueCPU&&) noexcept = default;
    ThreadSafeQueueCPU(const ThreadSafeQueueCPU&) = delete; // Allow move
    ThreadSafeQueueCPU& operator=(const ThreadSafeQueueCPU&) = delete;

private:
    std::queue<ACTION_DETAILS> fifoQueue; // fifo = First-In First-Out
    std::mutex mutex;
    std::condition_variable cond;
    bool shutdown = false;
};

struct NETWORK_INTERFACE {
    uint16_t type; // 0: IPv6, 1: IPv4
    uint16_t port; // The port we are either accepting connection or connecting to.
    char* ipAddress[16]; // IPv6 are 128 byte (=16 Byte), IPv4 will 32 bits i.e. 1st 4 Byte Only. 
};

struct VIEW_INSIDE_DATASETTAB {
    // Each dataSet tab can have multiple views.
    uint64_t viewID; //Unique view ID inside this dataSet tab. Randomly generated.
	bool isExtratedInOwnWindow = false; //If true, this view is shown in some other window.
	std::wstring viewName; //User assigned name of the view.
    float backgroundColor[4]; //RGBA
    float backgroundColorHue; // 0 to 360 degree.

};

struct StoredGeometryObject3D {
    VishwakarmaStorage::ObjectType objectType = VishwakarmaStorage::ObjectType::Unknown;
    uint64_t memoryId = 0;
    META_DATA* object = nullptr;
};

struct StoredLogicalObject {
    VishwakarmaStorage::ObjectType objectType = VishwakarmaStorage::ObjectType::Unknown;
    uint64_t memoryId = 0;
    META_DATA* object = nullptr;
};

struct InternalSubTab {
    VishwakarmaStorage::ObjectType containerType = VishwakarmaStorage::ObjectType::Unknown;
    uint64_t containerMemoryId = 0;
    std::string title;
    // Per-view camera (Scene3D views). Engineering thread writes, render threads read lock-free,
    // same 1-frame-staleness contract as the rest of the camera pipeline.
    CameraState camera;
};

// Lifecycle of one fixed sub-tab slot inside DATASETTAB.
// FREE -> OPEN -> PENDING_GPU_RELEASE -> FREE. A closed slot is only reused after every monitor's
// render fence passed the value recorded at close time, so in-flight frames finish first.
constexpr uint8_t SUBTAB_FREE = 0;
constexpr uint8_t SUBTAB_OPEN = 1;
constexpr uint8_t SUBTAB_PENDING_GPU_RELEASE = 2;

// SingleUIWindow kinds. A tab-host window shows the full top ribbon and a tab band.
// A view window shows only the content of one extracted sub-tab (no ribbon, no bands).
constexpr uint8_t WINDOW_KIND_TABHOST = 0;
constexpr uint8_t WINDOW_KIND_VIEW = 1;

struct DATASETTAB {
    uint64_t tabID;
    std::wstring fileName;
    std::wstring storageFilePath;
	std::vector<VIEW_INSIDE_DATASETTAB> views; //All views need not be inside single windows. Some views can be in other windows.
    int activeViewIndex = 0;
    float color[4] = {};
    float colorHue = 0;

    // Each opened dataSet is considered / shown as a TAB. It could consist of multiple .yyy & .zzz file.
    // It could either load from local disc attached to OS OR loaded from remote network share OR loaded from same application running on other computer on network.
    // Network share is different because we don't want to submit overburden remote shared server with design calculation transient files.
    
    /* Tab Codes.
    0 : Unsaved Tab. All data is in memory only.
    1 : Directly Opened from local file storage.
    2 : Directly Opened from Network file storage.
    3 : Subscribed to peers
    Tab codes.*/
    int mode = 0;

    uint32_t tabNo=0; //This also act as memoryGroupNo of the memory chunks in which engineering objects created under this tabs are located.

    /* In general, unless explicitly tuned, windows file full path including the "c:\" and terminating NULL character is 3+256+1=260
    character long. If long path is enabled, it is 2^16-1=32767
    https://learn.microsoft.com/en-us/windows/win32/fileio/maximum-file-path-limitation
    Linux also has 255 Character as file system Limit.  https://en.wikipedia.org/wiki/Ext4
    */
    int isShortPath = 0; //0: when it's a short path, 1 when it is long.
    char shortFileName[256] = {}; // Example: "DesignFile.bha"
    char shortFilePath[256] = {}; // Example: "C:\Folder1\Folder2\Folder3\"
    char* longFileName = NULL;
    char* longFilePath = NULL;
    NETWORK_INTERFACE networkFile; //If we are loading from same application running on another network computer.

    // Encryption Keys and ID of the file.
    char* filePublicKey[57] = {}; //ED448 Public Key
    char* fileSecretKey[57] = {}; //ED448 Private Key
    char* fileNonce[16] = {}; //Internal AES encryption key of the file.
    char* fileID[16] = {}; //SHA256 of Public Key truncated to 1st 128 bits.

    std::vector<uint64_t> allIDsInThisTab; //List of all engineering object IDs in this tab.
    std::vector<StoredLogicalObject> storageLogicalObjects; // Persisted organization objects in this tab.
    std::vector<StoredGeometryObject3D> storageObjects3D; // MVP persisted geometry objects in this tab.
    std::vector<uint64_t> expandedDataTreeNodeIds; // Expanded logical nodes in the visible data tree.

    // Fixed-slot registry of open sub-tabs (views), mirroring the allTabs/activeTabIndexes pattern.
    // Slots are engineering-thread owned (written under storageObjectsMutex before the list is
    // published); the index list is double buffered and published atomically so render/UI threads
    // read it lock-free. Fixed size avoids std::vector reallocation issues.
    InternalSubTab subTabs[MV_MAX_SUBTABS];
    uint16_t subTabIndexesA[MV_MAX_SUBTABS] = {}, subTabIndexesB[MV_MAX_SUBTABS] = {};
    std::atomic<uint16_t*> publishedSubTabIndexes{ nullptr };
    std::atomic<uint16_t>  publishedSubTabCount{ 0 };
    // Per-slot lifecycle enabling delayed release: a closed slot stays SUBTAB_PENDING_GPU_RELEASE
    // until every monitor's render fence passes subTabReleaseFenceValues[slot]; only then does it
    // become SUBTAB_FREE for reuse, so frames still referencing the view's GPU assets are done.
    std::atomic<uint8_t> subTabStates[MV_MAX_SUBTABS] = {};
    uint64_t subTabReleaseFenceValues[MV_MAX_SUBTABS] = {};
    // -1 = view is inline in the sub-tab band; >= 0 = allWindows slot of its extracted dedicated window.
    std::atomic<int16_t> subTabHostWindowSlots[MV_MAX_SUBTABS];

    // The allWindows slot currently hosting this tab's button in its tab band. A tab is hosted by
    // exactly one window; extraction / drag-drop merge just retargets this slot.
    std::atomic<int16_t> hostWindowSlot{ 0 };

    uint64_t activeInternalSubTabMemoryId = 0; // Zero means no high-level container is currently visible.
    // The view (sub-tab slot) scene/page input currently applies to. -1 = the inline-active
    // sub-tab; >= 0 = an extracted view the user last interacted with. Written by the UI thread
    // (WndProc), read by the engineering thread; also read by render threads to place selection
    // overlays / picks in the interacting window.
    std::atomic<int32_t> inputViewSubTabSlot{ -1 };
    uint64_t defaultScene3DMemoryId = 0;
    uint64_t activeScene3DMemoryId = 0; // Organizational parent for newly generated 3D objects.
    std::atomic<uint32_t> activePrimitive3DPlacementType{
        VishwakarmaStorage::ToNumber(VishwakarmaStorage::ObjectType::Unknown) };
    // Zoom Window mode: armed by Commands::ZOOM_WINDOW, the next 2 clicks define the rectangle to
    // zoom onto. The atomic is also read by the render thread to trail the cursor icon; the corner
    // fields are engineering-thread only.
    std::atomic<bool> zoomWindowMode{ false };
    bool zoomWindowHasFirstCorner = false;
    int zoomWindowFirstX = 0;
    int zoomWindowFirstY = 0;
    std::unique_ptr<std::mutex> storageObjectsMutex;

	PlatformTabGpu dx; // Per-tab GPU resources (DX12/Vulkan/Metal via GPUPlatformSelector.h).
    //ThreadSafeQueueCPU userInputQueue; // Dedicated Input Queue for this tab's engineering thread.
    //ThreadSafeQueueCPU todoCPUQueue;   // Dedicated Work Queue for this tab's engineering thread. Self TODOs.
    std::unique_ptr<ThreadSafeQueueCPU> userInputQueue;
    std::unique_ptr<ThreadSafeQueueCPU> todoCPUQueue;
    TabGeometryStorage geometry;
    std::unique_ptr<TabCad2DStorage> cad2d;

    /*Self TODOs are modification to enginering world data, like create new Beam, Modify existing Column etc.
    This is different from userInputQueue because this 2nd queue can come from filesystem thread, 
    network thread, engineering calculation thread, undo/redo action etc. */

    bool mouseLeftDown = false;
    bool mouseMiddleDown = false;
    bool mouseRightDown = false;
    bool isAltDown = false;
    bool isShiftDown = false;
    bool isCtrlDown = false;

    int lastMouseX = 0;
    int lastMouseY = 0;
    
    CameraState camera; //Currently it is per tab. Latter we may move it to per view.
    SelectionState selection; // 3D click-selection state (Selection3D module).
    bool autoCameraRotation = true;
    bool autoGenerateRandomGeometry = true;
    DataTreeView::State dataTreeView;
    std::atomic<bool> closeRequested{ false };
    std::atomic<bool> engineeringReleased{ false };
    // Tab-close GPU teardown handshake. The UI thread (CleanupReleasedTabs) requests with 1 after
    // the engineering thread has exited; GpuCopyThread tags the global render fence (2) and does
    // the actual release once every monitor's fence has passed it. All per-tab GPU state (dx
    // matrix writes, RCU geometry pages, cad2d records) is copy-thread-owned, so the release
    // must never run on the UI thread (a copy iteration may hold the pre-close published list).
    std::atomic<uint8_t> gpuReleaseState{ 0 }; // 0 = none, 1 = requested, 2 = fence-tagged
    uint64_t gpuReleaseFence = 0;              // Written/read by the copy thread only.
    
    DATASETTAB() {
        userInputQueue = std::make_unique<ThreadSafeQueueCPU>();
        todoCPUQueue = std::make_unique<ThreadSafeQueueCPU>();
        storageObjectsMutex = std::make_unique<std::mutex>();
        cad2d = std::make_unique<TabCad2DStorage>();
        publishedSubTabIndexes.store(subTabIndexesA, std::memory_order_relaxed);
        for (int i = 0; i < MV_MAX_SUBTABS; ++i) {
            subTabHostWindowSlots[i].store(-1, std::memory_order_relaxed);
        }
    }
    DATASETTAB(const DATASETTAB&) = delete;// Disable copy (mutex cannot copy). Otherwise it can't reside in std::vector.
    DATASETTAB& operator=(const DATASETTAB&) = delete;
    DATASETTAB(DATASETTAB&&) noexcept = default;// Allow move
    DATASETTAB& operator=(DATASETTAB&&) noexcept = default;
};

// Tab 0 id default application launch screen tab. It can't be closed.
// Tab 0 is also used to do all the experiments and benchmark during development.
inline uint32_t activeTab = 0; 

struct SingleUIWindow {
    //It can represent either collection of tabs ( 1 or more ) or single view belong to some tab in other windows.

    HWND hWnd = nullptr;
    std::vector<int> tabIds;
    int activeTabIndex = -1;
    int currentMonitorIndex; // The index of monitor returned by Windows API. It changes on monitor addition/removal.
    int requestedMonitorIndex;

    // WINDOW_KIND_TABHOST (full ribbon + tab band) or WINDOW_KIND_VIEW (extracted sub-tab content only).
    uint8_t windowKind = WINDOW_KIND_TABHOST;
    int viewParentTabIndex = -1; // View windows: tab whose sub-tab is shown (mirrors activeTabIndex).
    uint16_t viewSubTabSlot = 0; // View windows: slot into the parent tab's subTabs[].

    // Drag-to-extract state (owned by the render thread that draws this window, immediate-mode UI).
    int pressedTabId = -1;      // Tab button under the initial left-button press. -1 = none.
    int pressedSubTabSlot = -1; // Sub-tab button under the initial left-button press. -1 = none.
    std::atomic<uint32_t> migrationState{ 0 };
    /*  0 : Normal rendering, 1 : UI requested migration, 2 : Source render thread released window
    3 : Destination render thread acquiring, 4 : Destination initialized resources, 0 : Back to normal */

    std::atomic<uint32_t> resizeState{ 0 }; // Resize state machine. 0:idle, 1:resize requested by UI thread
    std::atomic<uint32_t> nextRequestedWidth{ 800 }, nextRequestedHeight{ 600 };

    RECT tabBandRect{};
    RECT viewBandRect{};
    RECT contentRect{};

    std::atomic<bool> isMigrating{ false };;// The "Switch" to turn rendering ON/OFF during migration.
    std::atomic<bool> isResizing{ false };
    bool isInSizeMove = false;
	uint16_t currentWidth = 800, currentHeight = 600;

    UIInput uiInput; // per-window input snapshot
    Commands activeDropdownAction = Commands::INVALID;

    // Right-side object properties pane (website/content/software/propertiesPane.md). UI-only state,
    // same class as activeDropdownAction: toggled directly in the immediate-mode hit test.
    bool rightPaneOpen = false;
    // Insert Asset pane: opened by the INSERT_ASSET2D ribbon button; shares the right-side pane
    // slot with the properties pane. Its dropdown picks the asset the next Page2D clicks place.
    bool assetInsertPaneOpen = false;
    UIDropdownState assetInsertDropdown;
    UITextEditState textEditState;                    // In-progress property-field edit (render thread owned).
    std::atomic<uint64_t> uiKeyboardCaptureCount{ 0 }; // != 0 while a UI text field has focus (WndProc suppresses shortcuts).
    std::atomic<uint32_t> rightOverlayWidthPx{ 0 };    // Icon bar (+ pane) width in px; input guards read it.

    // Chrome-style frameless window (website/content/software/tabs.md). The render thread publishes the
    // tab-band caption geometry here each frame; WndProc reads it in WM_NCHITTEST to place the OS
    // caption drag zone and the min/max/close buttons. Same publish-through-atomic pattern as
    // rightOverlayWidthPx. All zero until the first frame publishes them (WndProc falls back safely).
    std::atomic<int32_t> frameTabBarBottomPx{ 0 };    // Bottom of the draggable tab-bar strip, client px.
    std::atomic<int32_t> frameControlsLeftPx{ 0 };    // Left edge of the min/max/close block; it spans to the client right.
    std::atomic<int32_t> frameCaptionDragLeftPx{ 0 }; // Left edge of the empty drag zone (right of the '+' button).
    std::atomic<bool> isMaximized{ false };           // Set in WM_SIZE; selects the maximize vs restore glyph.

    PlatformWindowGpu dx;

    // BOILERPLATE TO FIX C2672 ERROR
    SingleUIWindow() = default;// Default Constructor
    SingleUIWindow(const SingleUIWindow& other)// Copy Constructor (Manually copy the atomic value)
        : hWnd(other.hWnd),
        tabIds(other.tabIds),
        activeTabIndex(other.activeTabIndex),
        currentMonitorIndex(other.currentMonitorIndex),
        requestedMonitorIndex(other.requestedMonitorIndex),
        windowKind(other.windowKind),
        viewParentTabIndex(other.viewParentTabIndex),
        viewSubTabSlot(other.viewSubTabSlot),
        tabBandRect(other.tabBandRect),
        viewBandRect(other.viewBandRect),
        contentRect(other.contentRect),
        dx(other.dx) // ComPtrs handle copying correctly
    {
        isMigrating.store(other.isMigrating.load());
        migrationState.store(other.migrationState.load());
        isResizing.store(other.isResizing.load());
        isInSizeMove = other.isInSizeMove;
        frameTabBarBottomPx.store(other.frameTabBarBottomPx.load());
        frameControlsLeftPx.store(other.frameControlsLeftPx.load());
        frameCaptionDragLeftPx.store(other.frameCaptionDragLeftPx.load());
        isMaximized.store(other.isMaximized.load());
    }

    // Move Constructor (Critical for std::vector performance)
    SingleUIWindow(SingleUIWindow&& other) noexcept
        : hWnd(other.hWnd),
        tabIds(std::move(other.tabIds)),
        activeTabIndex(other.activeTabIndex),
        currentMonitorIndex(other.currentMonitorIndex),
        requestedMonitorIndex(other.requestedMonitorIndex),
        windowKind(other.windowKind),
        viewParentTabIndex(other.viewParentTabIndex),
        viewSubTabSlot(other.viewSubTabSlot),
        tabBandRect(other.tabBandRect),
        viewBandRect(other.viewBandRect),
        contentRect(other.contentRect),
        dx(std::move(other.dx)) // Transfer ownership
    {
        migrationState.store(other.migrationState.load());
        isMigrating.store(other.isMigrating.load());
        isResizing.store(other.isResizing.load());
        isInSizeMove = other.isInSizeMove;
        frameTabBarBottomPx.store(other.frameTabBarBottomPx.load());
        frameControlsLeftPx.store(other.frameControlsLeftPx.load());
        frameCaptionDragLeftPx.store(other.frameCaptionDragLeftPx.load());
        isMaximized.store(other.isMaximized.load());
    }

    // Assignment Operator
    SingleUIWindow& operator=(const SingleUIWindow& other) {
        if (this != &other) {
            hWnd = other.hWnd;
            tabIds = other.tabIds;
            activeTabIndex = other.activeTabIndex;
            currentMonitorIndex = other.currentMonitorIndex;
            requestedMonitorIndex = other.requestedMonitorIndex;
            windowKind = other.windowKind;
            viewParentTabIndex = other.viewParentTabIndex;
            viewSubTabSlot = other.viewSubTabSlot;
            tabBandRect = other.tabBandRect;
            viewBandRect = other.viewBandRect;
            contentRect = other.contentRect;
            dx = other.dx;
            migrationState.store(other.migrationState.load());
            isMigrating.store(other.isMigrating.load());
            isResizing.store(other.isResizing.load());
            isInSizeMove = other.isInSizeMove;
            frameTabBarBottomPx.store(other.frameTabBarBottomPx.load());
            frameControlsLeftPx.store(other.frameControlsLeftPx.load());
            frameCaptionDragLeftPx.store(other.frameCaptionDragLeftPx.load());
            isMaximized.store(other.isMaximized.load());
        }
        return *this;
    }
};

void विश्वकर्मा(uint64_t tabID);
// Makes a tab-host window Chrome-style frameless (client extends over the caption + a DWM top margin
// for the drop shadow). Called once per tab-host window at creation and after DPI changes (tabs.md).
void ApplyFramelessFrame(HWND hWnd);
bool GetVisibleSceneViewportForTab(const DATASETTAB& tab, int& widthPx, int& heightPx, int& topPx);

// Lock-free scan of the published sub-tab list. Returns the slot index, or -1 when not open.
int FindPublishedSubTabSlot(const DATASETTAB& tab, uint64_t containerMemoryId);
// The sub-tab slot scene/page input currently targets: the extracted view the user last
// interacted with (inputViewSubTabSlot), else the inline-active sub-tab. -1 when none resolves.
int InputViewSlot(const DATASETTAB& tab);
// Container memoryId of the input view slot (0 when none).
uint64_t InputViewContainerId(const DATASETTAB& tab);
// Marks every open sub-tab slot for delayed GPU release and publishes an empty list.
// storageObjectsMutex must already be held by the caller.
void CloseAllInternalSubTabsLocked(DATASETTAB& tab);

// The tab that produced the most recent UI click (any window). ProcessPendingUIActions uses it to
// route ribbon commands to the tab of the window the user actually clicked in. -1 = none yet.
extern std::atomic<int32_t> g_uiActionSourceTabIndex;

// Register a newly created engineering std::thread into the global registry (takes ownership)
void AddEngineeringThread(std::thread&& t);
void JoinAllEngineeringThreads();

//Atomic ID generator for unique IDs across the application.
static std::atomic<uint64_t> global_id{1}; // start at 1 to reserve 0 .
inline uint64_t GetNewTempID(){ //fetch_add returns the old value before increment,
    return global_id.fetch_add(1, std::memory_order_relaxed); 
} 

//These variables are defined in this .h file so that we don't have to extern define them in all .cpp files again.
extern DATASETTAB allTabs[MV_MAX_TABS]; //They are all the dataset tabs opened in the application.
extern uint16_t activeTabIndexesA[MV_MAX_TABS], activeTabIndexesB[MV_MAX_TABS]; // double buffered index list
extern std::atomic<uint16_t*> publishedTabIndexes;
extern std::atomic<uint16_t>  publishedTabCount;

extern SingleUIWindow allWindows[MV_MAX_WINDOWS];
extern uint16_t activeWindowIndexesA[MV_MAX_WINDOWS], activeWindowIndexesB[MV_MAX_WINDOWS];
extern std::atomic<uint16_t*> publishedWindowIndexes;
extern std::atomic<uint16_t>  publishedWindowCount;
