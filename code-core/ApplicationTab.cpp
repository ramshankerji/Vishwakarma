// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

#include "ApplicationTab.h"
#include "विश्वकर्मा.h"
#include "UserInterface.h"
#include "colors.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <vector>

#ifdef _DEBUG
#include <iostream>
#endif

extern राम cpu;
extern शंकर gpu; // Monitor count, and the owner of the copy-thread counters below.

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
        // Application-tab views hold no geometry, but the set must still be seeded: the compositor
        // resolves what to draw from it, and an empty set would mean "render nothing" rather than
        // "render this container" (10M plan Step 6).
        view.containers.Clear();
        view.containers.Add(view.containerMemoryId);
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
/* Live counters for the Stats view (graphics.md Phase 6: "Wiring them into the ImprovementData
pipeline and the Application Tab stats pane is the remaining work" - this is the second half).

WHAT IS SAFE TO SAMPLE FROM A RENDER THREAD, and it is the whole reason this struct exists rather
than the view reaching into engine state directly:

  - gCopyStats members are atomics, written by the copy thread. Fine.
  - A PUBLISHED GeometryPageSnapshot is immutable, and the pages it names are immutable after
    publish, so every per-page field below can be read without a lock. That is the RCU contract the
    render threads already rely on to draw.
  - registry.committedCount is atomic and only ever grows.

What is deliberately NOT read: the copy thread's plain (non-atomic) per-tab bookkeeping -
instanceCount, the free/pending vectors, TiledInstanceBuffer::capacity. Those are copy-thread-owned
and a std::vector's size can be observed mid-update. The paired pending/free numbers therefore come
from gCopyStats, where the copy thread publishes them - see the caveat on that section's heading. */
struct TabStats {
    uint16_t tabIndex = 0;
    char name[48] = {};          // ASCII-filtered file name; the English MSDF atlas has no others.
    uint32_t scenePages = 0;
    uint32_t page2DPages = 0;
    uint64_t objects = 0;
    uint64_t pageBytes = 0;      // Total VRAM held by this tab's Scene3D pages.
    uint64_t liveBytes = 0;      // Vertex+index bytes still referenced by a live object.
    uint64_t holeBytes = 0;      // Bytes left behind by deleted / relocated objects.
    uint32_t registryCommitted = 0;
};

struct AppStats {
    uint32_t openTabs = 0;      // Including the Application Tab itself.
    uint32_t windows = 0;
    uint32_t monitors = 0;
    uint32_t cpuChunks = 0;     // Committed 4 MB arena chunks held by tabs.
    uint64_t gpuGeometryPages = 0;
    uint64_t gpuGeometryBytes = 0;
    std::vector<TabStats> tabs; // Engineering tabs only - tab 0 holds no geometry.
};

// Copy a tab's file name into fixed storage, filtering to printable ASCII. Read without a lock,
// the same way the data tree already reads tab.fileName one frame earlier.
void CopyTabName(char (&out)[48], const std::wstring& fileName) {
    size_t at = 0;
    for (wchar_t wide : fileName) {
        if (at >= sizeof(out) - 1) break;
        out[at++] = (wide >= 32 && wide < 127) ? static_cast<char>(wide) : '?';
    }
    out[at] = '\0';
}

