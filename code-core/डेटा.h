// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.

// This files defines our basic data types to be used by other domain specific data types.
#pragma once // Further to this, Global variables defined here need to be defined with "inline" prefix.
#include <cstdint>
#include <vector>
#include <string>
#include <variant>
#include <any>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <optional>
#include <new> // Required for std::align_val_t
#include "ID.h"
#include "MemoryManagerCPU.h"
#include "MemoryManagerGPU.h"
#include <d3dx12.h>
#include <dxgi1_6.h>
#include <wrl.h>
#include <d3dcompiler.h>
#include <DirectXMath.h> //Where from? https://github.com/Microsoft/DirectXMath ?
#include <unordered_map>

using namespace Microsoft::WRL;
using namespace DirectX;

/*Each data type will inherit this base struct.
STRICT WARNING: DO NOT ADD ANY MORE FIELDS TO THIS BASE STRUCT. DO NOT ALTER the sequence.
The sequence of fields in this struct has been specifically planned considering "compact struct packing" approach.
For our Data-Structure design approach, read commentary on MemoryManagerCPU.h
*/
// Forward declaration of the global memory manager.

extern राम cpuMemoryManager;

// A tab is assigned a unique memoryGroupNo, which is shared with the downstream analysis thread and so on.
extern uint32_t memoryGroupNo;

// Data for a single geometry object. All 3D entities will generate one for their own graphics representation.
// Currently we are using common heap, latter on we will transition them to our own heap allocator.
struct Vertex { // Struct for vertex data
    XMFLOAT3 position;
    XMFLOAT4 color;
};

struct GeometryData
{
    uint64_t id = 0; // Unique identifier for the geometry. It is the memoryID of the corresponding engineering object.
    std::vector<Vertex> vertices;
    std::vector<uint16_t> indices;
    XMFLOAT4 color;
	GeometryData() { color = XMFLOAT4(0.8f, 0.8f, 0.8f, 1.0f); } // Default color: light gray
};

struct META_DATA {
    uint64_t memoryID = 0;// This is temporary CPU ID inside currently running software. Scoped within each tab.
    uint64_t memoryIDParent = 0; // This is temporary CPU ID. "0" simply means it has not been initialized.
    uint64_t persistedId = 0;      // This is the unique ID within the saved file.
    uint64_t persistedParentId = 0;// This is the unique ID within the saved file.

    // For each loaded yyy/zzz file, we will have an index. To assist with saving things to disc.
    // This way we can handle a total of 4 Billion loaded files. This is our SYSTEM limit.
    uint32_t xxxFileIndex = 0;
    uint16_t dataType = 0; // Each unique class type. Derived class will set this value. To assist with linear scan etc. 
    uint16_t schemaVersion = 0; // Derived class will set this value. To assist with versioning of data structure.

    // Every time a variable changes, we increment this to signal other threads.
    std::atomic<uint64_t> dataVersion{ 1 }; // Incremented on each modification
    std::atomic<uint64_t> geometryRenderedVersion{ 0 }; // The version last seen by the GPU processing logic
    bool isDeleted{ false };                // Soft-delete flag

	META_DATA() { memoryID = MemoryID::next(); }; // Assign a unique memoryID at creation
	GeometryData GenerateGeometry() { return GeometryData(); }; // Default implementation. Derived class will override.

    /* Overload the `new` operator. This is the magic that intercepts object creation.
    Delegate the allocation request to the global memory manager,
    passing the required size and the current thread's tab ID.
    The manager will handle the raw allocation. C++ runtime then calls the constructor. */
    void* operator new(uint64_t size, uint32_t memoryGroupNo) {
        return cpuMemoryManager.Allocate(size, memoryGroupNo);
    }
    // It's good practice to provide overloaded new/delete for arrays as well.
    void* operator new[](uint64_t size, uint32_t memoryGroupNo) {
        return cpuMemoryManager.Allocate(size, memoryGroupNo);
    }
    /*No downstream derived class should create an object without specifying memoryGroupNo.
    This way we ensure more strict memory partitioning between isolated tabs.
    However in case it is missed anyway than we create on default tab 0. Memory may leak here.*/
    void* operator new(uint64_t size) { return cpuMemoryManager.Allocate(size, 0); };
    void* operator new[](uint64_t size) { return cpuMemoryManager.Allocate(size, 0);};

