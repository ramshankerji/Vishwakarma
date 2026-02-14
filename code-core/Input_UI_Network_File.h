// InputThreads.cpp
#include "डेटा.h"
#include <thread>
#include <chrono>
#include <iostream>
#include <random>
#include "विश्वकर्मा.h"

#include "UserInputProcessing.h"


extern std::atomic<bool> shutdownSignal; // Global flag to signal all threads to shut down.
// Global queue for inputs to send commands to the Main Logic Thread.

// TODO: Implement windows socket API for listening to other clients.
void NetworkInputThread() {
    std::cout << "Network Thread started." << std::endl;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> distrib(1, 50);

    while (!shutdownSignal) {
        // Simulate receiving an update for an existing object over the network.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
     std::cout << "Network Thread shutting down." << std::endl;
}

// TODO: Implement a mechanism where Vishwakarma Thread can notify the file handler thread
// to load specific files.
void FileInputThread() {
    std::cout << "File Load Thread started." << std::endl;
    // This thread could run once at the start to load a large scene file
    // and then terminate, or it could continuously monitor for file changes.
    // For this example, it loads 10 objects and then sleeps.
    // Push 10 create commands to first active tab (if any)
    uint16_t* tabList = publishedTabIndexes.load(std::memory_order_acquire);
    uint16_t tabCount = publishedTabCount.load(std::memory_order_acquire);

    if (tabCount > 0)
    {
        uint16_t firstTabIndex = tabList[0];
        DATASETTAB& tab = allTabs[firstTabIndex];
        for (int i = 0; i < 10; ++i){
            ACTION_DETAILS action{};
            action.actionType = ACTION_TYPE::CREATEPYRAMID;
            tab.todoCPUQueue->push(action);
        }
    }
    std::cout << "FILE: Initial bulk load complete." << std::endl;

    while(!shutdownSignal){
        // In a real app, this might poll for file updates. Here, we just sleep.
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
    std::cout << "File Load Thread shutting down." << std::endl;
}