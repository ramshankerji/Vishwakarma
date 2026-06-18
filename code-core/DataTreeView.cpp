// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

#include "DataTreeView.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <functional>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace DataTreeView {
namespace {

std::u32string Literal(const char* text) {
    std::u32string result;
    if (!text) return result;

    for (const char* p = text; *p; ++p) {
        result.push_back(static_cast<char32_t>(*p));
    }
    return result;
}

} // namespace

StateSnapshot Snapshot(const State& state) {
    StateSnapshot snapshot;
    snapshot.isVisible = state.isVisible.load(std::memory_order_acquire);
    snapshot.everythingExpanded = state.everythingExpanded.load(std::memory_order_acquire);
    snapshot.firstVisibleRow = state.firstVisibleRow.load(std::memory_order_acquire);
    return snapshot;
}

void ToggleVisibility(State& state) {
    const bool isVisible = state.isVisible.load(std::memory_order_acquire);
    state.isVisible.store(!isVisible, std::memory_order_release);
}

void ToggleEverything(State& state) {
    const bool isExpanded = state.everythingExpanded.load(std::memory_order_acquire);
    state.everythingExpanded.store(!isExpanded, std::memory_order_release);
}

void SetFirstVisibleRow(State& state, uint64_t rowIndex) {
    state.firstVisibleRow.store(rowIndex, std::memory_order_release);
}

void ScrollRows(State& state, int64_t rowDelta) {
    uint64_t current = state.firstVisibleRow.load(std::memory_order_acquire);
    if (rowDelta < 0) {
        const uint64_t amount = static_cast<uint64_t>(-(rowDelta + 1)) + 1;
        current = amount >= current ? 0 : current - amount;
    } else {
        const uint64_t amount = static_cast<uint64_t>(rowDelta);
        current = UINT64_MAX - current < amount ? UINT64_MAX : current + amount;
    }
    state.firstVisibleRow.store(current, std::memory_order_release);
}

void ResetScroll(State& state) {
    state.firstVisibleRow.store(0, std::memory_order_release);
    state.scrollbarDragging.store(false, std::memory_order_release);
    state.scrollbarDragGrabOffsetPx.store(0.0f, std::memory_order_release);
}

std::u32string AsciiToDisplayText(const char* text) {
    std::u32string result;
    if (!text) return result;

    for (const char* p = text; *p; ++p) {
        const unsigned char ch = static_cast<unsigned char>(*p);
        if (ch >= 0x20 && ch <= 0x7E) {
            result.push_back(static_cast<char32_t>(ch));
        } else {
            result.push_back(U'?');
        }
    }

    if (result.empty()) result = Literal("Untitled");
    return result;
}

std::u32string WideToDisplayText(std::wstring_view text) {
    std::u32string result;
    result.reserve(text.size());

    for (wchar_t ch : text) {
        if (ch >= 0x20 && ch <= 0x7E) {
            result.push_back(static_cast<char32_t>(ch));
        } else if (ch != 0) {
            result.push_back(U'?');
        }
    }

    if (result.empty()) result = Literal("Untitled");
    return result;
}

std::u32string UInt64ToDecimalText(uint64_t value) {
    std::array<char, 32> buffer{};
    auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, 10);
    if (ec != std::errc()) return {};

    std::u32string result;
    result.reserve(static_cast<size_t>(ptr - buffer.data()));
    for (const char* p = buffer.data(); p < ptr; ++p) {
        result.push_back(static_cast<char32_t>(*p));
    }
    return result;
}

