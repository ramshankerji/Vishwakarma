// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

// Platform-agnostic half of the UI overlay module: atlas bitmap building, ribbon layout
// precompute, quad/text tessellation (UIDrawContext) and the widget/hit-test logic that fills
// the per-frame UI geometry and emits UIActions. No graphics-API calls in this file - the
// platform half (UserInterface-<Platform>.cpp) uploads the atlases, owns the PSOs / buffers
// and issues the actual draw calls.

// windows.h arrives via विश्वकर्मा.h; keep its min/max macros off std::min/std::max (same as
// the NOMINMAX in UserInterface-DirectX12.h, which shielded this code before the split).
#define NOMINMAX

#include "UserInterface.h"
#include <algorithm>
#include "FontManager.h"
#include "..\build\NotoSansMSDF_Compiled.h"
#include "विश्वकर्मा.h"
#include "DataTreeView.h"
#include "UserInterfaceTranslationCompiled.h"
#include "SVGIconRenderer.h"
#include "ImprovementData.h"
#include "PropertyPane.h"
#include "SoftwareUpdate.h"
#include "CrockfordBase32.h"
#include "colors.h"
#include "fast_float/fast_float.h"
#include <array>
#include <bit>
#include <cfloat>
#include <charconv>
#include <cmath>
#include <mutex>
#include <utility>
std::atomic<uint32_t> actionWriteIndex;
// ASCII Character set.
std::string charset = " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~";
constexpr uint32_t UI_FONT_BOLD_FLAG = 0x80000000u;

struct UIAtlasRegion {
    float uvMinX = 0.0f;
    float uvMinY = 0.0f;
    float uvMaxX = 0.0f;
    float uvMaxY = 0.0f;
};

struct UIRoundedRectangleNineSlice {
    std::array<std::array<UIAtlasRegion, 3>, 3> regions{};
};

struct UIIconAtlasMetadata {
    UIRoundedRectangleNineSlice roundedRectangle{};
    std::array<char32_t, 4> dummyIconCodepoints{ U'\uE100', U'\uE101', U'\uE102', U'\uE103' };
    std::vector<char32_t> mixedIconCodepoints{};
};

// Per-monitor icon CPU bundle. UVs depend on the monitor's icon cell size (DPI-dependent), so this is
// stored one-per-monitor instead of as a single global. BuildIconAtlas fills one; BuildUIOverlay points
// the frame's UIDrawContext at the right one via ctx.iconData.
struct IconAtlasCPU {
    std::unordered_map<char32_t, Glyph> iconGlyphLookup; // private-use codepoints -> atlas UVs
    UIIconAtlasMetadata metadata;
};

static IconAtlasCPU gMonitorIconAtlas[MV_MAX_MONITORS];

IconAtlasCPU& MonitorIconAtlasCPU(int monitorId) { return gMonitorIconAtlas[monitorId]; }

// Reference cell size the procedural dummy icons were authored against; real cells scale from it.
constexpr int kIconReferenceCellSize = 24;
constexpr int kIconCellGap = 4;
constexpr int kIconStartY = 48;
constexpr int kIconAtlasMinDimension = 256;
constexpr int kIconAtlasMaxDimension = 4096;

static const char* LogicalObjectName(VishwakarmaStorage::ObjectType objectType, const META_DATA* object) {
    if (!object) return "";

    switch (objectType) {
    case VishwakarmaStorage::ObjectType::Folder:
        return static_cast<const FOLDER*>(object)->name;
    case VishwakarmaStorage::ObjectType::Page2D:
        return static_cast<const PAGE2D*>(object)->name;
    case VishwakarmaStorage::ObjectType::Scene3D:
        return static_cast<const SCENE3D*>(object)->name;
    default:
        return "";
    }
}

static std::u32string BuildTreeNodeLabel(VishwakarmaStorage::ObjectType objectType,
    const META_DATA* object, uint64_t memoryId) {
    if (VishwakarmaStorage::IsLogicalObjectType(objectType)) {
        const char* name = LogicalObjectName(objectType, object);
        if (name && name[0] != '\0') return DataTreeView::AsciiToDisplayText(name);
        return DataTreeView::AsciiToDisplayText(VishwakarmaStorage::ObjectTypeDisplayName(objectType));
    }

    std::u32string label = DataTreeView::AsciiToDisplayText(
        VishwakarmaStorage::ObjectTypeDisplayName(objectType));
    label.push_back(U' ');
    std::u32string idText = DataTreeView::UInt64ToDecimalText(memoryId);
    label.append(idText);
    return label;
}

static VishwakarmaStorage::ObjectType ActiveInternalSubTabType(
    const DATASETTAB& tab, uint64_t activeInternalSubTabMemoryId) {
    const int slot = FindPublishedSubTabSlot(tab, activeInternalSubTabMemoryId);
    if (slot < 0) return VishwakarmaStorage::ObjectType::Unknown;
    return tab.subTabs[slot].containerType;
}

static UIAtlasRegion MakeAtlasRegion(int x, int y, int w, int h, int atlasW, int atlasH) {
    UIAtlasRegion region{};
    region.uvMinX = (float)x / (float)atlasW;
    region.uvMinY = (float)y / (float)atlasH;
    region.uvMaxX = (float)(x + w) / (float)atlasW;
    region.uvMaxY = (float)(y + h) / (float)atlasH;
    return region;
}

UIColors uiLightDefaultColors, uiActiveColors; // Initialized to default light theme colors.

static void SetAtlasPixelRGBA(AtlasBitmap& atlas, int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (x < 0 || y < 0 || x >= atlas.width || y >= atlas.height) return;

    if (atlas.bytesPerPixel == 4) {
        const size_t offset = (static_cast<size_t>(y) * atlas.width + x) * 4u;
        atlas.pixels[offset + 0] = r;
        atlas.pixels[offset + 1] = g;
        atlas.pixels[offset + 2] = b;
        atlas.pixels[offset + 3] = a;
    }
    else {
        atlas.pixels[static_cast<size_t>(y) * atlas.width + x] = a;
    }
}

static void SetAtlasCoverage(AtlasBitmap& atlas, int x, int y, uint8_t coverage) {
    SetAtlasPixelRGBA(atlas, x, y, 255, 255, 255, coverage);
}

static void GenerateRoundedRectangleNineSlice(AtlasBitmap& atlas, int originX, int originY,
    int sourceSizePx, int sourceRadiusPx, UIRoundedRectangleNineSlice& outSlice) {
    const int atlasW = atlas.width;
    const int atlasH = atlas.height;

    for (int y = 0; y < sourceSizePx; ++y) {
        for (int x = 0; x < sourceSizePx; ++x) {
            const float px = (float)x + 0.5f;
            const float py = (float)y + 0.5f;
            const float nearestX = std::clamp(px, (float)sourceRadiusPx,
                (float)(sourceSizePx - sourceRadiusPx));
            const float nearestY = std::clamp(py, (float)sourceRadiusPx,
                (float)(sourceSizePx - sourceRadiusPx));
            const float dx = px - nearestX;
            const float dy = py - nearestY;
            const float distance = std::sqrt(dx * dx + dy * dy);
            const float coverage = std::clamp((float)sourceRadiusPx + 0.5f - distance, 0.0f, 1.0f);

            SetAtlasCoverage(atlas, originX + x, originY + y, (uint8_t)std::round(coverage * 255.0f));
        }
    }

    const int middle = sourceSizePx - 2 * sourceRadiusPx;
    const int widths[3] = { sourceRadiusPx, middle, sourceRadiusPx };
    const int heights[3] = { sourceRadiusPx, middle, sourceRadiusPx };
    int yCursor = originY;
    for (int row = 0; row < 3; ++row) {
        int xCursor = originX;
        for (int col = 0; col < 3; ++col) {
            outSlice.regions[row][col] =
                MakeAtlasRegion(xCursor, yCursor, widths[col], heights[row], atlasW, atlasH);
            xCursor += widths[col];
        }
        yCursor += heights[row];
    }
}

static void FillRect(AtlasBitmap& atlas, int x, int y, int w, int h, uint8_t coverage) {
    for (int yy = y; yy < y + h; ++yy) {
        for (int xx = x; xx < x + w; ++xx) {
            SetAtlasCoverage(atlas, xx, yy, coverage);
        }
    }
}

static void CopyRenderedSVGIconToAtlas(AtlasBitmap& atlas, const SVGIconRenderer::RenderedSVGIcon& icon,
    int originX, int originY, int cellSize) {
    if (icon.rgba.empty() || icon.width <= 0 || icon.height <= 0) return;

    const int copyW = std::min(icon.width, cellSize);
    const int copyH = std::min(icon.height, cellSize);
    const int dstX = originX + std::max(0, (cellSize - copyW) / 2);
    const int dstY = originY + std::max(0, (cellSize - copyH) / 2);

    for (int y = 0; y < copyH; ++y) {
        for (int x = 0; x < copyW; ++x) {
            const size_t srcOffset = (static_cast<size_t>(y) * icon.width + x) * 4u;
            SetAtlasPixelRGBA(atlas, dstX + x, dstY + y,
                icon.rgba[srcOffset + 0],
                icon.rgba[srcOffset + 1],
                icon.rgba[srcOffset + 2],
                icon.rgba[srcOffset + 3]);
        }
    }
}

static bool TryReserveIconCell(int iconIndex, int atlasW, int atlasH, int cellSize, int& outX, int& outY) {
    const int cellsPerRow = (atlasW + kIconCellGap) / (cellSize + kIconCellGap);
    if (cellsPerRow <= 0) return false;

    outX = (iconIndex % cellsPerRow) * (cellSize + kIconCellGap);
    outY = kIconStartY + (iconIndex / cellsPerRow) * (cellSize + kIconCellGap);
    return outX + cellSize <= atlasW && outY + cellSize <= atlasH;
}

static void StoreIconCellGlyph(IconAtlasCPU& out, char32_t codepoint, int x, int y, int atlasW, int atlasH,
    int cellSize, bool includeInFallbackPool = true) {
    Glyph glyph{};
    glyph.uvMinX = (float)x / atlasW;
    glyph.uvMinY = (float)y / atlasH;
    glyph.uvMaxX = (float)(x + cellSize) / atlasW;
    glyph.uvMaxY = (float)(y + cellSize) / atlasH;
    glyph.width = cellSize;
    glyph.height = cellSize;
    glyph.advanceX = cellSize;
    out.iconGlyphLookup[codepoint] = glyph;
    if (includeInFallbackPool) {
        out.metadata.mixedIconCodepoints.push_back(codepoint);
    }
}

static int ComputeIconAtlasDimension(size_t iconCount, int cellSize) {
    const size_t iconsToPlace = std::max<size_t>(iconCount, 1u);
    int dimension = kIconAtlasMinDimension;

    while (dimension < kIconAtlasMaxDimension) {
        const int cellsPerRow = (dimension + kIconCellGap) / (cellSize + kIconCellGap);
        if (cellsPerRow > 0) {
            const size_t rows =
                (iconsToPlace + static_cast<size_t>(cellsPerRow) - 1u) / static_cast<size_t>(cellsPerRow);
            const size_t requiredHeight = static_cast<size_t>(kIconStartY) +
                (rows - 1u) * static_cast<size_t>(cellSize + kIconCellGap) +
                static_cast<size_t>(cellSize);
            if (requiredHeight <= static_cast<size_t>(dimension)) return dimension;
        }

        dimension *= 2;
    }

    return kIconAtlasMaxDimension;
}

// Builds an icon atlas whose cells are `iconCellSizePx` px (the monitor's on-screen icon size) and fills
// `out` with the matching UVs + metadata. Called per monitor by the platform BuildMonitorIconAtlas.
AtlasBitmap BuildIconAtlas(int iconCellSizePx, IconAtlasCPU& out) {
    const int cellSize = std::max(1, iconCellSizePx);
    // Procedural dummy icons were authored in a 24 px reference cell; scale their coordinates to fit.
    auto S = [cellSize](int v) { return (int)std::lround((double)v * cellSize / (double)kIconReferenceCellSize); };

    const std::vector<SVGIconRenderer::RenderedSVGIcon> svgIcons =
        SVGIconRenderer::RenderEmbeddedSVGIcons(cellSize);
    const int atlasDimension = ComputeIconAtlasDimension(4u + svgIcons.size(), cellSize);
    const int atlasW = atlasDimension;
    const int atlasH = atlasDimension;

    AtlasBitmap atlas{};
    atlas.width = atlasW;
    atlas.height = atlasH;
    atlas.bytesPerPixel = 4;
    atlas.pixels.resize(static_cast<size_t>(atlasW) * atlasH * atlas.bytesPerPixel, 0);

    // The source rounded rectangle is split into 9 texture regions at draw time.
    // Destination corners are resized to ~2 mm in screen space by PushRoundedRectangle.
    GenerateRoundedRectangleNineSlice(atlas, 0, 0, 32, 8, out.metadata.roundedRectangle);

    out.iconGlyphLookup.clear();
    out.metadata.mixedIconCodepoints.clear();

    std::array<int, 4> iconXs{};
    std::array<int, 4> iconYs{};
    int nextIconIndex = 0;
    for (int i = 0; i < 4; ++i) {
        if (!TryReserveIconCell(nextIconIndex, atlasW, atlasH, cellSize, iconXs[i], iconYs[i])) continue;
        StoreIconCellGlyph(out, out.metadata.dummyIconCodepoints[i], iconXs[i], iconYs[i], atlasW, atlasH, cellSize);
        ++nextIconIndex;
    }

    // Dummy icon 0: plus
    FillRect(atlas, iconXs[0] + S(10), iconYs[0] + S(4), S(4), S(16), 255);
    FillRect(atlas, iconXs[0] + S(4), iconYs[0] + S(10), S(16), S(4), 255);

    // Dummy icon 1: folder-like block
    FillRect(atlas, iconXs[1] + S(4), iconYs[1] + S(8), S(16), S(11), 255);
    FillRect(atlas, iconXs[1] + S(6), iconYs[1] + S(5), S(7), S(4), 255);

    // Dummy icon 2: ring (drawn to fill the cell, radii scaled from the 24 px reference)
    const float ringCenter = cellSize * 0.5f;
    const float ringInner = 5.0f * cellSize / (float)kIconReferenceCellSize;
    const float ringOuter = 8.0f * cellSize / (float)kIconReferenceCellSize;
    for (int y = 0; y < cellSize; ++y) {
        for (int x = 0; x < cellSize; ++x) {
            const float dx = (float)x + 0.5f - ringCenter;
            const float dy = (float)y + 0.5f - ringCenter;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d >= ringInner && d <= ringOuter) {
                SetAtlasCoverage(atlas, iconXs[2] + x, iconYs[2] + y, 255);
            }
        }
    }

    // Dummy icon 3: 2x2 grid
    FillRect(atlas, iconXs[3] + S(4), iconYs[3] + S(4), S(7), S(7), 255);
    FillRect(atlas, iconXs[3] + S(13), iconYs[3] + S(4), S(7), S(7), 255);
    FillRect(atlas, iconXs[3] + S(4), iconYs[3] + S(13), S(7), S(7), 255);
    FillRect(atlas, iconXs[3] + S(13), iconYs[3] + S(13), S(7), S(7), 255);

    for (const SVGIconRenderer::RenderedSVGIcon& svgIcon : svgIcons) {
        int cellX = 0;
        int cellY = 0;
        if (!TryReserveIconCell(nextIconIndex, atlasW, atlasH, cellSize, cellX, cellY)) break;

        CopyRenderedSVGIconToAtlas(atlas, svgIcon, cellX, cellY, cellSize);
        StoreIconCellGlyph(out, SVGIconRenderer::IconForID(svgIcon.id), cellX, cellY, atlasW, atlasH, cellSize, false);
        ++nextIconIndex;
    }

    return atlas;
}

