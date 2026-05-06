// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
#pragma once

#include <fstream>
#include <vector>
#include <cstdint>
#include <iostream>

// Saves raw pixel data to a BMP file.
// bpp (Bytes Per Pixel) must be 1 (Grayscale), 3 (RGB), or 4 (RGBA).
inline bool SaveToBmp(const char* filename, const uint8_t* pixels, int width, int height, int bpp) {
    if (!pixels || width <= 0 || height <= 0 || (bpp != 1 && bpp != 3 && bpp != 4)) {
        std::cerr << "Invalid parameters for SaveToBmp." << std::endl;
        return false;
    }

    std::ofstream file(filename, std::ios::binary);
    if (!file) {
        std::cerr << "Failed to open file for writing: " << filename << std::endl;
        return false;
    }

    // BMP rows must be padded to a multiple of 4 bytes
    int rowStride = (width * bpp + 3) & ~3;
    int imageSize = rowStride * height;

    // 1. BMP File Header (14 bytes)
    uint8_t bmpHeader[14] = {
        'B', 'M',           // Signature
        0, 0, 0, 0,         // File size (filled later)
        0, 0, 0, 0,         // Reserved
        0, 0, 0, 0          // Data offset (filled later)
    };

    // 2. DIB Header (BITMAPINFOHEADER - 40 bytes)
    uint8_t dibHeader[40] = {
        40, 0, 0, 0,        // DIB header size
        0, 0, 0, 0,         // Width (filled later)
        0, 0, 0, 0,         // Height (filled later)
        1, 0,               // Planes (must be 1)
        0, 0,               // Bits per pixel (filled later)
        0, 0, 0, 0,         // Compression (0 = uncompressed)
        0, 0, 0, 0,         // Image size (filled later)
        0, 0, 0, 0,         // X pixels per meter
        0, 0, 0, 0,         // Y pixels per meter
        0, 0, 0, 0,         // Colors in color table
        0, 0, 0, 0          // Important color count
    };

    // 3. Grayscale Palette (1024 bytes) - Only required for 8-bit (1 byte per pixel) images
    std::vector<uint8_t> palette;
    int dataOffset = 14 + 40;
    if (bpp == 1) {
        palette.resize(1024);
        for (int i = 0; i < 256; ++i) {
            palette[i * 4 + 0] = i; // Blue
            palette[i * 4 + 1] = i; // Green
            palette[i * 4 + 2] = i; // Red
            palette[i * 4 + 3] = 0; // Reserved
        }
        dataOffset += 1024;
    }

    int fileSize = dataOffset + imageSize;

    // Populate dynamic header values
    bmpHeader[2] = (uint8_t)(fileSize);
    bmpHeader[3] = (uint8_t)(fileSize >> 8);
    bmpHeader[4] = (uint8_t)(fileSize >> 16);
    bmpHeader[5] = (uint8_t)(fileSize >> 24);

    bmpHeader[10] = (uint8_t)(dataOffset);
    bmpHeader[11] = (uint8_t)(dataOffset >> 8);
    bmpHeader[12] = (uint8_t)(dataOffset >> 16);
    bmpHeader[13] = (uint8_t)(dataOffset >> 24);

    dibHeader[4] = (uint8_t)(width);
    dibHeader[5] = (uint8_t)(width >> 8);
    dibHeader[6] = (uint8_t)(width >> 16);
    dibHeader[7] = (uint8_t)(width >> 24);

    // Negative height forces top-down drawing (standard memory layout)
    dibHeader[8] = (uint8_t)(-height);
    dibHeader[9] = (uint8_t)((-height) >> 8);
    dibHeader[10] = (uint8_t)((-height) >> 16);
    dibHeader[11] = (uint8_t)((-height) >> 24);

    dibHeader[14] = (uint8_t)(bpp * 8); // Bits per pixel

    dibHeader[20] = (uint8_t)(imageSize);
    dibHeader[21] = (uint8_t)(imageSize >> 8);
    dibHeader[22] = (uint8_t)(imageSize >> 16);
    dibHeader[23] = (uint8_t)(imageSize >> 24);

    // Write headers
    file.write(reinterpret_cast<char*>(bmpHeader), sizeof(bmpHeader));
    file.write(reinterpret_cast<char*>(dibHeader), sizeof(dibHeader));

    // Write palette if applicable
    if (bpp == 1) {
        file.write(reinterpret_cast<char*>(palette.data()), palette.size());
    }

    // Write Pixel Data
    std::vector<uint8_t> rowData(rowStride, 0);
    for (int y = 0; y < height; ++y) {
        const uint8_t* srcRow = pixels + y * (width * bpp);
        if (bpp == 4 || bpp == 3) {
            // BMP expects BGR(A) format, so we swap Red and Blue channels
            for (int x = 0; x < width; ++x) {
                rowData[x * bpp + 0] = srcRow[x * bpp + 2]; // B
                rowData[x * bpp + 1] = srcRow[x * bpp + 1]; // G
                rowData[x * bpp + 2] = srcRow[x * bpp + 0]; // R
                if (bpp == 4) rowData[x * bpp + 3] = srcRow[x * bpp + 3]; // A
            }
        }
        else {
            // 8-bit R8 Grayscale: direct copy
            std::copy(srcRow, srcRow + (width * bpp), rowData.begin());
        }
        file.write(reinterpret_cast<char*>(rowData.data()), rowStride);
    }

    file.close();
    return true;
}