    // Overload the `delete` operator. Delegate the free request to the global memory manager.
    void operator delete(void* ptr) { cpuMemoryManager.Free(reinterpret_cast<std::byte*>(ptr)); }
    void operator delete[](void* ptr) { cpuMemoryManager.Free(reinterpret_cast<std::byte*>(ptr)); }

    // Disable default heap allocations to prevent accidental misuse.
    // Making these private and not defining them will cause a compile-time error
    // if someone tries to call `::new ArenaObject`.

private:
    static void* operator new(uint64_t size, void* ptr) = delete;
    static void operator delete(void* memory, void* ptr) = delete;
};

// Following are some special data types designed to be dynamically allocated by our RAM Manager.

class CustomString {// System Limit: 4 GB for individual dynamically allocated properties.
    uint32_t allocatedBytes; //In general keep min(20%, 1KB) margin in initial allocation.
    uint32_t usedBytes;   //If used bytes is less than or equal to 8, we don't heap allocate,
    //i.e. store the byte directly in "bytes" variable. It's called Small String Optimization.
    std::byte* str; //utf8 encoded.

    CustomString() {
        allocatedBytes = 0;
        usedBytes = 0;
        str = nullptr;
    }
    //TODO: Improvised on this constructor to use our custom memory allocator.
};

struct c_string {};// To be deleted.

/* We don't have CustomDouble, CustomFloat, CustomInt, CustomLong etc. because they are fixed size variables.
Whenever we want them to optionally available as part of engineering data, and stored in memory when present,
it will be managed OptionalProperties Class as 1 boolean for each such dynamic property.
This way we avoid the high pointer overhead small dynamic properties.*/

/* Following struct demonstrate how various engineering Objects use the metaData.
The FOLDER is also an engineering data, helping with organization of other engineering data in User Interface.
This is our administrative element used for selection tree organization.
This will also provide the short-codes used for naming of various equipments & instruments
buildings, or any way people may want to organize their information.*/
struct FOLDER : META_DATA {
    
    //Mandatory Properties
    char name[128];     //Null terminated utf-8 encoded string.
    char shortCode[16]; //Short Prefix for naming use. utf-8 encoded.
    uint64_t previousSequenceNo, nextSequenceNo; //For display in selection tree.
    //TODO: Design an approach such that we can do sequencing using just 8 bytes instead of 16.

    uint16_t optionalFieldsFlags;  // Bit-mask for up to 16 Optional Fields - 8 Bytes.
    uint16_t systemFlags;          // 32 booleans for internal use only. Not persisted.

    //Variable Length Properties
    CustomString displayName; // Optionally 
};

// Thread-Safe Queue for Inter-Thread Communication 
enum class ACTION_TYPE : uint16_t { // Specifying uint16_t ensures that it is of 2 bytes only.
    // Remember this could change from software release to software release.
    // Hence these values shall not be persisted to disc or sent over network.
    // They are for internal thread coordination purpose only.

    // Keyboard Actions
    KEYDOWN = 0, //Sent when a key is pressed down.
    KEYUP = 1, //Sent when a key is released.
    CHAR = 2, //Sent when a character is input(based on keyboard layout and modifiers).
    DEADCHAR = 3,  //Sent for dead keys(accent characters used in combinations).
    SYSKEYDOWN = 4, //Sent when a system key(e.g., Alt or F10) is pressed.
    SYSKEYUP = 5, //Sent when a system key is released.
    SYSCHAR = 6, //Sent when a system key produces a character.
    SYSDEADCHAR = 7, //Sent for dead characters with system keys.
    UNICHAR = 8, //Used to send Unicode characters(rarely used; fall-back for WM_CHAR with Unicode).

