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
extern std::atomic<bool> shutdownSignal; // Externs for communication
std::atomic<uint64_t> g_nextPyramidId = 1;

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
        commandToCopyThreadQueue.push({ CommandToCopyThreadType::ADD, geometry, memoryId, targetTab->tabID });
    }

    // Push to the specific tab's ID list, not global currentTab
    // We need a lock here if the UI thread reads this list while we write to it! TODO
    // For now, assuming UI reads only on frame update, we might be "okay-ish" but ideally use a mutex.
    targetTab->allIDsInThisTab.push_back(memoryId);
    toCopyThreadCV.notify_one();
}

void विश्वकर्मा(uint64_t tabID) { //Main logic/engineering thread. The ringmaster of the application.
    std::cout << "Main Logic Thread विश्वकर्मा started." << std::endl;
    std::chrono::steady_clock::time_point lastPyramidAddTime;
    lastPyramidAddTime = std::chrono::steady_clock::now();// Initialize the timer

    // Initial Population: We need to find the tab first.
    // In a real scenario, we might wait here if the tab isn't created yet, but Main ensures creation before thread launch.
    {
        DATASETTAB* myTab = nullptr;
        uint16_t* tabList = publishedTabIndexes.load(std::memory_order_acquire);
        uint16_t tabCount = publishedTabCount.load(std::memory_order_acquire);
        for (uint16_t i = 0; i < tabCount; ++i) {
            DATASETTAB & t = allTabs[tabList[i]];
            if (t.tabID == tabID) { myTab = &t; break; }
        }
        
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
        uint16_t* tabList = publishedTabIndexes.load(std::memory_order_acquire);
        uint16_t tabCount = publishedTabCount.load(std::memory_order_acquire);
        
        for (uint16_t i = 0; i < tabCount; ++i) {
            DATASETTAB & t = allTabs[tabList[i]];
            if (t.tabID == tabID){ myTab = &t; break; }
        }
        
        if (myTab == nullptr) { // Handle Tab Closure
            std::cout << "Tab ID " << tabID << " not found (Closed?). Engineering thread exiting." << std::endl;
            break; // Exit the thread gracefully
        }

		// Automatic camera rotation for troubleshooting. Toggle using "r". To be removed later or made optional in UI.
        if(myTab->autoCameraRotation) UpdateCameraOrbit(myTab->camera); 
        
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
		bool isOrbiting = false, isPanning = false; // Track if we are in orbit/panning based on mouse state and modifiers.
        float distance = 0.0;
		float dx, dy, dz, vx, vy, vz;

        while (myTab->userInputQueue->try_pop(input)) {
            inputCount++;
            // Throttle: Skip intermediate MOUSEMOVE if >200/sec (check timestamp/rate)
            if (input.actionType == ACTION_TYPE::MOUSEMOVE && inputCount > 200) { continue; }  // Simple rate limit

            // Handle based on type
            switch (input.actionType) {
            case ACTION_TYPE::MOUSEMOVE:
                isPanning = myTab->mouseMiddleDown && myTab->isShiftDown;
                // Orbit if Middle Mouse is down, but NOT panning, OR if Alt+Left Click
                isOrbiting = (!isPanning && myTab->mouseMiddleDown) || (myTab->mouseLeftDown && myTab->isAltDown);

                dx = (input.x - myTab->lastMouseX);
                dy = (input.y - myTab->lastMouseY);

                // Calculate Vector from Target to Camera (View Vector)
                vx = myTab->camera.position.x - myTab->camera.target.x;
                vy = myTab->camera.position.y - myTab->camera.target.y;
                vz = myTab->camera.position.z - myTab->camera.target.z;
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
                    myTab->camera.position.x += moveX;
                    myTab->camera.position.y += moveY;
                    myTab->camera.position.z += moveZ;

                    myTab->camera.target.x += moveX;
                    myTab->camera.target.y += moveY;
                    myTab->camera.target.z += moveZ;
                }
                else if (isOrbiting) {// Orbit / Rotate around Focal Point (Target)
                    float sensitivity = 0.005f; // Adjust rotation speed here
                    dx = (input.x - myTab->lastMouseX) * sensitivity;
                    dy = (input.y - myTab->lastMouseY) * sensitivity;

                    // Calculate vector from Target to Camera (The Radius vector)
                    vx = myTab->camera.position.x - myTab->camera.target.x;
                    vy = myTab->camera.position.y - myTab->camera.target.y;
                    vz = myTab->camera.position.z - myTab->camera.target.z;

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
                    myTab->camera.position.x = myTab->camera.target.x + nx;
                    myTab->camera.position.y = myTab->camera.target.y + ny;
                    myTab->camera.position.z = myTab->camera.target.z + nz;

                    // Flag to Copy Thread that camera changed (if your engine requires explicit dirty flags)
                    // std::lock_guard<std::mutex> lock(toCopyThreadMutex);
                    // commandToCopyThreadQueue.push({ CommandToCopyThreadType::UPDATE_CAMERA, ... });
                }
				// Camera Safety Check to ensure camera and target are not at the same, crashing view matrix calculation.
                vx = myTab->camera.position.x - myTab->camera.target.x;
                vy = myTab->camera.position.y - myTab->camera.target.y;
                vz = myTab->camera.position.z - myTab->camera.target.z;
                if (vx * vx + vy * vy + vz * vz < 0.000001f) {
                    myTab->camera.position.z += 0.001f;}// tiny nudge along camera up
                
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
                float wheelSteps = input.delta / (float)WHEEL_DELTA; // Since new mouse send lots of events ?

                // Calculate the vector from Target to Position
                float dx = myTab->camera.position.x - myTab->camera.target.x;
                float dy = myTab->camera.position.y - myTab->camera.target.y;
                float dz = myTab->camera.position.z - myTab->camera.target.z;
                distance = std::sqrt(dx * dx + dy * dy + dz * dz);// Calculate current distance from target

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

                //std::cout << "Zoom Updated. New Distance: " << newDistance << "\n";// Debug logging (Optional)
                break;
            }
            case ACTION_TYPE::LBUTTONDOWN:
                myTab->mouseLeftDown = true;
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
				if (input.x == 67 || input.x == 99) { myTab->camera.Initialize(); } // 'c' & "C". Reset camera.
                break;

            case ACTION_TYPE::CAPTURECHANGED:
            case ACTION_TYPE::INPUT:  // For device reset Reset all button states
                myTab->mouseLeftDown = myTab->mouseRightDown = myTab->mouseMiddleDown = false;
                myTab->isShiftDown = myTab->isAltDown = myTab->isCtrlDown = false; // Reset modifiers too
                break;
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
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(10));// Sleep to yield CPU
        frameCounter++;
    } // End of while (!shutdownSignal), i.e. our primary application loop for this particular tab.

    //g_logicFenceCV.notify_all(); // Wake up threads for shutdown
    std::cout << "Main Logic Thread shutting down.\n" << std::endl;
}
