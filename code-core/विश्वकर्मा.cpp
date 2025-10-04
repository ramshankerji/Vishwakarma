// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.

/*This is the application's orchestrator. It consumes commands, updates the scene database, identifies dirty objects, 
and generates work for the GPU threads. This thread is also responsible for engineering calculations, consistency of Data etc.
*/

#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include "MemoryManagerCPU.h"
#include "विश्वकर्मा.h"
#include "डेटा.h"
#include "डेटा-सामान्य-3D.h"
#include "जीपीयू-नियंत्रक.h"
राम cpuMemoryManager;

// Global Variables.
static std::chrono::steady_clock::time_point lastPyramidAddTime; //Temporary. TODO: Remove.

// --- Externs for communication ---
extern std::atomic<bool> shutdownSignal;

// The "fence" for the Main Logic thread. Signals that a frame's logic is complete.
extern std::mutex g_logicFenceMutex;
extern std::condition_variable g_logicFenceCV;
extern uint64_t g_logicFrameCount;

// A shared pointer to the latest render packet for the render threads
extern std::mutex g_renderPacketMutex;
extern RenderPacket g_renderPacket;

// Thread Synchronization and Data Structures
std::atomic<bool> g_stopThreads = false;
std::atomic<uint64_t> g_nextPyramidId = 1;

DATASETTAB* currentTab;
std::vector<DATASETTAB> tabs;

void विश्वकर्मा() { //Main logic thread. The ringmaster of the application.
    std::cout << "Main Logic Thread विश्वकर्मा started." << std::endl;
    tabs.push_back(DATASETTAB());
    currentTab = &tabs[0]; //By default start with tab 0.
    // Generate the initial 10 pyramids
    for (int k = 0; k < 10; ++k) {
        PYRAMID* newPyramid = new PYRAMID(); //create on CPU RAM stack.
        newPyramid->Randomize();
        {
            std::lock_guard<std::mutex> lock(toCopyThreadMutex);
            commandToCopyThreadQueue.push({ CommandToCopyThreadType::ADD, 
                newPyramid->GetGeometry(), newPyramid->memoryID});
        }
        currentTab->allIDsInThisTab.push_back(newPyramid->memoryID);
        toCopyThreadCV.notify_one();
    }
    lastPyramidAddTime = std::chrono::steady_clock::now();// Initialize the timer

    uint64_t frameCounter = 0;

    while (!shutdownSignal) { // This is our primary application loop.
        currentTab = &tabs[activeTab];

        auto frameStart = std::chrono::high_resolution_clock::now();

        // Check timer and add a new pyramid every second.
        auto currentTime = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(currentTime - lastPyramidAddTime).count() >= 1) {
            PYRAMID* newPyramid = new PYRAMID(); //create on CPU RAM stack.
            newPyramid->Randomize();
            {
                std::lock_guard<std::mutex> lock(toCopyThreadMutex);
                commandToCopyThreadQueue.push({ CommandToCopyThreadType::ADD,
                    newPyramid->GetGeometry(), newPyramid->memoryID });
            }
            currentTab->allIDsInThisTab.push_back(newPyramid->memoryID);
            toCopyThreadCV.notify_one();
            lastPyramidAddTime = currentTime; // Reset the timer
        }

        // 1. Process all pending inputs from User, Network, File threads
        ACTION_DETAILS nextWorkTODO;
        while (bool todo = todoCPUQueue.try_pop(nextWorkTODO)) {
            std::cout << "Input received. Action Type = " << static_cast<int>(nextWorkTODO.actionType) <<"\n";
            if (nextWorkTODO.actionType == ACTION_TYPE::CREATEPYRAMID) {
                PYRAMID* newPyramid = new PYRAMID(); //create on CPU RAM stack.
                newPyramid->Randomize();
                {
                    std::lock_guard<std::mutex> lock(toCopyThreadMutex);
                    commandToCopyThreadQueue.push({ CommandToCopyThreadType::ADD,
                        newPyramid->GetGeometry(), newPyramid->memoryID });
                }
                currentTab->allIDsInThisTab.push_back(newPyramid->memoryID);
                toCopyThreadCV.notify_one();
            }
            if (todo == false) { // Means input queue was empty. We should sleep for 1 millisecond.
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }

        frameCounter++;
    } // End of while (!shutdownSignal), i.e. our primary application loop.

    g_logicFenceCV.notify_all(); // Wake up threads for shutdown
    std::cout << "Main Logic Thread shutting down." << std::endl;
}