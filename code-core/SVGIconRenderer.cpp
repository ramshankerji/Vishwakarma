// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

#include "SVGIconRenderer.h"

#include "SVGIconEmbeddedData.generated.h"

#include <lunasvg.h>
#include <zlib.h>

#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

namespace {

bool DecompressSVG(const SVGIconRenderer::Embedded::CompressedSVGIcon& icon, std::string& outSVG) {
    outSVG.assign(icon.uncompressedSize, '\0');

    uLongf destinationSize = static_cast<uLongf>(outSVG.size());
    const int status = uncompress(
        reinterpret_cast<Bytef*>(outSVG.data()),
        &destinationSize,
        reinterpret_cast<const Bytef*>(icon.compressedData),
        static_cast<uLong>(icon.compressedSize));

    if (status != Z_OK || destinationSize != icon.uncompressedSize) {
        std::cerr << "Failed to decompress embedded SVG icon: " << icon.fileName << "\n";
        outSVG.clear();
        return false;
    }

    return true;
}

} // namespace

namespace SVGIconRenderer {

bool HasEmbeddedSVGIcon(uint32_t iconID) noexcept {
    for (const auto& icon : Embedded::kSVGIcons) {
        if (icon.id == iconID) return true;
    }

    return false;
}

std::vector<RenderedSVGIcon> RenderEmbeddedSVGIcons(int pixelSize) {
    std::vector<RenderedSVGIcon> renderedIcons;
    if (pixelSize <= 0) return renderedIcons;

    renderedIcons.reserve(Embedded::kSVGIconCount);

    for (const auto& icon : Embedded::kSVGIcons) {
        std::string svgText;
        if (!DecompressSVG(icon, svgText)) continue;

        std::unique_ptr<lunasvg::Document> document =
            lunasvg::Document::loadFromData(svgText.data(), svgText.size());
        if (!document) {
            std::cerr << "Failed to parse embedded SVG icon: " << icon.fileName << "\n";
            continue;
        }

        lunasvg::Bitmap bitmap = document->renderToBitmap(pixelSize, pixelSize, 0x00000000u);
        if (bitmap.isNull() || bitmap.width() <= 0 || bitmap.height() <= 0) {
            std::cerr << "Failed to rasterize embedded SVG icon: " << icon.fileName << "\n";
            continue;
        }

        bitmap.convertToRGBA();

        RenderedSVGIcon rendered{};
        rendered.id = icon.id;
        rendered.fileName = icon.fileName;
        rendered.width = bitmap.width();
        rendered.height = bitmap.height();
        rendered.rgba.resize(static_cast<size_t>(rendered.width) * rendered.height * 4u);

        const int rowBytes = rendered.width * 4;
        const uint8_t* sourcePixels = bitmap.data();
        uint8_t* destinationPixels = rendered.rgba.data();
        for (int y = 0; y < rendered.height; ++y) {
            std::memcpy(destinationPixels + static_cast<size_t>(y) * rowBytes,
                sourcePixels + static_cast<size_t>(y) * bitmap.stride(),
                static_cast<size_t>(rowBytes));
        }

        renderedIcons.push_back(std::move(rendered));
    }

    return renderedIcons;
}

} // namespace SVGIconRenderer
