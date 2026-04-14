// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
#include <iostream>
// Global / static
#include "ft2build.h"
#include FT_FREETYPE_H
#include "FontManager.h"
FT_Library ft = nullptr;
FT_Face    ftFace = nullptr;
bool       fontInitialized = false;

#pragma comment(lib, "freetype.lib")

bool InitFontSystem() {
    if (fontInitialized) return true;

    if (FT_Init_FreeType(&ft)) {
        std::cerr << "FreeType init failed\n";
        return false;
    }

    if (FT_New_Face(ft, "C:\\Windows\\Fonts\\arial.ttf", 0, &ftFace)) {
        std::cerr << "Font load failed\n";
        return false;
    }

    FT_Set_Pixel_Sizes(ftFace, 0, 32); // consistent size

    fontInitialized = true;
    return true;
}
