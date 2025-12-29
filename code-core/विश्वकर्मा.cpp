// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.

/*This is the application's orchestrator. It consumes commands, updates the scene database, 
identifies dirty objects, and generates work for the GPU threads.
This thread is also responsible for engineering calculations, consistency of Data etc.
*/

#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <random> // Required for std::uniform_int_distribution
#include "MemoryManagerCPU.h"
#include "विश्वकर्मा.h"
#include "डेटा.h"
#include "डेटा-सामान्य-3D.h"
#include "MemoryManagerGPU-DirectX12.h"

राम cpu;
शंकर gpu;

// Global Variables.
static std::chrono::steady_clock::time_point lastPyramidAddTime; //Temporary. TODO: Remove.
extern std::atomic<bool> shutdownSignal; // Externs for communication

// Thread Synchronization and Data Structures
std::atomic<bool> g_stopThreads = false;
std::atomic<uint64_t> g_nextPyramidId = 1;

// Global State
// Different tabs represent different files opened in the software.Just like different website links open in different Internet browser tab.
// Tab No. 0 Show the opening screen.i.e.Not associated with any particular opened file. 1 DATASET = 1 TAB visible to user / to website.
uint8_t noOfOpenedDataset = 0;
// We will allow user to open as many files simultaneously as system RAM allows.
// Particularly, enterprise central repository may have thousands of projects.
// Hence this is one of the rare location where we allow dynamic allocation done by std:vector.
// std::vector Grows exponentially. 1.5x for GCC/Clang, 2x for MSVC.
std::vector<DATASETTAB> allTabs;          //They are all the dataset tabs opened in the application.
std::vector<SingleUIWindow> allUIWindows; /*Each tab will be hosted in exactly 1 windows. 
However some of the views of the tab can be extracted to other windows.
Each tab gets its own engineering thread, capable of doing background processing, receiving network data, file I/O etc.
However engineering threads do not directly talk to GPU. They submit the screen visible changes to the GPU Copy thread.
More importantly, engineering thread are responsible for maintianing data consistency,
tracking whcih objects are visible in which views, what are the dirty objects to be clearned up from GPU memory etc.
*/
int g_nextTabId = 1;

// Latter move this to विश्वकर्मा.h
//Remember these global codes outside any function run even before main() starts.
std::random_device rd; //Universal random number generator seed. Non-Deterministic. Obtained from OS.
std::mt19937 gen(rd()); //rd(): Calls the device we made above to get a single random number.
//std::mt19937: A specific algorithm famous for being very fast and having high statistical quality.
// Period of 2^{19937}-1. All subsequent random numbers are generated from this seeded mt19937 object.

inline void addRandomGemoetryElement(DATASETTAB* targetTab) {
	if (!targetTab) return; //Safety against NULL pointer dereference.
    GeometryData geometry;// These will hold the data of the randomly created shape.
    uint64_t memoryId;

    // Randomly select a shape type (0-7 for the 8 shapes available).
    // We use the GetRNG() helper function already available in "डेटा-सामान्य-3D.h".
    std::uniform_int_distribution<int> shapeDist(0, 7);
    int shapeType = shapeDist(GetRNG());

    // Note: Ensure your shape constructors (new PYRAMID()) use the correct memoryGroupNo if needed.
    // For now, assuming they use default or we will fix memory grouping later. TODO

    switch (shapeType) {// Create and randomize the chosen shape.
    case 0: {
        PYRAMID* shape = new PYRAMID();
        shape->Randomize();
        geometry = shape->GetGeometry();
        memoryId = shape->memoryID;
        break;
    }
    case 1: {
        CUBOID* shape = new CUBOID();
        shape->Randomize();
        geometry = shape->GetGeometry();
        memoryId = shape->memoryID;
        break;
    }
    case 2: {
        CONE* shape = new CONE();
        shape->Randomize();
        geometry = shape->GetGeometry();
        memoryId = shape->memoryID;
        break;
    }
    case 3: {
        CYLINDER* shape = new CYLINDER();
        shape->Randomize();
        geometry = shape->GetGeometry();
        memoryId = shape->memoryID;
        break;
    }
    case 4: {
        PARALLELEPIPED* shape = new PARALLELEPIPED();
        shape->Randomize();
        geometry = shape->GetGeometry();
        memoryId = shape->memoryID;
        break;
    }
    case 5: {
        SPHERE* shape = new SPHERE();
        shape->Randomize();
        geometry = shape->GetGeometry();
        memoryId = shape->memoryID;
        break;
    }
    case 6: {
        FRUSTUM_OF_PYRAMID* shape = new FRUSTUM_OF_PYRAMID();
        shape->Randomize();
        geometry = shape->GetGeometry();
        memoryId = shape->memoryID;
        break;
    }
    case 7: {
        FRUSTUM_OF_CONE* shape = new FRUSTUM_OF_CONE();
        shape->Randomize();
        geometry = shape->GetGeometry();
        memoryId = shape->memoryID;
        break;
    }
    }
    //Add the new shape's data to the command queue for the GPU.
    {
        std::lock_guard<std::mutex> lock(toCopyThreadMutex);
        commandToCopyThreadQueue.push({ CommandToCopyThreadType::ADD, geometry, memoryId });
    }

    // Push to the specific tab's ID list, not global currentTab
    // We need a lock here if the UI thread reads this list while we write to it! TODO
    // For now, assuming UI reads only on frame update, we might be "okay-ish" but ideally use a mutex.
    targetTab->allIDsInThisTab.push_back(memoryId);
    toCopyThreadCV.notify_one();
}

