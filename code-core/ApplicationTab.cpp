// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

#include "ApplicationTab.h"
#include "विश्वकर्मा.h"
#include "UserInterface.h"
#include "colors.h"

#include <charconv>

#ifdef _DEBUG
#include <iostream>
#endif

extern राम cpu;

namespace ApplicationTab {

namespace {
// One row per AppViewKind, indexed by kind - 1. ASCII throughout: both the sub-tab band and the
// content panel draw through the English MSDF atlas, which has no Devanagari glyphs.
struct AppViewDescriptor {
    const char* bandTitle;   // Button label in the sub-tab band (short - the buttons are narrow).
    const char* panelTitle;  // Heading inside the content panel.
    const char* placeholder; // What will eventually live here. Empty for views with real content.
};

constexpr AppViewDescriptor kAppViews[kAppViewCount] = {
    { "Launcher",  "Launcher",        "Recent local files and online servers will be listed here." },
    { "Profile",   "Profile",         "Login details, badges and achievements will be shown here." },
    { "Settings",  "Settings",        "Application level settings will be edited here." },
    { "Support",   "Support",         "Direct chat with the support team will run here." },
    { "Peer Chat", "Peer Chat",       "LAN chat with the local office will run here." },
    { "Docs",      "Documentation",   "A local copy of the online documentation will be read here." },
    { "Geometry",  "Common Geometry", "The instanced master geometry will be catalogued here." },
    { "Stats",     "Stats",           "" }, // Draws live counters instead of a placeholder line.
};

const AppViewDescriptor& DescriptorFor(AppViewKind kind) {
    return kAppViews[static_cast<uint8_t>(kind) - 1];
}
}

void InitializeApplicationTab(DATASETTAB& tab) {
    tab.tabID = kApplicationTabId;
    tab.tabNo = static_cast<uint32_t>(kApplicationTabId); // Also the CPU memory group number.
    // Used by window-title contexts; the tab band draws a word-mark icon instead of this label
    // (the label path is ASCII-filtered and the MSDF atlas cannot shape Devanagari conjuncts).
    tab.fileName = L"विश्वकर्मा";
    tab.dataTreeView.isVisible.store(false, std::memory_order_release);

    // The 8 views are opened here, once, before any render thread exists - so the published list
    // is immutable for the rest of the run and there is nothing to race on. No lock needed, and
    // list A stays the published buffer forever (nothing ever re-publishes tab 0's views).
    for (uint8_t kind = 1; kind <= kAppViewCount; ++kind) {
        const uint16_t slot = static_cast<uint16_t>(kind - 1);
        InternalSubTab& view = tab.subTabs[slot];
        view.containerMemoryId = ContainerIdForAppView(static_cast<AppViewKind>(kind));
        view.title = kAppViews[kind - 1].bandTitle;
        tab.subTabStates[slot].store(SUBTAB_OPEN, std::memory_order_release);
        tab.subTabIndexesA[slot] = slot;
    }
    tab.publishedSubTabIndexes.store(tab.subTabIndexesA, std::memory_order_release);
    tab.publishedSubTabCount.store(kAppViewCount, std::memory_order_release);
    tab.activeInternalSubTabMemoryId = ContainerIdForAppView(AppViewKind::Launcher);
}

void ActivateApplicationTabView(uint64_t containerMemoryId) {
    DATASETTAB& tab = allTabs[kApplicationTabId];
    // Validate against the published list rather than the id range: the band can only ever emit
    // one of these, and an unknown id would blank the content area.
    if (FindPublishedSubTabSlot(tab, containerMemoryId) < 0) return;
    // Same lock the engineering thread's ActivateInternalSubTab takes, so the compositor's locked
    // read of activeInternalSubTabMemoryId never races this write.
    std::lock_guard<std::mutex> lock(*tab.storageObjectsMutex);
    tab.activeInternalSubTabMemoryId = containerMemoryId;
}

namespace {
// Live counters for the Stats view. Everything here is either an atomic or the RCU snapshot the
// render threads already read every frame, so it is safe to sample from this thread with no locks.
struct AppStats {
    uint32_t openTabs = 0;      // Including the Application Tab itself.
    uint32_t windows = 0;
    uint32_t cpuChunks = 0;     // Committed 4 MB arena chunks held by tabs.
    uint64_t gpuGeometryPages = 0;
};

AppStats SampleStats() {
    AppStats stats{};
    uint16_t* tabList = publishedTabIndexes.load(std::memory_order_acquire);
    const uint16_t tabCount = publishedTabCount.load(std::memory_order_acquire);
    stats.openTabs = tabCount;
    stats.windows = publishedWindowCount.load(std::memory_order_acquire);
    stats.cpuChunks = cpu.liveChunkCount.load(std::memory_order_relaxed);
    for (uint16_t i = 0; tabList && i < tabCount; ++i) {
        const GeometryPageSnapshot* snapshot =
            allTabs[tabList[i]].geometry.activeSnapshot.load(std::memory_order_acquire);
        if (snapshot) stats.gpuGeometryPages += snapshot->pages.size();
    }
    return stats;
}

// "<label>: <value>" into caller-owned storage. std::to_chars keeps this allocation-free on a
// render thread; the buffer is sized for any label here plus a 20-digit number.
void FormatStatRow(char (&buffer)[96], const char* label, uint64_t value) {
    size_t at = 0;
    for (const char* p = label; *p && at < sizeof(buffer) - 24; ++p) buffer[at++] = *p;
    buffer[at++] = ':';
    buffer[at++] = ' ';
    const std::to_chars_result result = std::to_chars(buffer + at, buffer + sizeof(buffer) - 1, value);
    *result.ptr = '\0';
}
} // namespace

void BuildApplicationTabOverlay(UIDrawContext& ctx, DX12ResourcesUI& uiRes,
    uint64_t activeViewContainerId, float contentTopPx, float widthPx, float heightPx,
    float rowHeightPx, float textScale) {
    // One opaque panel over the whole content area. This is why the compositor needs no changes:
    // whatever it drew there (empty scene + sky gradient) is completely covered.
    const float panelHeight = heightPx - contentTopPx;
    if (panelHeight <= 0.0f || widthPx <= 0.0f) return;
    PushWidgetRect(ctx, uiRes, 0.0f, contentTopPx, widthPx, panelHeight,
        uiActiveColors.actionGroupBackground);

    if (!IsAppViewContainerId(activeViewContainerId)) return; // No view active: bare panel.
    const AppViewKind kind = AppViewKindForContainerId(activeViewContainerId);
    const AppViewDescriptor& view = DescriptorFor(kind);

    const float marginX = rowHeightPx;
    const float textWidth = widthPx - 2.0f * marginX;
    if (textWidth <= 0.0f) return;

    float rowY = contentTopPx + rowHeightPx;
    const float headingScale = textScale * 1.6f;
    PushWidgetText(ctx, uiRes, marginX, WidgetTextBaselineY(rowY, rowHeightPx, headingScale),
        view.panelTitle, textWidth, uiActiveColors.actionText, headingScale);
    rowY += rowHeightPx * 1.8f;

    if (kind != AppViewKind::Stats) {
        PushWidgetText(ctx, uiRes, marginX, WidgetTextBaselineY(rowY, rowHeightPx, textScale),
            view.placeholder, textWidth, kUIDisabledTextGray, textScale);
        return;
    }

    const AppStats stats = SampleStats();
    char row[96];
    FormatStatRow(row, "Open tabs (Application Tab included)", stats.openTabs);
    PushWidgetText(ctx, uiRes, marginX, WidgetTextBaselineY(rowY, rowHeightPx, textScale),
        row, textWidth, uiActiveColors.actionText, textScale);
    rowY += rowHeightPx;

    FormatStatRow(row, "Windows", stats.windows);
    PushWidgetText(ctx, uiRes, marginX, WidgetTextBaselineY(rowY, rowHeightPx, textScale),
        row, textWidth, uiActiveColors.actionText, textScale);
    rowY += rowHeightPx;

    FormatStatRow(row, "CPU arena chunks (4 MB each)", stats.cpuChunks);
    PushWidgetText(ctx, uiRes, marginX, WidgetTextBaselineY(rowY, rowHeightPx, textScale),
        row, textWidth, uiActiveColors.actionText, textScale);
    rowY += rowHeightPx;

    FormatStatRow(row, "CPU arena committed (MB)",
        static_cast<uint64_t>(stats.cpuChunks) * (SMALL_ALLOCATOR_CHUNK_SIZE / (1024u * 1024u)));
    PushWidgetText(ctx, uiRes, marginX, WidgetTextBaselineY(rowY, rowHeightPx, textScale),
        row, textWidth, uiActiveColors.actionText, textScale);
    rowY += rowHeightPx;

    FormatStatRow(row, "GPU geometry pages", stats.gpuGeometryPages);
    PushWidgetText(ctx, uiRes, marginX, WidgetTextBaselineY(rowY, rowHeightPx, textScale),
        row, textWidth, uiActiveColors.actionText, textScale);
}

#ifdef _DEBUG
void DebugVerifyQueuesEmpty() {
    DATASETTAB& tab = allTabs[kApplicationTabId];
    ACTION_DETAILS leaked{};
    while (tab.userInputQueue->try_pop(leaked)) {
        std::cerr << "[apptab][bug] userInputQueue received action "
                  << static_cast<int>(leaked.actionType) << std::endl;
    }
    while (tab.todoCPUQueue->try_pop(leaked)) {
        std::cerr << "[apptab][bug] todoCPUQueue received action "
                  << static_cast<int>(leaked.actionType) << std::endl;
    }
}
#endif

} // namespace ApplicationTab
