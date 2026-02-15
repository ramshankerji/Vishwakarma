// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.

/*
Windows Desktop C++ DirectX12 application for CAD / CAM use.

This file is our Architecture . Premitive data structures common to all platforms may be added here..

At startup, pickup the GPU with highest VRAM. All rendering happens here only. Only 1 device supported for rendering. 
However OS may send the display frame to monitora connected to other / integrated GPU.

VertexLayout Common to all geometry:
3x4 Bytes for Position, 4 Bytes for Normal, 4 Bytes for Color RGBA / 8 Bytes if HDR Monitor present. = 20 / 24 Bytes per vertex.
Anyway go with 24 Bytes format ONLY. Tone mapping (HDR -> SDR) should happen in the Pixel Shader.

Initially Hemispheric Ambient Lighting
Factor = (Normal.z \times 0.5) + 0.5
AmbientLight = Lerp(GroundColor, SkyColor, Factor)
Screen Space Ambient Occlusion (SSAO) to darken creases and corners in future revision.

Seperate render threads (1 per monitor) and single Copy thread. Copy thread is the ringmaster of VRAM!
Seperate render threads per monitor are in VSync with monitors unique refresh rate.  Hehe seperate render queue per monitor.

We use ExecuteIndirect command with start vertex location instead of DrawIndexedInstanced per object.

I want per tab VRAM isolation, each tab will be completely seperate. Except for unclosable tab 0 which stores common textures and UI elements.

Since I want to support 100s of simultaneous tab, I want to start with small heap say 4MB per tab and grow only heap size only when necessary.
Instead of allocating 1 giant 256MB buffer. Don't manually destroy heaps on tab switch. Use Evict. It allows the OS to handle the caching. 
If the user clicks back to a heavy tab, MakeResident is faster than re-creating heaps. Tab 0 is always resident. 
Eviction happens with a time lag of few seconds.  Advanced system memory budget based eviction strategy after rest of spec implemented.

There will be multiple views per tab. Each View will maintain a pair ( double buffered ) of ExecuteIndirect command buffer.
When an object is deleted, copy thread receive command from engineering thread. 
Copy thread than update the next double buffer and record the hole in Vertex/index buffer. Except for currently filling head buffer,

Maintain a Free-List Allocator (e.g., a Segregated Free List) on the CPU. Per Tab.
The Allocator knows: "I have a 12KB middle gap in Page 3, and a 40KB middle gap in Page 8."When a 10KB request comes in,
the Allocator immediately returns "Page 3". No iterating through Page objects.
If freelist says none of existing pages can accomodate new geometry, than create new heap/placedresource buffer.
Free list does not track internal holes created from deleting objects. Only middle empty space. Aggregate holes are tracked per page. Defragmented occasionally.

When a buffer gets >25% holes, it does creates a new defragmented buffer, once complete, switches over to new buffer.
For new geometry addition. Maximum 1 buffer is defragmented at a time (between 2 frames). Since max page size is 64MB, 
This will not produce high latemcy stall during aync with copy thread.

Root Signature puts the "Constants" (View/Proj matrix) in root constants or a very fast descriptor table,
as these don't change between pages. Only the VBV/IBV and the EI Argument Buffer change per batch/page.

Here is the realistic "Worst Case" Hierarchy for a CAD Frame:
• ​Index Depth (2): 16-bit vs 32-bit (Hardware Requirement) Examples: Nuts/Bolts (16) vs Engine Blocks (32)
• ​Transparency (2): Opaque vs Transparent (Sorting Requirement). Transparent objects must be drawn last for alpha blending.
• ​Topology (2): Triangles (Solid) vs Lines (Wireframe) (PSO Requirement). You cannot draw lines and triangles in the same call.
• ​Culling (2): Single-Sided vs Double-Sided (PSO Requirement) . Sheet metal vs Solids.
• ​Buffer Pages (N): How many 256MB blocks you are using.
​Total Unique Batches = 2 \times 2 \times 2 \times 2 \times N = 16 \times N

This will ensure no pipeline state reset while rendering single Page. ExecuteIndirect call for every Page.

‐---------------------------------------------------------------
​The industry standard solution for Normals is not 16-bit floats, but Packed 10-bit Integers.
​We use the format: DXGI_FORMAT_R10G10B10A2_UNORM.
• ​X: 10 bits (0 to 1023) • ​Y: 10 bits (0 to 1023) • ​Z: 10 bits (0 to 1023) • ​Padding: 2 bits (unused) • ​Total: 32 bits (4 Bytes)
​Why this is perfect for Normals:
• ​Size: It is 3x smaller than 12-byte normal. (4 bytes vs 12 bytes).
• ​Precision: 10 bits gives you 2^{10} = 1024 steps. Since normals are always between -1.0 and 1.0, this gives you a precision of roughly 0.002.
This is visually indistinguishable from 32-bit floats for lighting, even in high-end CAD.

Vertex Shader: Normal = Input.Normal * 2.0 - 1.0.
----------------------------------------------------------------

Vertex and Index buffer in same Page : superior architectural choice for three reasons:
• ​Halves the Allocation Overhead: You only manage 1 heap/resource per 4MB page instead of 2.
• ​Cache Locality: When the GPU fetches a mesh, the vertices and indices are physically close in VRAM (same memory page).
This can slightly improve cache hit rates.
• ​Vertices start at Offset 0 and grow UP.
• ​Indices start at Offset Max (4MB) and grow DOWN.
• ​Free Space is always the gap in the middle.
• ​Page Full when Vertex_Head_Ptr meets or crosses Index_Tail_Ptr.
• 32 Bytes mandatory gap in middle to address alignment concerns.

------------------------------------------------
Lazy Creation.
• ​When a user creates a new Tab, allocated memory = 0 MB.
• ​User draws a Bolt (Solid): Allocate Solid_Page_0 (4MB).
• ​User draws a Glass Window: Allocate Transparent_Page_0 (4MB).
• ​User never draws a Wireframe: Wireframe_Page remains null.

Resource state is together . I.e.  D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER |
D3D12_RESOURCE_STATE_INDEX_BUFFER
------------------------------------
Feature					Decision				Benefit
Page Content			Single Type Only		Zero PSO switching during Draw.
Growth Logic			Chained Doubling		4->8->16->32->64. No moving old data.
Max Page Size			64 MB					Prevents fragmentation failure on low-VRAM GPUs.
Allocation				Lazy (On Demand)		Keeps "Hello World" tabs lightweight.
Sub-Allocation			Double-Ended Stack		Maximizes usage for varying ratio of Vertex/Index Buffers.

----‐--------------------------
New geometry is appended (in the middle ) only if both new vertex and index buffers fit inside. Otherwise allocate new buffer.
Copy thread also does batching. It aggregates all(who fit in  current buffer) objects coming from engineering thread into single GPU upload.
The Copy Thread should consume batches of updates, coalescing them into single ExecuteCommandList calls where possible to reduce API overhead.

"Big Buffer" fallback. If Allocation_Size > Max_Page_Size, allocate a dedicated Committed Resource just for that object, bypassing the paging system.
Handles large STL. or terrain map. Treat "Big Buffers" as a special Page Type. Add a "Large Object List" to your loop.
Do not try to jam them into the standard EI logic if they require unique resource bindings per object. 1 seperate draw command for such Jumbo objects.

Create a separate std::vector<BigObject> in Tab structure. Rendering:
• ​Loop through Pages (ExecuteIndirect).
• ​Loop through BigObjects (Standard DrawIndexedInstanced or EI with count 1).

Defragmentation Logic:
Copy queue marks the page for defragmentation. All frames of that tab freeze. Keep presenting previous render output.
Any 1 of the rendering thread/queue reads the mark, Transition the resource to Common. Signal a fence.
Copy queue picks it up , once defragmented, return the new resource . I am willing to accept the freeze of few frames on screen.
This is a recognised engineering  tradeoff. Acceptable to CAD users.

EI Argument Buffers tightly coupled to the Memory Pages. When you defragment a Page, you must simultaneously rebuild its corresponding Argument Buffer.
Do not try to "patch" the Argument buffer; regenerate it for that Page.

Growth Logic: Similar to above defragmentation. How does my copy queue handle async ( without blocking render thread?) 
addition of 1 small geometry  say 10kb to already existing 64MB heap out of which 50MB is filled up. 
All Views/frames of that particular tab freeze. However other tabs being handled by render thread keep processing.
No thread stall. Transition that page to copy destination. Copy new data. Transituon back to render status for render thread to pick up.

--------------------------
RenderToTexture to implement frame freeze since swap chain is FLIP_DISCARD. 
Side benefits? ✔ HDR handling ✔ UI composition ✔ Multi-monitor flexibility ✔ Eviction safety ✔ Clean defrag freezes

-------------------------------------------------------------------------------
Known Issues / Limitations (to be resolved in latter revision):
1. Transparency sorting. accepting imperfect sorting for "Glass" pages during rotation, and doing a CPU Sort + Args Rebuild only when the camera stops.
2. Hot page for object drag / active mutation.
3. Evict logic.
4. Comput shader frustum culling.
5. Telemetry. Per-tab VRAM usage graphs. Page fragmentation heatmap. Eviction frequency counters. Copy queue stall tracking
6. Selection Highlighter methodology.
7. Mesh Shader on supported hardware (RTX2000 onwards, RX6000 onwards).
8. Instanced based LOD optimization . Optionally using compute shader.

Find out any other architectural pitfalls / challenges I need to look after. Think over it for long and reply.

With this our graphics design document ends.
-------------------------------------------------------------------------------
Miscellaneous aspects of Specification: 
There will be a uniform object it ( 64 bit ) unique across all objects across entire process memory. 
Each object can have up-to 16? different simultaneous variations of vertex geometry / graphics representation.
I am expecting 1000 to 5000 draw calls per frame ?
How should I handle multiple partially overlapping windows? Each windows can be independently resized or maximized / minimized.
Lowest distance between object and ALL the different view camera position shall be used by logic threads to decided the Level of Detail.
It will have some mechanism to manage memory over pressure. To signal the logic threads to reduce the level of detail within some distance.
Our GPU Memory manager will be a singleton. There will be only 1 instance of that class managing entire GPU memory.

Consider a Desktop PC. It has 2 discrete graphics card and 1 integrated graphics card.
1 Monitor is connected and active to each of these 3 devices. We can use exactly 1 device for rendering for all monitor!
Windows 10/11 WDDM supports heterogeneous multi-adapter. When window moves: DWM composites surfaces.
Frame copied across adapters if needed. This works but is slow since all frames need to traverse PCIe bus.
*/

