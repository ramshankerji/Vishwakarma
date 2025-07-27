// InputThreads.cpp
#include "डेटा.h"
#include <thread>
#include <chrono>
#include <iostream>
#include <random>

// Global flag to signal all threads to shut down.
extern std::atomic<bool> shutdownSignal;
// Global queue for inputs to send commands to the Main Logic Thread.
extern ThreadSafeQueue<InputCommand> g_inputCommandQueue;

// Used to generate unique IDs for new objects.
std::atomic<uint64_t> g_nextObjectId = 1;

// TODO: Implement a mechanism, to queue KeyPresses, Mouse movement and so on.
void UserInputThread() {
    std::cout << "User Input Thread started." << std::endl;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> distrib(1, 100);

    while (!shutdownSignal) {
        // Simulate user interaction (e.g., creating or modifying an object)
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        uint64_t id = g_nextObjectId.fetch_add(1);
        std::vector<std::byte> data(distrib(gen) * 16, std::byte{0xAA}); // Create object of random size
        
        CreateObjectCmd cmd{id, 0, data};
        g_inputCommandQueue.push(InputCommand(cmd));
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

        if (g_nextObjectId > 5) {
            uint64_t idToModify = distrib(gen);
            if (idToModify > 0) {
                 std::vector<std::byte> data(distrib(gen) * 24, std::byte{0xBB});
                 ModifyObjectCmd cmd{idToModify, data};
                 g_inputCommandQueue.push(InputCommand(cmd));
                 //std::cout << "NET: Modified object " << idToModify << std::endl;
            }
        }
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
        uint64_t id = g_nextObjectId.fetch_add(1);
        std::vector<std::byte> data(1024, std::byte{0xCC}); // 1KB objects
        CreateObjectCmd cmd{id, 0, data};
        g_inputCommandQueue.push(InputCommand(cmd));
    }
    std::cout << "FILE: Initial bulk load complete." << std::endl;

    while(!shutdownSignal){
        // In a real app, this might poll for file updates. Here, we just sleep.
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
    std::cout << "File Load Thread shutting down." << std::endl;
}