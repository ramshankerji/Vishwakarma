// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <iostream>

#include "डेटा.h"

/*
This file represents our 2D CAD Modules. These are the only ~20 entity we are going to have in 2D.
We are going to optimize the hell out of them.
Target CHALLENGE: Any combination of 100k Elements in 8 mili-seconds (125 fps) at 4K resolution.

TWO GENERATIONS LIVE IN THIS FILE, and the split is deliberate.

PART 1 is the Page2D object model that is actually in service. It was written during the Page2D
MVP and, until now, sat in RenderPage2D.h - a rendering header. That was the wrong home: the 3D
half keeps its engineering objects in डेटा-सामान्य-3D.h and its rendering structs in
RenderScene3D.h, which defines no engineering object at all. The 2D half had them in one file, so
PropertyPane.cpp had to include a RENDER header to see the object model. Part 1 is that content,
moved here unchanged. RenderPage2D.h now includes this file and keeps only what is genuinely
rendering: the GPU record ABI, view constants, and the copy-thread command queue.

PART 2 is the ORIGINAL 2D schema, below. None of it is in service - not one of those types is
referenced anywhere in the application. It is kept, annotated, because it is not superseded
junk: it carries design the shipped MVP went around rather than through. Layers, line types,
indexed colour palettes and back-to-front draw order are all in Part 2 and absent from Part 1,
and roughly half its types (DIMENSION, LEADER, NURBS, TABLE, HATCH_STYLE, POINT2D) have no Part 1
counterpart at all. Merging the two is a design decision per field - MVP-deferred versus
genuinely superseded - not a refactor, and it belongs with the object-model migration described
at mv.ramshanker.in/software/2drendering, "The 2D object model".

Every Part 2 struct is annotated with its Part 1 replacement, or with the fact that it has none.
*/

/* ================================================================================================
   PART 1 - THE LIVE Page2D OBJECT MODEL
   These are the nine record types the application actually creates, edits, draws and persists.
   Moved here from RenderPage2D.h; the field layouts are unchanged.

   They all derive META_DATA, so identity is shared with the 3D world. What is NOT yet shared is
   residency - see the note on Cad2DLineRecordCPU below.
   ============================================================================================= */

enum class Cad2DLineWeightMode : uint32_t {
    ModelComputerUnit = 0, // thickness is in Page2D ComputerUnits and zooms with the drawing
    ScreenPixel = 1,       // thickness is fixed in screen pixels
    PaperMM = 2            // thickness is millimeters converted through monitor DPI
};

enum class Cad2DTextJustification : uint32_t {
    TopLeft = 0,
    TopMiddle = 1,
    TopRight = 2,
    MiddleLeft = 3,
    Center = 4,
    MiddleRight = 5,
    BottomLeft = 6,
    BottomCenter = 7,
    BottomRight = 8
};

struct Cad2DPoint2D {
    double x = 0.0;
    double y = 0.0;
};

/* ALL NINE 2D RECORD TYPES DERIVE META_DATA. See mv.ramshanker.in/software/2drendering,
"The 2D object model". This one went first, alone, so the true cost per type was known before the
other eight followed.

There is now ONE object model: 2D and 3D objects share identity, soft-delete, schema version and
dataVersion, and generic code can dispatch on META_DATA instead of on which vector a record came
out of. What is NOT yet shared is RESIDENCY - these records still live by value in std::vector on
the heap, not in the arena - which is the second half of step 3 and what Optional64 waits on.

The seven fields that came out of each type, and what they became:
    objectId          -> memoryID              parentObjectId -> memoryIDGenerator
    containerMemoryId -> memoryIDContainer     persistedId, persistedParentId,
                                               schemaVersion, isDeleted  (same names, inherited)

ONE BEHAVIOUR CHANGE TO KNOW ABOUT: META_DATA's constructor issues a memoryID, so `memoryID` is
NEVER 0 on a fresh record. Code that tested `objectId == 0` to mean "not yet assigned" therefore
cannot be transliterated - each such site had to be re-read and decided individually. */
struct Cad2DLineRecordCPU : META_DATA {
    Cad2DLineRecordCPU() {
        // META_DATA asks every derived type to declare itself; nothing reads this for 2D yet, but
        // it is what lets generic code stop switching on which vector a record came out of.
        dataType = static_cast<uint16_t>(VishwakarmaStorage::ObjectType::Line2D);
    }
    double x1 = 0.0;
    double y1 = 0.0;
    double x2 = 0.0;
    double y2 = 0.0;
    float lineWeight = 0.25f;
    Cad2DLineWeightMode lineWeightMode = Cad2DLineWeightMode::PaperMM;
    uint32_t colorABGR = 0xFF000000u;
};
/* 88 -> 112 bytes: 43 bytes of hand-rolled identity fields replaced by META_DATA's 64, plus
padding. Asserted so the next field added here is a deliberate decision rather than a discovery,
exactly as sizeof(META_DATA) is. Sizes for all nine: 2Drendering.md, "Measured sizes". */
static_assert(sizeof(Cad2DLineRecordCPU) == 112,
    "Cad2DLineRecordCPU grew - see 2Drendering.md, Measured sizes.");

