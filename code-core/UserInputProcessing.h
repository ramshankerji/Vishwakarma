// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.

/*
Application Behaviour :

Main philosophy is that Input processing shall be agnostic to Operating System. 
So Windows App, Linux App, Mac App all will share the logic, leading to consistent behaviour on all platforms.
Their respective OS Layer will be only responsible for relaying the input to the correct engineering thread. 
Ex: Windows Procedure shall have sole responsibility to pass on the events to the tab in focus in current Window.
The engineering thread shall be responsible for processing / throttling it. 
Even identifying if the input is over Render Area or UI area is active tab's engineering thread job.
There must not be any mutexes between engineering thread and render thread since render thread picks up the 
camera state every frame,it will pick the latest value eventually. 1 frame delay is acceptable.
Engineering thread may be doing further processing like LOD adjustment etc. once it has updated the camera state variables.

Humans are in general limited by speed of physical movements. 
A real user can't be expected to realistically provide >200 keyboard input per second. Or >100 clicks per second.
A user could be typing 1 field at a time only. Though engineering threads may take >1 seconds to process all data.
This is why we have global input receiver, whereas all engineering threads pick up their relevant tasks.

In windows OS, the thread creating the UI Window gets the windows messages ( mouse keyboard etc.) of that window.
To keep it simple, all UserVisible windows are created by Main Thread only.
Main thread is also responsible for routing the windows messages to the correct engineering thread.
Main thread is also responsible for tab extraction / view extraction / resize / move / minimize / maximize etc.
Remember main thread is not doing any heavy lifting. It just routes the messages to correct engineering thread.

Gotcha to remember:
If user presses Left Mouse button and keep it pressed, than user presses Right mouse button and keep it pressed, 
the OS does not automatically release and fire the left mouse button UP! 
If another user disconnects the USB mouse after both buttons were kept pressed, we totally lost the mouse UP events.
This is why we must handle the device disconnect events and reset all button states in all tabs of all windows. ;-)

If we use a single global queue with "Target Tab IDs," 
we will run into a classic concurrency problem called Head-of-Line Blocking. 
Hence all tabs get their own input queues. Main thread simply routes the messages to correct tab's queue.

*/


// Thread-Safe Queue for Inter-Thread Communication 

#pragma once
#include <cstdint>
enum class ACTION_TYPE : uint16_t { // Specifying uint16_t ensures that it is of 2 bytes only.
    // Remember this could change from software release to software release.
    // Hence these values shall not be persisted to disc or sent over network.
    // They are for internal thread coordination purpose only.

	// Keyboard Actions [ 0 to 100 reserved for keyboard actions ]
    KEYDOWN = 0, //Sent when a key is pressed down.
    KEYUP = 1, //Sent when a key is released.
    CHAR = 2, //Sent when a character is input(based on keyboard layout and modifiers).
    DEADCHAR = 3,  //Sent for dead keys(accent characters used in combinations).
    SYSKEYDOWN = 4, //Sent when a system key(e.g., Alt or F10) is pressed.
    SYSKEYUP = 5, //Sent when a system key is released.
    SYSCHAR = 6, //Sent when a system key produces a character.
    SYSDEADCHAR = 7, //Sent for dead characters with system keys.
    UNICHAR = 8, //Used to send Unicode characters(rarely used; fall-back for WM_CHAR with Unicode).

	// Mouse Actions within applications [ 101 to 150 reserved for mouse actions in client area ]
    MOUSEMOVE = 101, //Mouse moved over the client area.
    LBUTTONDOWN = 102, //Left mouse button pressed.
    LBUTTONUP = 103, //Left mouse button released.
    LBUTTONDBLCLK = 104, //Left button double - clicked.
    RBUTTONDOWN = 105, //Right mouse button pressed.
    RBUTTONUP = 106, //Right mouse button released.
    RBUTTONDBLCLK = 107, //Right button double - clicked.
    MBUTTONDOWN = 108, //Middle mouse button pressed.
    MBUTTONUP = 109, //Middle mouse button released.
    MBUTTONDBLCLK = 110, //Middle button double - clicked.
    MOUSEWHEEL = 111, //Mouse wheel scrolled(vertical).
    MOUSEHWHEEL = 112, //Mouse wheel scrolled(horizontal).
    XBUTTONDOWN = 113, //XButton1 or XButton2 pressed(usually thumb buttons).
    XBUTTONUP = 114, //XButton1 or XButton2 released.
    XBUTTONDBLCLK = 115, //XButton1 or XButton2 double - clicked.

	// Mouse action in Non-Application areas. [ 151 to 200 reserved for mouse actions in non-client area ]
    NCMOUSEMOVE = 151, //Mouse moved over title bar, border, etc.
    NCLBUTTONDOWN = 152, //Left button down in non - client area.
    NCLBUTTONUP = 153, //Left button up in non - client area.
    NCLBUTTONDBLCLK = 154, //Double click in non - client area.
    NCRBUTTONDOWN = 155, //Right button down in non - client area.
    NCRBUTTONUP = 156, //Right button up in non - client area.
    NCRBUTTONDBLCLK = 157, //Double click in non - client area.
    NCMBUTTONDOWN = 158, //Middle button down in non - client area.
    NCMBUTTONUP = 159, //Middle button up in non - client area.
    NCMBUTTONDBLCLK = 160, //Double click in non - client area.
    NCXBUTTONDOWN = 161, //X button down in non - client area.
    NCXBUTTONUP = 162, //X button up in non - client area.
    NCXBUTTONDBLCLK = 163, //Double click in non - client area.

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

struct ACTION_DETAILS_OLD {
    ACTION_TYPE actionType;
    uint16_t    data1;
    uint32_t    data2;
    uint64_t    data3; //In case data3 is a pointer, data2 will store the size of bytes stored in data3.
};

enum class INPUT_SOURCE {
    MOUSE,
    KEYBOARD,
    SYSTEM
};

// Raw data container to pass from WndProc to Engineering Thread
struct ACTION_DETAILS {
    ACTION_TYPE actionType; // e.g., MOUSE_MOVE, KEY_DOWN, RESIZE
    INPUT_SOURCE source;
    
    // Raw Data Fields
    int x;       // Mouse X or Key Code
    int y;       // Mouse Y or Modifier Flags
    int delta;   // Scroll wheel delta
    uint64_t timestamp; // Time of event (crucial for accurate physics/throttling)
    
    // Window/View context (if needed by logic)
    // uint64_t windowID; 
};