/*
-------------------------------------------------------------------------------
TO DO LIST : As things get completed, they will be removed from this pending list and get incorporated appropriately in design document.
-------------------------------------------------------------------------------
Phase 1: The Visual Baseline (Get these out of the way)
Do this first so you aren't fighting "black screen" bugs later.
[Done] Release Downloads (New Repository).
[Done] Update Vertex format to include Normals. (Required for lighting).
[Done] Hemispherical Lighting in shader. (Verify normals are correct).
[Done] Mouse Zoom/Pan/Rotate (Basic).

Phase 2: The "Freeze" Infrastructure
Before you break the memory model, build the mechanism that hides the breakage.
[ ] Render To Texture (RTT) & Full-Screen Quad.
Goal: Detach the "Drawing" from the "Presenting."
Success State: You can resize the window, and the inner "Canvas" scales or freezes independently of the window border.
[ ] Face-wise Geometry colors. (Implementation detail).
[ ] Upgrade Vertices to HDR + Tonemapping. (Do this now while touching pixel shaders).

Phase 3: The API Pivot (The Hardest Part)
Switching to ExecuteIndirect changes how you pass data. Do this BEFORE implementing custom heaps to isolate variables.
[ ] [MISSING] Implement Structured Buffer for World Matrices.
Critical: You cannot do ExecuteIndirect for multiple objects without a way to tell the shader which object is being drawn. You need a StructuredBuffer<float4x4> and a root constant index.
[ ] DrawIndexedInstanced → ExecuteIndirect (EI).
Advice: Implement this using your current committed resources first. Just get the API call working.
[ ] Double buffered ExecuteIndirect Arguments.

Phase 4: The Memory Manager (The "Vishwakarma" Core)
Now that EI is working, replace the backing memory.
[ ] [MISSING] Global Upload Ring Buffer.
Critical: Your copy thread needs a staging area. If you don't build this, your "VRAM Pages" step will stall waiting on CreateCommittedResource for uploads.
[ ] VRAM Pages per Tab (The Stack Allocator).
Advice: Implement the "Double-Ended Stack" (Vertex Up, Index Down) here.
[ ] CPU-Side Free List Allocator. (The logic that tracks the holes).
[ ] Tab Management / View Management. (Integrating the heaps into the UI).

Phase 5: Advanced Features & Polish
[ ] VRAM Defragmentation. (Now safe to implement because RTT exists).
[ ] Click Selection / Window Selection. (Requires Raycasting against your CPU Free List/Data structures).
[ ] Instanced optimization for Pipes.
[ ] SSAO.

*/

