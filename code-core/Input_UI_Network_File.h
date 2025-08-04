// InputThreads.cpp
#include "डेटा.h"
#include <thread>
#include <chrono>
#include <iostream>
#include <random>
#include <chrono>

extern void AddRandomPyramid();
extern ThreadSafeQueueCPU todoCPUQueue;
extern enum class ACTION_TYPE actions;

// Global flag to signal all threads to shut down.
extern std::atomic<bool> shutdownSignal;
// Global queue for inputs to send commands to the Main Logic Thread.

// TODO: Implement a mechanism, to queue KeyPresses, Mouse movement and so on.
void UserInputThread() {
    std::cout << "User Input Thread started." << std::endl;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> distrib(1, 100);

    while (!shutdownSignal) {
        // Simulate user interaction (e.g., creating or modifying an object)
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        std::vector<std::byte> data(distrib(gen) * 16, std::byte{0xAA}); // Create object of random size
        
        // Check timer and add a new pyramid every second. This is used to simulate user Input.
        static std::chrono::steady_clock::time_point lastPyramidAddTime;
        auto currentTime = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(currentTime - lastPyramidAddTime).count() >= 1) {
            todoCPUQueue.push(ACTION_DETAILS( { ACTION_TYPE::CREATEPYRAMID, 0,0,0 }));
            lastPyramidAddTime = currentTime; // Reset the timer
        }
        //std::cout << "USER: Created object " << id << std::endl;
    }
    std::cout << "User Input Thread shutting down." << std::endl;
}

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
        todoCPUQueue.push(ACTION_DETAILS({ ACTION_TYPE::CREATEPYRAMID, 0,0,0 }));
    }
    std::cout << "FILE: Initial bulk load complete." << std::endl;

    while(!shutdownSignal){
        // In a real app, this might poll for file updates. Here, we just sleep.
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
    std::cout << "File Load Thread shutting down." << std::endl;
}