struct Cad2DPolylineRecordCPU : META_DATA {
    Cad2DPolylineRecordCPU() { dataType = static_cast<uint16_t>(VishwakarmaStorage::ObjectType::Polyline2D); }
    std::vector<Cad2DPoint2D> points;
    float lineWeight = 0.25f;
    Cad2DLineWeightMode lineWeightMode = Cad2DLineWeightMode::PaperMM;
    uint32_t colorABGR = 0xFF000000u;
};

struct Cad2DPolygonRecordCPU : META_DATA {
    Cad2DPolygonRecordCPU() { dataType = static_cast<uint16_t>(VishwakarmaStorage::ObjectType::Polygon2D); }
    uint32_t lineSegmentCount = 4;
    double centerX = 0.0;
    double centerY = 0.0;
    double radius = 0.0;
    double rotationDegrees = 45.0;
    float lineWeight = 0.25f;
    Cad2DLineWeightMode lineWeightMode = Cad2DLineWeightMode::PaperMM;
    uint32_t colorABGR = 0xFF000000u;
};

struct Cad2DCircleRecordCPU : META_DATA {
    Cad2DCircleRecordCPU() { dataType = static_cast<uint16_t>(VishwakarmaStorage::ObjectType::Circle2D); }
    double centerX = 0.0;
    double centerY = 0.0;
    double radius = 0.0;
    float lineWeight = 0.25f;
    Cad2DLineWeightMode lineWeightMode = Cad2DLineWeightMode::PaperMM;
    uint32_t colorABGR = 0xFF000000u;
};

struct Cad2DEllipseRecordCPU : META_DATA {
    Cad2DEllipseRecordCPU() { dataType = static_cast<uint16_t>(VishwakarmaStorage::ObjectType::Ellipse2D); }
    double centerX = 0.0;
    double centerY = 0.0;
    double radiusX = 0.0;
    double radiusY = 0.0;
    double rotationRadians = 0.0; // CCW rotation of the radius axes about the center.
    float lineWeight = 0.25f;
    Cad2DLineWeightMode lineWeightMode = Cad2DLineWeightMode::PaperMM;
    uint32_t colorABGR = 0xFF000000u;
};

struct Cad2DArcRecordCPU : META_DATA {
    Cad2DArcRecordCPU() { dataType = static_cast<uint16_t>(VishwakarmaStorage::ObjectType::Arc2D); }
    double centerX = 0.0;
    double centerY = 0.0;
    double radiusX = 0.0;
    double radiusY = 0.0;
    // CCW rotation of the radius axes about the center. Start/end stay world coordinates; the
    // sweep runs CCW between their parameter angles measured in the rotated local frame.
    double rotationRadians = 0.0;
    double startX = 0.0;
    double startY = 0.0;
    double endX = 0.0;
    double endY = 0.0;
    float lineWeight = 0.25f;
    Cad2DLineWeightMode lineWeightMode = Cad2DLineWeightMode::PaperMM;
    uint32_t colorABGR = 0xFF000000u;
};

struct Cad2DTextRecordCPU : META_DATA {
    Cad2DTextRecordCPU() { dataType = static_cast<uint16_t>(VishwakarmaStorage::ObjectType::Text2D); }
    double x = 0.0;
    double y = 0.0;
    float textHeightCU = 3.5f;
    float rotationRadians = 0.0f;
    uint32_t colorABGR = 0xFF000000u;
    uint64_t font = 0; // 0 = Noto Sans MVP font
    Cad2DTextJustification justification = Cad2DTextJustification::Center;
    float xOffsetCU = 0.0f;
    float yOffsetCU = 0.0f;
    std::string text;
};

