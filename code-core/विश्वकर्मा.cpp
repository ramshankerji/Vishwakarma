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
More importantly, engineering thread are responsible for maintaining data consistency,
tracking which objects are visible in which views, what are the dirty objects to be cleaned up from GPU memory etc.
*/
int g_nextTabId = 1;

// Latter move this to विश्वकर्मा.h
//Remember these global codes outside any function run even before main() starts.
std::random_device rd; //Universal random number generator seed. Non-Deterministic. Obtained from OS.
std::mt19937 gen(rd()); //rd(): Calls the device we made above to get a single random number.
//std::mt19937: A specific algorithm famous for being very fast and having high statistical quality.
// Period of 2^{19937}-1. All subsequent random numbers are generated from this seeded mt19937 object.

inline void addRandomGeometryElement(DATASETTAB* targetTab) {
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
            for (int k = 0; k < 10; ++k) addRandomGeometryElement(myTab);
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

		UpdateCameraOrbit(myTab->camera); // Engineering thread updates camera orbit continuously.
        
        // Check timer and add a new pyramid every second.
        auto currentTime = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(currentTime - lastPyramidAddTime).count() >= 1) {
            addRandomGeometryElement(myTab);
            lastPyramidAddTime = currentTime; // Reset the timer
            // Optional: Log to prove background work is happening
            // std::cout << "Tab " << tabIndex << " generated object." << std::endl;
        }

        // Process User Inputs First (Lightweight: Camera, Selection, Throttling)
        ACTION_DETAILS input;
        int inputCount = 0;  // For throttling detection
        auto inputStart = std::chrono::steady_clock::now();
        while (myTab->userInputQueue->try_pop(input)) {
            inputCount++;
            // Throttle: Skip intermediate MOUSEMOVE if >200/sec (check timestamp/rate)
            if (input.actionType == ACTION_TYPE::MOUSEMOVE && inputCount > 200) { continue; }  // Simple rate limit

            // Handle based on type
            switch (input.actionType) {
            case ACTION_TYPE::MOUSEMOVE:
                if (myTab->mouseLeftDown) {  // Example: Drag-rotate camera
                    // Update camera based on delta (input.x - lastMouseX)
                    // Push to commandToCopyThreadQueue if view changes dirty geometry.
                }
                myTab->lastMouseX = input.x;
                myTab->lastMouseY = input.y;
                // Check if in render area (vs UI): Compare input.x/y to myTab->views[activeViewIndex].rect or contentRect.
                break;
            case ACTION_TYPE::MOUSEWHEEL:
            {
                float wheelSteps = input.delta / (float)WHEEL_DELTA; // Since new mouse send lots of events ?

                // Calculate the vector from Target to Position
                float dx = myTab->camera.position.x - myTab->camera.target.x;
                float dy = myTab->camera.position.y - myTab->camera.target.y;
                float dz = myTab->camera.position.z - myTab->camera.target.z;
                float distance = std::sqrt(dx * dx + dy * dy + dz * dz);// Calculate current distance from target

                // Determine Zoom Factor. Standard mouse wheel delta is 120. 
                // Delta > 0 (Wheel Forward) -> Zoom IN  (Factor < 1.0). Delta < 0 (Wheel Back)    -> Zoom OUT (Factor > 1.0)
				//float zoomFactor = (input.delta > 0) ? 0.9f : 1.1f; // Binary zoom. Not smooth.                
                float zoomFactor = std::pow(0.9f, wheelSteps);// Smooth zoom instead of binary zoom
                float newDistance = distance * zoomFactor;// Apply Zoom

                // Safety Clamping. Prevent getting stuck at 0 (locking the camera) or going too far
                if (newDistance < 1.0f) newDistance = 1.0f;
                if (newDistance > myTab->camera.farZ - 10.0f) newDistance = myTab->camera.farZ - 10.0f;

                // Update Position. We keep the same direction, just change the magnitude
                if (distance > 0.002f) {
                    float scale = newDistance / distance;
                    myTab->camera.position.x = myTab->camera.target.x + (dx * scale);
                    myTab->camera.position.y = myTab->camera.target.y + (dy * scale);
                    myTab->camera.position.z = myTab->camera.target.z + (dz * scale);
                }

                std::cout << "Zoom Updated. New Distance: " << newDistance << "\n";// Debug logging (Optional)
                break;
            }
            case ACTION_TYPE::LBUTTONDOWN:
                myTab->mouseLeftDown = true;
                // SetCapture(hWnd) if needed for drag (but handle in main thread?).
                break;
            case ACTION_TYPE::LBUTTONUP:
                myTab->mouseLeftDown = false;
                break;
                // Similarly for other buttons, keys (e.g., 'P' for CREATEPYRAMID -> push to todoCPUQueue).
            case ACTION_TYPE::KEYDOWN:
                if (input.x == 'P') {  // Example mapping
                    ACTION_DETAILS todo;
                    todo.actionType = ACTION_TYPE::CREATEPYRAMID;
                    // Fill other fields...
                    myTab->todoCPUQueue->push(todo);
                }
                break;
            case ACTION_TYPE::CAPTURECHANGED:
            case ACTION_TYPE::INPUT:  // For device reset
                // Reset all button states
                myTab->mouseLeftDown = myTab->mouseRightDown = myTab->mouseMiddleDown = false;
                break;
                // Handle wheel for zoom, etc.
            }
        }
        // After loop: If inputCount high, log or adjust (e.g., sleep if bursty).

        // Existing todoCPUQueue processing remains (for self-TODOs like CREATEPYRAMID).

        // Input Processing (Specific to this Tab). Previously todoCPUQueue was global. now it is Local. 
        // Process all pending inputs from User, Network, File threads
        ACTION_DETAILS nextWorkTODO;
        while (bool todo = myTab->todoCPUQueue->try_pop(nextWorkTODO)) {
            std::cout << "Input received. Action Type = " << static_cast<int>(nextWorkTODO.actionType) <<"\n";
            if (nextWorkTODO.actionType == ACTION_TYPE::CREATEPYRAMID) {
                //addRandomGeometryElement();
            }
            if (todo == false) { // Means input queue was empty. We should sleep for 1 millisecond.
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(10));// Sleep to yield CPU
        frameCounter++;
    } // End of while (!shutdownSignal), i.e. our primary application loop for this particular tab.

    //g_logicFenceCV.notify_all(); // Wake up threads for shutdown
    std::cout << "Main Logic Thread shutting down.\n" << std::endl;
}