LayoutMetrics CalculateLayout(const BuildRequest& request) {
    LayoutMetrics layout;
    if (request.viewportHeightPx <= 0.0f ||
        request.pixelsPerMMX <= 0.0f || request.pixelsPerMMY <= 0.0f) {
        return layout;
    }

    layout.x = kPinnedIconRailWidthMM * request.pixelsPerMMX;
    layout.width = kTreeWidthMM * request.pixelsPerMMX;
    layout.y = request.viewportTopPx + kTreeTopPaddingMM * request.pixelsPerMMY;
    layout.height = std::max(0.0f,
        request.viewportHeightPx - (kTreeTopPaddingMM + kTreeBottomPaddingMM) * request.pixelsPerMMY);
    layout.rowHeight = std::max(1.0f, std::round(kRowHeightMM * request.pixelsPerMMY));
    layout.indent = kIndentMM * request.pixelsPerMMX;
    layout.toggleWidth = std::max(8.0f, std::round(kToggleButtonWidthMM * request.pixelsPerMMX));
    layout.scrollbarX = layout.x;
    layout.scrollbarY = layout.y;
    layout.scrollbarWidth = std::max(6.0f, std::round(kScrollbarWidthMM * request.pixelsPerMMX));
    layout.scrollbarHeight = layout.height;
    const float scrollbarGap = kScrollbarGapMM * request.pixelsPerMMX;
    layout.contentX = layout.x + layout.scrollbarWidth + scrollbarGap;
    layout.contentWidth = std::max(0.0f, layout.width - layout.scrollbarWidth - scrollbarGap);
    layout.visibleRowCapacity = static_cast<size_t>(std::floor(layout.height / layout.rowHeight));
    return layout;
}

bool ContainsPoint(const LayoutMetrics& layout, float x, float y) {
    return layout.width > 0.0f && layout.height > 0.0f &&
        x >= layout.x && x < layout.x + layout.width &&
        y >= layout.y && y < layout.y + layout.height;
}