    // Mouse Actions within applications
    MOUSEMOVE = 11, //Mouse moved over the client area.
    LBUTTONDOWN = 12, //Left mouse button pressed.
    LBUTTONUP = 13, //Left mouse button released.
    LBUTTONDBLCLK = 14, //Left button double - clicked.
    RBUTTONDOWN = 15, //Right mouse button pressed.
    RBUTTONUP = 16, //Right mouse button released.
    RBUTTONDBLCLK = 17, //Right button double - clicked.
    MBUTTONDOWN = 18, //Middle mouse button pressed.
    MBUTTONUP = 19, //Middle mouse button released.
    MBUTTONDBLCLK = 20, //Middle button double - clicked.
    MOUSEWHEEL = 21, //Mouse wheel scrolled(vertical).
    MOUSEHWHEEL = 22, //Mouse wheel scrolled(horizontal).
    XBUTTONDOWN = 23, //XButton1 or XButton2 pressed(usually thumb buttons).
    XBUTTONUP = 24, //XButton1 or XButton2 released.
    XBUTTONDBLCLK = 25, //XButton1 or XButton2 double - clicked.

    // Mouse action in Non-Application areas.
    NCMOUSEMOVE = 31, //Mouse moved over title bar, border, etc.
    NCLBUTTONDOWN = 32, //Left button down in non - client area.
    NCLBUTTONUP = 33, //Left button up in non - client area.
    NCLBUTTONDBLCLK = 34, //Double click in non - client area.
    NCRBUTTONDOWN = 35, //Right button down in non - client area.
    NCRBUTTONUP = 36, //Right button up in non - client area.
    NCRBUTTONDBLCLK = 37, //Double click in non - client area.
    NCMBUTTONDOWN = 38, //Middle button down in non - client area.
    NCMBUTTONUP = 39, //Middle button up in non - client area.
    NCMBUTTONDBLCLK = 40, //Double click in non - client area.
    NCXBUTTONDOWN = 41, //X button down in non - client area.
    NCXBUTTONUP = 42, //X button up in non - client area.
    NCXBUTTONDBLCLK = 43, //Double click in non - client area.

    CAPTURECHANGED = 51, //Sent when a window loses mouse capture (e.g., during drag, mouse released outside).
    // We are not yet going for High-precision input. Latter on when we develop snapping mechanism, we may use this.
    INPUT = 52, //For high-precision input (e.g., games), use WM_INPUT after calling RegisterRawInputDevices.

    // Numbers between 0x400 ( = 1024 ) to 0x7FFF ( = 32767 ) are allocated to WM_USER by windows.
    // We should be using this range only for our internal messaging needs. 
    // Whenever in future, we implement inter-process communications we will use this range only.
    // Reserving from 1024 to 10000 for Inter-Process communications.
    // 30000 to 32767 we are reserving for experiments.

    CREATEPYRAMID = 30001
};

struct ACTION_DETAILS {
    ACTION_TYPE actionType;
    uint16_t    data1;
    uint32_t    data2;
    uint64_t    data3; //In case data3 is a pointer, data2 will store the size of bytes stored in data3.
};

class ThreadSafeQueueCPU {
public:
    void push(ACTION_DETAILS value) {
        std::lock_guard<std::mutex> lock(mutex);
        fifoQueue.push(std::move(value));
        cond.notify_one();
    }

    // Non-blocking pop
    bool try_pop(ACTION_DETAILS& value) {
        std::lock_guard<std::mutex> lock(mutex);
        if (fifoQueue.empty()) {
            return false;
        }
        value = std::move(fifoQueue.front());
        fifoQueue.pop();
        return true;
    }

    // Shuts down the queue, waking up any waiting threads
    void shutdownQueue() {
        std::lock_guard<std::mutex> lock(mutex);
        shutdown = true;
        cond.notify_all();
    }

private:
    std::queue<ACTION_DETAILS> fifoQueue; // fifo = First-In First-Out
    std::mutex mutex;
    std::condition_variable cond;
    bool shutdown = false;
};

inline ThreadSafeQueueCPU todoCPUQueue;