void विश्वकर्मा(uint64_t tabID) { //Main logic/engineering thread. The ringmaster of the application.
    std::cout << "Main Logic Thread विश्वकर्मा started." << std::endl;
    lastPyramidAddTime = std::chrono::steady_clock::now();// Initialize the timer

    // Initial Population: We need to find the tab first.
    // In a real scenario, we might wait here if the tab isn't created yet, but Main ensures creation before thread launch.
    {
        DATASETTAB* myTab = nullptr;
        for (auto& tab : allTabs) { if (tab.tabID == tabID) { myTab = &tab; break; } }
        if (myTab) { // Generate the initial 10 pyramids
            for (int k = 0; k < 10; ++k) addRandomGemoetryElement(myTab);
        }
    }

    uint64_t frameCounter = 0;

    while (!shutdownSignal) { // This is our primary application loop.
        auto frameStart = std::chrono::high_resolution_clock::now();

        // Dynamic Lookup: Find the tab pointer based on ID
        // This handles the case where vector reallocates or tabs shift.
        DATASETTAB* myTab = nullptr;
        for (auto& tab : allTabs) { if (tab.tabID == tabID) {myTab = &tab;  break; } }

        if (myTab == nullptr) { // Handle Tab Closure
            std::cout << "Tab ID " << tabID << " not found (Closed?). Engineering thread exiting." << std::endl;
            break; // Exit the thread gracefully
        }

        // Check timer and add a new pyramid every second.
        auto currentTime = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(currentTime - lastPyramidAddTime).count() >= 1) {
            addRandomGemoetryElement(myTab);
            lastPyramidAddTime = currentTime; // Reset the timer
            // Optional: Log to prove background work is happening
            // std::cout << "Tab " << tabIndex << " generated object." << std::endl;
        }

        // Input Processing (Specific to this Tab)
        // Note: Currently todoCPUQueue is global. You need a way to filter messages for THIS tab.
        // For now, we will skip queue processing or assume all threads consume generic global inputs (risky).
        // A better approach is: todoCPUQueue should be per-tab or messages should have tabID.
        // 
        // Process all pending inputs from User, Network, File threads
        ACTION_DETAILS nextWorkTODO;
        while (bool todo = todoCPUQueue.try_pop(nextWorkTODO)) {
            std::cout << "Input received. Action Type = " << static_cast<int>(nextWorkTODO.actionType) <<"\n";
            if (nextWorkTODO.actionType == ACTION_TYPE::CREATEPYRAMID) {
                //addRandomGemoetryElement();
            }
            if (todo == false) { // Means input queue was empty. We should sleep for 1 millisecond.
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(10));// Sleep to yield CPU
        frameCounter++;
    } // End of while (!shutdownSignal), i.e. our primary application loop for this particular tab.

    //g_logicFenceCV.notify_all(); // Wake up threads for shutdown
    std::cout << "Main Logic Thread shutting down." << std::endl;
}