BuildResult BuildRows(const BuildRequest& request, const StateSnapshot& state) {
    BuildResult result;
    result.layout = CalculateLayout(request);
    if (!state.isVisible || request.viewportHeightPx <= 0.0f ||
        request.pixelsPerMMX <= 0.0f || request.pixelsPerMMY <= 0.0f) {
        return result;
    }

    const LayoutMetrics& layout = result.layout;
    const size_t maxRows = layout.visibleRowCapacity;
    if (maxRows == 0) return result;

    const size_t nodeCount = request.nodes ? request.nodes->size() : 0;
    const size_t reserveRows = (std::min)(maxRows, static_cast<size_t>(1 + nodeCount));
    result.rows.reserve(reserveRows);

    std::unordered_set<uint64_t> expandedNodeIds;
    if (request.expandedNodeIds) {
        expandedNodeIds.reserve(request.expandedNodeIds->size());
        for (uint64_t objectId : *request.expandedNodeIds) {
            expandedNodeIds.insert(objectId);
        }
    }

    std::unordered_map<uint64_t, std::vector<size_t>> childrenByParent;
    if (request.nodes) {
        childrenByParent.reserve(request.nodes->size() + 1);
        for (size_t i = 0; i < request.nodes->size(); ++i) {
            childrenByParent[(*request.nodes)[i].parentObjectId].push_back(i);
        }
    }

    result.totalRowCount = 1;
    if (state.everythingExpanded && request.nodes) {
        std::unordered_set<uint64_t> countVisited;
        countVisited.reserve(request.nodes->size());
        std::function<void(uint64_t)> countChildren = [&](uint64_t parentId) {
            auto childrenIt = childrenByParent.find(parentId);
            if (childrenIt == childrenByParent.end()) return;

            for (size_t childIndex : childrenIt->second) {
                const Node& node = (*request.nodes)[childIndex];
                if (node.objectId == 0 || !countVisited.insert(node.objectId).second) continue;
                ++result.totalRowCount;
                const bool isExpanded = expandedNodeIds.find(node.objectId) != expandedNodeIds.end();
                if (isExpanded) countChildren(node.objectId);
            }
        };
        countChildren(0);
    }

    result.maxFirstVisibleRow = result.totalRowCount > maxRows
        ? result.totalRowCount - maxRows
        : 0;
    result.firstVisibleRow = (std::min)(
        static_cast<size_t>((std::min<uint64_t>)(state.firstVisibleRow, SIZE_MAX)),
        result.maxFirstVisibleRow);

    size_t logicalRowIndex = 0;
    auto pushRow = [&](RowKind kind, const std::u32string& label, uint32_t depth,
        bool hasToggle, bool isExpanded, uint64_t objectId, bool canBecomeActiveBranch) {
        const size_t currentRowIndex = logicalRowIndex++;
        if (currentRowIndex < result.firstVisibleRow) return true;
        if (result.rows.size() >= maxRows) return false;

        Row row;
        row.kind = kind;
        row.label = label;
        row.depth = depth;
        row.hasToggle = hasToggle;
        row.isExpanded = isExpanded;
        row.objectId = objectId;
        row.canBecomeActiveBranch = canBecomeActiveBranch;
        row.isActiveBranch = canBecomeActiveBranch && objectId == request.activeBranchObjectId;
        row.x = layout.contentX + layout.indent * static_cast<float>(depth);
        row.y = layout.y + layout.rowHeight * static_cast<float>(result.rows.size());
        row.width = std::max(0.0f, layout.contentWidth - layout.indent * static_cast<float>(depth));
        row.height = layout.rowHeight;
        row.toggleX = row.x;
        row.toggleY = row.y;
        row.toggleWidth = hasToggle ? layout.toggleWidth : 0.0f;
        row.toggleHeight = layout.rowHeight;
        row.textX = row.x + (hasToggle ? layout.toggleWidth : 0.0f);
        row.textMaxWidth = std::max(0.0f, layout.contentX + layout.contentWidth - row.textX);
        result.rows.push_back(std::move(row));
        return result.rows.size() < maxRows;
    };

    const bool rootHasChildren = childrenByParent.find(0) != childrenByParent.end();
    const std::u32string tabLabel = WideToDisplayText(request.tabName);
    bool keepBuilding = pushRow(RowKind::TabName, tabLabel, 0,
        rootHasChildren, state.everythingExpanded, 0, false);

    if (keepBuilding && state.everythingExpanded && request.nodes) {
        std::unordered_set<uint64_t> visited;
        visited.reserve(request.nodes->size());

        std::function<bool(uint64_t, uint32_t)> pushChildren = [&](uint64_t parentId, uint32_t depth) {
            auto childrenIt = childrenByParent.find(parentId);
            if (childrenIt == childrenByParent.end()) return true;

            for (size_t childIndex : childrenIt->second) {
                const Node& node = (*request.nodes)[childIndex];
                if (node.objectId == 0 || !visited.insert(node.objectId).second) continue;

                auto grandchildrenIt = childrenByParent.find(node.objectId);
                const bool hasChildren =
                    grandchildrenIt != childrenByParent.end() && !grandchildrenIt->second.empty();
                const bool isExpanded = expandedNodeIds.find(node.objectId) != expandedNodeIds.end();
                if (!pushRow(RowKind::ObjectNode, node.label, depth, hasChildren, isExpanded,
                    node.objectId, node.canBecomeActiveBranch)) {
                    return false;
                }

                if (hasChildren && isExpanded) {
                    if (!pushChildren(node.objectId, depth + 1)) return false;
                }
            }
            return true;
        };

        pushChildren(0, 1);
    }

    result.scrollbar.trackX = layout.scrollbarX;
    result.scrollbar.trackY = layout.scrollbarY;
    result.scrollbar.trackWidth = layout.scrollbarWidth;
    result.scrollbar.trackHeight = layout.scrollbarHeight;
    result.scrollbar.thumbX = layout.scrollbarX;
    result.scrollbar.thumbWidth = layout.scrollbarWidth;
    result.scrollbar.isScrollable = result.maxFirstVisibleRow > 0;

    if (layout.scrollbarHeight > 0.0f) {
        const float visibleFraction = result.totalRowCount == 0
            ? 1.0f
            : std::min(1.0f, static_cast<float>(maxRows) / static_cast<float>(result.totalRowCount));
        const float minimumThumbHeight = std::max(8.0f,
            std::round(kMinimumScrollbarThumbMM * request.pixelsPerMMY));
        result.scrollbar.thumbHeight = result.scrollbar.isScrollable
            ? std::clamp(layout.scrollbarHeight * visibleFraction,
                std::min(minimumThumbHeight, layout.scrollbarHeight), layout.scrollbarHeight)
            : layout.scrollbarHeight;
        const float thumbTravel = std::max(0.0f,
            layout.scrollbarHeight - result.scrollbar.thumbHeight);
        const float scrollFraction = result.maxFirstVisibleRow == 0
            ? 0.0f
            : static_cast<float>(result.firstVisibleRow) /
                static_cast<float>(result.maxFirstVisibleRow);
        result.scrollbar.thumbY = layout.scrollbarY + thumbTravel * scrollFraction;
    }

    return result;
}

