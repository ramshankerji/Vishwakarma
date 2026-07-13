// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

#include "UserInputProcessing.h"

struct DATASETTAB;

// Page2D zoom bounds in pixels per ComputerUnit, shared by every place that clamps or
// defaults the view zoom (input mapping, wheel zoom, zoom max/window, render constants,
// printing). The low bound must stay small enough that a whole imported DXF drawing
// (which can span 100,000+ drawing units) fits in the viewport at once.
constexpr float kCad2DZoomMinPixelsPerCU = 0.0001f;
constexpr float kCad2DZoomMaxPixelsPerCU = 5000.0f;

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

struct Cad2DLineRecordCPU {
    uint64_t objectId = 0;
    uint64_t containerMemoryId = 0;
    uint64_t persistedId = 0;
    uint64_t persistedParentId = 0;
    double x1 = 0.0;
    double y1 = 0.0;
    double x2 = 0.0;
    double y2 = 0.0;
    float lineWeight = 0.25f;
    Cad2DLineWeightMode lineWeightMode = Cad2DLineWeightMode::PaperMM;
    uint32_t colorABGR = 0xFF000000u;
    uint16_t schemaVersion = 0;
    bool isDeleted = false;
};

struct Cad2DPolylineRecordCPU {
    uint64_t objectId = 0;
    uint64_t containerMemoryId = 0;
    uint64_t persistedId = 0;
    uint64_t persistedParentId = 0;
    std::vector<Cad2DPoint2D> points;
    float lineWeight = 0.25f;
    Cad2DLineWeightMode lineWeightMode = Cad2DLineWeightMode::PaperMM;
    uint32_t colorABGR = 0xFF000000u;
    uint16_t schemaVersion = 0;
    bool isDeleted = false;
};

struct Cad2DPolygonRecordCPU {
    uint64_t objectId = 0;
    uint64_t containerMemoryId = 0;
    uint64_t persistedId = 0;
    uint64_t persistedParentId = 0;
    uint32_t lineSegmentCount = 4;
    double centerX = 0.0;
    double centerY = 0.0;
    double radius = 0.0;
    double rotationDegrees = 45.0;
    float lineWeight = 0.25f;
    Cad2DLineWeightMode lineWeightMode = Cad2DLineWeightMode::PaperMM;
    uint32_t colorABGR = 0xFF000000u;
    uint16_t schemaVersion = 0;
    bool isDeleted = false;
};

struct Cad2DCircleRecordCPU {
    uint64_t objectId = 0;
    uint64_t containerMemoryId = 0;
    uint64_t persistedId = 0;
    uint64_t persistedParentId = 0;
    double centerX = 0.0;
    double centerY = 0.0;
    double radius = 0.0;
    float lineWeight = 0.25f;
    Cad2DLineWeightMode lineWeightMode = Cad2DLineWeightMode::PaperMM;
    uint32_t colorABGR = 0xFF000000u;
    uint16_t schemaVersion = 0;
    bool isDeleted = false;
};

struct Cad2DEllipseRecordCPU {
    uint64_t objectId = 0;
    uint64_t containerMemoryId = 0;
    uint64_t persistedId = 0;
    uint64_t persistedParentId = 0;
    double centerX = 0.0;
    double centerY = 0.0;
    double radiusX = 0.0;
    double radiusY = 0.0;
    double rotationRadians = 0.0; // CCW rotation of the radius axes about the center.
    float lineWeight = 0.25f;
    Cad2DLineWeightMode lineWeightMode = Cad2DLineWeightMode::PaperMM;
    uint32_t colorABGR = 0xFF000000u;
    uint16_t schemaVersion = 0;
    bool isDeleted = false;
};

struct Cad2DArcRecordCPU {
    uint64_t objectId = 0;
    uint64_t containerMemoryId = 0;
    uint64_t persistedId = 0;
    uint64_t persistedParentId = 0;
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
    uint16_t schemaVersion = 0;
    bool isDeleted = false;
};

struct Cad2DTextRecordCPU {
    uint64_t objectId = 0;
    uint64_t containerMemoryId = 0;
    uint64_t persistedId = 0;
    uint64_t persistedParentId = 0;
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
    uint16_t schemaVersion = 0;
    bool isDeleted = false;
};

enum class CommandToCopyThread2DType : uint8_t {
    AddLine = 0,
    AddText = 1,
    AddPolyline = 2,
    AddPolygon = 3,
    AddCircle = 4,
    AddEllipse = 5,
    AddArc = 6,
    SelectionRefresh = 7 // No geometry; forces a page rebuild so selection flags re-apply.
};

// GPU record 'flags' bit set for the currently selected 2D objects; the 2D vertex shaders read it
// and override the stroke color to the deep-blue selection color. See selection.md.
constexpr uint32_t kCad2DSelectedFlag = 1u;

// ---------- GPU record ABI layouts (shared by the HLSL / future SPIR-V / MSL shaders) ----------
// Byte-exact shader input layouts, identical on every platform. The static_asserts are the
// cross-platform ABI contract - do not change sizes without updating every Shader2D_* shader.

// Two floats spelled portably (same layout as HLSL float2 / DirectX::XMFLOAT2 / GLSL vec2).
struct Cad2DFloat2 {
    float x;
    float y;
};

struct Cad2DLineGPURecord {
    float x1;
    float y1;
    float x2;
    float y2;
    float lineWeight;
    uint32_t lineWeightMode;
    uint32_t colorABGR;
    uint32_t flags;
};
static_assert(sizeof(Cad2DLineGPURecord) == 32, "Cad2DLineGPURecord must be 32 bytes.");

