// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.
#pragma once

#include <cstdint>
#include <vector>
#include "MemoryManagerCPU.h"
#include "MemoryManagerGPU-DirectX12.h"
#include "UserInputProcessing.h"
extern struct CPU_RAM_4MB;

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
    uint64_t viewID; //Unique view ID inside this dataSet tab. Randomely generated.
	bool isExtratedInOwnWindow = false; //If true, this view is shown in some other window.
	std::wstring viewName; //User assigned name of the view.
    float backgroundColor[4]; //RGBA
    float backgroundColorHue; // 0 to 360 degree.

};

struct DATASETTAB {
    uint64_t tabID;
    std::wstring fileName;
	std::vector<VIEW_INSIDE_DATASETTAB> views; //All views need not be inside single windows. Some views can be in other windows.
    int activeViewIndex = 0;
    float color[4];
    float colorHue;

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

    uint32_t tabNo; //This also act as memoryGroupNo of the memory chunks in which engineering objects created under this tabs are located.

    /* In general, unless explicitly tuned, windows file full path including the "c:\" and terminating NULL character is 3+256+1=260
    character long. If long path is enabled, it is 2^16-1=32767
    https://learn.microsoft.com/en-us/windows/win32/fileio/maximum-file-path-limitation
    Linux also has 255 Character as file system Limit.  https://en.wikipedia.org/wiki/Ext4
    */
    int isShortPath = 0; //0: when it's a short path, 1 when it is long.
    char shortFileName[256]; // Example: "DesignFile.bha"
    char shortFilePath[256]; // Example: "C:\Folder1\Folder2\Folder3\"
    char* longFileName = NULL;
    char* longFilePath = NULL;
    NETWORK_INTERFACE networkFile; //If we are loading from same application running on another network computer.

    // Encryption Keys and ID of the file.
    char* filePublicKey[57]; //ED448 Public Key
    char* fileSecretKey[57]; //ED448 Private Key
    char* fileNonce[16]; //Internal AES encryption key of the file.
    char* fileID[16]; //SHA256 of Public Key truncated to 1st 128 bits.

	std::vector<uint64_t> allIDsInThisTab; //List of all engineering object IDs in this tab.

	DX12ResourcesPerTab dx; // DirectX12 resources specific to this tab.
    //ThreadSafeQueueCPU userInputQueue; // Dedicated Input Queue for this tab's engineering thread.
    //ThreadSafeQueueCPU todoCPUQueue;   // Dedicated Work Queue for this tab's engineering thread. Self TODOs.
    std::unique_ptr<ThreadSafeQueueCPU> userInputQueue;
    std::unique_ptr<ThreadSafeQueueCPU> todoCPUQueue;

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
    bool autoCameraRotation = true;
    
    DATASETTAB() {
        userInputQueue = std::make_unique<ThreadSafeQueueCPU>();
        todoCPUQueue = std::make_unique<ThreadSafeQueueCPU>();
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

    RECT tabBandRect{};
    RECT viewBandRect{};
    RECT contentRect{};

	std::atomic<bool> isMigrating = false;// The "Switch" to turn rendering ON/OFF during migration.
    DX12ResourcesPerWindow dx;

    // BOILERPLATE TO FIX C2672 ERROR
    SingleUIWindow() = default;// Default Constructor
    SingleUIWindow(const SingleUIWindow& other)// Copy Constructor (Manually copy the atomic value)
        : hWnd(other.hWnd),
        tabIds(other.tabIds),
        activeTabIndex(other.activeTabIndex),
        currentMonitorIndex(other.currentMonitorIndex),
        tabBandRect(other.tabBandRect),
        viewBandRect(other.viewBandRect),
        contentRect(other.contentRect),
        dx(other.dx) // ComPtrs handle copying correctly
    {
        isMigrating.store(other.isMigrating.load());
    }

    // Move Constructor (Critical for std::vector performance)
    SingleUIWindow(SingleUIWindow&& other) noexcept
        : hWnd(other.hWnd),
        tabIds(std::move(other.tabIds)),
        activeTabIndex(other.activeTabIndex),
        currentMonitorIndex(other.currentMonitorIndex),
        tabBandRect(other.tabBandRect),
        viewBandRect(other.viewBandRect),
        contentRect(other.contentRect),
        dx(std::move(other.dx)) // Transfer ownership
    {
        isMigrating.store(other.isMigrating.load());
    }

    // Assignment Operator
    SingleUIWindow& operator=(const SingleUIWindow& other) {
        if (this != &other) {
            hWnd = other.hWnd;
            tabIds = other.tabIds;
            activeTabIndex = other.activeTabIndex;
            currentMonitorIndex = other.currentMonitorIndex;
            tabBandRect = other.tabBandRect;
            viewBandRect = other.viewBandRect;
            contentRect = other.contentRect;
            dx = other.dx;
            isMigrating.store(other.isMigrating.load());
        }
        return *this;
    }
};

void विश्वकर्मा(uint64_t tabID);

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
