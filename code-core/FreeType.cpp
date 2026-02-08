/*

// Initialize FreeType face only once
if (!initialized) {
    FT_Library ft;
    if (FT_Init_FreeType(&ft)) {
        std::cerr << "Could not initialize FreeType library" << std::endl;
        return -1;
    }

    if (FT_New_Face(ft, "C:\\Windows\\Fonts\\arial.ttf", 0, &face)) {
        std::cerr << "Could not open font" << std::endl;
        return -1;
    }

    FT_Set_Pixel_Sizes(face, 0, 24); // Set font size to 48 pixels high
    initialized = true;
}

// Get the dimensions of the client area
RECT rect;
GetClientRect(hWnd, &rect);
*/
