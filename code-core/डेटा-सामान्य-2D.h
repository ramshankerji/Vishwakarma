// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.

#include <vector>
#include <iostream>

#include "डेटा.h"

/*
This file represents our 2D CAD Modules. These are the only ~20 entity we are going to have in 2D.
We are going to optimize the hell out of them. 
Target CHALLENGE: Any combination of 100k Elements in 8 mili-seconds (125 fps) at 4K resolution.
*/

struct SIMPLE_LINE {
    META_DATA metaData;

    //Mandatory Properties
    double x1, y1, x2, y2;
    uint32_t backToFrontOrderNo;
    uint32_t lineTypeIndex;   // For 2D we, maintain a line-type Index array in application for fast access.
    uint32_t colorIndex;      // Similarly indexed for fast access.
    uint32_t layerIndex;      // Index into a fast access index array of Layers. Stored different in database.
    uint16_t lineTypeScale;   // Stored as pair of 2 8-bit number. One for before decimal, one after decimal.
    uint16_t printThicknessMM;// Stored as pair of 2 8-bit number. One for before decimal, one after decimal.
    
    uint16_t optionalFieldsFlags;  // Bit-mask for up to 16 Optional Fields - 8 Bytes.
    uint16_t systemFlags;          // 32 booleans for internal use only. Not persisted.

    //Optional Properties
    uint64_t color; // This optional attribute is for override. In general they will refer to Index.
};

struct POLY_LINE { //Closed poly lines acts as polygon.
    META_DATA metaData;

    //Mandatory Properties
    double x1, y1, x2, y2;
    uint32_t backToFrontOrderNo;
    uint32_t lineTypeIndex;   // For 2D we, maintain a line-type Index array in application for fast access.
    uint32_t colorIndex;      // Similarly indexed for fast access.
    uint32_t layerIndex;      // Index into a fast access index array of Layers. Stored different in database.
    uint16_t lineTypeScale;   // Stored as pair of 2 8-bit number. One for before decimal, one after decimal.
    uint16_t printThicknessMM;// Stored as pair of 2 8-bit number. One for before decimal, one after decimal.

    uint16_t optionalFieldsFlags;  // Bit-mask for up to 16 Optional Fields - 8 Bytes.
    uint16_t systemFlags;          // 32 booleans for internal use only. Not persisted.

    bool isClosed; // If it is closed, it becomes a polygon.

    //Optional Properties
    uint64_t color; // This optional attribute is for override. In general they will refer to Index.
};

struct TRIANGLE {
    META_DATA metaData;

    //Mandatory Properties
    double x1, y1, x2, y2, x3, y3;
    uint32_t backToFrontOrderNo;
    uint32_t lineTypeIndex;   // For 2D we, maintain a line-type Index array in application for fast access.
    uint32_t colorIndex;      // Similarly indexed for fast access.
    uint32_t layerIndex;      // Index into a fast access index array of Layers. Stored different in database.
    uint16_t lineTypeScale;   // Stored as pair of 2 8-bit number. One for before decimal, one after decimal.
    uint16_t printThicknessMM;// Stored as pair of 2 8-bit number. One for before decimal, one after decimal.

    uint16_t optionalFieldsFlags;  // Bit-mask for up to 16 Optional Fields - 8 Bytes.
    uint16_t systemFlags;          // 32 booleans for internal use only. Not persisted.

    //Optional Properties
    uint64_t color; // This optional attribute is for override. In general they will refer to Index.
};

struct RECTANGLE {
    META_DATA metaData;

    //Mandatory Properties
    double centerX, centerY, xLength, yWidth;
    float rotationDegree = 0;
    uint32_t backToFrontOrderNo;
    uint32_t lineTypeIndex;   // For 2D we, maintain a line-type Index array in application for fast access.
    uint32_t colorIndex;      // Similarly indexed for fast access.
    uint32_t layerIndex;      // Index into a fast access index array of Layers. Stored different in database.
    uint16_t lineTypeScale;   // Stored as pair of 2 8-bit number. One for before decimal, one after decimal.
    uint16_t printThicknessMM;// Stored as pair of 2 8-bit number. One for before decimal, one after decimal.

