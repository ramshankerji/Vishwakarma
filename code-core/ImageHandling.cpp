// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.
/* This file is a thin wrapper around libpng, libjpeg etc. 
So that our application has a single consistent interface for loading images from both disk and memory. 
It also handles the normalization of different PNG formats to a common RGBA8 format, 
which simplifies rendering code later on. */

#include<iostream>
#include<png.h>

struct PngMemoryReaderState { // Structure to keep track of memory reading
    const unsigned char* buffer;
    size_t size;
    size_t current_pos;
};

// Custom Callback for libpng to read from memory
void PngReadFromMemory(png_structp png_ptr, png_bytep outBytes, png_size_t byteCountToRead) {
    PngMemoryReaderState* state = (PngMemoryReaderState*)png_get_io_ptr(png_ptr);

    if (state->current_pos + byteCountToRead > state->size) {
        png_error(png_ptr, "Read Error: Unexpected end of PNG data in memory.");
    }

    memcpy(outBytes, state->buffer + state->current_pos, byteCountToRead);
    state->current_pos += byteCountToRead;
}

// Load PNG from raw memory bytes
void LoadPngImageFromMemory(const void* data, size_t size, unsigned char** image_data, int* width, int* height) {
    if (!data || size == 0) abort();

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) abort();

    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_read_struct(&png, NULL, NULL);
        abort();
    }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        abort();
    }

    // Use custom read function instead of png_init_io ---
    PngMemoryReaderState state = { (const unsigned char*)data, size, 0 };
    png_set_read_fn(png, &state, PngReadFromMemory);
    // -------------------------------------------------------------------

    png_read_info(png, info);

    *width = png_get_image_width(png, info);
    *height = png_get_image_height(png, info);
    png_byte color_type = png_get_color_type(png, info);
    png_byte bit_depth = png_get_bit_depth(png, info);

    // Logic for normalization
    if (bit_depth == 16) png_set_strip_16(png);
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);

    if (color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_filler(png, 0xFF, PNG_FILLER_AFTER);

    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);

    png_read_update_info(png, info);

    *image_data = (unsigned char*)malloc(png_get_rowbytes(png, info) * (*height));
    png_bytep* row_pointers = (png_bytep*)malloc(sizeof(png_bytep) * (*height));

    for (int y = 0; y < *height; y++) {
        row_pointers[y] = (*image_data) + y * png_get_rowbytes(png, info);
    }

    png_read_image(png, row_pointers);

    free(row_pointers);
    png_destroy_read_struct(&png, &info, NULL);
}

void LoadPngImage(const char* filename, unsigned char** image_data, int* width, int* height)
{
    image_data = nullptr;
    width = nullptr;
    height = nullptr;
    //In case there is an error, we just return with null pointers. The caller should check for this.
#pragma warning(disable: 4996)
    FILE* fp = fopen(filename, "rb");
    if (!fp) return;//abort();

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) return;//abort();

    png_infop info = png_create_info_struct(png);
    if (!info) return;//abort();

    if (setjmp(png_jmpbuf(png))) return;//abort();

    png_init_io(png, fp);

    png_read_info(png, info);

    *width = png_get_image_width(png, info);
    *height = png_get_image_height(png, info);
    png_byte color_type = png_get_color_type(png, info);
    png_byte bit_depth = png_get_bit_depth(png, info);

    if (bit_depth == 16)
        png_set_strip_16(png);

    if (color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png);

    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(png);

    if (png_get_valid(png, info, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png);

    if (color_type == PNG_COLOR_TYPE_RGB ||
        color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_filler(png, 0xFF, PNG_FILLER_AFTER);

    if (color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);

    png_read_update_info(png, info);

    *image_data = (unsigned char*)malloc(png_get_rowbytes(png, info) * (*height));
    png_bytep* row_pointers = (png_bytep*)malloc(sizeof(png_bytep) * (*height));
    for (int y = 0; y < *height; y++) {
        row_pointers[y] = (*image_data) + y * png_get_rowbytes(png, info);
    }

    png_read_image(png, row_pointers);

    fclose(fp);
    free(row_pointers);
    png_destroy_read_struct(&png, &info, NULL);

}
