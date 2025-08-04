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

/*Each data type will inherit this base struct.
STRICT WARNING: DO NOT ADD ANY MORE FIELDS TO THIS BASE STRUCT.
The sequence of fields in this struct has been specifically planned considering 
"compact struct packing" approach. DO NOT ALTER the sequence.

For our Data-Structure design approach, reach commentary on डेटा-CPURAM-Manager.h
*/
// Represents the meta-data stored at the beginning of each object in a RAM chunk.
struct META_DATA {
    uint64_t id;                  // 8 bytes 
    uint64_t parentId;      // 8 bytes 
    
    // In general our dataSize will be maximum few kilo bytes per object only.
    // The following will limit the maximum external data (ex: Image, PDF, other native files etc.)
    // TODO: Notice that our MEMORY manager is not capable to handle more then 4MB data per object.
    uint32_t dataSize; // Total size of data of the inherited struct, inclusive of optional fields.

    // For each loaded yyy/zzz file, we will have an index. To assist with saving things to disc.
    // This way we can handle a total of 4 Billion loaded files. This is our SYSTEM limit.
    uint32_t xxxFileIndex;

    uint16_t dataType; // Each unique class type. To assist with linear scan etc.

    uint16_t reserved1; //reserved for future use.
    uint16_t reserved2; //reserved for future use.
    uint16_t reserved3; //reserved for future use.
    
    // So we have minimum 32 Byte overhead per Object.

    // --- Architecture-specific fields ---
    std::atomic<uint64_t> dataVersion{ 1 }; // Incremented on each modification
    uint64_t lastProcessedVersion{ 0 };     // The version last seen by the GPU processing logic
    bool isDeleted{ false };                // Soft-delete flag
    // Padding to ensure 8-byte alignment can be added if necessary
};

// Following are some special data types designed to be dynamically allocated by our RAM Manager.

struct c_string {// System Limit: 4 GB for individual dynamically allocated properties.
    uint32_t allocatedBytes; //In general keep min(20%, 1KB) margin in initial allocation.
    uint32_t usedBytes;   //If used bytes is less than or equal to 8, we don't heap allocate,
    //i.e. store the byte directly in "bytes" variable. It's called Small String Optimization.
    std::byte* str; //utf8 encoded.
};

// System Limit: 4 GB for individual dynamically allocated properties.
struct c_double {
    // Used by Polylines, Contour Plates etc.
    uint32_t allocatedBytes; //In general keep min(20%, 1KB) margin in initial allocation.
    uint32_t usedBytes;
    double* d;
};

struct c_float {// System Limit: 4 GB for individual dynamically allocated properties.
    // Used by Polylines, Contour Plates etc.
    uint32_t allocatedBytes; //In general keep min(20%, 1KB) margin in initial allocation.
    uint32_t usedBytes;
    float* n;
};

struct c_uint64 {// System Limit: 4 GB for individual dynamically allocated properties.
    // Used by Polylines, Contour Plates etc.
    uint32_t allocatedBytes; //In general keep min(20%, 1KB) margin in initial allocation.
    uint32_t usedBytes;
    uint64_t* n;
};

struct c_uint32 {// System Limit: 4 GB for individual dynamically allocated properties.
    // Used by Polylines, Contour Plates etc.
    uint32_t allocatedBytes; //In general keep min(20%, 1KB) margin in initial allocation.
    uint32_t usedBytes;
    uint32_t* n;
};

struct c_uint16 {// System Limit: 4 GB for individual dynamically allocated properties.
    // Used by Polylines, Contour Plates etc.
    uint32_t allocatedBytes; //In general keep min(20%, 1KB) margin in initial allocation.
    uint32_t usedBytes;
    uint16_t* n;
};

struct c_uint8 {// System Limit: 4 GB for individual dynamically allocated properties.
    // Used by Polylines, Contour Plates etc.
    uint32_t allocatedBytes; //In general keep min(20%, 1KB) margin in initial allocation.
    uint32_t usedBytes;
    uint8_t* n;
};

struct FOLDER {
    META_DATA metaData;
    /*This is our administrative element used for selection tree organization.
    This will also provide the short-codes used for naming of various equipments & instruments
    buildings, or any way people may want to organize their information.*/

    //Mandatory Properties
    char name[128];     //Null terminated utf-8 encoded string.
    char shortCode[16]; //Short Prefix for naming use. utf-8 encoded.
    uint64_t previousSequenceNo, nextSequenceNo; //For display in selection tree.
    //TODO: Design an approach such that we can do sequencing using just 8 bytes instead of 16.

    uint16_t optionalFieldsFlags;  // Bit-mask for up to 16 Optional Fields - 8 Bytes.
    uint16_t systemFlags;          // 32 booleans for internal use only. Not persisted.

    //Optional Properties
    c_string displayName; // Optionally 
};

//////////////////////////////////////////////////////////

// --- Thread-Safe Queue for Inter-Thread Communication ---
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

// --- Command Definitions for the Input->MainLogic Queue ---


// RAM Manager related structs.
struct CreateObjectCmd {
    uint64_t desiredId;
    uint64_t parentId;
    std::vector<std::byte> data; // The actual object data
};

struct ModifyObjectCmd {
    uint64_t id;
    std::vector<std::byte> newData;
};

struct DeleteObjectCmd {
    uint64_t id;
};

// Using a variant for type-safe command storage
using InputCommand = std::variant<CreateObjectCmd, ModifyObjectCmd, DeleteObjectCmd>;
///////////////////////////////////////////////////////////////////////////////////////////////////
// --- GPU-related Definitions ---

// Information about a resource currently residing in VRAM
struct GpuResourceInfo {
    uint64_t vramOffset; // Simulated VRAM address
    size_t size;
    // In a real DX12 app, this would hold ID3D12Resource*, D3D12_VERTEX_BUFFER_VIEW, etc.
};

// Commands for the MainLogic -> GpuCopy Thread Queue
struct GpuUploadCmd {
    uint64_t objectId;
    uint64_t objectDataVersion;
    std::vector<std::byte> data; // Vertex/Index data to be uploaded
};

struct GpuFreeCmd {
    uint64_t objectId;
    GpuResourceInfo resourceToFree; // The specific VRAM resource to be reclaimed
};

using GpuCommand = std::variant<GpuUploadCmd, GpuFreeCmd>;

// Packet of work for a Render Thread for one frame
struct RenderPacket {
    uint64_t frameNumber;
    std::vector<uint64_t> visibleObjectIds;
};

class ThreadSafeQueueGPU {
public:
    void push(GpuCommand value) {
        std::lock_guard<std::mutex> lock(mutex);
        fifoQueue.push(std::move(value));
        cond.notify_one();
    }

    // Non-blocking pop
    bool try_pop(GpuCommand& value) {
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
    std::queue<GpuCommand> fifoQueue; // fifo = First-In First-Out
    std::mutex mutex;
    std::condition_variable cond;
    bool shutdown = false;
};

inline ThreadSafeQueueGPU g_gpuCommandQueue;



