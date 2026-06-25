// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
#pragma once

#include <cstdint>
#include <vector>

namespace SVGIconRenderer {

struct RenderedSVGIcon {
    uint32_t id = 0;
    const char* fileName = "";
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
};

constexpr char32_t NoIcon = U'x';

constexpr char32_t IconForID(uint32_t iconID) noexcept {
    return static_cast<char32_t>(iconID);
}

bool HasEmbeddedSVGIcon(uint32_t iconID) noexcept;
std::vector<RenderedSVGIcon> RenderEmbeddedSVGIcons(int pixelSize);

} // namespace SVGIconRenderer