bool HitTestEverythingToggle(const std::vector<Row>& rows, float mouseX, float mouseY) {
    for (const Row& row : rows) {
        if (row.kind != RowKind::TabName || !row.hasToggle) continue;
        if (mouseX >= row.toggleX && mouseX < row.toggleX + row.toggleWidth &&
            mouseY >= row.toggleY && mouseY < row.toggleY + row.toggleHeight) {
            return true;
        }
    }
    return false;
}

bool HitTestToggle(const std::vector<Row>& rows, float mouseX, float mouseY, uint64_t& objectId) {
    for (const Row& row : rows) {
        if (!row.hasToggle) continue;
        if (mouseX >= row.toggleX && mouseX < row.toggleX + row.toggleWidth &&
            mouseY >= row.toggleY && mouseY < row.toggleY + row.toggleHeight) {
            objectId = row.objectId;
            return true;
        }
    }
    objectId = 0;
    return false;
}

bool HitTestActiveBranch(const std::vector<Row>& rows, float mouseX, float mouseY, uint64_t& objectId) {
    for (const Row& row : rows) {
        if (!row.canBecomeActiveBranch) continue;
        if (mouseX >= row.textX && mouseX < row.x + row.width &&
            mouseY >= row.y && mouseY < row.y + row.height) {
            objectId = row.objectId;
            return true;
        }
    }
    objectId = 0;
    return false;
}

bool HitTestScrollbarTrack(const Scrollbar& scrollbar, float mouseX, float mouseY) {
    return mouseX >= scrollbar.trackX && mouseX < scrollbar.trackX + scrollbar.trackWidth &&
        mouseY >= scrollbar.trackY && mouseY < scrollbar.trackY + scrollbar.trackHeight;
}

bool HitTestScrollbarThumb(const Scrollbar& scrollbar, float mouseX, float mouseY) {
    return mouseX >= scrollbar.thumbX && mouseX < scrollbar.thumbX + scrollbar.thumbWidth &&
        mouseY >= scrollbar.thumbY && mouseY < scrollbar.thumbY + scrollbar.thumbHeight;
}

uint64_t ScrollbarRowForMouseY(const BuildResult& result, float mouseY, float grabOffsetPx) {
    if (!result.scrollbar.isScrollable || result.maxFirstVisibleRow == 0) return 0;

    const float thumbTravel = result.scrollbar.trackHeight - result.scrollbar.thumbHeight;
    if (thumbTravel <= 0.0f) return 0;

    const float thumbY = std::clamp(
        mouseY - grabOffsetPx,
        result.scrollbar.trackY,
        result.scrollbar.trackY + thumbTravel);
    const float scrollFraction = (thumbY - result.scrollbar.trackY) / thumbTravel;
    return static_cast<uint64_t>(std::llround(
        scrollFraction * static_cast<float>(result.maxFirstVisibleRow)));
}

} // namespace DataTreeView
