// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

#include "CommonNamedNumbers.h"   // VishwakarmaStorage::ObjectType
#include "UserInputProcessing.h"
/* The nine Cad2D*RecordCPU engineering records, Cad2DPoint2D and the two presentation enums used
to be declared HERE. They are the 2D object model, not a rendering concern, so they moved to the
2D data header alongside the original 2D schema - matching the 3D half, where RenderScene3D.h
defines no engineering object at all and the types live in the 3D data header. This file still
needs them because CommandToCopyThread2D carries records by value. */
#include "डेटा-सामान्य-2D.h"

struct DATASETTAB;
struct PropertyTypeDescriptor;

// Page2D zoom bounds in pixels per ComputerUnit, shared by every place that clamps or
// defaults the view zoom (input mapping, wheel zoom, zoom max/window, render constants,
// printing). The low bound must stay small enough that a whole imported DXF drawing
// (which can span 100,000+ drawing units) fits in the viewport at once.
constexpr float kCad2DZoomMinPixelsPerCU = 0.0001f;
constexpr float kCad2DZoomMaxPixelsPerCU = 5000.0f;

enum class CommandToCopyThread2DType : uint8_t {
    AddLine = 0,
    AddText = 1,
    AddPolyline = 2,
    AddPolygon = 3,
    AddCircle = 4,
    AddEllipse = 5,
    AddArc = 6,
    SelectionRefresh = 7, // No geometry; forces a page rebuild so selection flags re-apply.
    ReportIngestStats = 8 // Debug diagnostics: print the container's record counts + bbox once
                          // every command queued before it has been ingested.
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
void EnqueueCad2DIngestStatsReport(uint64_t tabID, uint64_t containerMemoryId);
bool HasPendingCad2DCopyCommands();
void PopAllCad2DCopyCommands(std::vector<CommandToCopyThread2D>& outCommands);

uint64_t Cad2DFindTargetPage2DMemoryId(DATASETTAB& tab);
bool Cad2DIsActivePage2D(DATASETTAB& tab);

/* One frame's worth of Page2D selection, for the properties pane. The pane runs on a render thread
and must not hold a pointer into the record vectors, so the field values are COPIED OUT while
cpuRecordsMutex is held.

Cad2DReadPaneSelection returns false when the input view is not a Page2D at all - that is the
caller's signal to fall back to the 3D selection. It returns true with count == 0 for an empty 2D
selection, which is a different thing entirely and must not silently show 3D's selection instead. */
struct Cad2DPaneSelection {
    size_t   count = 0;                 // Objects selected in the active Page2D.
    uint64_t objectId = 0;              // Valid only when count == 1.
    VishwakarmaStorage::ObjectType objectType = VishwakarmaStorage::ObjectType::Unknown;
    const PropertyTypeDescriptor* table = nullptr;  // nullptr for the variable-arity types.
    double   values[16] = {};
    uint8_t  valueCount = 0;
};
bool Cad2DReadPaneSelection(DATASETTAB& tab, Cad2DPaneSelection& out);
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
// Converts the current 2D selection into a new asset: creates a Asset2DDefinition (random
// assetNumber, base point = bounding-box center, hidden master copies) plus the first
// Asset2DInsert, and re-parents the selected records to that insert. Drawing stays unchanged.
void Cad2DCreateAssetFromSelection(DATASETTAB& tab);
// Arms asset-insert mode: every following Page2D click places an instance of the asset selected
// in the Insert Asset pane (assetInsertSelectedDefinitionId; falls back to the first definition).
void Cad2DBeginAssetInsert(DATASETTAB& tab);
// Reusable asset instantiation, shared by the interactive Insert Asset click path and the DXF
// importer. Places one instance of the definition (by memory id) at (xCU, yCU) on containerMemoryId:
// creates a Cad2DAssetInsertRecordCPU and page copies of the definition's master records with the
// instance transform baked in (member = insert + R(rotation) * S(scale) * (master - base),
// parentObjectId = the new insert), enqueued for rendering. Negative scale mirrors along that
// axis. Returns false if the definition id is unknown or a scale is zero / non-finite. Locks the
// tab's cpuRecordsMutex internally.
bool Cad2DInstantiateAsset(DATASETTAB& tab, uint64_t containerMemoryId, uint64_t definitionObjectId,
    double xCU, double yCU, double scaleX = 1.0, double scaleY = 1.0, double rotationDegrees = 0.0);
// Creates an asset definition from imported master geometry (DXF BLOCK). The master records are
// stored hidden (containerMemoryId = 0, parentObjectId = the new definition) and never rendered;
// their coordinates stay in the block's own frame, baseX/baseY is the insert base point. Assigns a
// random unique assetNumber. Returns the new definition's memory id (0 on empty masters). Locks the
// tab's cpuRecordsMutex internally.
uint64_t Cad2DCreateAssetDefinition(DATASETTAB& tab, double baseX, double baseY,
    const std::vector<Cad2DLineRecordCPU>& masterLines,
    const std::vector<Cad2DTextRecordCPU>& masterTexts,
    const std::vector<Cad2DPolygonRecordCPU>& masterPolygons);
bool Cad2DHandleInput(DATASETTAB& tab, const ACTION_DETAILS& input);
void Cad2DAutoGenerateDemoContent(DATASETTAB& tab);
// Zoom Max / Zoom Focus: recenter the view on the objects of the active Page2D and rescale
// zoomPixelsPerCU so they fit the viewport. selectedOnly limits the fit to the current 2D
// selection (falls back to all objects when nothing is selected).
void Cad2DZoomToExtents(DATASETTAB& tab, bool selectedOnly);
// Zoom Window: the two clicked pixels define a rectangle; the view recenters on it and the zoom
// grows so the rectangle fills the viewport.
void Cad2DZoomToWindow(DATASETTAB& tab, int x0, int y0, int x1, int y1);