AtlasBitmap BuildMSDFFontAtlas() { // Called by the platform InitUIResources; declared in UserInterface.h.
    AtlasBitmap atlas{};
    atlas.width = NotoSansMSDF_Width;
    atlas.height = NotoSansMSDF_Height;
    atlas.bytesPerPixel = 4;
    atlas.pixels.assign(NotoSansMSDF_Pixels,
        NotoSansMSDF_Pixels + (size_t)atlas.width * (size_t)atlas.height * (size_t)atlas.bytesPerPixel);

    glyphLookup.clear();
    for (const auto& entry : NotoSansMSDF_Glyphs) {
        const char32_t codepoint = entry.first;
        const MSDFGlyph& msdf = entry.second;

        Glyph glyph{};
        glyph.uvMinX = msdf.atlasLeft / (float)atlas.width;
        glyph.uvMaxX = msdf.atlasRight / (float)atlas.width;
        glyph.uvMinY = ((float)atlas.height - msdf.atlasTop) / (float)atlas.height;
        glyph.uvMaxY = ((float)atlas.height - msdf.atlasBottom) / (float)atlas.height;

        glyph.width = std::max(0, (int)std::ceil((msdf.planeRight - msdf.planeLeft) * NotoSansMSDF_Size));
        glyph.height = std::max(0, (int)std::ceil((msdf.planeTop - msdf.planeBottom) * NotoSansMSDF_Size));
        glyph.bearingX = (int)std::floor(msdf.planeLeft * NotoSansMSDF_Size);
        glyph.bearingY = (int)std::ceil(msdf.planeTop * NotoSansMSDF_Size);
        glyph.advanceX = std::max(0, (int)std::round(msdf.advance * NotoSansMSDF_Size));

        glyphLookup[codepoint] = glyph;
    }

    return atlas;
}