// Non-parametric asset definition. Virtual container object: never rendered or hit-tested itself.
// Its master geometry is regular Cad2D*RecordCPU records with parentObjectId = this objectId and
// containerMemoryId = 0 (so no page ever draws them). assetNumber is the user-visible numeric id.
struct Cad2DAssetDefinitionRecordCPU : META_DATA {
    Cad2DAssetDefinitionRecordCPU() {
        dataType = static_cast<uint16_t>(VishwakarmaStorage::ObjectType::Asset2DDefinition);
    }
    // memoryIDContainer and memoryIDGenerator stay 0: a definition is TAB-level, so no page holds
    // it and nothing generates it. BuildRowsFromTab writes its parent_id as 0 to match.
    uint32_t assetNumber = 0; // Random numeric id shown in the Insert Asset dropdown.
    double baseX = 0.0; // Insert base point: middle of the bounding box of the source objects.
    double baseY = 0.0; // Master geometry keeps source page coordinates; each insert places
                        // members at insert + R(rotation) * S(scale) * (master - base).
};

// One placed instance of an asset on a Page2D. Virtual container object: never rendered itself;
// its member records live on the page (containerMemoryId = page) with parentObjectId = this
// objectId, so the draw side needs no asset awareness. Selection expands through the parent.
struct Cad2DAssetInsertRecordCPU : META_DATA {
    Cad2DAssetInsertRecordCPU() {
        dataType = static_cast<uint16_t>(VishwakarmaStorage::ObjectType::Asset2DInsert);
    }
    // memoryIDContainer is the owning Page2D. memoryIDGenerator stays 0: an insert is placed by a
    // user, not stamped out of anything.
    uint64_t definitionObjectId = 0; // Memory id of the Cad2DAssetDefinitionRecordCPU.
    double x = 0.0; // Insert point in page ComputerUnits.
    double y = 0.0;
    // Per-instance transform (schema v2), baked into the member records at placement:
    // member = insert + R(rotation) * S(scale) * (master - base). Negative scale = mirror.
    double scaleX = 1.0;
    double scaleY = 1.0;
    double rotationDegrees = 0.0; // Counter-clockwise.
};

/* ================================================================================================
   PART 2 - THE ORIGINAL 2D SCHEMA. NOT IN SERVICE. KEPT FOR THE MERGE.

   Nothing below is referenced anywhere in the application. Do not wire any of it up piecemeal;
   the merge with Part 1 is a per-field decision and wants doing in one pass.

   WHAT PART 1 DOES NOT HAVE, and these do - the reason this half is worth keeping:
     layerIndex ................ layers. Part 1 has no layer concept whatsoever.
     lineTypeIndex/Scale ....... dashed / centre / hidden line types. Absent from Part 1.
     backToFrontOrderNo ........ explicit draw order. Absent from Part 1.
     colorIndex ................ an indexed palette; Part 1 went direct with colorABGR.
     optionalFieldsFlags ....... the hand-rolled precursor to Optional64 (OptionalProperties.h),
                                 which now exists properly and would replace it.

   WHAT PART 1 HAS AND THESE DO NOT: container / owner ids, lineWeightMode's three modes,
   rotationRadians on arc and ellipse, text justification and offsets, isDeleted, schemaVersion.

   So neither half is a superset of the other. That is the whole difficulty.

   These also use META_DATA by COMPOSITION (a metaData member) rather than inheritance, which is
   the opposite of every 3D type. Inheritance is what the unification settles on.
   ============================================================================================= */

// DEAD. Replaced by Cad2DLineRecordCPU (Part 1).
// Carries over to the merge: layerIndex, lineTypeIndex, lineTypeScale, colorIndex,
// backToFrontOrderNo. Part 1's lineWeight + lineWeightMode supersede printThicknessMM.
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

// DEAD. Replaced by Cad2DPolylineRecordCPU (Part 1), which splits this type in two: the open case
// is the polyline, and the closed case became the separate parametric Cad2DPolygonRecordCPU
// (centre + radius + segment count) rather than an isClosed flag on a point list. An arbitrary
// CLOSED point list - which this struct expresses and Part 1 does not - is still missing.
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

// DEAD, and NO Part 1 replacement exists. A triangle is currently drawn as three lines or as a
// 3-segment polygon; neither is the same object.
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

// DEAD, and NO true Part 1 replacement. Cad2DPolygonRecordCPU with lineSegmentCount 4 draws a
// SQUARE about a centre - it cannot express independent xLength and yWidth, so a rectangle is
// currently four lines. This one is a genuine gap, not a supersession.
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

// DEAD. Replaced by Cad2DCircleRecordCPU (Part 1). Same geometry; the presentation fields differ.
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

