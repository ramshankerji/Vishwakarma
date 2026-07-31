// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
#pragma once
#include <cstdint>

#include "CommonNamedNumbers.h" // VishwakarmaStorage::ObjectType
#include "RenderScene3D.h"      // CameraState

struct SingleUIWindow;
struct DATASETTAB;

// Which container/view a window displays this frame. Resolved once per window per frame by the
// render thread (GpuRenderThread) before dispatching to the Scene3D / Page2D renderers.
struct WindowViewTarget {
    uint64_t containerMemoryId = 0; // Primary container. 0 = nothing to render (view extracted elsewhere).
    // Every container this SubTab draws (10M plan Step 6). Empty when there is nothing to render;
    // the renderers consult this rather than containerMemoryId, which stays for identity/UI use.
    SubTabContainerSet containers;
    VishwakarmaStorage::ObjectType containerType = VishwakarmaStorage::ObjectType::Unknown;
    int renderSlot = -1;            // Sub-tab slot this window displays. -1 = none.
    CameraState camera;             // Per-view camera (Scene3D) or tab-level fallback.
    bool isInputViewWindow = false; // Only this window records selection overlays / picks.
};

// Platform-agnostic heart of the compositor: decide which sub-tab/view of the tab this window
// shows (content-only extracted view vs inline active sub-tab, extracted views never inline).
WindowViewTarget ResolveWindowViewTarget(const SingleUIWindow& window, DATASETTAB& tab);
