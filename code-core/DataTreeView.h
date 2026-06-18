// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace DataTreeView {

constexpr uint32_t kToggleEverythingUIAction = 0xE0000003u;
constexpr uint32_t kToggleNodeUIAction = 0xE0000004u;
constexpr uint32_t kSetActiveBranchUIAction = 0xE0000005u;

constexpr float kPinnedIconRailWidthMM = 8.0f;
constexpr float kTreeWidthMM = 56.0f;
constexpr float kTreeTopPaddingMM = 2.0f;
constexpr float kTreeBottomPaddingMM = 2.0f;
constexpr float kRowHeightMM = 3.5f;
constexpr float kIndentMM = 4.0f;
constexpr float kToggleButtonWidthMM = 3.5f;
constexpr float kScrollbarWidthMM = 2.5f;
constexpr float kScrollbarGapMM = 0.75f;
constexpr float kMinimumScrollbarThumbMM = 6.0f;

enum class RowKind : uint8_t {
    TabName,
    ObjectNode
};

struct State {
    std::atomic<bool> isVisible{ false };
    std::atomic<bool> everythingExpanded{ true };
    std::atomic<uint64_t> firstVisibleRow{ 0 };
    std::atomic<bool> scrollbarDragging{ false };
    std::atomic<float> scrollbarDragGrabOffsetPx{ 0.0f };
};

struct StateSnapshot {
    bool isVisible = false;
    bool everythingExpanded = false;
    uint64_t firstVisibleRow = 0;
};

struct LayoutMetrics {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float contentX = 0.0f;
    float contentWidth = 0.0f;
    float rowHeight = 0.0f;
    float indent = 0.0f;
    float toggleWidth = 0.0f;
    float scrollbarX = 0.0f;
    float scrollbarY = 0.0f;
    float scrollbarWidth = 0.0f;
    float scrollbarHeight = 0.0f;
    size_t visibleRowCapacity = 0;
};

struct Node {
    uint64_t objectId = 0;
    uint64_t parentObjectId = 0;
    std::u32string label;
    bool canBecomeActiveBranch = false;
};

struct BuildRequest {
    std::wstring_view tabName;
    const std::vector<Node>* nodes = nullptr;
    const std::vector<uint64_t>* expandedNodeIds = nullptr;
    uint64_t activeBranchObjectId = 0;
    float viewportTopPx = 0.0f;
    float viewportHeightPx = 0.0f;
    float pixelsPerMMX = 1.0f;
    float pixelsPerMMY = 1.0f;
};

struct Row {
    RowKind kind = RowKind::TabName;
    std::u32string label;
    uint64_t objectId = 0;
    uint32_t depth = 0;
    bool hasToggle = false;
    bool isExpanded = false;
    bool canBecomeActiveBranch = false;
    bool isActiveBranch = false;

    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float textX = 0.0f;
    float textMaxWidth = 0.0f;
    float toggleX = 0.0f;
    float toggleY = 0.0f;
    float toggleWidth = 0.0f;
    float toggleHeight = 0.0f;
};

struct Scrollbar {
    bool isScrollable = false;
    float trackX = 0.0f;
    float trackY = 0.0f;
    float trackWidth = 0.0f;
    float trackHeight = 0.0f;
    float thumbX = 0.0f;
    float thumbY = 0.0f;
    float thumbWidth = 0.0f;
    float thumbHeight = 0.0f;
};

struct BuildResult {
    std::vector<Row> rows;
    LayoutMetrics layout;
    Scrollbar scrollbar;
    size_t totalRowCount = 0;
    size_t firstVisibleRow = 0;
    size_t maxFirstVisibleRow = 0;
};

StateSnapshot Snapshot(const State& state);
void ToggleVisibility(State& state);
void ToggleEverything(State& state);
void SetFirstVisibleRow(State& state, uint64_t rowIndex);
void ScrollRows(State& state, int64_t rowDelta);
void ResetScroll(State& state);

LayoutMetrics CalculateLayout(const BuildRequest& request);
bool ContainsPoint(const LayoutMetrics& layout, float x, float y);
BuildResult BuildRows(const BuildRequest& request, const StateSnapshot& state);
bool HitTestEverythingToggle(const std::vector<Row>& rows, float mouseX, float mouseY);
bool HitTestToggle(const std::vector<Row>& rows, float mouseX, float mouseY, uint64_t& objectId);
bool HitTestActiveBranch(const std::vector<Row>& rows, float mouseX, float mouseY, uint64_t& objectId);
bool HitTestScrollbarTrack(const Scrollbar& scrollbar, float mouseX, float mouseY);
bool HitTestScrollbarThumb(const Scrollbar& scrollbar, float mouseX, float mouseY);
uint64_t ScrollbarRowForMouseY(const BuildResult& result, float mouseY, float grabOffsetPx);

std::u32string AsciiToDisplayText(const char* text);
std::u32string WideToDisplayText(std::wstring_view text);
std::u32string UInt64ToDecimalText(uint64_t value);

} // namespace DataTreeView