// DEAD. Replaced by Cad2DEllipseRecordCPU (Part 1), which stores rotation in RADIANS as
// rotationRadians rather than the rotationDegree here. Watch that unit change when merging.
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

// DEAD. Replaced by Cad2DArcRecordCPU (Part 1). Note the two are not the same shape at all: this
// one is two points, while Part 1's is an elliptical arc - centre, two radii, rotation, and start
// and end points whose parameter angles bound a CCW sweep.
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

// DEAD, and NO Part 1 replacement. Points are a snapping and surveying primitive; the Page2D world
// has no way to author one today.
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

// DEAD, and NO Part 1 replacement - Part 1 has no line-type concept, so every 2D line the
// application draws today is solid. This is the style table the lineTypeIndex fields point into.
struct LINE_TYPE_STYLE {
    META_DATA metaData;

    //Mandatory Properties
    uint16_t systemDefinedLineTypeNo;

    uint16_t optionalFieldsFlags;  // Bit-mask for up to 16 Optional Fields - 8 Bytes.
    uint16_t systemFlags;          // 32 booleans for internal use only. Not persisted.

    //Optional Properties
    uint64_t customLineTypeDashDot;
};

// DEAD, and NO Part 1 replacement. Nothing in the application hatches a region yet.
struct HATCH_STYLE {
    META_DATA metaData;

    //Mandatory Properties
    uint16_t systemDefinedHatchTypeNo;

    uint16_t optionalFieldsFlags;  // Bit-mask for up to 16 Optional Fields - 8 Bytes.
    uint16_t systemFlags;          // 32 booleans for internal use only. Not persisted.

    //Optional Properties
    uint64_t customHatchDetails;
};

// DEAD. Replaced by Cad2DTextRecordCPU (Part 1), which is the fuller type - it carries the string
// itself, a position, justification and offsets, none of which are here.
// NAMING HAZARD: TEXT is also a Windows macro (winnt.h). It survives only because that macro is
// function-like and `struct TEXT {` has no following parenthesis. Rename on merge.
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

// DEAD, and NO Part 1 replacement. Cad2DTextRecordCPU is a single run of text; multi-line flowed
// paragraphs with wrapping are a separate type and do not exist.
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

// DEAD, NO Part 1 replacement, and never fleshed out here either - it has no geometry fields at
// all. Dimensions are the largest single gap in the 2D feature set.
struct DIMENSION {
    META_DATA metaData;

    //Mandatory Properties

    uint16_t optionalFieldsFlags;  // Bit-mask for up to 16 Optional Fields - 8 Bytes.
    uint16_t systemFlags;          // 32 booleans for internal use only. Not persisted.

    //Optional Properties
};

// DEAD, NO Part 1 replacement, no geometry fields yet.
struct LEADER { //Implement this simply as minor variant of poly-line with special line endings.
    META_DATA metaData;

    //Mandatory Properties

    uint16_t optionalFieldsFlags;  // Bit-mask for up to 16 Optional Fields - 8 Bytes.
    uint16_t systemFlags;          // 32 booleans for internal use only. Not persisted.

    //Optional Properties
};

// DEAD, NO Part 1 replacement, no geometry fields yet. Every curve the application draws today is
// a circular or elliptical arc; free-form splines do not exist.
struct NURBS {
    META_DATA metaData;

    //Mandatory Properties

    uint16_t optionalFieldsFlags;  // Bit-mask for up to 16 Optional Fields - 8 Bytes.
    uint16_t systemFlags;          // 32 booleans for internal use only. Not persisted.

    //Optional Properties
};

// DEAD, NO Part 1 replacement, no fields yet.
struct TABLE {
    META_DATA metaData;

    //Mandatory Properties

    uint16_t optionalFieldsFlags;  // Bit-mask for up to 16 Optional Fields - 8 Bytes.
    uint16_t systemFlags;          // 32 booleans for internal use only. Not persisted.

    //Optional Properties
};

// DEAD. Replaced by Cad2DAssetInsertRecordCPU (Part 1) - one placed instance of an asset, which
// additionally carries the per-instance scale and rotation this struct never had.
struct BLOCK {
    META_DATA metaData;

    //Mandatory Properties

    uint16_t optionalFieldsFlags;  // Bit-mask for up to 16 Optional Fields - 8 Bytes.
    uint16_t systemFlags;          // 32 booleans for internal use only. Not persisted.

    //Optional Properties
};

// DEAD. Replaced by Cad2DAssetDefinitionRecordCPU (Part 1). Note the spelling - DEFINATION.
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
