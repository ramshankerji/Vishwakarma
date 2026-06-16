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

constexpr float kPinnedIconRailWidthMM = 8.0f;
constexpr float kTreeWidthMM = 56.0f;
constexpr float kTreeTopPaddingMM = 2.0f;
constexpr float kTreeBottomPaddingMM = 2.0f;
constexpr float kRowHeightMM = 3.5f;
constexpr float kIndentMM = 4.0f;
constexpr float kToggleButtonWidthMM = 3.5f;

enum class RowKind : uint8_t {
    TabName,
    EverythingFolder,
    ObjectId
};

struct State {
    std::atomic<bool> isVisible{ false };
    std::atomic<bool> everythingExpanded{ false };
};

struct StateSnapshot {
    bool isVisible = false;
    bool everythingExpanded = false;
};

struct BuildRequest {
    std::wstring_view tabName;
    const std::vector<uint64_t>* objectIds = nullptr;
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

StateSnapshot Snapshot(const State& state);
void ToggleVisibility(State& state);
void ToggleEverything(State& state);

std::vector<Row> BuildRows(const BuildRequest& request, const StateSnapshot& state);
bool HitTestEverythingToggle(const std::vector<Row>& rows, float mouseX, float mouseY);

std::u32string WideToDisplayText(std::wstring_view text);
std::u32string UInt64ToDecimalText(uint64_t value);

} // namespace DataTreeView