    uint16_t optionalFieldsFlags;  // Bit-mask for up to 16 Optional Fields - 8 Bytes.
    uint16_t systemFlags;          // 32 booleans for internal use only. Not persisted.

    //Optional Properties
    uint64_t color; // This optional attribute is for override. In general they will refer to Index.
};

struct CIRCLE {
    META_DATA metaData;

    //Mandatory Properties
    double centerX, centerY, radius;
    uint32_t backToFrontOrderNo;
    uint32_t lineTypeIndex;   // For 2D we, maintain a line-type Index array in application for fast access.
    uint32_t colorIndex;      // Similarly indexed for fast access.
    uint32_t layerIndex;      // Index into a fast access index array of Layers. Stored different in database.
    uint16_t lineTypeScale;   // Stored as pair of 2 8-bit number. One for before decimal, one after decimal.
    uint16_t printThicknessMM;// Stored as pair of 2 8-bit number. One for before decimal, one after decimal.

    uint16_t optionalFieldsFlags;  // Bit-mask for up to 16 Optional Fields - 8 Bytes.
    uint16_t systemFlags;          // 32 booleans for internal use only. Not persisted.

    //Optional Properties
    uint64_t color; // This optional attribute is for override. In general they will refer to Index.
};

struct ELLIPSE {
    META_DATA metaData;

    //Mandatory Properties
    double centerX, centerY, majorRadiusX, minorRadiusY;
    float rotationDegree;
    uint32_t backToFrontOrderNo;
    uint32_t lineTypeIndex;   // For 2D we, maintain a line-type Index array in application for fast access.
    uint32_t colorIndex;      // Similarly indexed for fast access.
    uint32_t layerIndex;      // Index into a fast access index array of Layers. Stored different in database.
    uint16_t lineTypeScale;   // Stored as pair of 2 8-bit number. One for before decimal, one after decimal.
    uint16_t printThicknessMM;// Stored as pair of 2 8-bit number. One for before decimal, one after decimal.

    uint16_t optionalFieldsFlags;  // Bit-mask for up to 16 Optional Fields - 8 Bytes.
    uint16_t systemFlags;          // 32 booleans for internal use only. Not persisted.

    //Optional Properties
    uint64_t color; // This optional attribute is for override. In general they will refer to Index.
};

struct ARC {
    META_DATA metaData;

    //Mandatory Properties
    double x1, y1, x2, y2;
    uint32_t backToFrontOrderNo;
    uint32_t lineTypeIndex;   // For 2D we, maintain a line-type Index array in application for fast access.
    uint32_t colorIndex;      // Similarly indexed for fast access.
    uint32_t layerIndex;      // Index into a fast access index array of Layers. Stored different in database.
    uint16_t lineTypeScale;   // Stored as pair of 2 8-bit number. One for before decimal, one after decimal.
    uint16_t printThicknessMM;// Stored as pair of 2 8-bit number. One for before decimal, one after decimal.

    uint16_t optionalFieldsFlags;  // Bit-mask for up to 16 Optional Fields - 8 Bytes.
    uint16_t systemFlags;          // 32 booleans for internal use only. Not persisted.

    //Optional Properties
    uint64_t color; // This optional attribute is for override. In general they will refer to Index.
};

struct POINT2D { //Windows header already defines 'POINTS' for some other purpose.
    META_DATA metaData;

    //Mandatory Properties
    double x, y;
    uint32_t backToFrontOrderNo;
    uint32_t layerIndex;      // Index into a fast access index array of Layers. Stored different in database.
    
    uint16_t optionalFieldsFlags;  // Bit-mask for up to 16 Optional Fields - 8 Bytes.
    uint16_t systemFlags;          // 32 booleans for internal use only. Not persisted.

    //Optional Properties
    uint64_t color; // This optional attribute is for override. In general they will refer to Index.
    uint64_t pointRepresentation;
};

struct LINE_TYPE_STYLE {
    META_DATA metaData;

    //Mandatory Properties
    uint16_t systemDefinedLineTypeNo;

    uint16_t optionalFieldsFlags;  // Bit-mask for up to 16 Optional Fields - 8 Bytes.
    uint16_t systemFlags;          // 32 booleans for internal use only. Not persisted.