struct Cad2DCurveGPURecord {
    float centerX;
    float centerY;
    float radiusX;
    float radiusY;
    float startX;
    float startY;
    float endX;
    float endY;
    float lineWeight;
    uint32_t lineWeightMode;
    uint32_t colorABGR;
    uint32_t curveType;
    uint32_t flags;
    float rotationRadians; // CCW rotation of the radius axes about the center.
    uint32_t padding1;
    uint32_t padding2;
};
static_assert(sizeof(Cad2DCurveGPURecord) == 64, "Cad2DCurveGPURecord must be 64 bytes.");

struct Cad2DTextVertex {
    float x;
    float y;
    float u;
    float v;
    uint32_t colorABGR;
    uint32_t atlasIndex;
};
static_assert(sizeof(Cad2DTextVertex) == 24, "Cad2DTextVertex must match Shader2D_TextVertex input.");

struct Cad2DViewConstants {
    Cad2DFloat2 viewCenterCU;
    float zoomPixelsPerCU;
    float dpiY;
    Cad2DFloat2 viewportSizePx;
    float minLineWeightPx;
    float padding0;
};
static_assert(sizeof(Cad2DViewConstants) == 32, "Cad2DViewConstants must stay 16-byte aligned.");

struct Cad2DViewState {
    std::atomic<double> centerXCU{ 0.0 };
    std::atomic<double> centerYCU{ 0.0 };
    std::atomic<float> zoomPixelsPerCU{ 2.0f };

    // Back to the default view. Used when a sub-tab slot is (re)assigned to a Page2D so a recycled
    // slot does not inherit the previous Page2D's pan/zoom.
    void Reset() {
        centerXCU.store(0.0, std::memory_order_release);
        centerYCU.store(0.0, std::memory_order_release);
        zoomPixelsPerCU.store(2.0f, std::memory_order_release);
    }
};

// Modal transforms applied to the current 2D selection of the active Page2D. Armed by the ribbon
// EDIT_* buttons; the next mouse clicks provide the reference points (2 for Copy/Offset/Mirror/
// Move, 3 for Rotate: center, start line, end line). Move/Rotate edit in place; Copy/Offset/
// Mirror create new objects and keep the source.
enum class Cad2DTransformKind : uint32_t {
    None = 0,
    Copy = 1,
    Offset = 2,
    Mirror = 3,
    Rotate = 4,
    Move = 5
};

struct CommandToCopyThread2D {
    CommandToCopyThread2DType type = CommandToCopyThread2DType::AddLine;
    uint64_t id = 0;
    uint64_t tabID = 0;
    uint64_t containerMemoryId = 0;
    Cad2DLineRecordCPU line;
    Cad2DPolylineRecordCPU polyline;
    Cad2DPolygonRecordCPU polygon;
    Cad2DCircleRecordCPU circle;
    Cad2DEllipseRecordCPU ellipse;
    Cad2DArcRecordCPU arc;
    Cad2DTextRecordCPU text;
};

void EnqueueCad2DLine(uint64_t tabID, uint64_t containerMemoryId, Cad2DLineRecordCPU line);
void EnqueueCad2DPolyline(uint64_t tabID, uint64_t containerMemoryId, Cad2DPolylineRecordCPU polyline);
void EnqueueCad2DPolygon(uint64_t tabID, uint64_t containerMemoryId, Cad2DPolygonRecordCPU polygon);
void EnqueueCad2DCircle(uint64_t tabID, uint64_t containerMemoryId, Cad2DCircleRecordCPU circle);
void EnqueueCad2DEllipse(uint64_t tabID, uint64_t containerMemoryId, Cad2DEllipseRecordCPU ellipse);
void EnqueueCad2DArc(uint64_t tabID, uint64_t containerMemoryId, Cad2DArcRecordCPU arc);
void EnqueueCad2DText(uint64_t tabID, uint64_t containerMemoryId, Cad2DTextRecordCPU text);
void EnqueueCad2DSelectionRefresh(uint64_t tabID, uint64_t containerMemoryId);
bool HasPendingCad2DCopyCommands();
void PopAllCad2DCopyCommands(std::vector<CommandToCopyThread2D>& outCommands);

uint64_t Cad2DFindTargetPage2DMemoryId(DATASETTAB& tab);
bool Cad2DIsActivePage2D(DATASETTAB& tab);
void Cad2DCancelCreation(DATASETTAB& tab);
void Cad2DBeginLineCreation(DATASETTAB& tab);
void Cad2DBeginPolylineCreation(DATASETTAB& tab);
void Cad2DBeginPolygonCreation(DATASETTAB& tab);
void Cad2DBeginCircleCreation(DATASETTAB& tab);
void Cad2DBeginEllipseCreation(DATASETTAB& tab);
void Cad2DBeginArcCreation(DATASETTAB& tab);
void Cad2DBeginTextCreation(DATASETTAB& tab);
// Arms a selection transform (Cad2DTransformKind) on the active Page2D. No-op when the 2D
// selection is empty. The following clicks are consumed by Cad2DHandleInput; ESC cancels.
void Cad2DBeginTransform2D(DATASETTAB& tab, Cad2DTransformKind kind);
bool Cad2DHandleInput(DATASETTAB& tab, const ACTION_DETAILS& input);
void Cad2DAutoGenerateDemoContent(DATASETTAB& tab);
// Zoom Max / Zoom Focus: recenter the view on the objects of the active Page2D and rescale
// zoomPixelsPerCU so they fit the viewport. selectedOnly limits the fit to the current 2D
// selection (falls back to all objects when nothing is selected).
void Cad2DZoomToExtents(DATASETTAB& tab, bool selectedOnly);
// Zoom Window: the two clicked pixels define a rectangle; the view recenters on it and the zoom
// grows so the rectangle fills the viewport.
void Cad2DZoomToWindow(DATASETTAB& tab, int x0, int y0, int x1, int y1);