AppStats SampleStats() {
    AppStats stats{};
    uint16_t* tabList = publishedTabIndexes.load(std::memory_order_acquire);
    const uint16_t tabCount = publishedTabCount.load(std::memory_order_acquire);
    stats.openTabs = tabCount;
    stats.windows = publishedWindowCount.load(std::memory_order_acquire);
    stats.monitors = static_cast<uint32_t>(gpu.currentMonitorCount);
    stats.cpuChunks = cpu.liveChunkCount.load(std::memory_order_relaxed);
    stats.tabs.reserve(tabCount);

    for (uint16_t i = 0; tabList && i < tabCount; ++i) {
        const uint16_t tabIndex = tabList[i];
        DATASETTAB& tab = allTabs[tabIndex];

        TabStats perTab{};
        perTab.tabIndex = tabIndex;
        CopyTabName(perTab.name, tab.fileName);
        perTab.registryCommitted =
            tab.dx.registry.committedCount.load(std::memory_order_acquire);

        const GeometryPageSnapshot* snapshot =
            tab.geometry.activeSnapshot.load(std::memory_order_acquire);
        if (snapshot) {
            perTab.scenePages = static_cast<uint32_t>(snapshot->pages.size());
            for (const GeometryPage* page : snapshot->pages) {
                if (!page) continue;
                perTab.objects += page->objectCount;
                perTab.pageBytes += page->pageSize;
                /* Derived, not read from GeometryPage::liveBytes - that field is declared but
                never written by anything, so it would report 0 for every page. The double-ended
                layout makes the real figure exact: vertices occupy [0, vertexHead) and indices
                [indexTail, pageSize), and holeBytes is the part of that no live object still owns. */
                const uint64_t used = static_cast<uint64_t>(page->vertexHead) +
                    (page->pageSize - page->indexTail);
                perTab.holeBytes += page->holeBytes;
                perTab.liveBytes += used - (std::min)(used, static_cast<uint64_t>(page->holeBytes));
            }
        }
        if (tab.cad2d) {
            const Cad2DPageSnapshot* pageSnapshot =
                tab.cad2d->activeSnapshot.load(std::memory_order_acquire);
            if (pageSnapshot) perTab.page2DPages = static_cast<uint32_t>(pageSnapshot->pages.size());
        }

        stats.gpuGeometryPages += perTab.scenePages;
        stats.gpuGeometryBytes += perTab.pageBytes;
        if (!IsApplicationTab(tabIndex)) stats.tabs.push_back(perTab);
    }
    return stats;
}

// Decimal into caller-owned storage. std::to_chars keeps the whole view allocation-free apart from
// the tab vector above; 48 bytes covers two 20-digit numbers and the separator.
void FormatUInt(char (&buffer)[48], uint64_t value) {
    const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof(buffer) - 1, value);
    *result.ptr = '\0';
}

// "<a> / <b>", for the paired pending/free counters.
void FormatPair(char (&buffer)[48], uint64_t a, uint64_t b) {
    char* at = std::to_chars(buffer, buffer + sizeof(buffer) - 1, a).ptr;
    *at++ = ' '; *at++ = '/'; *at++ = ' ';
    at = std::to_chars(at, buffer + sizeof(buffer) - 1, b).ptr;
    *at = '\0';
}

// "Tab <index> - <name>". The heading of one per-tab block.
void FormatTabHeading(char (&buffer)[96], const TabStats& tab) {
    size_t at = 0;
    for (const char* p = "Tab "; *p; ++p) buffer[at++] = *p;
    at = static_cast<size_t>(std::to_chars(buffer + at, buffer + sizeof(buffer) - 1,
        static_cast<uint32_t>(tab.tabIndex)).ptr - buffer);
    if (tab.name[0] != '\0') {
        buffer[at++] = ' '; buffer[at++] = '-'; buffer[at++] = ' ';
        for (const char* p = tab.name; *p && at < sizeof(buffer) - 1; ++p) buffer[at++] = *p;
    }
    buffer[at] = '\0';
}

constexpr uint64_t kBytesPerMB = 1024ull * 1024ull;
constexpr uint64_t kBytesPerKB = 1024ull;

/* Scroll state for the Stats view. A file-local singleton rather than per-window state, because
tab 0 is itself a singleton and its Stats view is only ever drawn by the one window hosting it.
Written and read by render threads, hence atomic; nothing here is control flow for the engine. */
std::atomic<float> gStatsScrollPx{ 0.0f };
std::atomic<bool> gStatsScrollbarDragging{ false };
std::atomic<float> gStatsScrollGrabOffsetPx{ 0.0f };
} // namespace

