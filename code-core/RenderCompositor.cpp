// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

// Platform-agnostic part of the compositor. No graphics-API calls in this file: the per-monitor
// render thread (GpuRenderThread, RenderCompositor-<Platform>.cpp) calls into here to decide
// WHAT to render before recording any platform commands.

#include "विश्वकर्मा.h"

#include "RenderCompositor.h"

WindowViewTarget ResolveWindowViewTarget(const SingleUIWindow& window, DATASETTAB& tab) {
    WindowViewTarget target;
    uint64_t activeInternalSubTabMemoryId = 0;
    VishwakarmaStorage::ObjectType activeInternalSubTabType =
        VishwakarmaStorage::ObjectType::Unknown;
    int renderSlot = -1; // Sub-tab slot this window displays. -1 = none.
    const bool contentOnlyWindow = window.windowKind == WINDOW_KIND_VIEW;
    if (contentOnlyWindow) {
        // Extracted view window: render exactly the hosted sub-tab slot, regardless of
        // which sub-tab is active inline. Same GeometryPage, possibly another monitor.
        const uint16_t slot = window.viewSubTabSlot;
        if (slot < MV_MAX_SUBTABS &&
            tab.subTabStates[slot].load(std::memory_order_acquire) == SUBTAB_OPEN) {
            activeInternalSubTabMemoryId = tab.subTabs[slot].containerMemoryId;
            activeInternalSubTabType = tab.subTabs[slot].containerType;
            renderSlot = slot;
        }
    }
    else if (tab.storageObjectsMutex) {
        {
            std::lock_guard<std::mutex> lock(*tab.storageObjectsMutex);
            activeInternalSubTabMemoryId = tab.activeInternalSubTabMemoryId;
        }
        const int activeSlot = FindPublishedSubTabSlot(tab, activeInternalSubTabMemoryId);
        if (activeSlot >= 0) {
            activeInternalSubTabType = tab.subTabs[activeSlot].containerType;
            // A sub-tab lives in exactly 1 view; while that view is extracted into its
            // own window it is not drawn inline as well (any container type).
            if (tab.subTabHostWindowSlots[activeSlot].load(std::memory_order_acquire) >= 0) {
                activeInternalSubTabMemoryId = 0;
                activeInternalSubTabType = VishwakarmaStorage::ObjectType::Unknown;
            }
            else {
                renderSlot = activeSlot;
            }
        }
    }
    target.containerMemoryId = activeInternalSubTabMemoryId;
    target.containerType = activeInternalSubTabType;
    target.renderSlot = renderSlot;
    // Per-view camera: each Scene3D sub-tab carries its own camera; content shown
    // without a sub-tab falls back to the tab-level camera.
    target.camera = renderSlot >= 0 &&
        tab.subTabs[renderSlot].containerType == VishwakarmaStorage::ObjectType::Scene3D
        ? tab.subTabs[renderSlot].camera : tab.camera;
    // Selection overlays and pick requests belong to the view the user interacts
    // with; only the window displaying that view records them, so two windows of the
    // same tab do not clobber the shared per-tab overlay / pick resources.
    target.isInputViewWindow = renderSlot == InputViewSlot(tab);
    return target;
}
