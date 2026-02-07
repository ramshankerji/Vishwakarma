// InputThreads.cpp
#include "डेटा.h"
#include <thread>
#include <chrono>
#include <iostream>
#include <random>
#include <chrono>

#include "UserInputProcessing.h"

extern void AddRandomPyramid();
extern std::vector<DATASETTAB> allTabs;
extern enum class ACTION_TYPE actions;

// Global flag to signal all threads to shut down.
extern std::atomic<bool> shutdownSignal;
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
    for(int i=0; i<10; ++i) {
        allTabs[0].todoCPUQueue->push(ACTION_DETAILS{ .actionType = ACTION_TYPE::CREATEPYRAMID });
    }
    std::cout << "FILE: Initial bulk load complete." << std::endl;

    while(!shutdownSignal){
        // In a real app, this might poll for file updates. Here, we just sleep.
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
    std::cout << "File Load Thread shutting down." << std::endl;
}