void BuildApplicationTabOverlay(UIDrawContext& ctx, DX12ResourcesUI& uiRes, const UIInput& input,
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

    /* The Stats view: every engine counter, global block first, then one block per engineering
    tab, scrolled as one list. Two passes over the same emitter - the first measures total content
    height (drawing suppressed), the second draws. Measuring first is what lets the scroll offset
    be clamped and the scrollbar thumb sized correctly on the SAME frame the content changes;
    carrying last frame's height instead would lag by a frame every time a tab opens. */
    const AppStats stats = SampleStats();

    const float clipTop = rowY;                          // First pixel a stats row may occupy.
    const float clipBottom = contentTopPx + panelHeight;
    const float viewportHeight = clipBottom - clipTop;
    if (viewportHeight <= 0.0f) return;

    const float scrollbarWidth = (std::max)(6.0f, rowHeightPx * 0.35f);
    const float scrollbarX = widthPx - scrollbarWidth;
    // Two columns: labels left, numbers at a fixed offset. A fixed column beats right-alignment
    // here because the value strings are short and this keeps the whole view measurement-free.
    const float valueX = marginX + (std::min)(textWidth * 0.62f, rowHeightPx * 18.0f);
    const float labelWidth = (std::max)(0.0f, valueX - marginX - rowHeightPx * 0.4f);
    const float valueWidth = (std::max)(0.0f, scrollbarX - valueX - rowHeightPx * 0.4f);

    const uint32_t headingColor = uiActiveColors.tabBackgroundText;
    const uint32_t labelColor = kUIDisabledTextGray;
    const uint32_t valueColor = uiActiveColors.actionText;

    float scrollPx = gStatsScrollPx.load(std::memory_order_relaxed);
    float contentY = 0.0f;
    float contentHeightPx = 0.0f;
    bool measuring = true;

    // One row of the list. Rows are drawn only when they fit ENTIRELY inside the band, so a
    // partially scrolled row can never bleed over the "Stats" heading above clipTop. At full
    // scroll the last row's bottom lands exactly on clipBottom, so nothing is unreachable.
    auto Row = [&](const char* label, const char* value) {
        const float y = clipTop + contentY - scrollPx;
        contentY += rowHeightPx;
        if (measuring || y < clipTop || y + rowHeightPx > clipBottom) return;
        const float baseline = WidgetTextBaselineY(y, rowHeightPx, textScale);
        PushWidgetText(ctx, uiRes, marginX, baseline, label, labelWidth, labelColor, textScale);
        if (value) {
            PushWidgetText(ctx, uiRes, valueX, baseline, value, valueWidth, valueColor, textScale);
        }
    };

    char value[48];
    auto RowValue = [&](const char* label, uint64_t number) {
        FormatUInt(value, number);
        Row(label, value);
    };
    auto RowPair = [&](const char* label, uint64_t a, uint64_t b) {
        FormatPair(value, a, b);
        Row(label, value);
    };
    auto Heading = [&](const char* text) {
        contentY += rowHeightPx * 0.6f; // Breathing room above a section.
        const float y = clipTop + contentY - scrollPx;
        contentY += rowHeightPx;
        if (measuring || y < clipTop || y + rowHeightPx > clipBottom) return;
        PushWidgetText(ctx, uiRes, marginX, WidgetTextBaselineY(y, rowHeightPx, textScale),
            text, textWidth, headingColor, textScale);
    };

    for (int pass = 0; pass < 2; ++pass) {
        measuring = (pass == 0);
        contentY = 0.0f;

        Heading("APPLICATION");
        RowValue("Open tabs (Application Tab included)", stats.openTabs);
        RowValue("Windows", stats.windows);
        RowValue("Monitors", stats.monitors);
        RowValue("CPU arena chunks (4 MB each)", stats.cpuChunks);
        RowValue("CPU arena committed (MB)",
            static_cast<uint64_t>(stats.cpuChunks) * (SMALL_ALLOCATOR_CHUNK_SIZE / kBytesPerMB));
        RowValue("Scene3D geometry pages (all tabs)", stats.gpuGeometryPages);
        RowValue("Scene3D page VRAM (MB)", stats.gpuGeometryBytes / kBytesPerMB);

        // Cumulative since launch. These only ever climb, so a rate has to be read by watching
        // them change - they answer "how much work has this session done", not "how busy is it now".
        Heading("COPY THREAD - CUMULATIVE");
        RowValue("Batches drained", gCopyStats.batches.load(std::memory_order_relaxed));
        RowValue("Chunks published", gCopyStats.chunks.load(std::memory_order_relaxed));
        RowValue("Commands applied", gCopyStats.commands.load(std::memory_order_relaxed));
        RowValue("Pages cloned", gCopyStats.pagesCloned.load(std::memory_order_relaxed));
        RowValue("Pages compacted", gCopyStats.pagesCompacted.load(std::memory_order_relaxed));
        RowValue("Clone traffic (MB)",
            gCopyStats.clonedBytes.load(std::memory_order_relaxed) / kBytesPerMB);
        RowValue("Staged through upload ring (MB)",
            gCopyStats.ringBytes.load(std::memory_order_relaxed) / kBytesPerMB);
        RowValue("Oversize staging fallbacks",
            gCopyStats.oversizeStaging.load(std::memory_order_relaxed));
        RowValue("Transform-only moves (zero clones)",
            gCopyStats.transformOnlyEdits.load(std::memory_order_relaxed));
        RowValue("Visibility mask writes", gCopyStats.maskWrites.load(std::memory_order_relaxed));

        // Gauges: what is true right now. Sustained growth in the retire backlog is the
        // frozen-monitor failure mode that ends in VRAM exhaustion.
        Heading("COPY THREAD - LIVE");
        RowValue("Commands still queued", gCopyStats.queueDeferred.load(std::memory_order_relaxed));
        RowValue("Objects hidden in some SubTab",
            gCopyStats.hiddenInstances.load(std::memory_order_relaxed));
        RowValue("Retire backlog (pages + snapshots)",
            gCopyStats.liveRetireBacklog.load(std::memory_order_relaxed));

        // High-water marks. A peak reads like a stuck value when it is only recording a stall that
        // has since cleared - compare against the live gauges above before concluding anything.
        Heading("COPY THREAD - PEAK");
        RowValue("Upload ring high water (KB)",
            gCopyStats.ringHighWater.load(std::memory_order_relaxed) / kBytesPerKB);
        RowValue("Retire backlog peak",
            gCopyStats.peakRetireBacklog.load(std::memory_order_relaxed));
        RowValue("Max active pages in one tab",
            gCopyStats.maxActivePages.load(std::memory_order_relaxed));

        /* These four are per-tab quantities that the copy thread publishes into GLOBAL atomics
        from inside its per-tab sweep loop, so what survives is whichever tab it swept last. Shown
        here rather than in the per-tab blocks below because that is what the numbers actually are;
        making them genuinely per-tab means giving each tab its own counters. */
        Heading("COPY THREAD - LAST SWEPT TAB");
        RowPair("Instance indexes (pending / free)",
            gCopyStats.pendingIndexes.load(std::memory_order_relaxed),
            gCopyStats.freeIndexes.load(std::memory_order_relaxed));
        RowPair("Arena slots (pending / free)",
            gCopyStats.pendingSlots.load(std::memory_order_relaxed),
            gCopyStats.freeSlots.load(std::memory_order_relaxed));

        // One block per engineering tab. Tab 0 is skipped by SampleStats - it holds no geometry.
        char tabHeading[96];
        for (const TabStats& perTab : stats.tabs) {
            FormatTabHeading(tabHeading, perTab);
            Heading(tabHeading);
            RowValue("Scene3D pages", perTab.scenePages);
            RowValue("Page2D pages", perTab.page2DPages);
            RowValue("Objects resident", perTab.objects);
            RowValue("Page VRAM (KB)", perTab.pageBytes / kBytesPerKB);
            RowValue("Live geometry (KB)", perTab.liveBytes / kBytesPerKB);
            RowValue("Holes (KB)", perTab.holeBytes / kBytesPerKB);
            // Against USED bytes, not page capacity: that is the ratio page compaction triggers on.
            const uint64_t used = perTab.liveBytes + perTab.holeBytes;
            RowValue("Fragmentation (%)", used == 0 ? 0 : perTab.holeBytes * 100 / used);
            RowValue("Instance registry committed", perTab.registryCommitted);
        }
        if (stats.tabs.empty()) {
            Heading("ENGINEERING TABS");
            Row("None open.", nullptr);
        }

        if (!measuring) break;

        // Between the passes: clamp the offset against the height just measured, then apply this
        // frame's input to it. Wheel anywhere over the panel; drag on the scrollbar.
        contentHeightPx = contentY;
        const float maxScroll = (std::max)(0.0f, contentHeightPx - viewportHeight);
        const float trackHeight = viewportHeight;
        const float thumbHeight = contentHeightPx > 0.0f
            ? std::clamp(trackHeight * (viewportHeight / contentHeightPx),
                (std::min)(rowHeightPx * 2.0f, trackHeight), trackHeight)
            : trackHeight;
        const float thumbTravel = trackHeight - thumbHeight;
        const float thumbY = maxScroll > 0.0f
            ? clipTop + thumbTravel * std::clamp(scrollPx / maxScroll, 0.0f, 1.0f)
            : clipTop;

        bool dragging = gStatsScrollbarDragging.load(std::memory_order_acquire);
        if (!input.leftButtonDown || input.leftButtonReleasedThisFrame || maxScroll <= 0.0f) {
            dragging = false;
            gStatsScrollbarDragging.store(false, std::memory_order_release);
        }

        const bool overPanel = input.mouseX >= 0.0f && input.mouseX < widthPx &&
            input.mouseY >= clipTop && input.mouseY < clipBottom;
        const bool overTrack = overPanel && input.mouseX >= scrollbarX;
        const bool overThumb = overTrack &&
            input.mouseY >= thumbY && input.mouseY < thumbY + thumbHeight;

        if (overPanel && input.mouseWheelDelta != 0) {
            // Three rows per notch, matching the data tree's feel. Over the track too - a wheel
            // that stops working when the pointer strays onto the scrollbar reads as a bug.
            scrollPx -= (input.mouseWheelDelta / static_cast<float>(WHEEL_DELTA)) * rowHeightPx * 3.0f;
        }
        if (input.leftButtonPressedThisFrame && overTrack && maxScroll > 0.0f) {
            // Grabbing the thumb keeps the pointer where it landed; clicking the bare track
            // centres the thumb under the pointer and jumps there.
            gStatsScrollGrabOffsetPx.store(
                overThumb ? input.mouseY - thumbY : thumbHeight * 0.5f, std::memory_order_release);
            gStatsScrollbarDragging.store(true, std::memory_order_release);
            dragging = true;
        }
        if (dragging && thumbTravel > 0.0f) {
            const float grab = gStatsScrollGrabOffsetPx.load(std::memory_order_acquire);
            const float travelled =
                std::clamp(input.mouseY - grab - clipTop, 0.0f, thumbTravel);
            scrollPx = maxScroll * (travelled / thumbTravel);
        }

        scrollPx = std::clamp(scrollPx, 0.0f, maxScroll);
        gStatsScrollPx.store(scrollPx, std::memory_order_relaxed);

        // Scrollbar, drawn before the rows so a row can never be painted over by the track.
        if (maxScroll > 0.0f) {
            const float drawnThumbY = clipTop + thumbTravel * (scrollPx / maxScroll);
            PushWidgetRect(ctx, uiRes, scrollbarX, clipTop, scrollbarWidth, trackHeight, 0x66333333);
            PushWidgetRect(ctx, uiRes, scrollbarX, drawnThumbY, scrollbarWidth, thumbHeight,
                dragging ? 0xFF3399FF : (overThumb ? 0xFF5CB4FF : 0xCC8A8A8A));
        }
    }
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