#include <DirectXMath.h>

struct CameraState { // Each view gets its own camera state. 
    //This is part of the "View" data structure, not the "Tab" data structure. Each tab can have multiple views.
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT3 target;
    DirectX::XMFLOAT3 up;
    float fov;
    float aspect;
    float nearZ;
    float farZ;

    CameraState() { Initialize(); }
    void Initialize() {
        position = { 0.0f, -10.0f, 2.0f };
        target = { 0.0f, 0.0f,  0.0f };
        up = { 0.0f, 0.0f,  1.0f }; // Z-Up is perfect for an XY orbit.

        fov = DirectX::XMConvertToRadians(60.0f);
        aspect = 1.0f; // SAFE DEFAULT
        nearZ = 0.1f;
        farZ = 1000.0f;
    }
};

inline void InitCamera(CameraState& cam, float aspectRatio)
{
    cam.position = { 0.0f, -10.0f, 2.0f };
    cam.target = { 0.0f, 0.0f,  0.0f };
    cam.up = { 0.0f, 0.0f,  1.0f }; // Z-Up is perfect for an XY orbit.

    cam.fov = DirectX::XMConvertToRadians(60.0f);
    cam.aspect = aspectRatio;
    cam.nearZ = 0.1f;
    cam.farZ = 1000.0f;
}

inline void UpdateCameraOrbit(CameraState& cam)
{
    static float rotationAngle = 0.0f;
    rotationAngle += 0.002f;   // per-frame speed 

    // Calculate the 2D radius from the target on the XY plane. We ignore Z here to prevent the "spiral away" bug.
    float dx = cam.position.x - cam.target.x;
    float dy = cam.position.y - cam.target.y;
    float radius = hypotf(dx, dy);
    if (radius < 0.001f) radius = 10.0f;// Safety check to prevent radius becoming 0 (which locks the camera)

    float x = cam.target.x + cosf(rotationAngle) * radius; // Orbit in XY plane
    float y = cam.target.y + sinf(rotationAngle) * radius;
    float z = cam.position.z;// Z remains static (height)
    cam.position = { x, y, z };
}
