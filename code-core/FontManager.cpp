// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
#include <iostream>
#include <windows.h>
// Global / static
// Global / static
#include "ft2build.h"
#include FT_FREETYPE_H
#include "FontManager.h"
#include "resource.h"

FT_Library ft = nullptr;
FT_Face    ftFace = nullptr;
FT_Face    ftIconFace = nullptr;
bool       fontInitialized = false;

#pragma comment(lib, "freetype.lib")

static bool LoadFontFaceFromResource(int resourceId, FT_Face& outFace) {
    HRSRC fontResource = FindResource(nullptr, MAKEINTRESOURCE(resourceId), RT_RCDATA);
    if (!fontResource) return false;

    HGLOBAL fontData = LoadResource(nullptr, fontResource);
    if (!fontData) return false;

    const auto* fontBytes = static_cast<const FT_Byte*>(LockResource(fontData));
    DWORD fontSize = SizeofResource(nullptr, fontResource);
    if (!fontBytes || fontSize == 0) return false;

    return FT_New_Memory_Face(ft, fontBytes, static_cast<FT_Long>(fontSize), 0, &outFace) == 0;
}



bool InitFontSystem() {
    if (fontInitialized) return true;

    if (FT_Init_FreeType(&ft)) {
        std::cerr << "FreeType init failed\n";
        return false;
    }

    if (!LoadFontFaceFromResource(IDR_NOTO_SANS_FONT, ftFace)) {
        std::cerr << "Font resource not found\n";
        return false;
    }

    FT_Set_Pixel_Sizes(ftFace, 0, 32); // consistent size

    // Load icon font exclusively from embedded resources so the binary remains
    // self-contained. If the resource is missing the icon pool will be smaller
    // (that's acceptable) and we continue without loading from disk.
    if (LoadFontFaceFromResource(IDR_SVG_ICONS_FONT, ftIconFace)) {
        FT_Select_Charmap(ftIconFace, FT_ENCODING_UNICODE);
    }
    else {
        std::cerr << "Embedded icon font resource not found; continuing with procedural UI icons only\n";
    }

    fontInitialized = true;
    return true;
}
