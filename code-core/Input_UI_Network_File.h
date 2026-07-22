// InputThreads.cpp
#include "डेटा.h"
#include <thread>
#include <chrono>
#include <iostream>
#include <random>
#include "विश्वकर्मा.h"

#include "ApplicationTab.h"
#include "UserInputProcessing.h"
#include "ExtensionCommunications.h"
#include "RenderPage2D.h" // Cad2DFindTargetPage2DMemoryId for the DXF auto-import dev hook.


extern std::atomic<bool> shutdownSignal; // Global flag to signal all threads to shut down.
// Global queue for inputs to send commands to the Main Logic Thread.

// "First tab" for the bulk load and the dev/testing hooks below means the first ENGINEERING tab:
// the published list starts with the Application Tab, whose todoCPUQueue has no consumer and must
// never be pushed to (tabs.md Decision 3).
static DATASETTAB* FirstEngineeringTab() {
    uint16_t* tabList = publishedTabIndexes.load(std::memory_order_acquire);
    uint16_t tabCount = publishedTabCount.load(std::memory_order_acquire);
    for (uint16_t i = 0; i < tabCount; ++i) {
        if (!ApplicationTab::IsApplicationTab(tabList[i])) return &allTabs[tabList[i]];
    }
    return nullptr;
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
    // Push 10 create commands to first active tab (if any)
    if (DATASETTAB* tab = FirstEngineeringTab())
    {
        for (int i = 0; i < 10; ++i){
            ACTION_DETAILS action{};
            action.actionType = ACTION_TYPE::CREATEPYRAMID;
            tab->todoCPUQueue->push(action);
        }

    }

    // Dev/testing hook: auto-import a STAAD file at startup when requested.
    // Polls for a published tab first, since tabs may not exist yet when this
    // thread starts.
    wchar_t autoImportStd[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"VISHWAKARMA_AUTO_IMPORT_STD", autoImportStd, MAX_PATH) > 0) {
        for (int attempt = 0; attempt < 100 && !shutdownSignal; ++attempt) {
            if (DATASETTAB* tab = FirstEngineeringTab()) {
                ExtensionCommunications::QueueImportStdFile(tab, autoImportStd);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

#ifdef _DEBUG
    // Dev/testing hook (Debug only): activate the default Page2D, import a DXF into it,
    // then fit the view — exercises the whole 2D import pipeline and its console
    // diagnostics without any GUI interaction.
    wchar_t autoImportDxf[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"VISHWAKARMA_AUTO_IMPORT_DXF", autoImportDxf, MAX_PATH) > 0) {
        for (int attempt = 0; attempt < 300 && !shutdownSignal; ++attempt) {
            if (FirstEngineeringTab()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (DATASETTAB* firstTab = FirstEngineeringTab()) {
            DATASETTAB& tab = *firstTab;
            uint64_t pageMemoryId = 0;
            for (int attempt = 0; attempt < 300 && !shutdownSignal; ++attempt) {
                pageMemoryId = Cad2DFindTargetPage2DMemoryId(tab);
                if (pageMemoryId != 0) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (pageMemoryId != 0 && tab.todoCPUQueue) {
                ACTION_DETAILS open{};
                open.actionType = ACTION_TYPE::OPEN_INTERNAL_SUB_TAB;
                open.source = INPUT_SOURCE::SYSTEM;
                open.objectId = pageMemoryId;
                open.timestamp = GetTickCount64();
                tab.todoCPUQueue->push(open);
                ExtensionCommunications::QueueImportDxfFile(&tab, autoImportDxf);
                // Give the worker + copy-thread ingest time to finish, then fit the view.
                for (int i = 0; i < 300 && !shutdownSignal; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                ACTION_DETAILS zoom{};
                zoom.actionType = ACTION_TYPE::ZOOM_MAX_EXTENTS;
                zoom.source = INPUT_SOURCE::SYSTEM;
                zoom.timestamp = GetTickCount64();
                tab.todoCPUQueue->push(zoom);
                std::cout << "FILE: auto-import DXF queued with zoom-extents." << std::endl;
            } else {
                std::cout << "FILE: auto-import DXF found no Page2D." << std::endl;
            }
        }
    }
#endif
    std::cout << "FILE: Initial bulk load complete." << std::endl;

    while(!shutdownSignal){
        // In a real app, this might poll for file updates. Here, we just sleep.
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
    std::cout << "File Load Thread shutting down." << std::endl;
}