static uint32_t StableRandomUIColour(uint32_t seed) {
    seed ^= seed >> 16;
    seed *= 0x7FEB352Du;
    seed ^= seed >> 15;
    seed *= 0x846CA68Bu;
    seed ^= seed >> 16;

    uint32_t r = 80u + ((seed >> 0) & 0x7Fu);
    uint32_t g = 80u + ((seed >> 8) & 0x7Fu);
    uint32_t b = 80u + ((seed >> 16) & 0x7Fu);
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

static float TextScaleForHeight(float targetHeight) {
    auto glyphIt = glyphLookup.find(U'M');
    if (glyphIt == glyphLookup.end() || glyphIt->second.height <= 0) return 1.0f;

    return targetHeight / (float)glyphIt->second.height;
}

static float MeasureUIStringWidth(const char32_t* text, float scale) {
    if (!text) return 0.0f;

    float cursorX = 0.0f;
    float maxRight = 0.0f;

    for (const char32_t* p = text; *p; ++p) {
        auto glyphIt = glyphLookup.find(*p);
        if (glyphIt == glyphLookup.end()) continue;

        const Glyph& g = glyphIt->second;
        float glyphLeft = cursorX + (float)g.bearingX * scale;
        float glyphRight = glyphLeft + (float)g.width * scale;
        if (glyphRight > maxRight) maxRight = glyphRight;
        cursorX += (float)g.advanceX * scale;
    }

    return std::max(cursorX, maxRight);
}

static const char32_t* LocalizedUIString(UITextID stringID) {
    const char32_t* text = GetUILocalizedString(stringID, UILanguage::English);
    return text ? text : U"";
}

static const char32_t* LocalizedControlLabel(const UIControlDefinition& ctrl) {
    const char32_t* label = LocalizedUIString(ctrl.nameStringID);
    if (*label != U'\0') return label;

    if (ctrl.action == Commands::INVALID || ctrl.type == 0 || ctrl.type == 3) {
        return LocalizedUIString(ctrl.nameStringID);
    }

    return U"";
}

static void ClampTopRibbonScroll(UITopRibbonLayout& layout, float viewportWidth) {
    const float maxScroll = std::max(0.0f, layout.totalContentWidthPx - viewportWidth);
    layout.scrollOffsetPx = std::clamp(layout.scrollOffsetPx, 0.0f, maxScroll);
}

void PrecomputeTopRibbonLayout(UITopRibbonLayout& layout, float monitorDPIX, float monitorDPIY) {
    // Floor the layout DPI so coarse monitors scale the whole ribbon up instead of minifying it.
    monitorDPIX = std::max(monitorDPIX, UI_MIN_LAYOUT_DPI);
    monitorDPIY = std::max(monitorDPIY, UI_MIN_LAYOUT_DPI);

    const float previousScroll = layout.scrollOffsetPx;
    layout = UITopRibbonLayout{};

    layout.dpiX = monitorDPIX;
    layout.dpiY = monitorDPIY;

    const float pixelsPerMMx = monitorDPIX / 25.4f;
    const float pixelsPerMMy = monitorDPIY / 25.4f;
    layout.buttonWidthPx = std::round(UI_BUTTON_WIDTH_MM * pixelsPerMMx);
    layout.iconSizePx = std::round(UI_ICON_SIZE_MM * pixelsPerMMy);
    layout.textHeightPx = std::round(UI_TEXT_HEIGHT_MM * pixelsPerMMy);
    layout.buttonHeightPx = std::max(std::round(UI_BUTTON_HEIGHT_MM * pixelsPerMMy),
        std::max(layout.iconSizePx, layout.textHeightPx) + 4.0f);
    layout.iconReservedWidthPx = layout.iconSizePx + 4.0f;
    layout.textStartOffsetPx = layout.iconReservedWidthPx + 4.0f;
    layout.textEndInsetPx = 6.0f;
    layout.buttonGapPx = UI_BUTTON_GAP_MM * pixelsPerMMx;
    layout.tabBarHeightPx = std::round(UI_TAB_BAR_HEIGHT_MM * pixelsPerMMy);
    layout.roundedCornerRadiusPx = std::max(1.0f, std::round(UI_BUTTON_CORNER_RADIUS_MM * pixelsPerMMy));
    layout.uiTextScale = TextScaleForHeight(layout.textHeightPx);
    layout.actionGroupLabelY = (UI_TAB_BAR_HEIGHT_MM + UI_DIVIDER_GAP_PX) * pixelsPerMMy;
    layout.actionGroupLabelHeightPx = UI_ACTION_GROUP_LABEL_HEIGHT_MM * pixelsPerMMy;
    layout.topActionGroupY = (UI_TAB_BAR_HEIGHT_MM + UI_DIVIDER_GAP_PX +
        UI_ACTION_GROUP_LABEL_HEIGHT_MM) * pixelsPerMMy + 5.0f;
    layout.actionSubGroupLabelY = (UI_TAB_BAR_HEIGHT_MM + UI_DIVIDER_GAP_PX +
        UI_ACTION_GROUP_LABEL_HEIGHT_MM + UI_DIVIDER_GAP_PX +
        UI_ACTION_GROUP_HEIGHT_MM + UI_DIVIDER_GAP_PX) * pixelsPerMMy + 5.0f;
    layout.internalTabBarY = std::round((UI_TAB_BAR_HEIGHT_MM + UI_DIVIDER_GAP_PX +
        UI_ACTION_GROUP_LABEL_HEIGHT_MM + UI_DIVIDER_GAP_PX +
        UI_ACTION_GROUP_HEIGHT_MM + UI_DIVIDER_GAP_PX +
        UI_ACTION_GROUP_LABEL_HEIGHT_MM + UI_DIVIDER_GAP_PX) * pixelsPerMMy) + 7.0f;
    layout.internalTabBarHeightPx = std::round(UI_INTERNAL_TAB_BAR_HEIGHT_MM * pixelsPerMMy);
    layout.topUITotalHeightPx = layout.internalTabBarY + layout.internalTabBarHeightPx;


    const float groupNavPaddingPx = 8.0f;
    const float groupNavGapPx = 2.0f * pixelsPerMMx;
    float groupNavX = 0.0f;
    for (size_t i = 0; i < TotalTopUIActionGroups; ++i) {
        const char32_t* label = LocalizedUIString(topUIActionGroupNames[i].labelStringID);
        layout.actionGroups[i].navX = groupNavX;
        layout.actionGroups[i].navWidth = std::ceil(MeasureUIStringWidth(label, layout.uiTextScale) +
            2.0f * groupNavPaddingPx);
        groupNavX += layout.actionGroups[i].navWidth + groupNavGapPx;
    }
    layout.actionGroupNavTotalWidthPx = groupNavX > 0.0f ? groupNavX - groupNavGapPx : 0.0f;

    std::array<bool, TotalTopUIActionGroups> groupSeen{};
    float currentX = 5.0f;
    float verticalSlotMaxSize = 0.0f;
    float contentRight = currentX;
    int activeSubGroupIndex = -1;
    size_t activeSubGroupRun = 0;

    for (size_t i = 0; i < TotalUIControls; ++i) {
        const UIControlDefinition& ctrl = AllUIControls[i];
        const size_t groupIndex = ctrl.actionGroupIndex;
        const size_t subGroupIndex = ctrl.actionSubGroupIndex;

        if (groupIndex < TotalTopUIActionGroups && !groupSeen[groupIndex]) {
            layout.actionGroups[groupIndex].contentStartX = currentX;
            layout.actionGroups[groupIndex].contentEndX = currentX;
            groupSeen[groupIndex] = true;
        }

        if (subGroupIndex < TotalTopUIActionSubGroups &&
            !layout.actionSubGroups[subGroupIndex].hasControls) {
            layout.actionSubGroups[subGroupIndex].contentStartX = currentX;
            layout.actionSubGroups[subGroupIndex].contentEndX = currentX;
            layout.actionSubGroups[subGroupIndex].hasControls = true;
        }

        if (activeSubGroupIndex != (int)subGroupIndex &&
            layout.actionSubGroupRunCount < layout.actionSubGroupRuns.size()) {
            activeSubGroupIndex = (int)subGroupIndex;
            activeSubGroupRun = layout.actionSubGroupRunCount++;
            layout.actionSubGroupRuns[activeSubGroupRun].subGroupIndex = ctrl.actionSubGroupIndex;
            layout.actionSubGroupRuns[activeSubGroupRun].contentStartX = currentX;
            layout.actionSubGroupRuns[activeSubGroupRun].contentEndX = currentX;
        }

        float btnWidth = layout.buttonWidthPx;
        float btnY = layout.topActionGroupY;
        const char32_t* label = LocalizedControlLabel(ctrl);
        if (ctrl.showText && *label != U'\0') {
            float contentWidth = layout.textStartOffsetPx +
                MeasureUIStringWidth(label, layout.uiTextScale) + layout.textEndInsetPx;
            btnWidth = std::max(btnWidth, contentWidth);
        }
        if (ctrl.noOfVerticalSlots > 1) {
            btnY += ctrl.verticalSlotNo * (layout.buttonHeightPx + 1.0f);
        }

        layout.controls[i] = { currentX, btnY, btnWidth, layout.buttonHeightPx };
        verticalSlotMaxSize = std::max(verticalSlotMaxSize, btnWidth);

        const bool endOfColumn = (i + 1 == TotalUIControls) || (AllUIControls[i + 1].verticalSlotNo == 0);
        if (endOfColumn) {
            const float columnRight = currentX + verticalSlotMaxSize;
            if (groupIndex < TotalTopUIActionGroups) {
                layout.actionGroups[groupIndex].contentEndX =
                    std::max(layout.actionGroups[groupIndex].contentEndX, columnRight);
            }
            if (subGroupIndex < TotalTopUIActionSubGroups) {
                layout.actionSubGroups[subGroupIndex].contentEndX =
                    std::max(layout.actionSubGroups[subGroupIndex].contentEndX, columnRight);
            }
            if (layout.actionSubGroupRunCount > 0) {
                layout.actionSubGroupRuns[activeSubGroupRun].contentEndX =
                    std::max(layout.actionSubGroupRuns[activeSubGroupRun].contentEndX, columnRight);
            }
            contentRight = std::max(contentRight, columnRight);

            if (i + 1 < TotalUIControls) {
                currentX = columnRight + layout.buttonGapPx;
            }
            verticalSlotMaxSize = 0.0f;
        }
    }

    for (size_t gIdx = 0; gIdx < TotalTopUIActionGroups; ++gIdx) {
        layout.actionGroups[gIdx].contentWidth = std::max(0.0f, layout.actionGroups[gIdx].contentEndX - layout.actionGroups[gIdx].contentStartX);
    }

    layout.totalContentWidthPx = contentRight + 30.0f;
    layout.scrollOffsetPx = previousScroll;
    layout.isValid = true;
}

static float MapRibbonToNav(float x, const UITopRibbonLayout& layout) {
    if (TotalTopUIActionGroups == 0) return 0.0f;

    const auto& firstGroup = layout.actionGroups[0];
    if (x <= firstGroup.contentStartX) {
        return firstGroup.navX;
    }
    const auto& lastGroup = layout.actionGroups[TotalTopUIActionGroups - 1];
    if (x >= lastGroup.contentEndX) {
        return lastGroup.navX + lastGroup.navWidth;
    }

    for (size_t i = 0; i < TotalTopUIActionGroups; ++i) {
        const auto& grp = layout.actionGroups[i];
        if (x >= grp.contentStartX && x <= grp.contentEndX) {
            float width = grp.contentWidth;
            if (width <= 0.0f) return grp.navX;
            float t = (x - grp.contentStartX) / width;
            return grp.navX + t * grp.navWidth;
        }
        if (i + 1 < TotalTopUIActionGroups) {
            const auto& nextGrp = layout.actionGroups[i + 1];
            if (x > grp.contentEndX && x < nextGrp.contentStartX) {
                float gapWidth = nextGrp.contentStartX - grp.contentEndX;
                if (gapWidth <= 0.0f) return nextGrp.navX;
                float t = (x - grp.contentEndX) / gapWidth;
                float startNav = grp.navX + grp.navWidth;
                float endNav = nextGrp.navX;
                return startNav + t * (endNav - startNav);
            }
        }
    }

    return 0.0f;
}

// PushRect
void PushRect( UIDrawContext& ctx, float x, float y, float w, float h,
    uint32_t color, DX12ResourcesUI& uiRes) {
    if (ctx.vertexCount + 4 > uiRes.maxVertices) return;
    if (ctx.indexCount + 6 > uiRes.maxIndices) return;

    uint16_t base = ctx.vertexCount;

    ctx.vertexPtr[0] = { x,y,0,0,color, UI_ENGLISH_ATLAS_SLOT };
    ctx.vertexPtr[1] = { x + w,y,0,0,color, UI_ENGLISH_ATLAS_SLOT };
    ctx.vertexPtr[2] = { x + w,y + h,0,0,color, UI_ENGLISH_ATLAS_SLOT };
    ctx.vertexPtr[3] = { x,y + h,0,0,color, UI_ENGLISH_ATLAS_SLOT };

    ctx.indexPtr[0] = base + 0;
    ctx.indexPtr[1] = base + 1;
    ctx.indexPtr[2] = base + 2;
    ctx.indexPtr[3] = base + 0;
    ctx.indexPtr[4] = base + 2;
    ctx.indexPtr[5] = base + 3;
}

static void PushTexturedQuad(UIDrawContext& ctx, float x, float y, float w, float h,
    const UIAtlasRegion& region, uint32_t atlasIndex, uint32_t color, DX12ResourcesUI& uiRes) {
    if (ctx.vertexCount + 4 > uiRes.maxVertices) return;
    if (ctx.indexCount + 6 > uiRes.maxIndices) return;

    uint16_t base = ctx.vertexCount;
    ctx.vertexPtr[0] = { x,     y,     region.uvMinX, region.uvMinY, color, atlasIndex };
    ctx.vertexPtr[1] = { x + w, y,     region.uvMaxX, region.uvMinY, color, atlasIndex };
    ctx.vertexPtr[2] = { x + w, y + h, region.uvMaxX, region.uvMaxY, color, atlasIndex };
    ctx.vertexPtr[3] = { x,     y + h, region.uvMinX, region.uvMaxY, color, atlasIndex };

    ctx.indexPtr[0] = base + 0;
    ctx.indexPtr[1] = base + 1;
    ctx.indexPtr[2] = base + 2;
    ctx.indexPtr[3] = base + 0;
    ctx.indexPtr[4] = base + 2;
    ctx.indexPtr[5] = base + 3;

    ctx.vertexPtr += 4;
    ctx.indexPtr += 6;
    ctx.vertexCount += 4;
    ctx.indexCount += 6;
}

void PushRoundedRectangle(UIDrawContext& ctx, float x, float y, float w, float h, float radiusPx,
    uint32_t color, DX12ResourcesUI& uiRes) {
    if (w <= 0.0f || h <= 0.0f) return;
    if (!ctx.iconData) return; // nine-slice regions live in the per-monitor icon metadata
    if (ctx.vertexCount + 36 > uiRes.maxVertices) return;
    if (ctx.indexCount + 54 > uiRes.maxIndices) return;

    const float clampedRadius = std::max(1.0f, std::min(radiusPx, 0.5f * std::min(w, h)));
    const float xCuts[4] = { x, x + clampedRadius, x + w - clampedRadius, x + w };
    const float yCuts[4] = { y, y + clampedRadius, y + h - clampedRadius, y + h };

    /*
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            PushTexturedQuad(ctx, xCuts[col], yCuts[row],
                xCuts[col + 1] - xCuts[col], yCuts[row + 1] - yCuts[row],
                ctx.iconData->metadata.roundedRectangle.regions[row][col],
                UI_ICON_ATLAS_SLOT, color, uiRes);
        }
    }*/
	// Unrolled the above loops for better performance (fewer function calls, better instruction-level parallelism)
    PushTexturedQuad(ctx, xCuts[0], yCuts[0], xCuts[1] - xCuts[0], yCuts[1] - yCuts[0],
        ctx.iconData->metadata.roundedRectangle.regions[0][0], UI_ICON_ATLAS_SLOT, color, uiRes);
    PushTexturedQuad(ctx, xCuts[1], yCuts[0], xCuts[2] - xCuts[1], yCuts[1] - yCuts[0],
        ctx.iconData->metadata.roundedRectangle.regions[0][1], UI_ICON_ATLAS_SLOT, color, uiRes);
    PushTexturedQuad(ctx, xCuts[2], yCuts[0], xCuts[3] - xCuts[2], yCuts[1] - yCuts[0],
        ctx.iconData->metadata.roundedRectangle.regions[0][2], UI_ICON_ATLAS_SLOT, color, uiRes);
    PushTexturedQuad(ctx, xCuts[0], yCuts[1], xCuts[1] - xCuts[0], yCuts[2] - yCuts[1],
        ctx.iconData->metadata.roundedRectangle.regions[1][0], UI_ICON_ATLAS_SLOT, color, uiRes);
    PushTexturedQuad(ctx, xCuts[1], yCuts[1], xCuts[2] - xCuts[1], yCuts[2] - yCuts[1],
        ctx.iconData->metadata.roundedRectangle.regions[1][1], UI_ICON_ATLAS_SLOT, color, uiRes);
    PushTexturedQuad(ctx, xCuts[2], yCuts[1], xCuts[3] - xCuts[2], yCuts[2] - yCuts[1],
        ctx.iconData->metadata.roundedRectangle.regions[1][2], UI_ICON_ATLAS_SLOT, color, uiRes);
    PushTexturedQuad(ctx, xCuts[0], yCuts[2], xCuts[1] - xCuts[0], yCuts[3] - yCuts[2],
        ctx.iconData->metadata.roundedRectangle.regions[2][0], UI_ICON_ATLAS_SLOT, color, uiRes);
    PushTexturedQuad(ctx, xCuts[1], yCuts[2], xCuts[2] - xCuts[1], yCuts[3] - yCuts[2],
        ctx.iconData->metadata.roundedRectangle.regions[2][1], UI_ICON_ATLAS_SLOT, color, uiRes);
    PushTexturedQuad(ctx, xCuts[2], yCuts[2], xCuts[3] - xCuts[2], yCuts[3] - yCuts[2],
        ctx.iconData->metadata.roundedRectangle.regions[2][2], UI_ICON_ATLAS_SLOT, color, uiRes);
}

void PushTopRoundedRectangle(UIDrawContext& ctx, float x, float y, float w, float h, float radiusPx,
    uint32_t color, DX12ResourcesUI& uiRes) {
    if (w <= 0.0f || h <= 0.0f) return;
    if (!ctx.iconData) return; // nine-slice regions live in the per-monitor icon metadata
    if (ctx.vertexCount + 24 > uiRes.maxVertices) return;
    if (ctx.indexCount + 36 > uiRes.maxIndices) return;

    const float clampedRadius = std::max(1.0f, std::min(radiusPx, 0.5f * std::min(w, h)));
    const float xCuts[4] = { x, x + clampedRadius, x + w - clampedRadius, x + w };
    const float yCuts[3] = { y, y + clampedRadius, y + h };

    // Row 0 (Top part with rounded corners)
    PushTexturedQuad(ctx, xCuts[0], yCuts[0], xCuts[1] - xCuts[0], yCuts[1] - yCuts[0],
        ctx.iconData->metadata.roundedRectangle.regions[0][0], UI_ICON_ATLAS_SLOT, color, uiRes);
    PushTexturedQuad(ctx, xCuts[1], yCuts[0], xCuts[2] - xCuts[1], yCuts[1] - yCuts[0],
        ctx.iconData->metadata.roundedRectangle.regions[0][1], UI_ICON_ATLAS_SLOT, color, uiRes);
    PushTexturedQuad(ctx, xCuts[2], yCuts[0], xCuts[3] - xCuts[2], yCuts[1] - yCuts[0],
        ctx.iconData->metadata.roundedRectangle.regions[0][2], UI_ICON_ATLAS_SLOT, color, uiRes);

    // Row 1 (Bottom part with sharp corners utilizing the flat middle row)
    PushTexturedQuad(ctx, xCuts[0], yCuts[1], xCuts[1] - xCuts[0], yCuts[2] - yCuts[1],
        ctx.iconData->metadata.roundedRectangle.regions[1][0], UI_ICON_ATLAS_SLOT, color, uiRes);
    PushTexturedQuad(ctx, xCuts[1], yCuts[1], xCuts[2] - xCuts[1], yCuts[2] - yCuts[1],
        ctx.iconData->metadata.roundedRectangle.regions[1][1], UI_ICON_ATLAS_SLOT, color, uiRes);
    PushTexturedQuad(ctx, xCuts[2], yCuts[1], xCuts[3] - xCuts[2], yCuts[2] - yCuts[1],
        ctx.iconData->metadata.roundedRectangle.regions[1][2], UI_ICON_ATLAS_SLOT, color, uiRes);
}

static void PushIcon(UIDrawContext& ctx, float x, float y, float w, float h, char32_t iconCodepoint,
    uint32_t color, DX12ResourcesUI& uiRes) {
    if (!ctx.iconData) return;
    auto iconIt = ctx.iconData->iconGlyphLookup.find(iconCodepoint);
    if (iconIt == ctx.iconData->iconGlyphLookup.end()) return;
    const Glyph& glyph = iconIt->second;
    UIAtlasRegion icon{ glyph.uvMinX, glyph.uvMinY, glyph.uvMaxX, glyph.uvMaxY };
    // Snap to whole pixels: the icon atlas is rasterised 1:1 to the on-screen size, so any fractional
    // origin/size would resample and smear an otherwise pixel-exact glyph.
    PushTexturedQuad(ctx, std::round(x), std::round(y), std::round(w), std::round(h),
        icon, UI_ICON_ATLAS_SLOT, color, uiRes);
}

// Returns true if clicked this frame
bool PushInteractiveRect(UIDrawContext& ctx, float x, float y, float w, float h, uint32_t baseColor,
    uint32_t id, const UIInput& input, DX12ResourcesUI& uiRes, bool enabled = true) {
    uint32_t color = baseColor;

    bool hovered = enabled && (input.mouseX >= x && input.mouseX < x + w &&
        input.mouseY >= y && input.mouseY < y + h);

    if (hovered) color = 0xFF555555; // hover tint (TODO: theme-aware)
    if (hovered && input.leftButtonDown) color = 0xFF333333; // pressed tint
    if (!enabled) color = 0xFF1E1E1E; // If disabled, force a darker/grayer base color
    
    PushRect(ctx, x, y, w, h, color, uiRes);

    if (!enabled) return false;// Disabled controls do NOT respond to clicks
    if (hovered && input.leftButtonPressedThisFrame) {
        return true;
    }
    return false;
}

void PushText(UIDrawContext& ctx, float x, float y, const char* text, uint32_t color, DX12ResourcesUI& uiRes)
{
    float cursorX = x;
    uint32_t glyphCount = 0;

    for (const char* p = text; *p; ++p)
    {
        // Bounds Checking (Crucial for text strings)
        if (ctx.vertexCount + (glyphCount + 1) * 4 > uiRes.maxVertices) return;
        if (ctx.indexCount + (glyphCount + 1) * 6 > uiRes.maxIndices) return;

        char c = *p;
        if (glyphLookup.find(c) == glyphLookup.end()) continue;

        const Glyph& g = glyphLookup[c];
        if (g.width <= 0 || g.height <= 0) {
            cursorX += g.advanceX;
            continue;
        }

        float xpos = cursorX + g.bearingX;
        float ypos = y - g.bearingY;
        float w = (float)g.width;
        float h = (float)g.height;
        uint32_t vertexOffset = glyphCount * 4;
        uint32_t indexOffset = glyphCount * 6;
        
        // Add 4 vertices. Write relative to the current pointer (0, 1, 2, 3)
        uint32_t vidx = ctx.vertexCount + vertexOffset;
        ctx.vertexPtr[vertexOffset + 0] = { xpos,     ypos,     g.uvMinX, g.uvMinY, color, UI_ENGLISH_ATLAS_SLOT };
        ctx.vertexPtr[vertexOffset + 1] = { xpos + w, ypos,     g.uvMaxX, g.uvMinY, color, UI_ENGLISH_ATLAS_SLOT };
        ctx.vertexPtr[vertexOffset + 2] = { xpos + w, ypos + h, g.uvMaxX, g.uvMaxY, color, UI_ENGLISH_ATLAS_SLOT };
        ctx.vertexPtr[vertexOffset + 3] = { xpos,     ypos + h, g.uvMinX, g.uvMaxY, color, UI_ENGLISH_ATLAS_SLOT };

        // Add 6 indices. Write indices relative to the current index pointer
        ctx.indexPtr[indexOffset + 0] = vidx + 0;
        ctx.indexPtr[indexOffset + 1] = vidx + 1;
        ctx.indexPtr[indexOffset + 2] = vidx + 2;
        ctx.indexPtr[indexOffset + 3] = vidx + 0;
        ctx.indexPtr[indexOffset + 4] = vidx + 2;
        ctx.indexPtr[indexOffset + 5] = vidx + 3;

        glyphCount++;
        cursorX += g.advanceX;
    }

    ctx.vertexPtr += glyphCount * 4;
    ctx.indexPtr += glyphCount * 6;
    ctx.vertexCount += glyphCount * 4;
    ctx.indexCount += glyphCount * 6;
}

namespace {
// File-scope helpers for reusable widgets (BuildUIDropdown). Unlike raw PushRect, they advance
// the vertex/index counters themselves, mirroring BuildUIOverlay's local pushRect/pushTextClipped.
void PushWidgetRect(UIDrawContext& ctx, DX12ResourcesUI& uiRes, float x, float y, float w, float h,
    uint32_t color) {
    if (ctx.vertexCount + 4 > uiRes.maxVertices || ctx.indexCount + 6 > uiRes.maxIndices) return;
    PushRect(ctx, x, y, w, h, color, uiRes);
    ctx.vertexPtr += 4;
    ctx.indexPtr += 6;
    ctx.vertexCount += 4;
    ctx.indexCount += 6;
}

// ASCII text scaled by 'textScale', clipped to maxWidth, baseline at baselineY.
void PushWidgetText(UIDrawContext& ctx, DX12ResourcesUI& uiRes, float x, float baselineY,
    const char* text, float maxWidth, uint32_t color, float textScale) {
    if (!text || maxWidth <= 0.0f) return;
    float cursorX = x;
    const float textRight = x + maxWidth;
    for (const char* p = text; *p; ++p) {
        auto glyphIt = glyphLookup.find(static_cast<char32_t>(static_cast<unsigned char>(*p)));
        if (glyphIt == glyphLookup.end()) continue;
        const Glyph& g = glyphIt->second;
        if (g.width <= 0 || g.height <= 0) {
            cursorX += (float)g.advanceX * textScale;
            continue;
        }
        const float xpos = std::floor(cursorX + (float)g.bearingX * textScale + 0.5f);
        const float ypos = std::floor(baselineY - (float)g.bearingY * textScale + 0.5f);
        const float glyphWidth = (float)g.width * textScale;
        const float glyphHeight = (float)g.height * textScale;
        if (xpos + glyphWidth > textRight) break;
        if (ctx.vertexCount + 4 > uiRes.maxVertices || ctx.indexCount + 6 > uiRes.maxIndices) return;

        uint16_t base = ctx.vertexCount;
        ctx.vertexPtr[0] = { xpos,              ypos,               g.uvMinX, g.uvMinY, color, UI_ENGLISH_ATLAS_SLOT };
        ctx.vertexPtr[1] = { xpos + glyphWidth, ypos,               g.uvMaxX, g.uvMinY, color, UI_ENGLISH_ATLAS_SLOT };
        ctx.vertexPtr[2] = { xpos + glyphWidth, ypos + glyphHeight, g.uvMaxX, g.uvMaxY, color, UI_ENGLISH_ATLAS_SLOT };
        ctx.vertexPtr[3] = { xpos,              ypos + glyphHeight, g.uvMinX, g.uvMaxY, color, UI_ENGLISH_ATLAS_SLOT };
        ctx.indexPtr[0] = base + 0;
        ctx.indexPtr[1] = base + 1;
        ctx.indexPtr[2] = base + 2;
        ctx.indexPtr[3] = base + 0;
        ctx.indexPtr[4] = base + 2;
        ctx.indexPtr[5] = base + 3;
        ctx.vertexPtr += 4;
        ctx.indexPtr += 6;
        ctx.vertexCount += 4;
        ctx.indexCount += 6;
        cursorX += (float)g.advanceX * textScale;
    }
}

float WidgetTextBaselineY(float y, float h, float textScale) {
    auto glyphIt = glyphLookup.find(U'M');
    if (glyphIt == glyphLookup.end()) return y + h * 0.7f;
    const Glyph& g = glyphIt->second;
    return y + h * 0.5f + (float)g.bearingY * textScale - (float)g.height * textScale * 0.5f;
}
} // namespace

int BuildUIDropdown(UIDrawContext& ctx, DX12ResourcesUI& uiRes, const UIInput& input,
    UIDropdownState& state, float x, float y, float width, float rowHeightPx, float textScale,
    const char* const* items, int itemCount, int selectedIndex) {
    if (itemCount <= 0) {
        state.isOpen = false;
        return -1;
    }
    int selected = std::clamp(selectedIndex, 0, itemCount - 1);

    // Hit-test first (immediate mode: reacts to last frame's layout, draws this frame's state).
    if (input.leftButtonPressedThisFrame) {
        const bool fieldHit = input.mouseX >= x && input.mouseX < x + width &&
            input.mouseY >= y && input.mouseY < y + rowHeightPx;
        if (fieldHit) {
            state.isOpen = !state.isOpen;
        } else if (state.isOpen) {
            for (int i = 0; i < itemCount; ++i) {
                const float itemY = y + rowHeightPx * (float)(i + 1);
                if (input.mouseX >= x && input.mouseX < x + width &&
                    input.mouseY >= itemY && input.mouseY < itemY + rowHeightPx) {
                    selected = i;
                    break;
                }
            }
            state.isOpen = false; // Any click outside the field closes (selecting or not).
        }
    }

    const float textPad = std::max(1.0f, rowHeightPx * 0.2f);
    const float markerWidth = rowHeightPx; // Square slot for the open/close marker.
    const uint32_t fieldBg = 0xFFF7F7F7u;
    const uint32_t textColor = 0xFF000000u;

    // Closed field: current value plus a 'v' marker at the right edge.
    PushWidgetRect(ctx, uiRes, x, y, width, rowHeightPx, fieldBg);
    PushWidgetText(ctx, uiRes, x + textPad, WidgetTextBaselineY(y, rowHeightPx, textScale),
        items[selected], width - markerWidth - 2.0f * textPad, textColor, textScale);
    PushWidgetText(ctx, uiRes, x + width - markerWidth + textPad,
        WidgetTextBaselineY(y, rowHeightPx, textScale), "v", markerWidth - textPad,
        0xFF666666u, textScale);

    if (state.isOpen) {
        const float listY = y + rowHeightPx;
        PushWidgetRect(ctx, uiRes, x, listY, width, rowHeightPx * (float)itemCount, 0xFFCCCCCCu); // Border frame.
        for (int i = 0; i < itemCount; ++i) {
            const float itemY = listY + rowHeightPx * (float)i;
            const bool hovered = input.mouseX >= x && input.mouseX < x + width &&
                input.mouseY >= itemY && input.mouseY < itemY + rowHeightPx;
            const uint32_t rowBg = hovered ? 0xFFC4D5CDu : (i == selected ? 0xFFE8F2EDu : 0xFFFFFFFFu);
            PushWidgetRect(ctx, uiRes, x + 1.0f, itemY + 1.0f, width - 2.0f, rowHeightPx - 2.0f, rowBg);
            PushWidgetText(ctx, uiRes, x + textPad, WidgetTextBaselineY(itemY, rowHeightPx, textScale),
                items[i], width - 2.0f * textPad, textColor, textScale);
        }
    }
    return selected;
}

// Launch splash (drawn at the very end of BuildUIOverlay below). Its own colour constant so it can
// be tuned without touching ribbon hover. This is #ffeaa7 written ABGR (0xAABBGGRR), the packing
// ShaderUIPixel.hlsl decodes; a literal 0xFFFFEAA7 here would come out pale blue instead.
constexpr uint32_t kSplashBackgroundColor = 0xFFA7EAFFu;
constexpr uint32_t kSplashTextColor = 0xFF000000u; // Black: symmetric, so channel-order agnostic.
constexpr uint64_t kSplashDurationMs = 5000ULL;
// icon_1_logo.svg, embedded via SVGIconManifest.h and rasterised into every monitor's icon atlas.
// Drawn with a white vertex colour so the atlas keeps the logo's own tricolour (shader multiplies).
constexpr char32_t kSplashLogoCodepoint = SVGIconRenderer::IconForID(1u);

std::atomic<uint64_t> g_splashOverlayStartTick{ 0 };

// Portable half of RenderUIOverlay (UserInterface-<Platform>.cpp): lays out and hit-tests every
// widget (tab bands, ribbon, data tree, property pane, cursor icons), fills ctx with the frame's
// UI geometry and emits UIActions. The caller binds the pipeline and draws ctx afterwards.
void BuildUIOverlay(SingleUIWindow& window, UIDrawContext& ctx, DX12ResourcesUI& uiRes,
    UITopRibbonLayout& topRibbonLayout, float monitorDPIX, float monitorDPIY, const UIInput& input,
    uint64_t activeInternalSubTabMemoryId, int monitorId) {
    // Floor the layout DPI identically to PrecomputeTopRibbonLayout so the dirty-check below compares
    // clamped-vs-clamped and every pixelsPerMM conversion in this function stays consistent.
    monitorDPIX = std::max(monitorDPIX, UI_MIN_LAYOUT_DPI);
    monitorDPIY = std::max(monitorDPIY, UI_MIN_LAYOUT_DPI);

    // Point this frame's draw context at the icon lookup/metadata built for this monitor (its UVs match
    // the monitor's icon cell size). PushIcon / PushRoundedRectangle read ctx.iconData.
    if (monitorId >= 0 && monitorId < MV_MAX_MONITORS) {
        ctx.iconData = &gMonitorIconAtlas[monitorId];
    }

    // Any click inside this window makes its active tab the routing context for ribbon commands
    // (ProcessPendingUIActions reads it). Needed once tabs live in more than one window.
    const int windowSlot = static_cast<int>(&window - allWindows);
    if (input.leftButtonPressedThisFrame && window.activeTabIndex >= 0) {
        g_uiActionSourceTabIndex.store(window.activeTabIndex, std::memory_order_release);
    }

    float W = (float)window.dx.WindowWidth;
    float H = (float)window.dx.WindowHeight;
    if (!topRibbonLayout.isValid || topRibbonLayout.dpiX != monitorDPIX || topRibbonLayout.dpiY != monitorDPIY) {
        PrecomputeTopRibbonLayout(topRibbonLayout, monitorDPIX, monitorDPIY);
    }
    if (input.mouseWheelDelta != 0 && input.mouseY >= 0.0f && input.mouseY < topRibbonLayout.topUITotalHeightPx) {
        const float wheelSteps = input.mouseWheelDelta / (float)WHEEL_DELTA;
        const float scrollStepPx = std::max(topRibbonLayout.buttonWidthPx * 2.0f, 120.0f);
        topRibbonLayout.scrollOffsetPx -= wheelSteps * scrollStepPx;
    }
    ClampTopRibbonScroll(topRibbonLayout, W);

    float pixelsPerMMx = monitorDPIX / 25.4f;
    float pixelsPerMMy = monitorDPIY / 25.4f;
    float iconSizePx = topRibbonLayout.iconSizePx;
    float buttonHeightPx = topRibbonLayout.buttonHeightPx;
	float iconReservedWidthPx = topRibbonLayout.iconReservedWidthPx;
    float textStartOffsetPx = topRibbonLayout.textStartOffsetPx;
    float textEndInsetPx = topRibbonLayout.textEndInsetPx;
    float tabBarHeightPx = topRibbonLayout.tabBarHeightPx;
    float topUITotalHeightPx = topRibbonLayout.topUITotalHeightPx;
    float roundedCornerRadiusPx = topRibbonLayout.roundedCornerRadiusPx;
    const VishwakarmaStorage::ObjectType activeInternalSubTabType =
        (window.activeTabIndex >= 0 && window.activeTabIndex < MV_MAX_TABS)
        ? ActiveInternalSubTabType(allTabs[window.activeTabIndex], activeInternalSubTabMemoryId)
        : VishwakarmaStorage::ObjectType::Unknown;
    const bool useDarkDataTreeText =
        activeInternalSubTabMemoryId == 0 ||
        activeInternalSubTabType == VishwakarmaStorage::ObjectType::Page2D ||
        activeInternalSubTabType == VishwakarmaStorage::ObjectType::Scene3D;
    const uint32_t dataTreeTextColor = useDarkDataTreeText ? 0xFF000000 : 0xFFFFFFFF;
    const uint32_t dataTreeActiveColor = 0xFF3399FF;
    const uint32_t dataTreeHoverColor = 0xFFFF9933;

    auto canPushRect = [&]() {
        return ctx.vertexCount + 4 <= uiRes.maxVertices &&
            ctx.indexCount + 6 <= uiRes.maxIndices;
    };

    auto incrementVertexIndexCounters = [&]() {
        ctx.vertexPtr += 4;
        ctx.indexPtr += 6;
        ctx.vertexCount += 4;
        ctx.indexCount += 6;
    };

    auto pushRect = [&](float x, float y, float w, float h, uint32_t color) {
        bool pushed = canPushRect();
        PushRect(ctx, x, y, w, h, color, uiRes);
        if (pushed) incrementVertexIndexCounters();
    };

    const float uiTextScale = topRibbonLayout.uiTextScale;

    auto pushTextClipped = [&](float x, float y, const char32_t* text, float maxWidth, uint32_t color,
        float scale, bool bold = false) {
        if (!text || maxWidth <= 0.0f) return;

        float cursorX = x;
        float textRight = x + maxWidth;

        for (const char32_t* p = text; *p; ++p) {
            if (*p > 0x7F) continue;

            auto glyphIt = glyphLookup.find(*p);
            if (glyphIt == glyphLookup.end()) continue;

            const Glyph& g = glyphIt->second;
            if (g.width <= 0 || g.height <= 0) {
                cursorX += (float)g.advanceX * scale;
                continue;
            }

            // It is always better to be aligned to pixels for better text clarity.
            float xpos = std::floor(cursorX + (float)g.bearingX * scale + 0.5f);
            float ypos = std::floor(y - (float)g.bearingY * scale + 0.5f);
            float glyphWidth = (float)g.width * scale;
            float glyphHeight = (float)g.height * scale;
            float glyphRight = xpos + glyphWidth;

            if (glyphRight > textRight) break;
            if (ctx.vertexCount + 4 > uiRes.maxVertices) return;
            if (ctx.indexCount + 6 > uiRes.maxIndices) return;

            uint16_t base = ctx.vertexCount;
            const uint32_t atlasIndex = UI_ENGLISH_ATLAS_SLOT | (bold ? UI_FONT_BOLD_FLAG : 0u);
            ctx.vertexPtr[0] = { xpos,                  ypos,                   g.uvMinX, g.uvMinY, color, atlasIndex };
            ctx.vertexPtr[1] = { xpos + glyphWidth,     ypos,                   g.uvMaxX, g.uvMinY, color, atlasIndex };
            ctx.vertexPtr[2] = { xpos + glyphWidth,     ypos + glyphHeight,     g.uvMaxX, g.uvMaxY, color, atlasIndex };
            ctx.vertexPtr[3] = { xpos,                  ypos + glyphHeight,     g.uvMinX, g.uvMaxY, color, atlasIndex };

            ctx.indexPtr[0] = base + 0;
            ctx.indexPtr[1] = base + 1;
            ctx.indexPtr[2] = base + 2;
            ctx.indexPtr[3] = base + 0;
            ctx.indexPtr[4] = base + 2;
            ctx.indexPtr[5] = base + 3;

            ctx.vertexPtr += 4;
            ctx.indexPtr += 6;
            ctx.vertexCount += 4;
            ctx.indexCount += 6;
            cursorX += (float)g.advanceX * scale;
        }
    };

    auto textBaselineY = [&](float y, float h, float scale) {
        auto glyphIt = glyphLookup.find(U'M');
        if (glyphIt == glyphLookup.end()) return y + h * 0.7f;

        const Glyph& g = glyphIt->second;
        return y + h * 0.5f + (float)g.bearingY * scale - (float)g.height * scale * 0.5f;
    };

    // ENGINEERING / PROJECT TABs
    // Action ids for engineering thread control (UI -> engineering)
    constexpr uint32_t ACTION_ENGINEERING_CLOSE = 0xE0000001u;
    constexpr uint32_t ACTION_ENGINEERING_CREATE = 0xE0000002u;

    float currentX = 0.0f;

    uint16_t tabCount = publishedTabCount.load(std::memory_order_acquire);
    uint16_t* tabList = publishedTabIndexes.load(std::memory_order_acquire);

    // Only tabs hosted by this window appear in its tab band. Extraction / drag-drop merge simply
    // retarget DATASETTAB::hostWindowSlot.
    uint16_t hostedTabs[MV_MAX_TABS];
    uint16_t hostedCount = 0;
    for (uint16_t i = 0; i < tabCount; ++i) {
        if (allTabs[tabList[i]].hostWindowSlot.load(std::memory_order_acquire) == windowSlot) {
            hostedTabs[hostedCount++] = tabList[i];
        }
    }
    tabCount = hostedCount;
    tabList = hostedTabs;

    if (canPushRect()) {
        PushRect(ctx, 0.0f, 0.0f, 5000.0f, topUITotalHeightPx, uiActiveColors.actionGroupBackground, uiRes);//
        incrementVertexIndexCounters();
    }

    if (canPushRect()) {
        PushRect(ctx, 0.0f, 0.0f, 5000.0f, tabBarHeightPx, uiActiveColors.tabBackground, uiRes);//
        incrementVertexIndexCounters();
    }
    // We will allow tabs to shrink progressively when too many tabs exist.
    // Compute sizing constraints
    const float defaultTabWidth = 160.0f; // legacy fixed width in pixels
    const float plusButtonWidth = buttonHeightPx; // reserve square area for '+'
    const float minTabWidth = std::max(4.0f * pixelsPerMMx, 8.0f); // 4mm minimum as requested, but at least 8px

    // Determine how many slots we need to fit: tabs + one slot for '+' button
    uint16_t slotsNeeded = tabCount + 1;
    float availableForTabs = std::max(0.0f, W - plusButtonWidth);

    float tentativeWidth = availableForTabs / (float)slotsNeeded;
    float tabWidthPx = defaultTabWidth;
    uint16_t visibleTabs = tabCount;

    if (tentativeWidth >= defaultTabWidth) {
        tabWidthPx = defaultTabWidth;
    } else if (tentativeWidth >= minTabWidth) {
        tabWidthPx = tentativeWidth;
    } else {
        // If tentative width is below minimum, we must hide some tabs.
        visibleTabs = (uint16_t)std::floor(availableForTabs / minTabWidth);
        if (visibleTabs > tabCount) visibleTabs = tabCount;
        tabWidthPx = minTabWidth;
    }

    // Render visible tabs only; hidden tabs are not drawn (will be handled by horizontal scroll in future)
    // Gap between tabs: 0.5 mm on either side
    float gapPx = 0.5f * pixelsPerMMx;
    for (uint16_t i = 0; i < visibleTabs; i++) {
        uint16_t tabID = tabList[i];
        bool isActive = (window.activeTabIndex == tabID);

        // area for this tab (slot)
        float tabX = currentX;
        float tabW = tabWidthPx;

        // content area inset by half-mm gaps on either side
        float contentX = tabX + gapPx;
        float contentW = std::max(0.0f, tabW - 2.0f * gapPx);

        // X (close) button sizing — square inside tab on the right
        float xBtnSize = std::round(std::min(tabW * 0.5f, std::max( (float)std::round(UI_ICON_SIZE_MM * pixelsPerMMx), 10.0f)));
        if (xBtnSize + 4.0f > tabW) xBtnSize = std::max(4.0f, tabW - 4.0f);
        float xBtnX = tabX + tabW - xBtnSize - 4.0f;
        float xBtnY = std::floor((tabBarHeightPx - xBtnSize) * 0.5f);

        // Entire tab background — draw only inside content area leaving gaps between tabs
        if (isActive) {
            PushTopRoundedRectangle(ctx, contentX, 0.0f, contentW, tabBarHeightPx, roundedCornerRadiusPx, uiActiveColors.actionGroupBackground, uiRes);
        } else {
            bool pushed = canPushRect();
            PushRect(ctx, contentX, 0.0f, contentW, tabBarHeightPx, uiActiveColors.tabBackground, uiRes);
            if (pushed) incrementVertexIndexCounters();
        }

        // Check clicks on the X button first to avoid activating the tab when user intends to close
        bool xHovered = input.mouseX >= xBtnX && input.mouseX < xBtnX + xBtnSize &&
            input.mouseY >= xBtnY && input.mouseY < xBtnY + xBtnSize;
        if (xHovered && input.leftButtonPressedThisFrame) {
            // Signal close intent to engineering thread. Pass tabID in parameter p1.
            PushUIAction(ACTION_ENGINEERING_CLOSE, (uint32_t)tabID, 0);
        }

        // If user clicked on non-X area of tab, activate it
        bool tabHovered = input.mouseX >= tabX && input.mouseX < tabX + tabW &&
            input.mouseY >= 0 && input.mouseY < tabBarHeightPx;
        if (!xHovered && tabHovered && input.leftButtonPressedThisFrame) {
            window.activeTabIndex = tabID; // Render thread will draw this tab's geometry on the next frame.
            window.pressedTabId = tabID;   // Candidate for drag-out extraction while the button stays down.
        }

        // Draw the X button: only draw rounded background when hovered, otherwise render as plain text
        char32_t xChar[2] = { U'x', U'\0' };
        if (xHovered) {
            PushRoundedRectangle(ctx, xBtnX, xBtnY, xBtnSize, xBtnSize, std::max(1.0f, roundedCornerRadiusPx * 0.6f),
                0xFF444444, uiRes);
            pushTextClipped(xBtnX + 2.0f, textBaselineY(xBtnY, xBtnSize, uiTextScale), xChar, xBtnSize - 4.0f, 0xFFFFFFFF, uiTextScale);
        } else {
            // Render as plain small text matching tab text color
            pushTextClipped(xBtnX + 2.0f, textBaselineY(xBtnY, xBtnSize, uiTextScale), xChar, xBtnSize - 4.0f, uiActiveColors.tabBackgroundText, uiTextScale);
        }

        // Draw label clipped to remaining area (avoid overlapping with X)
        std::u32string tabLabel;
        tabLabel.reserve(allTabs[tabID].fileName.size());
        for (wchar_t ch : allTabs[tabID].fileName) {
            if (ch <= 0x7F) tabLabel.push_back(static_cast<char32_t>(ch));
        }

        float labelMaxWidth = contentW - (8.0f + xBtnSize + 4.0f);
        pushTextClipped(contentX + 8.0f, textBaselineY(0.0f, tabBarHeightPx, uiTextScale), tabLabel.c_str(), labelMaxWidth, uiActiveColors.tabBackgroundText, uiTextScale);

        currentX += tabW;

        // Draw 1px vertical separator centered in the gap between tabs (only between tabs)
        if (i + 1 < visibleTabs) {
            float sepX = tabX + tabW; // center of gap between this tab and next
            // align to pixel for crispness
            float sepXi = std::floor(sepX + 0.5f);
            pushRect(sepXi, 2.0f, 1.0f, tabBarHeightPx - 4.0f, uiActiveColors.actionGroupSeperator);
        }
    }

    // If some tabs are hidden, we may draw a subtle indicator (ellipsis) — skip for now

    // Dragging a tab button downward out of the band extracts the tab into its own window
    // (with the full ribbon). The last hosted tab of a window is not extractable.
    if (input.leftButtonReleasedThisFrame || !input.leftButtonDown) {
        window.pressedTabId = -1;
    } else if (window.pressedTabId >= 0 && input.mouseY > tabBarHeightPx * 2.5f) {
        if (tabCount > 1) {
            PushUIAction(kExtractTabUIAction, static_cast<uint32_t>(window.pressedTabId), 0);
        }
        window.pressedTabId = -1;
    }

    // Render '+' create new thread button at the end of tab bar
    float plusX = currentX + 6.0f; // small padding before plus
    float plusSize = std::max(plusButtonWidth, std::round(UI_ICON_SIZE_MM * pixelsPerMMy) + 8.0f);
    bool plusHovered = input.mouseX >= plusX && input.mouseX < plusX + plusSize &&
        input.mouseY >= (tabBarHeightPx - plusSize) * 0.5f && input.mouseY < (tabBarHeightPx - plusSize) * 0.5f + plusSize;
    // '+' button: show rounded background only on hover; otherwise render as plain icon/text
    if (plusHovered) {
        PushRoundedRectangle(ctx, plusX, (tabBarHeightPx - plusSize) * 0.5f, plusSize, plusSize, roundedCornerRadiusPx,
            0xFF444444, uiRes);
        if (ctx.iconData && !ctx.iconData->metadata.mixedIconCodepoints.empty()) {
            PushIcon(ctx, plusX + (plusSize - iconSizePx) * 0.5f, (tabBarHeightPx - iconSizePx) * 0.5f,
                iconSizePx, iconSizePx, ctx.iconData->metadata.mixedIconCodepoints[0], 0xFFFFFFFF, uiRes);
        }
    } else {
        if (ctx.iconData && !ctx.iconData->metadata.mixedIconCodepoints.empty()) {
            PushIcon(ctx, plusX + (plusSize - iconSizePx) * 0.5f, (tabBarHeightPx - iconSizePx) * 0.5f,
                iconSizePx, iconSizePx, ctx.iconData->metadata.mixedIconCodepoints[0], uiActiveColors.tabBackgroundText, uiRes);
        }
    }
    if (plusHovered && input.leftButtonPressedThisFrame) {
        // p1 = window slot, so the new tab is hosted by the window whose '+' was clicked.
        PushUIAction(ACTION_ENGINEERING_CREATE, static_cast<uint64_t>(windowSlot), 0);
    }

    // TOP BUTTONS (ACTION GROUP BAR)
    const float buttonGap = topRibbonLayout.buttonGapPx;
    const float actionGroupLabelY = topRibbonLayout.actionGroupLabelY;
	const float groupLabelHeight = topRibbonLayout.actionGroupLabelHeightPx;
    const float topActionGroupY = topRibbonLayout.topActionGroupY;
    const float actionSubGroupLabelY = topRibbonLayout.actionSubGroupLabelY;
    const float ribbonScrollX = topRibbonLayout.scrollOffsetPx;

    // Draw the 5-pixel high extent-of-ribbon-visible visualization bar in the 5px gap below Action Group labels.
    // The gap starts at topActionGroupY - 5.0f.
    float extentX = MapRibbonToNav(topRibbonLayout.scrollOffsetPx, topRibbonLayout);
    float extentRight = MapRibbonToNav(topRibbonLayout.scrollOffsetPx + W, topRibbonLayout);
    float extentW = std::max(1.0f, extentRight - extentX);
    // Draw active indicator (orange)
    pushRect(extentX, topActionGroupY - 5.0f, extentW, 5.0f, 0xFF3399FF);

    for (size_t groupIndex = 0; groupIndex < TotalTopUIActionGroups; ++groupIndex) {
        const UIActionGroupNames& group = topUIActionGroupNames[groupIndex];
        const UITopRibbonActionGroupLayout& groupLayout = topRibbonLayout.actionGroups[groupIndex];
        const char32_t* label = LocalizedUIString(group.labelStringID);
        const bool hovered = group.isEnabled &&
            input.mouseX >= groupLayout.navX && input.mouseX < groupLayout.navX + groupLayout.navWidth &&
            input.mouseY >= actionGroupLabelY && input.mouseY < actionGroupLabelY + groupLabelHeight;

        if (hovered) {
            pushRect(groupLayout.navX, actionGroupLabelY, groupLayout.navWidth, groupLabelHeight,
                uiActiveColors.tabBackgroundHover);
        }
        if (hovered && input.leftButtonPressedThisFrame) {
            topRibbonLayout.scrollOffsetPx = groupLayout.contentStartX;
            ClampTopRibbonScroll(topRibbonLayout, W);
        }

        pushTextClipped(groupLayout.navX + 4.0f, textBaselineY(actionGroupLabelY, groupLabelHeight, uiTextScale),
            label, groupLayout.navWidth - 8.0f, uiActiveColors.actionText, uiTextScale);
    }

    for (size_t runIndex = 0; runIndex < topRibbonLayout.actionSubGroupRunCount; ++runIndex) {
        const UITopRibbonSubGroupRunLayout& run = topRibbonLayout.actionSubGroupRuns[runIndex];
        if (run.subGroupIndex >= TotalTopUIActionSubGroups) continue;

        const UIActionGroupNames& subGroup = topUIActionSubGroupNames[run.subGroupIndex];
        const char32_t* label = LocalizedUIString(subGroup.labelStringID);
        const float runX = run.contentStartX - ribbonScrollX;
        const float runWidth = std::max(0.0f, run.contentEndX - run.contentStartX);
        const float labelWidth = MeasureUIStringWidth(label, uiTextScale);
        const float labelX = runX + std::max(4.0f, (runWidth - labelWidth) * 0.5f);

        pushTextClipped(labelX, textBaselineY(actionSubGroupLabelY, groupLabelHeight, uiTextScale),
            label, std::max(0.0f, runWidth - 8.0f), uiActiveColors.actionText, uiTextScale);

        if (runIndex + 1 < topRibbonLayout.actionSubGroupRunCount) {
            const float lineX = std::floor(run.contentEndX + buttonGap * 0.5f - ribbonScrollX);
            const float lineHeight = 3.0f * topRibbonLayout.buttonHeightPx + 2.0f;
            if (lineX >= -1.0f && lineX <= W + 1.0f) {
                pushRect(lineX, topActionGroupY, 1.0f, lineHeight, 0xFF555555);
            }
        }
    }

    for (size_t i = 0; i < TotalUIControls; ++i) {
        const auto& ctrl = AllUIControls[i];
        const UITopRibbonControlLayout& ctrlLayout = topRibbonLayout.controls[i];
        const float btnX = ctrlLayout.x - ribbonScrollX;
        const float btnY = ctrlLayout.y;
        const float btnWidth = ctrlLayout.width;
        const float btnHeight = ctrlLayout.height;
        const char32_t* label = LocalizedControlLabel(ctrl);
        uint32_t baseColor = StableRandomUIColour((uint32_t)ctrl.action ^ ((uint32_t)i * 0x9E3779B9u));// Render
        uint32_t iconColor = StableRandomUIColour(((uint32_t)ctrl.action << 1) ^ 0xA511E9B3u ^ (uint32_t)i);
        char32_t resolvedIconChar = ctrl.iconChar;
        const uint32_t actionIconID = static_cast<uint32_t>(ctrl.action);
        if (resolvedIconChar == SVGIconRenderer::NoIcon && SVGIconRenderer::HasEmbeddedSVGIcon(actionIconID)) {
            resolvedIconChar = SVGIconRenderer::IconForID(actionIconID);
        }

        const uint32_t iconID = static_cast<uint32_t>(resolvedIconChar);
        const bool hasDedicatedSVGIcon =
            resolvedIconChar != SVGIconRenderer::NoIcon &&
            SVGIconRenderer::HasEmbeddedSVGIcon(iconID) &&
            ctx.iconData &&
            ctx.iconData->iconGlyphLookup.find(resolvedIconChar) != ctx.iconData->iconGlyphLookup.end();
        const bool controlVisible = btnX + btnWidth >= 0.0f && btnX <= W;
        bool hovered = false;

        if (ctrl.type == 1 || ctrl.type == 2) {                     // Button or Dropdown trigger
            hovered = controlVisible && ctrl.isEnabled && (input.mouseX >= btnX && input.mouseX < btnX + btnWidth &&
                input.mouseY >= btnY && input.mouseY < btnY + btnHeight);
            uint32_t drawColor = hovered && input.leftButtonDown ? 0xFF333333 : baseColor;
            if (hovered && !input.leftButtonDown) drawColor = 0xFF555555;
            if (controlVisible) {
                if (hovered) {
                    PushRoundedRectangle(ctx, btnX, btnY, btnWidth, btnHeight, roundedCornerRadiusPx,
                        drawColor, uiRes);
                } else if (!hasDedicatedSVGIcon) {
                    float highlightWidth = ctrl.showText ? iconReservedWidthPx : btnWidth;
                    PushRoundedRectangle(ctx, btnX, btnY, highlightWidth, btnHeight, roundedCornerRadiusPx,
                        baseColor, uiRes);
                }
            }

            bool clicked = hovered && input.leftButtonPressedThisFrame;

            if (clicked && ctrl.isEnabled) {
                ImprovementData::RecordRibbonAction((uint32_t)ctrl.action); // Usage statistics.
                PushUIAction((uint32_t)ctrl.action);
                if (ctrl.zIndex == 1) { // Dropdown trigger
                    window.activeDropdownAction = ctrl.action;
                }
                if (ctrl.action == Commands::INSERT_ASSET2D) {
                    // Companion right-side pane for picking the asset; the engineering thread
                    // arms asset-insert mode through the pushed action above.
                    window.assetInsertPaneOpen = true;
                    window.rightPaneOpen = false; // The two panes share the right-side slot.
                }
            }

        }
        else if (ctrl.type == 3) {
            // Future textbox
            if (controlVisible) {
                PushRoundedRectangle(ctx, btnX, btnY, btnWidth, btnHeight, roundedCornerRadiusPx,
                    0xFF1E1E1E, uiRes);
            }
        }
        else {
            // Plain label
            if (controlVisible) {
                PushRoundedRectangle(ctx, btnX, btnY, btnWidth, btnHeight, roundedCornerRadiusPx,
                    0xFF2D2D30, uiRes);
            }
        }

        if (!controlVisible) continue;

        float iconX = btnX + (iconReservedWidthPx - iconSizePx) * 0.5f;
        float iconY = btnY + (btnHeight - iconSizePx) * 0.5f;
        if (hasDedicatedSVGIcon) {
            const uint32_t dedicatedIconColor = ctrl.isEnabled ? 0xFFFFFFFF : uiActiveColors.actionIconDisabled;
            PushIcon(ctx, iconX, iconY, iconSizePx, iconSizePx, resolvedIconChar, dedicatedIconColor, uiRes);
        }
        else if (ctx.iconData && !ctx.iconData->metadata.mixedIconCodepoints.empty()) {
            const uint32_t randomIconIndex =
                ((uint32_t)ctrl.action ^ (uint32_t)i) % (uint32_t)ctx.iconData->metadata.mixedIconCodepoints.size();
            PushIcon(ctx, iconX, iconY, iconSizePx, iconSizePx,
                ctx.iconData->metadata.mixedIconCodepoints[randomIconIndex], iconColor, uiRes);
        }

        if (ctrl.showText) {
            float textX = btnX + textStartOffsetPx;
            float textWidth = btnWidth - textStartOffsetPx - textEndInsetPx;
            uint32_t textColor = 0xFFFFFFFF; // default hovered/active color (white)
            if (!hovered) {
                textColor = ctrl.isEnabled ? uiActiveColors.actionText : kUIDisabledTextGray;
            }
            pushTextClipped(textX, textBaselineY(btnY, btnHeight, uiTextScale),
                label, textWidth, textColor, uiTextScale);
        }
    }

    const int activeTabIndex = window.activeTabIndex;

    // INTERNAL HIGH-LEVEL CONTAINER TABS (Scene3D, Page2D, and future container types).
    // Rendered from the active tab's published fixed-slot sub-tab list (lock-free).
    const float internalBarY = topRibbonLayout.internalTabBarY;
    const float internalBarHeight = topRibbonLayout.internalTabBarHeightPx;
    if (internalBarHeight > 0.0f) {
        pushRect(0.0f, internalBarY, W, internalBarHeight, uiActiveColors.tabBackground);

        uint16_t* subTabList = nullptr;
        uint16_t subTabListCount = 0;
        DATASETTAB* activeTabData = nullptr;
        if (activeTabIndex >= 0 && activeTabIndex < MV_MAX_TABS) {
            activeTabData = &allTabs[activeTabIndex];
            subTabList = activeTabData->publishedSubTabIndexes.load(std::memory_order_acquire);
            subTabListCount = activeTabData->publishedSubTabCount.load(std::memory_order_acquire);
        }

        const size_t internalTabCount = subTabList ? subTabListCount : 0;
        if (internalTabCount > 0) {
            const float defaultInternalTabWidth = 180.0f;
            const float minimumInternalTabWidth = std::max(4.0f * pixelsPerMMx, 8.0f);
            const float internalGapPx = 0.5f * pixelsPerMMx;
            const float availableWidth = std::max(0.0f, W);
            float internalTabWidth = std::min(defaultInternalTabWidth,
                availableWidth / static_cast<float>(internalTabCount));
            internalTabWidth = std::max(minimumInternalTabWidth, internalTabWidth);
            size_t visibleInternalTabs = std::min(internalTabCount,
                static_cast<size_t>(std::floor(availableWidth / internalTabWidth)));

            float internalX = 0.0f;
            for (size_t i = 0; i < visibleInternalTabs; ++i) {
                const uint16_t subTabSlot = subTabList[i];
                const InternalSubTab& subTab = activeTabData->subTabs[subTabSlot];
                const bool isExtracted =
                    activeTabData->subTabHostWindowSlots[subTabSlot].load(std::memory_order_acquire) >= 0;
                const bool isActive = subTab.containerMemoryId == activeInternalSubTabMemoryId;
                const float tabX = internalX;
                const float contentX = tabX + internalGapPx;
                const float contentWidth = std::max(0.0f, internalTabWidth - 2.0f * internalGapPx);
                const float closeSize = std::max(8.0f, internalBarHeight * 0.62f);
                const float closeX = contentX + contentWidth - closeSize - 2.0f;
                const float closeY = internalBarY + (internalBarHeight - closeSize) * 0.5f;

                const bool tabHovered =
                    input.mouseX >= tabX && input.mouseX < tabX + internalTabWidth &&
                    input.mouseY >= internalBarY && input.mouseY < internalBarY + internalBarHeight;
                const bool closeHovered =
                    input.mouseX >= closeX && input.mouseX < closeX + closeSize &&
                    input.mouseY >= closeY && input.mouseY < closeY + closeSize;

                uint32_t tabColor = isActive ? uiActiveColors.tabActive : uiActiveColors.tabBackground;
                if (!isActive && tabHovered) tabColor = uiActiveColors.tabBackgroundHover;
                PushTopRoundedRectangle(ctx, contentX, internalBarY, contentWidth, internalBarHeight,
                    roundedCornerRadiusPx, tabColor, uiRes);

                if (input.leftButtonPressedThisFrame) {
                    if (closeHovered) {
                        PushUIAction(InternalSubTabs::kCloseUIAction,
                            static_cast<uint32_t>(activeTabIndex), subTab.containerMemoryId);
                    } else if (tabHovered) {
                        PushUIAction(InternalSubTabs::kActivateUIAction,
                            static_cast<uint32_t>(activeTabIndex), subTab.containerMemoryId);
                        window.pressedSubTabSlot = subTabSlot; // Drag-out extraction candidate.
                    }
                }

                std::u32string title = DataTreeView::AsciiToDisplayText(subTab.title.c_str());
                // Extracted views keep their button in the band, drawn in the accent color; clicking
                // an extracted Page2D focuses its dedicated window instead of activating inline.
                const uint32_t textColor = isExtracted ? dataTreeActiveColor
                    : (isActive ? uiActiveColors.tabActiveText : uiActiveColors.tabBackgroundText);
                pushTextClipped(contentX + 6.0f,
                    textBaselineY(internalBarY, internalBarHeight, uiTextScale),
                    title.c_str(), std::max(0.0f, closeX - contentX - 10.0f),
                    textColor, uiTextScale, isActive);

                const char32_t closeText[2] = { U'x', U'\0' };
                if (closeHovered) {
                    PushRoundedRectangle(ctx, closeX, closeY, closeSize, closeSize,
                        std::max(1.0f, roundedCornerRadiusPx * 0.6f), 0xFF444444, uiRes);
                }
                pushTextClipped(closeX + closeSize * 0.28f,
                    textBaselineY(closeY, closeSize, uiTextScale), closeText,
                    closeSize * 0.55f, textColor, uiTextScale);

                internalX += internalTabWidth;
            }
        }

        // Dragging a sub-tab button downward out of the band extracts that view into its own
        // content-only window (or focuses the existing one when already extracted).
        if (input.leftButtonReleasedThisFrame || !input.leftButtonDown) {
            window.pressedSubTabSlot = -1;
        } else if (window.pressedSubTabSlot >= 0 && activeTabData &&
            input.mouseY > internalBarY + internalBarHeight * 2.5f) {
            const uint16_t slot = static_cast<uint16_t>(window.pressedSubTabSlot);
            if (slot < MV_MAX_SUBTABS &&
                activeTabData->subTabStates[slot].load(std::memory_order_acquire) == SUBTAB_OPEN) {
                PushUIAction(InternalSubTabs::kExtractUIAction,
                    static_cast<uint32_t>(activeTabIndex), activeTabData->subTabs[slot].containerMemoryId);
            }
            window.pressedSubTabSlot = -1;
        }
    }

    if (activeTabIndex >= 0 && activeTabIndex < MV_MAX_TABS) {
        DATASETTAB& tab = allTabs[activeTabIndex];
        DataTreeView::StateSnapshot dataTreeState = DataTreeView::Snapshot(tab.dataTreeView);

        if (dataTreeState.isVisible) {
            DataTreeView::BuildRequest treeRequest;
            treeRequest.tabName = tab.fileName;
            treeRequest.activeBranchObjectId = 0;
            treeRequest.viewportTopPx = topUITotalHeightPx;
            treeRequest.viewportHeightPx = std::max(0.0f, H - topUITotalHeightPx);
            treeRequest.pixelsPerMMX = pixelsPerMMx;
            treeRequest.pixelsPerMMY = pixelsPerMMy;

            const DataTreeView::LayoutMetrics treeLayout = DataTreeView::CalculateLayout(treeRequest);
            const bool treeHovered =
                DataTreeView::ContainsPoint(treeLayout, input.mouseX, input.mouseY);
            if (treeHovered && input.mouseWheelDelta != 0) {
                const float wheelSteps = input.mouseWheelDelta / static_cast<float>(WHEEL_DELTA);
                int64_t rowDelta = static_cast<int64_t>(std::llround(-wheelSteps * 3.0f));
                if (rowDelta == 0) rowDelta = input.mouseWheelDelta > 0 ? -1 : 1;
                DataTreeView::ScrollRows(tab.dataTreeView, rowDelta);
                dataTreeState = DataTreeView::Snapshot(tab.dataTreeView);
            }

            std::vector<DataTreeView::Node> treeNodes;
            std::vector<uint64_t> expandedNodeIds;
            uint64_t activeBranchObjectId = 0;
            if (tab.storageObjectsMutex) {
                std::lock_guard<std::mutex> lock(*tab.storageObjectsMutex);
                treeNodes.reserve(tab.storageLogicalObjects.size() + tab.storageObjects3D.size());
                expandedNodeIds = tab.expandedDataTreeNodeIds;
                if (activeInternalSubTabType == VishwakarmaStorage::ObjectType::Page2D) {
                    activeBranchObjectId = activeInternalSubTabMemoryId;
                } else {
                    activeBranchObjectId = tab.activeScene3DMemoryId;
                    if (activeBranchObjectId == 0 &&
                        activeInternalSubTabType == VishwakarmaStorage::ObjectType::Scene3D) {
                        activeBranchObjectId = activeInternalSubTabMemoryId;
                    }
                }

                for (const StoredLogicalObject& object : tab.storageLogicalObjects) {
                    if (!object.object) continue;
                    DataTreeView::Node node;
                    node.objectId = object.object->memoryID;
                    node.parentObjectId = object.object->memoryIDParent;
                    node.label = BuildTreeNodeLabel(object.objectType, object.object, object.memoryId);
                    node.canBecomeActiveBranch =
                        object.objectType == VishwakarmaStorage::ObjectType::Scene3D ||
                        object.objectType == VishwakarmaStorage::ObjectType::Page2D;
                    treeNodes.push_back(std::move(node));
                }

                for (const StoredGeometryObject3D& object : tab.storageObjects3D) {
                    if (!object.object) continue;
                    DataTreeView::Node node;
                    node.objectId = object.object->memoryID;
                    node.parentObjectId = object.object->memoryIDParent;
                    node.label = BuildTreeNodeLabel(object.objectType, object.object, object.memoryId);
                    treeNodes.push_back(std::move(node));
                }
            }

            treeRequest.nodes = &treeNodes;
            treeRequest.expandedNodeIds = &expandedNodeIds;
            treeRequest.activeBranchObjectId = activeBranchObjectId;

            DataTreeView::BuildResult treeResult =
                DataTreeView::BuildRows(treeRequest, dataTreeState);
            if (treeResult.firstVisibleRow != dataTreeState.firstVisibleRow) {
                DataTreeView::SetFirstVisibleRow(
                    tab.dataTreeView, static_cast<uint64_t>(treeResult.firstVisibleRow));
                dataTreeState.firstVisibleRow = treeResult.firstVisibleRow;
            }

            bool scrollbarDragging =
                tab.dataTreeView.scrollbarDragging.load(std::memory_order_acquire);
            if (input.leftButtonReleasedThisFrame || !input.leftButtonDown ||
                !treeResult.scrollbar.isScrollable) {
                scrollbarDragging = false;
                tab.dataTreeView.scrollbarDragging.store(false, std::memory_order_release);
            }

            const bool scrollbarTrackHovered = DataTreeView::HitTestScrollbarTrack(
                treeResult.scrollbar, input.mouseX, input.mouseY);
            const bool scrollbarThumbHovered = DataTreeView::HitTestScrollbarThumb(
                treeResult.scrollbar, input.mouseX, input.mouseY);

            if (input.leftButtonPressedThisFrame && scrollbarTrackHovered &&
                treeResult.scrollbar.isScrollable) {
                float grabOffsetPx = scrollbarThumbHovered
                    ? input.mouseY - treeResult.scrollbar.thumbY
                    : treeResult.scrollbar.thumbHeight * 0.5f;
                tab.dataTreeView.scrollbarDragGrabOffsetPx.store(
                    grabOffsetPx, std::memory_order_release);
                tab.dataTreeView.scrollbarDragging.store(true, std::memory_order_release);
                scrollbarDragging = true;

                const uint64_t targetRow = DataTreeView::ScrollbarRowForMouseY(
                    treeResult, input.mouseY, grabOffsetPx);
                DataTreeView::SetFirstVisibleRow(tab.dataTreeView, targetRow);
                dataTreeState = DataTreeView::Snapshot(tab.dataTreeView);
                treeResult = DataTreeView::BuildRows(treeRequest, dataTreeState);
            } else if (scrollbarDragging && input.leftButtonDown) {
                const float grabOffsetPx =
                    tab.dataTreeView.scrollbarDragGrabOffsetPx.load(std::memory_order_acquire);
                const uint64_t targetRow = DataTreeView::ScrollbarRowForMouseY(
                    treeResult, input.mouseY, grabOffsetPx);
                if (targetRow != dataTreeState.firstVisibleRow) {
                    DataTreeView::SetFirstVisibleRow(tab.dataTreeView, targetRow);
                    dataTreeState = DataTreeView::Snapshot(tab.dataTreeView);
                    treeResult = DataTreeView::BuildRows(treeRequest, dataTreeState);
                }
            }

            const uint32_t scrollbarTrackColor = 0x66333333;
            const uint32_t scrollbarThumbColor = scrollbarDragging
                ? 0xFF3399FF
                : (scrollbarThumbHovered ? 0xFF5CB4FF : 0xCC8A8A8A);
            pushRect(treeResult.scrollbar.trackX, treeResult.scrollbar.trackY,
                treeResult.scrollbar.trackWidth, treeResult.scrollbar.trackHeight,
                scrollbarTrackColor);
            pushRect(treeResult.scrollbar.thumbX, treeResult.scrollbar.thumbY,
                treeResult.scrollbar.thumbWidth, treeResult.scrollbar.thumbHeight,
                scrollbarThumbColor);

            const std::vector<DataTreeView::Row>& treeRows = treeResult.rows;
            uint64_t hoveredToggleObjectId = 0;
            const bool toggleHovered =
                DataTreeView::HitTestToggle(treeRows, input.mouseX, input.mouseY, hoveredToggleObjectId);
            uint64_t hoveredBranchObjectId = 0;
            const bool branchHovered = !toggleHovered &&
                DataTreeView::HitTestActiveBranch(
                    treeRows, input.mouseX, input.mouseY, hoveredBranchObjectId);

            if (input.leftButtonPressedThisFrame) {
                if (toggleHovered) {
                    if (hoveredToggleObjectId == 0) {
                        PushUIAction(DataTreeView::kToggleEverythingUIAction, static_cast<uint32_t>(activeTabIndex), 0);
                    } else {
                        PushUIAction(DataTreeView::kToggleNodeUIAction,
                            static_cast<uint32_t>(activeTabIndex), hoveredToggleObjectId);
                    }
                } else if (branchHovered) {
                    PushUIAction(DataTreeView::kSetActiveBranchUIAction,
                        static_cast<uint32_t>(activeTabIndex), hoveredBranchObjectId);
                    if (input.leftButtonDoubleClickedThisFrame) {
                        PushUIAction(InternalSubTabs::kOpenUIAction,
                            static_cast<uint32_t>(activeTabIndex), hoveredBranchObjectId);
                    }
                }
            }

            for (const DataTreeView::Row& row : treeRows) {
                const bool rowToggleHovered = toggleHovered && row.objectId == hoveredToggleObjectId;
                const bool rowBranchHovered = branchHovered && row.objectId == hoveredBranchObjectId;
                const uint32_t labelColor = row.isActiveBranch
                    ? dataTreeActiveColor
                    : (rowBranchHovered ? dataTreeHoverColor : dataTreeTextColor);
                const uint32_t toggleColor = row.isActiveBranch
                    ? dataTreeActiveColor
                    : (rowToggleHovered ? dataTreeHoverColor : dataTreeTextColor);
                const float baselineY = textBaselineY(row.y, row.height, uiTextScale);

                if (row.hasToggle) {
                    const char32_t toggleText[2] = { row.isExpanded ? U'-' : U'+', U'\0' };
                    pushTextClipped(row.toggleX, baselineY, toggleText, row.toggleWidth,
                        toggleColor, uiTextScale);
                }

                pushTextClipped(row.textX, baselineY, row.label.c_str(), row.textMaxWidth,
                    labelColor, uiTextScale, row.isActiveBranch);
            }
        }
    }

    // ---- Right icon bar + object properties pane (website/content/software/propertiesPane.md) ----
    {
        const float rightIconBarWidthPx = std::round(UI_RIGHT_ICONBAR_WIDTH_MM * pixelsPerMMx);
        const float rightPaneWidthPx = std::round(UI_RIGHT_PANE_WIDTH_MM * pixelsPerMMx);
        const float overlayTop = topUITotalHeightPx;
        const float overlayHeight = std::max(0.0f, H - overlayTop);
        const float iconBarX = W - rightIconBarWidthPx;

        // Icon bar background + left separator.
        pushRect(iconBarX, overlayTop, rightIconBarWidthPx, overlayHeight, uiActiveColors.actionGroupBackground);
        pushRect(iconBarX, overlayTop, 1.0f, overlayHeight, uiActiveColors.actionGroupSeperator);

        // Properties toggle button (square, at the top of the bar). Toggles UI-only state directly.
        const float btnSize = rightIconBarWidthPx;
        const bool btnHovered = input.mouseX >= iconBarX && input.mouseX < iconBarX + btnSize &&
            input.mouseY >= overlayTop && input.mouseY < overlayTop + btnSize;
        uint32_t btnColor = window.rightPaneOpen ? dataTreeActiveColor : uiActiveColors.actionGroupBackground;
        if (btnHovered) btnColor = uiActiveColors.actionGroupHoverBackground;
        pushRect(iconBarX, overlayTop, btnSize, btnSize, btnColor);
        const float iconInset = std::max(1.0f, std::round(1.0f * pixelsPerMMx));
        PushIcon(ctx, iconBarX + iconInset, overlayTop + iconInset, btnSize - 2.0f * iconInset,
            btnSize - 2.0f * iconInset, UIIconForCommand(Commands::PROPERTIES_PANE), 0xFF333333u, uiRes);
        if (btnHovered && input.leftButtonPressedThisFrame) {
            window.rightPaneOpen = !window.rightPaneOpen;
            window.assetInsertPaneOpen = false; // The two panes share the right-side slot.
            ImprovementData::RecordRibbonAction(static_cast<uint32_t>(Commands::PROPERTIES_PANE));
        }

        UITextEditState& edit = window.textEditState;
        float paneWidthPx = 0.0f;

        if (window.assetInsertPaneOpen) {
            edit.focusedFieldKey = 0; // No property edit while the Insert Asset pane is shown.
            paneWidthPx = rightPaneWidthPx;
            const float paneX = W - rightIconBarWidthPx - rightPaneWidthPx;
            pushRect(paneX, overlayTop, rightPaneWidthPx, overlayHeight, uiActiveColors.actionGroupBackground);
            pushRect(paneX, overlayTop, 1.0f, overlayHeight, uiActiveColors.actionGroupSeperator);

            const float pad = std::round(2.0f * pixelsPerMMx);
            const float rowH = buttonHeightPx;
            const float rowGap = std::max(1.0f, std::round(0.6f * pixelsPerMMy));
            float rowY = overlayTop + pad;

            pushTextClipped(paneX + pad, textBaselineY(rowY, rowH, uiTextScale),
                LocalizedUIString(UITextID::INSERT_ASSET2D), rightPaneWidthPx - 2.0f * pad,
                uiActiveColors.actionText, uiTextScale);
            rowY += rowH + rowGap;

            // Snapshot the active tab's asset definitions: their numeric ids fill the dropdown.
            constexpr int kMaxAssetItems = 64;
            char itemText[kMaxAssetItems][12];
            const char* itemPtrs[kMaxAssetItems];
            uint64_t itemDefinitionIds[kMaxAssetItems];
            int itemCount = 0;
            uint64_t selectedDefinitionId = 0;
            TabCad2DStorage* cad2d = nullptr;
            if (activeTabIndex >= 0 && activeTabIndex < MV_MAX_TABS) {
                cad2d = allTabs[activeTabIndex].cad2d.get();
            }
            if (cad2d) {
                selectedDefinitionId =
                    cad2d->assetInsertSelectedDefinitionId.load(std::memory_order_acquire);
                std::lock_guard<std::mutex> lock(cad2d->cpuRecordsMutex);
                for (const Cad2DAssetDefinitionRecordCPU& d : cad2d->assetDefinitionRecords) {
                    if (d.isDeleted) continue;
                    if (itemCount >= kMaxAssetItems) break;
                    auto res = std::to_chars(itemText[itemCount], itemText[itemCount] + 11, d.assetNumber);
                    *res.ptr = '\0';
                    itemPtrs[itemCount] = itemText[itemCount];
                    itemDefinitionIds[itemCount] = d.objectId;
                    ++itemCount;
                }
            }

            if (itemCount == 0) {
                pushTextClipped(paneX + pad, textBaselineY(rowY, rowH, uiTextScale),
                    U"No assets defined yet.", rightPaneWidthPx - 2.0f * pad,
                    uiActiveColors.actionText, uiTextScale);
            } else {
                int selectedIndex = 0; // Defaults to the first definition when none is chosen.
                for (int i = 0; i < itemCount; ++i) {
                    if (itemDefinitionIds[i] == selectedDefinitionId) { selectedIndex = i; break; }
                }
                const int newIndex = BuildUIDropdown(ctx, uiRes, input, window.assetInsertDropdown,
                    paneX + pad, rowY, rightPaneWidthPx - 2.0f * pad, rowH, uiTextScale,
                    itemPtrs, itemCount, selectedIndex);
                if (cad2d && newIndex >= 0 && newIndex < itemCount) {
                    cad2d->assetInsertSelectedDefinitionId.store(itemDefinitionIds[newIndex],
                        std::memory_order_release);
                }
            }
        } else if (window.rightPaneOpen) {
            paneWidthPx = rightPaneWidthPx;
            const float paneX = W - rightIconBarWidthPx - rightPaneWidthPx;
            pushRect(paneX, overlayTop, rightPaneWidthPx, overlayHeight, uiActiveColors.actionGroupBackground);
            pushRect(paneX, overlayTop, 1.0f, overlayHeight, uiActiveColors.actionGroupSeperator);

            // Snapshot the selection + copy the selected object's raw fields under the tab locks.
            VishwakarmaStorage::ObjectType selType = VishwakarmaStorage::ObjectType::Unknown;
            uint64_t selId = 0;
            size_t selectionCount = 0;
            const PropertyTypeDescriptor* table = nullptr;
            float fieldValues[16] = {};
            uint8_t fieldValueCount = 0;

            if (activeTabIndex >= 0 && activeTabIndex < MV_MAX_TABS) {
                DATASETTAB& tab = allTabs[activeTabIndex];
                uint64_t singleSelectedId = 0;
                {
                    std::lock_guard<std::mutex> lock(tab.selection.selectedMutex);
                    selectionCount = tab.selection.selectedObjectIds.size();
                    if (selectionCount == 1) singleSelectedId = tab.selection.selectedObjectIds[0];
                }
                if (selectionCount == 1 && tab.storageObjectsMutex) {
                    std::lock_guard<std::mutex> lock(*tab.storageObjectsMutex);
                    for (const StoredGeometryObject3D& stored : tab.storageObjects3D) {
                        if (stored.memoryId == singleSelectedId && stored.object) {
                            selType = stored.objectType;
                            selId = stored.object->memoryID;
                            table = FindPropertyTable(selType);
                            if (table) {
                                fieldValueCount = table->fieldCount;
                                for (uint8_t i = 0; i < fieldValueCount; ++i) {
                                    fieldValues[i] = table->fields[i].get(stored.object);
                                }
                            }
                            break;
                        }
                    }
                }
            }

            // Selection change (different memoryID than the edit started on) cancels any edit.
            if (edit.focusedFieldKey != 0 && edit.editingObjectId != selId) edit.focusedFieldKey = 0;

            // Row layout: label on the left ~24mm, value field on the right ~36mm, 2mm padding.
            const float pad = std::round(2.0f * pixelsPerMMx);
            const float rowH = buttonHeightPx;
            const float rowGap = std::max(1.0f, std::round(0.6f * pixelsPerMMy));
            const float labelW = std::round(24.0f * pixelsPerMMx);
            const float fieldW = std::round(36.0f * pixelsPerMMx);
            const float contentX = paneX + pad;
            const float fieldX = contentX + labelW;
            const float textPad = std::max(1.0f, std::round(0.5f * pixelsPerMMx));
            float rowY = overlayTop + pad;

            auto asciiToU32 = [](const char* s, char32_t* out, size_t cap) {
                size_t i = 0;
                for (; s && s[i] && i + 1 < cap; ++i) out[i] = static_cast<char32_t>(static_cast<unsigned char>(s[i]));
                out[i] = 0;
            };
            auto measureU32 = [&](const char32_t* s, uint8_t len) {
                float w = 0.0f;
                for (uint8_t i = 0; i < len; ++i) {
                    auto it = glyphLookup.find(s[i]);
                    if (it != glyphLookup.end()) w += static_cast<float>(it->second.advanceX) * uiTextScale;
                }
                return w;
            };
            auto drawStaticRow = [&](const char32_t* label, const char* value) {
                pushTextClipped(contentX, textBaselineY(rowY, rowH, uiTextScale), label, labelW,
                    uiActiveColors.actionText, uiTextScale);
                char32_t valueU32[64];
                asciiToU32(value, valueU32, 64);
                pushTextClipped(fieldX, textBaselineY(rowY, rowH, uiTextScale), valueU32, fieldW,
                    uiActiveColors.actionText, uiTextScale);
                rowY += rowH + rowGap;
            };

            if (selectionCount == 1) {
                drawStaticRow(LocalizedUIString(UITextID::PropObjectType),
                    VishwakarmaStorage::ObjectTypeDisplayName(selType));
                char idText[vishwakarma::crockford_base32::kEncodedUInt64LengthWithNull];
                vishwakarma::crockford_base32::EncodeUInt64ToCString(selId, idText);
                drawStaticRow(LocalizedUIString(UITextID::PropObjectId), idText);

                bool fieldClicked = false;
                for (uint8_t i = 0; table && i < fieldValueCount; ++i) {
                    const PropertyFieldDescriptor& fd = table->fields[i];
                    const uint64_t key = (selId << 8) | (static_cast<uint64_t>(i) + 1);
                    bool focused = (edit.focusedFieldKey == key && edit.editingObjectId == selId);

                    const float fy = rowY;
                    const bool fieldHovered = input.mouseX >= fieldX && input.mouseX < fieldX + fieldW &&
                        input.mouseY >= fy && input.mouseY < fy + rowH;

                    // Live value in shortest round-trip form (also used to seed the buffer on focus).
                    char liveText[32];
                    { auto r = std::to_chars(liveText, liveText + sizeof(liveText) - 1, fieldValues[i]);
                      *r.ptr = '\0'; }

                    if (input.leftButtonPressedThisFrame && fieldHovered) {
                        fieldClicked = true;
                        if (!focused) { // Focus + seed from the currently displayed value (caret at end).
                            edit.length = 0;
                            for (const char* p = liveText; *p && edit.length < 31; ++p) {
                                edit.buffer[edit.length++] = static_cast<char32_t>(static_cast<unsigned char>(*p));
                            }
                            edit.buffer[edit.length] = 0;
                            edit.caret = edit.length;
                            edit.focusedFieldKey = key;
                            edit.editingObjectId = selId;
                            focused = true;
                        }
                    }

                    // Parse the buffer (fully consumed) and run the shared MVP validator.
                    auto evaluateBuffer = [&](double& outValue) -> bool {
                        char ascii[32];
                        size_t n = 0;
                        for (uint8_t k = 0; k < edit.length && n + 1 < sizeof(ascii); ++k) {
                            char32_t c = edit.buffer[k];
                            if (c >= 128) return false;
                            ascii[n++] = static_cast<char>(c);
                        }
                        if (n == 0) return false;
                        auto res = fast_float::from_chars(ascii, ascii + n, outValue);
                        if (res.ec != std::errc() || res.ptr != ascii + n) return false;
                        return ValidatePropertyEdit(*table, fieldValues, fieldValueCount, i,
                            static_cast<float>(outValue));
                    };

                    if (focused) {
                        for (uint8_t c = 0; c < input.textInputCount; ++c) {
                            char32_t ch = input.textInputThisFrame[c];
                            if (ch == U'\r' || ch == U'\n') { // Enter: validate, commit, drop focus.
                                double parsed = 0.0;
                                if (evaluateBuffer(parsed)) {
                                    const uint64_t p1 = (static_cast<uint64_t>(activeTabIndex) << 8) | i;
                                    PushUIAction(kPropertyCommitUIAction, p1, selId,
                                        std::bit_cast<uint64_t>(parsed));
                                    edit.focusedFieldKey = 0;
                                    focused = false;
                                }
                                break;
                            } else if (ch == 0x1B) { // Escape: revert + drop focus.
                                edit.focusedFieldKey = 0;
                                focused = false;
                                break;
                            } else if (ch == U'\b') {
                                if (edit.length > 0) { edit.buffer[--edit.length] = 0; edit.caret = edit.length; }
                            } else if ((ch >= U'0' && ch <= U'9') || ch == U'.' || ch == U'-' ||
                                ch == U'+' || ch == U'e' || ch == U'E') {
                                if (edit.length < 31) {
                                    edit.buffer[edit.length++] = ch;
                                    edit.buffer[edit.length] = 0;
                                    edit.caret = edit.length;
                                }
                            }
                        }
                    }

                    // Field background + focus border (accent when valid, red while invalid).
                    const uint32_t fieldBg = 0xFFF7F7F7u;
                    if (focused) {
                        double v = 0.0;
                        const uint32_t border = evaluateBuffer(v) ? dataTreeActiveColor : 0xFF0000FFu;
                        pushRect(fieldX, fy, fieldW, rowH, border);
                        pushRect(fieldX + 1.0f, fy + 1.0f, fieldW - 2.0f, rowH - 2.0f, fieldBg);
                    } else {
                        pushRect(fieldX, fy, fieldW, rowH, fieldBg);
                    }

                    pushTextClipped(contentX, textBaselineY(rowY, rowH, uiTextScale),
                        LocalizedUIString(fd.labelStringID), labelW, uiActiveColors.actionText, uiTextScale);

                    if (focused) {
                        pushTextClipped(fieldX + textPad, textBaselineY(fy, rowH, uiTextScale), edit.buffer,
                            fieldW - 2.0f * textPad, 0xFF000000u, uiTextScale);
                        if (((GetTickCount64() / 500ULL) % 2ULL) == 0ULL) {
                            const float caretX = std::min(fieldX + textPad + measureU32(edit.buffer, edit.length),
                                fieldX + fieldW - textPad);
                            pushRect(caretX, fy + 2.0f, 1.0f, rowH - 4.0f, 0xFF000000u);
                        }
                    } else {
                        char32_t liveU32[32];
                        asciiToU32(liveText, liveU32, 32);
                        pushTextClipped(fieldX + textPad, textBaselineY(fy, rowH, uiTextScale), liveU32,
                            fieldW - 2.0f * textPad, 0xFF000000u, uiTextScale);
                    }

                    rowY += rowH + rowGap;
                }

                // A click anywhere that is not a field drops focus without committing (MVP UX).
                if (input.leftButtonPressedThisFrame && !fieldClicked) edit.focusedFieldKey = 0;
            } else {
                // Multi-selection or empty selection: a static count line, no field editing.
                edit.focusedFieldKey = 0;
                char num[16];
                { auto r = std::to_chars(num, num + sizeof(num) - 1, static_cast<unsigned long long>(selectionCount));
                  *r.ptr = '\0'; }
                char line[48];
                size_t li = 0;
                for (const char* p = num; *p && li + 1 < sizeof(line); ++p) line[li++] = *p;
                for (const char* p = " objects selected"; *p && li + 1 < sizeof(line); ++p) line[li++] = *p;
                line[li] = '\0';
                char32_t lineU32[64];
                asciiToU32(line, lineU32, 64);
                pushTextClipped(contentX, textBaselineY(rowY, rowH, uiTextScale), lineU32,
                    rightPaneWidthPx - 2.0f * pad, uiActiveColors.actionText, uiTextScale);
            }
        } else {
            edit.focusedFieldKey = 0; // Pane closed → no active edit.
        }

        // Publish overlay width + keyboard-capture state for the WndProc / engineering input guards.
        window.rightOverlayWidthPx.store(
            static_cast<uint32_t>(std::lround(rightIconBarWidthPx + paneWidthPx)), std::memory_order_release);
        window.uiKeyboardCaptureCount.store(edit.focusedFieldKey != 0 ? 1u : 0u, std::memory_order_release);
    }

    // ACTIVE DROPDOWN (placeholder)
    if (window.activeDropdownAction != Commands::INVALID) {
        float dropX = 400.0f;   // TODO: track real button X for proper positioning
        float dropY = topActionGroupY + 80.0f;
        pushRect(dropX, dropY, 160, 220, 0xFF1E1E1E);
        window.activeDropdownAction = Commands::INVALID;   // immediate-mode auto-close
    }

    if (activeTabIndex >= 0 && activeTabIndex < MV_MAX_TABS &&
        activeInternalSubTabType == VishwakarmaStorage::ObjectType::Page2D) {
        DATASETTAB& tab = allTabs[activeTabIndex];
        const bool lineCreationMode =
            tab.cad2d && tab.cad2d->lineCreationMode.load(std::memory_order_acquire);
        const bool polylineCreationMode =
            tab.cad2d && tab.cad2d->polylineCreationMode.load(std::memory_order_acquire);
        const bool polygonCreationMode =
            tab.cad2d && tab.cad2d->polygonCreationMode.load(std::memory_order_acquire);
        const bool circleCreationMode =
            tab.cad2d && tab.cad2d->circleCreationMode.load(std::memory_order_acquire);
        const bool ellipseCreationMode =
            tab.cad2d && tab.cad2d->ellipseCreationMode.load(std::memory_order_acquire);
        const bool arcCreationMode =
            tab.cad2d && tab.cad2d->arcCreationMode.load(std::memory_order_acquire);
        const bool textCreationMode =
            tab.cad2d && tab.cad2d->textCreationMode.load(std::memory_order_acquire);
        const auto transformKind = static_cast<Cad2DTransformKind>(tab.cad2d
            ? tab.cad2d->transform2DKind.load(std::memory_order_acquire) : 0u);
        if (lineCreationMode || polylineCreationMode || polygonCreationMode ||
            circleCreationMode || ellipseCreationMode || arcCreationMode ||
            transformKind != Cad2DTransformKind::None) {
            const float cursorIconSize = iconSizePx;
            const float cursorIconGap = 6.0f;
            Commands cursorCommand = Commands::CREATE_LINE;
            if (polygonCreationMode) cursorCommand = Commands::CREATE_POLYGON;
            else if (polylineCreationMode) cursorCommand = Commands::CREATE_POLYLINE;
            else if (circleCreationMode) cursorCommand = Commands::CREATE_CIRCLE;
            else if (ellipseCreationMode) cursorCommand = Commands::CREATE_ELLIPSE;
            else if (arcCreationMode) cursorCommand = Commands::CREATE_ARC;
            else if (transformKind == Cad2DTransformKind::Copy) cursorCommand = Commands::EDIT_COPY;
            else if (transformKind == Cad2DTransformKind::Offset) cursorCommand = Commands::EDIT_OFFSET;
            else if (transformKind == Cad2DTransformKind::Mirror) cursorCommand = Commands::EDIT_MIRROR;
            else if (transformKind == Cad2DTransformKind::Rotate) cursorCommand = Commands::EDIT_ROTATE;
            else if (transformKind == Cad2DTransformKind::Move) cursorCommand = Commands::EDIT_MOVE;
            const char32_t cursorIcon =
                SVGIconRenderer::IconForID(static_cast<uint32_t>(cursorCommand));
            const float iconX = std::clamp(
                input.mouseX - cursorIconSize * 0.5f, 0.0f, std::max(0.0f, W - cursorIconSize));
            const float iconY = std::clamp(
                input.mouseY + cursorIconGap, 0.0f, std::max(0.0f, H - cursorIconSize));
            PushIcon(ctx, iconX, iconY, cursorIconSize, cursorIconSize, cursorIcon, 0xFF000000u, uiRes);
        }

        const int page2DViewSlot = FindPublishedSubTabSlot(tab, activeInternalSubTabMemoryId);
        if (textCreationMode && page2DViewSlot >= 0 &&
            tab.cad2d->textCreationHasAnchor.load(std::memory_order_acquire) &&
            ((GetTickCount64() / 500ULL) % 2ULL) == 0ULL) {
            int viewportWidth = 0, viewportHeight = 0, viewportTop = 0;
            if (GetVisibleSceneViewportForTab(tab, viewportWidth, viewportHeight, viewportTop) &&
                viewportWidth > 0 && viewportHeight > 0) {
                const Cad2DViewState& view = tab.cad2d->views[page2DViewSlot];
                const float zoom = std::max(
                    view.zoomPixelsPerCU.load(std::memory_order_acquire),
                    kCad2DZoomMinPixelsPerCU);
                const double centerX = view.centerXCU.load(std::memory_order_acquire);
                const double centerY = view.centerYCU.load(std::memory_order_acquire);
                const double anchorX = tab.cad2d->textCreationXCU.load(std::memory_order_acquire);
                const double anchorY = tab.cad2d->textCreationYCU.load(std::memory_order_acquire);
                std::string draftText;
                {
                    std::lock_guard<std::mutex> lock(tab.cad2d->cpuRecordsMutex);
                    draftText = tab.cad2d->textCreationDraft;
                }

                float caretOffsetCU = 0.0f;
                if (!draftText.empty()) {
                    const float textScale = 9.0f / NotoSansMSDF_Size;
                    float cursorXCU = 0.0f;
                    float minX = FLT_MAX;
                    float maxX = -FLT_MAX;
                    bool hasGlyphBounds = false;
                    for (unsigned char c : draftText) {
                        if (c > 0x7F) continue;
                        const auto glyphIt = glyphLookup.find(static_cast<char32_t>(c));
                        if (glyphIt == glyphLookup.end()) continue;

                        const Glyph& glyph = glyphIt->second;
                        if (glyph.width > 0 && glyph.height > 0) {
                            const float x0 = cursorXCU + static_cast<float>(glyph.bearingX) * textScale;
                            const float x1 = x0 + static_cast<float>(glyph.width) * textScale;
                            minX = std::min(minX, x0);
                            maxX = std::max(maxX, x1);
                            hasGlyphBounds = true;
                        }
                        cursorXCU += static_cast<float>(glyph.advanceX) * textScale;
                    }
                    const float alignX = hasGlyphBounds ? -(minX + maxX) * 0.5f : -cursorXCU * 0.5f;
                    caretOffsetCU = cursorXCU + alignX;
                }

                const float caretScreenX = static_cast<float>((double)viewportWidth * 0.5 +
                    (anchorX + (double)caretOffsetCU - centerX) * (double)zoom);
                const float caretScreenY = static_cast<float>((double)viewportTop +
                    (double)viewportHeight * 0.5 - (anchorY - centerY) * (double)zoom);

                if (caretScreenX >= 0.0f && caretScreenX < static_cast<float>(viewportWidth) &&
                    caretScreenY >= static_cast<float>(viewportTop) &&
                    caretScreenY < static_cast<float>(viewportTop + viewportHeight)) {
                    const float caretHeight = std::clamp(9.0f * zoom, 10.0f,
                        std::max(10.0f, static_cast<float>(viewportHeight)));
                    const float caretWidth = 2.0f;
                    const float caretX = std::clamp(caretScreenX - caretWidth * 0.5f,
                        0.0f, std::max(0.0f, W - caretWidth));
                    const float caretY = std::clamp(caretScreenY - caretHeight * 0.5f,
                        static_cast<float>(viewportTop),
                        std::max(static_cast<float>(viewportTop),
                            static_cast<float>(viewportTop + viewportHeight) - caretHeight));
                    pushRect(caretX, caretY, caretWidth, caretHeight, 0xFF000000u);
                }
            }
        }
    }

    if (activeTabIndex >= 0 && activeTabIndex < MV_MAX_TABS &&
        activeInternalSubTabType == VishwakarmaStorage::ObjectType::Scene3D) {
        DATASETTAB& tab = allTabs[activeTabIndex];
        const auto primitiveType = static_cast<VishwakarmaStorage::ObjectType>(
            tab.activePrimitive3DPlacementType.load(std::memory_order_acquire));
        Commands cursorCommand = Commands::INVALID;
        switch (primitiveType) {
        case VishwakarmaStorage::ObjectType::Cuboid:
            cursorCommand = Commands::CREATE_CUBOID;
            break;
        case VishwakarmaStorage::ObjectType::Cylinder:
            cursorCommand = Commands::CREATE_CYLINDER;
            break;
        case VishwakarmaStorage::ObjectType::Sphere:
            cursorCommand = Commands::CREATE_SPHERE;
            break;
        case VishwakarmaStorage::ObjectType::Pyramid:
            cursorCommand = Commands::CREATE_PYRAMID;
            break;
        case VishwakarmaStorage::ObjectType::Cone:
            cursorCommand = Commands::CREATE_CONE;
            break;
        case VishwakarmaStorage::ObjectType::Torus:
            cursorCommand = Commands::CREATE_TORUS;
            break;
        case VishwakarmaStorage::ObjectType::Ellipsoid:
            cursorCommand = Commands::CREATE_ELLIPSOID;
            break;
        default:
            break;
        }

        if (cursorCommand != Commands::INVALID) {
            const float cursorIconSize = iconSizePx;
            const float cursorIconGap = 6.0f;
            const char32_t cursorIcon =
                SVGIconRenderer::IconForID(static_cast<uint32_t>(cursorCommand));
            const float iconX = std::clamp(
                input.mouseX - cursorIconSize * 0.5f, 0.0f, std::max(0.0f, W - cursorIconSize));
            const float iconY = std::clamp(
                input.mouseY + cursorIconGap, 0.0f, std::max(0.0f, H - cursorIconSize));
            PushIcon(ctx, iconX, iconY, cursorIconSize, cursorIconSize, cursorIcon, 0xFF000000u, uiRes);
        }
    }

    // Zoom Window mode cursor: trail the command icon while the mode waits for its 2 corner
    // clicks. Unlike primitive placement this applies to both Scene3D and Page2D views.
    if (activeTabIndex >= 0 && activeTabIndex < MV_MAX_TABS &&
        (activeInternalSubTabType == VishwakarmaStorage::ObjectType::Scene3D ||
         activeInternalSubTabType == VishwakarmaStorage::ObjectType::Page2D) &&
        allTabs[activeTabIndex].zoomWindowMode.load(std::memory_order_acquire)) {
        const float cursorIconSize = iconSizePx;
        const float cursorIconGap = 6.0f;
        const char32_t cursorIcon =
            SVGIconRenderer::IconForID(static_cast<uint32_t>(Commands::ZOOM_WINDOW));
        const float iconX = std::clamp(
            input.mouseX - cursorIconSize * 0.5f, 0.0f, std::max(0.0f, W - cursorIconSize));
        const float iconY = std::clamp(
            input.mouseY + cursorIconGap, 0.0f, std::max(0.0f, H - cursorIconSize));
        PushIcon(ctx, iconX, iconY, cursorIconSize, cursorIconSize, cursorIcon, 0xFF000000u, uiRes);
    }

    // "Restart to Update" toast: the update thread has downloaded, verified and staged a newer
    // setup; it gets applied on the next launch (SoftwareUpdateOnAppLaunch). Bottom-right corner,
    // purely informational. Symmetric grey/white colours are channel-order agnostic.
    if (g_softwareUpdateStagedForRestart.load(std::memory_order_relaxed)) {
        const char32_t* toastText = LocalizedUIString(UITextID::RestartToUpdate);
        const float toastPaddingXPx = 3.0f * pixelsPerMMx;
        const float toastMarginPx = 3.0f * pixelsPerMMy;
        const float toastHeightPx = buttonHeightPx;
        const float toastTextWidthPx = MeasureUIStringWidth(toastText, uiTextScale);
        const float toastWidthPx = toastTextWidthPx + 2.0f * toastPaddingXPx;
        const float toastX = W - toastWidthPx - toastMarginPx;
        const float toastY = H - toastHeightPx - toastMarginPx;
        PushRoundedRectangle(ctx, toastX, toastY, toastWidthPx, toastHeightPx,
            roundedCornerRadiusPx, 0xFF333333u, uiRes);
        pushTextClipped(toastX + toastPaddingXPx, textBaselineY(toastY, toastHeightPx, uiTextScale),
            toastText, toastTextWidthPx + 1.0f, 0xFFFFFFFFu, uiTextScale);
    }

    // Launch splash: a centred credit card shown for the first kSplashDurationMs of the run, from
    // the moment the GPU engine and every monitor's icon atlas came up (wWinMain sets the tick).
    // Purely informational — no hover, no click, no dismissal; it simply stops being drawn. Pushed
    // last so it lands on top of everything else in the frame's single draw call. English only:
    // two of the lines are URLs and one is a proper name, so there is nothing to localize.
    const uint64_t splashStartTick = g_splashOverlayStartTick.load(std::memory_order_relaxed);
    if (splashStartTick != 0) {
        if (GetTickCount64() - splashStartTick >= kSplashDurationMs) {
            g_splashOverlayStartTick.store(0, std::memory_order_relaxed); // Expired: skip every later frame.
        } else {
            static constexpr const char32_t* splashLines[] = {
                U"Developed with Love by Team INDIA",
                U"Lead by Ram Shanker",
                U"https://mv.ramshanker.in",
                U"https://github.com/ramshankerji/Vishwakarma",
            };
            constexpr size_t splashLineCount = std::size(splashLines);

            // 20 px at the UI_MIN_LAYOUT_DPI reference, scaling with DPI like every other UI dimension.
            const float splashTextHeightPx = std::round(20.0f * monitorDPIY / UI_MIN_LAYOUT_DPI);
            const float splashTextScale = TextScaleForHeight(splashTextHeightPx);
            const float splashLineHeightPx = splashTextHeightPx * 1.6f; // Text height plus leading.
            const float splashPaddingXPx = splashTextHeightPx * 1.5f;
            const float splashPaddingYPx = splashTextHeightPx * 0.8f;

            float splashLineWidthPx[splashLineCount] = {};
            float splashWidestLinePx = 0.0f;
            for (size_t i = 0; i < splashLineCount; ++i) {
                splashLineWidthPx[i] = MeasureUIStringWidth(splashLines[i], splashTextScale);
                splashWidestLinePx = std::max(splashWidestLinePx, splashLineWidthPx[i]);
            }

            const float splashHeightPx =
                splashLineHeightPx * (float)splashLineCount + 2.0f * splashPaddingYPx;
            // A logo flanks the text on both sides at 40 % of the card height, sitting on the card's
            // horizontal centreline. Layout is [pad][logo][pad][text][pad][logo][pad].
            const float splashLogoSizePx = std::round(splashHeightPx * 0.4f);
            const float splashWidthPx =
                splashWidestLinePx + 2.0f * splashLogoSizePx + 4.0f * splashPaddingXPx;
            const float splashX = std::round((W - splashWidthPx) * 0.5f);
            const float splashY = std::round((H - splashHeightPx) * 0.5f);

            PushRoundedRectangle(ctx, splashX, splashY, splashWidthPx, splashHeightPx,
                roundedCornerRadiusPx * 2.0f, kSplashBackgroundColor, uiRes);
            const float splashLogoY = splashY + (splashHeightPx - splashLogoSizePx) * 0.5f;
            PushIcon(ctx, splashX + splashPaddingXPx, splashLogoY,
                splashLogoSizePx, splashLogoSizePx, kSplashLogoCodepoint, 0xFFFFFFFFu, uiRes);
            PushIcon(ctx, splashX + splashWidthPx - splashPaddingXPx - splashLogoSizePx, splashLogoY,
                splashLogoSizePx, splashLogoSizePx, kSplashLogoCodepoint, 0xFFFFFFFFu, uiRes);
            for (size_t i = 0; i < splashLineCount; ++i) {
                const float lineY = splashY + splashPaddingYPx + splashLineHeightPx * (float)i;
                const float lineX = std::round(splashX + (splashWidthPx - splashLineWidthPx[i]) * 0.5f);
                pushTextClipped(lineX, textBaselineY(lineY, splashLineHeightPx, splashTextScale),
                    splashLines[i], splashLineWidthPx[i] + 1.0f, kSplashTextColor, splashTextScale,
                    i == 0);
            }
        }
    }
}
