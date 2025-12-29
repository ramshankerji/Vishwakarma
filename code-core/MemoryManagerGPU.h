// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.

/*
Windows Desktop C++ DirectX12 application for CAD / CAM use. I need to develop a mini graphics engine for the application.
Here are the requirements:

It will support only 1 Graphics Devices. The one having highest VRAM.
It will support maximum 4 monitors. All monitors will be handled by a dedicated render thread specific to that monitor. 
So total maximum 4 render thread. Monitors can have different refresh rate. 
Application has multiple tabs for multiple project / file opening. Each tab can be managed by 1 or more logic threads.
Each tab can generate multiple views ( windows ) which may be spread out across different monitors. 
However each window will necessarily be handled by 1 monitor thread only.
It should re-render only those views / windows per frame which have been signaled by any of the logic thread to update.
There will be a uniform object it ( 64 bit ) unique across all objects across entire process memory. 
Each object can have up-to 8 different simultaneous variations of vertex geometry / graphics representation.
Each of the representations of an object can have following variations: Opaque / Transparent, Instanced / Custom Vertexes, Index 
/ RGB colors. There will be only 1 Copy thread, catering to all 100s of logic and render thread.
Frame generation shall be locked to the refresh rate of the monitor. Not targeting extreme fps like games.
Our graphics manage calls will reserve (committed) memory in 128 MB Chunks. Create new chunks when existing chunks get full. 
Objects will be located in this committed memory using Placed resource.
It will periodically discard deleted geometry representation. Deletion signaled by render threads.
Once in a while, copy thread will do the defragmentation to free up contiguous memory.
The render command issues separate draw call for different contiguous blocks of memory as applicable. 
I am expecting 1000 to 5000 draw calls per frame. 
How should I handle multiple partially overlapping windows? Each windows can be independently resized or maximized / 
minimized.
The common shapes for use in instantiation, shall be common to all windows / views and shall reside in GPU memory permanently.
Few textures could, such as those belonging to UI / Text could also be permanent residents of GPU Memory.
Logic threads will have different level of details based on camera distance. 
Lowest distance between object and ALL the different view camera position shall be used by logic threads to decided the Level of Detail.
It will have some mechanism to manage memory over pressure. To signal the logic threads to reduce the level of detail within some distance.
Our GPU Memory manager will be a singleton. There will be only 1 instance of that class managing entire GPU memory.

Give me some ideas on how to organize GPU memory when all windows / views can have different subset of objects to be rendered. 
I mean, even if all the objects are stored in memory, and say contiguous for 1 view so that we can set them SetVertexBuffer and 
SetIndexBuffer. However another view could have many vertexes removed from this contiguous list. 
Wouldn't it complicate the SetVertexBuffer / SetIndexBuffer for other views? It seems, I will have explosion in segmented Draw calls,
If I want to reuse the memory. Maybe I can keep separate index buffer for each view pointing into same vertex buffer, 
this way we need to duplicate indexes only per view? Please provide the explanation only. Do not update any code yet.
This logic is for Phase 1. Once we have phase 1 running, I will implement the Computer Shaders in phase 2 to offload some CPU work.

I have a very rudimentary graphics engine working. Obviously not as per the above specification. 
With many dead code blocks as well. Go ahead, implement the complete GPU Memory manager excluding the complex memory defragmentation.
Keep a placeholder function for defragmentation. Take class name from attached file. 
Think over it properly and than reply. I will be mostly copying the code you produce.


Please provide commentary on the above specification. Do not implement the specification yet.
*/

/*
Concept Question: Ask this to any contemporary AI:
Consider a Desktop PC. It has 2 discrete graphics card and 1 integrated graphics card. 1 Monitor is connected and active to each of these 3 devices. 
Can Windows 11 handle moving  application window from 1 screen to another smoothly? 
What if application is attached to 1st device, and window is moved from the monitor connected to 2nd device to the monitor connected to integrated GPU?
*/

