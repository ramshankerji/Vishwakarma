// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
#pragma once

#include <cstdint>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

#include "UserInputProcessing.h"

struct DATASETTAB;

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
bool Cad2DHandleInput(DATASETTAB& tab, const ACTION_DETAILS& input);
void Cad2DAutoGenerateDemoContent(DATASETTAB& tab);
// Zoom Max / Zoom Focus: rescale zoomPixelsPerCU so the objects of the active Page2D fit the
// viewport. The view center stays fixed; only the zoom changes. selectedOnly limits the fit to
// the current 2D selection (falls back to all objects when nothing is selected).
void Cad2DZoomToExtents(DATASETTAB& tab, bool selectedOnly);
