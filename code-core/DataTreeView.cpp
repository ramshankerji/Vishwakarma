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

std::vector<Row> BuildRows(const BuildRequest& request, const StateSnapshot& state) {
    std::vector<Row> rows;
    if (!state.isVisible || request.viewportHeightPx <= 0.0f ||
        request.pixelsPerMMX <= 0.0f || request.pixelsPerMMY <= 0.0f) {
        return rows;
    }

    const float treeX = kPinnedIconRailWidthMM * request.pixelsPerMMX;
    const float treeWidth = kTreeWidthMM * request.pixelsPerMMX;
    const float rowHeight = std::max(1.0f, std::round(kRowHeightMM * request.pixelsPerMMY));
    const float indent = kIndentMM * request.pixelsPerMMX;
    const float toggleWidth = std::max(8.0f, std::round(kToggleButtonWidthMM * request.pixelsPerMMX));
    const float treeY = request.viewportTopPx + kTreeTopPaddingMM * request.pixelsPerMMY;
    const float availableHeight = std::max(0.0f,
        request.viewportHeightPx - (kTreeTopPaddingMM + kTreeBottomPaddingMM) * request.pixelsPerMMY);
    const size_t maxRows = static_cast<size_t>(std::floor(availableHeight / rowHeight));
    if (maxRows == 0) return rows;

    const size_t nodeCount = request.nodes ? request.nodes->size() : 0;
    const size_t reserveRows = (std::min)(maxRows, static_cast<size_t>(1 + nodeCount));
    rows.reserve(reserveRows);

    auto pushRow = [&](RowKind kind, std::u32string label, uint32_t depth,
        bool hasToggle, bool isExpanded, uint64_t objectId, bool canBecomeActiveBranch) {
        if (rows.size() >= maxRows) return false;

        Row row;
        row.kind = kind;
        row.label = std::move(label);
        row.depth = depth;
        row.hasToggle = hasToggle;
        row.isExpanded = isExpanded;
        row.objectId = objectId;
        row.canBecomeActiveBranch = canBecomeActiveBranch;
        row.isActiveBranch = canBecomeActiveBranch && objectId == request.activeBranchObjectId;
        row.x = treeX + indent * static_cast<float>(depth);
        row.y = treeY + rowHeight * static_cast<float>(rows.size());
        row.width = std::max(0.0f, treeWidth - indent * static_cast<float>(depth));
        row.height = rowHeight;
        row.toggleX = row.x;
        row.toggleY = row.y;
        row.toggleWidth = hasToggle ? toggleWidth : 0.0f;
        row.toggleHeight = rowHeight;
        row.textX = row.x + (hasToggle ? toggleWidth : 0.0f);
        row.textMaxWidth = std::max(0.0f, treeX + treeWidth - row.textX);
        rows.push_back(std::move(row));
        return true;
    };

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

    const bool rootHasChildren = childrenByParent.find(0) != childrenByParent.end();
    pushRow(RowKind::TabName, WideToDisplayText(request.tabName), 0,
        rootHasChildren, state.everythingExpanded, 0, false);

    if (!state.everythingExpanded || !request.nodes) return rows;

    std::unordered_set<uint64_t> visited;
    visited.reserve(request.nodes->size());

    std::function<bool(uint64_t, uint32_t)> pushChildren = [&](uint64_t parentId, uint32_t depth) {
        auto childrenIt = childrenByParent.find(parentId);
        if (childrenIt == childrenByParent.end()) return true;

        for (size_t childIndex : childrenIt->second) {
            const Node& node = (*request.nodes)[childIndex];
            if (node.objectId == 0 || !visited.insert(node.objectId).second) continue;

            auto grandchildrenIt = childrenByParent.find(node.objectId);
            const bool hasChildren = grandchildrenIt != childrenByParent.end() && !grandchildrenIt->second.empty();
            const bool isExpanded = expandedNodeIds.find(node.objectId) != expandedNodeIds.end();
            if (!pushRow(RowKind::ObjectNode, node.label, depth, hasChildren, isExpanded, node.objectId,
                node.canBecomeActiveBranch)) {
                return false;
            }

            if (hasChildren && isExpanded) {
                if (!pushChildren(node.objectId, depth + 1)) return false;
            }
        }
        return true;
    };

    pushChildren(0, 1);
    return rows;
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

} // namespace DataTreeView
