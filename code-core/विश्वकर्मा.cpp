// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.

/*This is the application's orchestrator. It consumes commands, updates the scene database, identifies dirty objects, 
and generates work for the GPU threads.
This thread is also responsible for engineering calculations, consistency of Data etc.
*/
#include "विश्वकर्मा.h"

// MainLogicThread.cpp
#include "डेटा.h"
#include "CPURAM-Manager.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <vector>

// --- Externs for communication ---
extern std::atomic<bool> shutdownSignal;
extern राम cpuRAMManager;

// The "fence" for the Main Logic thread. Signals that a frame's logic is complete.
extern std::mutex g_logicFenceMutex;
extern std::condition_variable g_logicFenceCV;
extern uint64_t g_logicFrameCount;

// A shared pointer to the latest render packet for the render threads
extern std::mutex g_renderPacketMutex;
extern RenderPacket g_renderPacket;
extern void AddRandomPyramid();

void विश्वकर्मा() { //Main logic thread. The ringmaster of the application.
    std::cout << "Main Logic Thread विश्वकर्मा started." << std::endl;
    uint64_t frameCounter = 0;

    while (!shutdownSignal) {
        auto frameStart = std::chrono::high_resolution_clock::now();

        // 1. Process all pending inputs from User, Network, File threads
        ACTION_DETAILS nextWorkTODO;
        while (bool todo = todoCPUQueue.try_pop(nextWorkTODO)) {
            std::cout << "Input received. Action Type = " << static_cast<int>(nextWorkTODO.actionType) <<"\n";
            if (nextWorkTODO.actionType == ACTION_TYPE::CREATEPYRAMID) {
                AddRandomPyramid();
            }
            if (todo == false) { // Means input queue was empty. We should sleep for 1 millisecond.
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }

        // 2. Scene Logic, Culling, and Identifying Dirty Objects
        std::vector<uint64_t> visibleObjectIds;
        auto idMap = cpuRAMManager.GetIdMapSnapshot();

        for (const auto& [id, location] : idMap) {
            auto headerOpt = cpuRAMManager.GetMETA_DATA(id);
            if (!headerOpt) continue;
            META_DATA* header = *headerOpt;

            // Simple culling: don't render deleted objects.
            if (header->isDeleted) {
                // If it's deleted and hasn't been processed, tell GPU to free it.
                // A real app would check if it has a corresponding GPU resource first.
                if (header->lastProcessedVersion == 0) { // Simple check
                    // This is where we would create a GpuFreeCmd.
                    // For simplicity, we assume the GpuThreads.cpp handles this.
                }
                continue; // Don't add to render packet
            }
            
            visibleObjectIds.push_back(id); // For now, all non-deleted objects are visible

            uint64_t currentVersion = header->dataVersion.load(std::memory_order_relaxed);
            if (currentVersion > header->lastProcessedVersion) {
                // This object is "dirty". Generate work for the GPU Copy thread.
                //std::cout << "LOGIC: Object " << id << " is dirty. Version: " << currentVersion << std::endl;
                
                auto payloadOpt = cpuRAMManager.GetObjectPayload(id);
                if (payloadOpt) {
                    // In a real app, you'd generate vertices from this payload.
                    // Here, we just copy the raw payload as a stand-in for vertex data.
                    std::vector<std::byte> gpuData(header->dataSize);
                    std::memcpy(gpuData.data(), *payloadOpt, header->dataSize);

                    GpuUploadCmd gpuCmd{id, currentVersion, std::move(gpuData)};
                    //g_gpuCommandQueue.push(GpuCommand(gpuCmd));
                }

                // Mark as processed
                header->lastProcessedVersion = currentVersion;
            }
        }
        
        // 3. Prepare Render Packet for Render Threads
        {
            std::lock_guard<std::mutex> lock(g_renderPacketMutex);
            g_renderPacket.frameNumber = frameCounter;
            g_renderPacket.visibleObjectIds = std::move(visibleObjectIds);
        }

        // 4. Signal Fence: Let the GPU Copy Thread know it can start processing this frame's data.
        {
            std::lock_guard<std::mutex> lock(g_logicFenceMutex);
            g_logicFrameCount = frameCounter;
        }
        g_logicFenceCV.notify_all(); // Wake up any threads waiting on this fence.

        // --- Frame Limiter for this thread (optional) ---
        // std::this_thread::sleep_until(frameStart + std::chrono::milliseconds(16)); // ~60 FPS
        frameCounter++;
    }
    g_logicFenceCV.notify_all(); // Wake up threads for shutdown
    std::cout << "Main Logic Thread shutting down." << std::endl;
}