    //Optional Properties
    uint64_t customLineTypeDashDot;
};

struct HATCH_STYLE {
    META_DATA metaData;

    //Mandatory Properties
    uint16_t systemDefinedHatchTypeNo;

    uint16_t optionalFieldsFlags;  // Bit-mask for up to 16 Optional Fields - 8 Bytes.
    uint16_t systemFlags;          // 32 booleans for internal use only. Not persisted.

    //Optional Properties
    uint64_t customHatchDetails;
};

struct TEXT {
    META_DATA metaData;

    //Mandatory Properties
    uint64_t font;
    float fontSize;
    uint16_t fontVarientNo;

    uint16_t optionalFieldsFlags;  // Bit-mask for up to 16 Optional Fields - 8 Bytes.
    uint16_t systemFlags;          // 32 booleans for internal use only. Not persisted.

    //Optional Properties
};

struct PARAGRAPH {
    META_DATA metaData;

    //Mandatory Properties
    uint64_t font;
    float fontSize;
    uint16_t fontVarientNo;

    uint16_t optionalFieldsFlags;  // Bit-mask for up to 16 Optional Fields - 8 Bytes.
    uint16_t systemFlags;          // 32 booleans for internal use only. Not persisted.

    //Optional Properties
};

struct DIMENSION {
    META_DATA metaData;

    //Mandatory Properties

    uint16_t optionalFieldsFlags;  // Bit-mask for up to 16 Optional Fields - 8 Bytes.
    uint16_t systemFlags;          // 32 booleans for internal use only. Not persisted.

    //Optional Properties
};

struct LEADER { //Implement this simply as minor variant of poly-line with special line endings.
    META_DATA metaData;

    //Mandatory Properties

    uint16_t optionalFieldsFlags;  // Bit-mask for up to 16 Optional Fields - 8 Bytes.
    uint16_t systemFlags;          // 32 booleans for internal use only. Not persisted.

    //Optional Properties
};

struct NURBS {
    META_DATA metaData;

    //Mandatory Properties

    uint16_t optionalFieldsFlags;  // Bit-mask for up to 16 Optional Fields - 8 Bytes.
    uint16_t systemFlags;          // 32 booleans for internal use only. Not persisted.

    //Optional Properties
};

struct TABLE {
    META_DATA metaData;

    //Mandatory Properties

    uint16_t optionalFieldsFlags;  // Bit-mask for up to 16 Optional Fields - 8 Bytes.
    uint16_t systemFlags;          // 32 booleans for internal use only. Not persisted.

    //Optional Properties
};

struct BLOCK {
    META_DATA metaData;

    //Mandatory Properties

    uint16_t optionalFieldsFlags;  // Bit-mask for up to 16 Optional Fields - 8 Bytes.
    uint16_t systemFlags;          // 32 booleans for internal use only. Not persisted.

    //Optional Properties
};

struct BLOCK_DEFINATION {
    META_DATA metaData;

    //Mandatory Properties

    uint16_t optionalFieldsFlags;  // Bit-mask for up to 16 Optional Fields - 8 Bytes.
    uint16_t systemFlags;          // 32 booleans for internal use only. Not persisted.

    //Optional Properties
};

/*
LEGACY FUNCTION FROM GDI ERA OF DEVELOPMENT. TO BE DISCARDED. KEPT FOR ALGORITHMIC REFERENCE.

To draw a line from (𝑥1,𝑦1) (x1, y1) to (𝑥2,𝑦2)(x2, y2) using the
SetPixel function in C++, you can use Bresenham's Line Algorithm.
This algorithm is efficient and works well with integer arithmetic,
making it suitable for drawing lines on raster displays.

void Draw2DLine(HDC hdc, int x1, int y1, int x2, int y2) {
    int dx = abs(x2 - x1);
    int dy = abs(y2 - y1);
    int sx = (x1 < x2) ? 1 : -1;
    int sy = (y1 < y2) ? 1 : -1;
    int err = dx - dy;

    while (true) {
        SetPixel(hdc, x1, y1, RGB(0, 0, 0)); // Set the pixel at (x1, y1)

        if (x1 == x2 && y1 == y2) break;
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x1 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y1 += sy;
        }
    }
}
*/