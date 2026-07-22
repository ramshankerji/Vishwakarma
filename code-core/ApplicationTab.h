// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
#pragma once

// The Application Tab (website/content/software/tabs.md): allTabs[0]. It carries no engineering
// file, cannot be closed, extracted or merged, and has NO engineering thread - the main UI thread
// is its only writer. Its memory group (tabNo 0) stays reserved for the future instancing masters.

#include <cstdint>

struct DATASETTAB;
struct UIDrawContext;   // UserInterface.h
struct DX12ResourcesUI; // Platform GPU resources; full definition in UserInterface-<Platform>.h.

namespace ApplicationTab {

constexpr uint64_t kApplicationTabId = 0;

inline bool IsApplicationTab(uint64_t tabID) { return tabID == kApplicationTabId; }

// The 8 fixed views of the Application Tab, in sub-tab band order. The enumerator value is also
// the low byte of the view's synthetic container id below, so slot k-1 holds view kind k.
enum class AppViewKind : uint8_t {
    Launcher = 1,
    Profile,
    Settings,
    Support,
    PeerChat,
    Documentation,
    CommonGeometry,
    Stats
};
constexpr uint8_t kAppViewCount = 8;

// Real container memory ids come from GetNewTempID(), which counts up from 1 - so low constants
// would collide and the views take the top of the range instead. Never persisted: the storage
// schema knows nothing about them, and InternalSubTab::containerType stays Unknown.
constexpr uint64_t kAppViewContainerIdBase = 0xFFFFFFFFFFFFFF00ULL;

constexpr uint64_t ContainerIdForAppView(AppViewKind kind) {
    return kAppViewContainerIdBase + static_cast<uint64_t>(kind);
}

constexpr bool IsAppViewContainerId(uint64_t containerMemoryId) {
    return containerMemoryId > kAppViewContainerIdBase &&
        containerMemoryId <= kAppViewContainerIdBase + kAppViewCount;
}

// Only meaningful when IsAppViewContainerId(containerMemoryId) is true.
constexpr AppViewKind AppViewKindForContainerId(uint64_t containerMemoryId) {
    return static_cast<AppViewKind>(containerMemoryId - kAppViewContainerIdBase);
}

// Fills allTabs[0] at startup, before any render or engineering thread exists.
void InitializeApplicationTab(DATASETTAB& tab);

// Makes one view the active one. Runs on the UI thread: tab 0 has no engineering thread to
// service InternalSubTabs::kActivateUIAction (tabs.md Decision 9). Ignores unknown ids.
void ActivateApplicationTabView(uint64_t containerMemoryId);

// Draws tab 0's content area - one opaque panel from contentTopPx down, then the active view.
// Called from BuildUIOverlay on a render thread, so it only reads published/atomic state.
void BuildApplicationTabOverlay(UIDrawContext& ctx, DX12ResourcesUI& uiRes,
    uint64_t activeViewContainerId, float contentTopPx, float widthPx, float heightPx,
    float rowHeightPx, float textScale);

#ifdef _DEBUG
// Decision 3 sentinel: tab 0's queues have no consumer, so nothing may ever push to them or they
// grow without bound. Drains and reports whatever slipped past the guards.
void DebugVerifyQueuesEmpty();
#endif

} // namespace ApplicationTab
