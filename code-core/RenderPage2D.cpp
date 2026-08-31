// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <random>
#include <utility>

#include "CommonNamedNumbers.h"
#include "GPUPlatformSelector.h"
#include "PropertyPane.h"
#include "RenderPage2D.h"
#include "Snap.h"

#include "विश्वकर्मा.h"
#include "ID.h"

namespace {
std::mutex gCad2DCopyQueueMutex;
std::queue<CommandToCopyThread2D> gCad2DCopyQueue;
constexpr uint32_t kDefaultPolygonLineSegmentCount = 4;
constexpr double kDefaultPolygonRotationDegrees = 45.0;
constexpr double kMinPolygonRadiusCU = 1.0e-9;
constexpr double kMinCurveRadiusCU = 1.0e-9;
constexpr float kDefaultTextHeightCU = 9.0f;

void ClearLineCreationState(TabCad2DStorage& storage) {
    storage.lineCreationMode.store(false, std::memory_order_release);
    storage.lineCreationHasPreviousPoint.store(false, std::memory_order_release);
    storage.polylineCreationMode.store(false, std::memory_order_release);
    storage.polygonCreationMode.store(false, std::memory_order_release);
    storage.polygonCreationHasCenter.store(false, std::memory_order_release);
    storage.polygonCreationCenterXCU.store(0.0, std::memory_order_release);
    storage.polygonCreationCenterYCU.store(0.0, std::memory_order_release);
    storage.circleCreationMode.store(false, std::memory_order_release);
    storage.circleCreationHasCenter.store(false, std::memory_order_release);
    storage.circleCreationCenterXCU.store(0.0, std::memory_order_release);
    storage.circleCreationCenterYCU.store(0.0, std::memory_order_release);
    storage.ellipseCreationMode.store(false, std::memory_order_release);
    storage.ellipseCreationStep.store(0, std::memory_order_release);
    storage.ellipseCreationCenterXCU.store(0.0, std::memory_order_release);
    storage.ellipseCreationCenterYCU.store(0.0, std::memory_order_release);
    storage.ellipseCreationRadiusXCU.store(0.0, std::memory_order_release);
    storage.arcCreationMode.store(false, std::memory_order_release);
    storage.arcCreationStep.store(0, std::memory_order_release);
    storage.arcCreationCenterXCU.store(0.0, std::memory_order_release);
    storage.arcCreationCenterYCU.store(0.0, std::memory_order_release);
    storage.arcCreationStartXCU.store(0.0, std::memory_order_release);
    storage.arcCreationStartYCU.store(0.0, std::memory_order_release);
    storage.textCreationMode.store(false, std::memory_order_release);
    storage.textCreationHasAnchor.store(false, std::memory_order_release);
    storage.textCreationXCU.store(0.0, std::memory_order_release);
    storage.textCreationYCU.store(0.0, std::memory_order_release);
    storage.assetInsertMode.store(false, std::memory_order_release);
    storage.transform2DKind.store(0, std::memory_order_release);
    storage.transform2DStep.store(0, std::memory_order_release);
    storage.transform2DP1XCU.store(0.0, std::memory_order_release);
    storage.transform2DP1YCU.store(0.0, std::memory_order_release);
    storage.transform2DP2XCU.store(0.0, std::memory_order_release);
    storage.transform2DP2YCU.store(0.0, std::memory_order_release);
    std::lock_guard<std::mutex> lock(storage.cpuRecordsMutex);
    storage.polylineCreationObjectId = 0;
    storage.polylineCreationPoints.clear();
    storage.textCreationObjectId = 0;
    storage.textCreationDraft.clear();
}

// The Page2D pan/zoom state for the sub-tab the input currently targets. All input-driven 2D work
// (coordinate mapping, pan, wheel-zoom, zoom-extents/window, selection tolerance) goes through the
// view the user is interacting with; the render / print paths pass their displayed slot instead.
static Cad2DViewState& Cad2DInputView(DATASETTAB& tab) {
    int slot = InputViewSlot(tab);
    if (slot < 0 || slot >= MV_MAX_SUBTABS) slot = 0;
    return tab.viewports[slot].page2DView; // Pan/zoom lives in the Viewport (10M plan Step 6).
}

bool Page2DCoordinateFromInput(DATASETTAB& tab, const ACTION_DETAILS& input,
    double& outXCU, double& outYCU) {
    if (!tab.cad2d) return false;

    int viewportWidth = 0, viewportHeight = 0, viewportTop = 0;
    if (!GetVisibleSceneViewportForTab(tab, viewportWidth, viewportHeight, viewportTop)) {
        return false;
    }
    if (input.x < 0 || input.x >= viewportWidth ||
        input.y < viewportTop || input.y >= viewportTop + viewportHeight) {
        return false;
    }

    const Cad2DViewState& view = Cad2DInputView(tab);
    const double zoom = (std::max)(
        (double)view.zoomPixelsPerCU.load(std::memory_order_acquire),
        (double)kCad2DZoomMinPixelsPerCU);
    const double centerX = view.centerXCU.load(std::memory_order_acquire);
    const double centerY = view.centerYCU.load(std::memory_order_acquire);
    const double offsetX = (double)input.x - (double)viewportWidth * 0.5;
    const double offsetY = (double)viewportHeight * 0.5 - (double)(input.y - viewportTop);

    outXCU = centerX + offsetX / zoom;
    outYCU = centerY + offsetY / zoom;
    return true;
}

/* The snapped form of the mapping above, and the one every creation / transform tool takes its point
   from, so that a click lands on an exact value instead of on sixteen digits of float noise.

   Selection deliberately keeps calling the raw mapping (snapping.md locked decision 14):
   Cad2DHandleSelectionClick hit-tests with a small tolerance measured from the cursor, and a point
   already pulled onto some object's endpoint would select the wrong object.

   Defined further down, next to the record hit-testing it shares its geometry helpers with. */
static bool Page2DSnappedPointFromInput(DATASETTAB& tab, const ACTION_DETAILS& input,
    double& outXCU, double& outYCU, SnapResult* outResult = nullptr);

void HandleLineCreationClick(DATASETTAB& tab, double xCU, double yCU) {
    TabCad2DStorage& storage = *tab.cad2d;
    if (!storage.lineCreationHasPreviousPoint.load(std::memory_order_acquire)) {
        storage.lineCreationPreviousXCU.store(xCU, std::memory_order_release);
        storage.lineCreationPreviousYCU.store(yCU, std::memory_order_release);
        storage.lineCreationHasPreviousPoint.store(true, std::memory_order_release);
        return;
    }

    const uint64_t page2DMemoryId = Cad2DFindTargetPage2DMemoryId(tab);
    if (page2DMemoryId == 0) return;

    Cad2DLineRecordCPU line{};
    line.memoryIDContainer = page2DMemoryId;
    line.x1 = storage.lineCreationPreviousXCU.load(std::memory_order_acquire);
    line.y1 = storage.lineCreationPreviousYCU.load(std::memory_order_acquire);
    line.x2 = xCU;
    line.y2 = yCU;
    line.lineWeight = 1.0f;
    line.lineWeightMode = Cad2DLineWeightMode::ScreenPixel;
    line.colorABGR = 0xFF000000u;
    line.schemaVersion = VishwakarmaStorage::kGeometry2DLineSchemaVersion;
    EnqueueCad2DLine(tab.tabID, page2DMemoryId, line);

    storage.lineCreationPreviousXCU.store(xCU, std::memory_order_release);
    storage.lineCreationPreviousYCU.store(yCU, std::memory_order_release);
}

void HandlePolylineCreationClick(DATASETTAB& tab, double xCU, double yCU) {
    const uint64_t page2DMemoryId = Cad2DFindTargetPage2DMemoryId(tab);
    if (page2DMemoryId == 0) return;

    Cad2DPolylineRecordCPU polyline{};
    bool shouldEnqueue = false;
    {
        TabCad2DStorage& storage = *tab.cad2d;
        std::lock_guard<std::mutex> lock(storage.cpuRecordsMutex);
        storage.polylineCreationPoints.push_back({ xCU, yCU });
        if (storage.polylineCreationPoints.size() < 2) return;

        if (storage.polylineCreationObjectId == 0) {
            storage.polylineCreationObjectId = MemoryID::next();
        }

        polyline.memoryID = storage.polylineCreationObjectId;
        polyline.memoryIDContainer = page2DMemoryId;
        polyline.points = storage.polylineCreationPoints;
        polyline.lineWeight = 1.0f;
        polyline.lineWeightMode = Cad2DLineWeightMode::ScreenPixel;
        polyline.colorABGR = 0xFF000000u;
        polyline.schemaVersion = VishwakarmaStorage::kGeometry2DPolylineSchemaVersion;
        shouldEnqueue = true;
    }

    if (shouldEnqueue) {
        EnqueueCad2DPolyline(tab.tabID, page2DMemoryId, std::move(polyline));
    }
}

void HandlePolygonCreationClick(DATASETTAB& tab, double xCU, double yCU) {
    TabCad2DStorage& storage = *tab.cad2d;
    if (!storage.polygonCreationHasCenter.load(std::memory_order_acquire)) {
        storage.polygonCreationCenterXCU.store(xCU, std::memory_order_release);
        storage.polygonCreationCenterYCU.store(yCU, std::memory_order_release);
        storage.polygonCreationHasCenter.store(true, std::memory_order_release);
        return;
    }

    const uint64_t page2DMemoryId = Cad2DFindTargetPage2DMemoryId(tab);
    if (page2DMemoryId == 0) return;

    const double centerX = storage.polygonCreationCenterXCU.load(std::memory_order_acquire);
    const double centerY = storage.polygonCreationCenterYCU.load(std::memory_order_acquire);
    const double radius = std::hypot(xCU - centerX, yCU - centerY);
    if (radius <= kMinPolygonRadiusCU) return;

    Cad2DPolygonRecordCPU polygon{};
    polygon.memoryIDContainer = page2DMemoryId;
    polygon.lineSegmentCount = kDefaultPolygonLineSegmentCount;
    polygon.centerX = centerX;
    polygon.centerY = centerY;
    polygon.radius = radius;
    polygon.rotationDegrees = kDefaultPolygonRotationDegrees;
    polygon.lineWeight = 1.0f;
    polygon.lineWeightMode = Cad2DLineWeightMode::ScreenPixel;
    polygon.colorABGR = 0xFF000000u;
    polygon.schemaVersion = VishwakarmaStorage::kGeometry2DPolygonSchemaVersion;
    EnqueueCad2DPolygon(tab.tabID, page2DMemoryId, polygon);

    storage.polygonCreationHasCenter.store(false, std::memory_order_release);
}

void HandleCircleCreationClick(DATASETTAB& tab, double xCU, double yCU) {
    TabCad2DStorage& storage = *tab.cad2d;
    if (!storage.circleCreationHasCenter.load(std::memory_order_acquire)) {
        storage.circleCreationCenterXCU.store(xCU, std::memory_order_release);
        storage.circleCreationCenterYCU.store(yCU, std::memory_order_release);
        storage.circleCreationHasCenter.store(true, std::memory_order_release);
        return;
    }

    const uint64_t page2DMemoryId = Cad2DFindTargetPage2DMemoryId(tab);
    if (page2DMemoryId == 0) return;

    const double centerX = storage.circleCreationCenterXCU.load(std::memory_order_acquire);
    const double centerY = storage.circleCreationCenterYCU.load(std::memory_order_acquire);
    const double radius = std::hypot(xCU - centerX, yCU - centerY);
    if (radius <= kMinCurveRadiusCU) return;

    Cad2DCircleRecordCPU circle{};
    circle.memoryIDContainer = page2DMemoryId;
    circle.centerX = centerX;
    circle.centerY = centerY;
    circle.radius = radius;
    circle.lineWeight = 1.0f;
    circle.lineWeightMode = Cad2DLineWeightMode::ScreenPixel;
    circle.colorABGR = 0xFF000000u;
    circle.schemaVersion = VishwakarmaStorage::kGeometry2DCircleSchemaVersion;
    EnqueueCad2DCircle(tab.tabID, page2DMemoryId, circle);

    storage.circleCreationHasCenter.store(false, std::memory_order_release);
}

void HandleEllipseCreationClick(DATASETTAB& tab, double xCU, double yCU) {
    TabCad2DStorage& storage = *tab.cad2d;
    const uint32_t step = storage.ellipseCreationStep.load(std::memory_order_acquire);
    if (step == 0) {
        storage.ellipseCreationCenterXCU.store(xCU, std::memory_order_release);
        storage.ellipseCreationCenterYCU.store(yCU, std::memory_order_release);
        storage.ellipseCreationRadiusXCU.store(0.0, std::memory_order_release);
        storage.ellipseCreationStep.store(1, std::memory_order_release);
        return;
    }

    const double centerX = storage.ellipseCreationCenterXCU.load(std::memory_order_acquire);
    const double centerY = storage.ellipseCreationCenterYCU.load(std::memory_order_acquire);
    if (step == 1) {
        double radiusX = std::abs(xCU - centerX);
        if (radiusX <= kMinCurveRadiusCU) radiusX = std::hypot(xCU - centerX, yCU - centerY);
        if (radiusX <= kMinCurveRadiusCU) return;
        storage.ellipseCreationRadiusXCU.store(radiusX, std::memory_order_release);
        storage.ellipseCreationStep.store(2, std::memory_order_release);
        return;
    }

    double radiusY = std::abs(yCU - centerY);
    if (radiusY <= kMinCurveRadiusCU) radiusY = std::hypot(xCU - centerX, yCU - centerY);
    const double radiusX = storage.ellipseCreationRadiusXCU.load(std::memory_order_acquire);
    if (radiusX <= kMinCurveRadiusCU || radiusY <= kMinCurveRadiusCU) return;

    const uint64_t page2DMemoryId = Cad2DFindTargetPage2DMemoryId(tab);
    if (page2DMemoryId == 0) return;

    Cad2DEllipseRecordCPU ellipse{};
    ellipse.memoryIDContainer = page2DMemoryId;
    ellipse.centerX = centerX;
    ellipse.centerY = centerY;
    ellipse.radiusX = radiusX;
    ellipse.radiusY = radiusY;
    ellipse.lineWeight = 1.0f;
    ellipse.lineWeightMode = Cad2DLineWeightMode::ScreenPixel;
    ellipse.colorABGR = 0xFF000000u;
    ellipse.schemaVersion = VishwakarmaStorage::kGeometry2DEllipseSchemaVersion;
    EnqueueCad2DEllipse(tab.tabID, page2DMemoryId, ellipse);

    storage.ellipseCreationStep.store(0, std::memory_order_release);
    storage.ellipseCreationRadiusXCU.store(0.0, std::memory_order_release);
}

void HandleArcCreationClick(DATASETTAB& tab, double xCU, double yCU) {
    TabCad2DStorage& storage = *tab.cad2d;
    const uint32_t step = storage.arcCreationStep.load(std::memory_order_acquire);
    if (step == 0) {
        storage.arcCreationCenterXCU.store(xCU, std::memory_order_release);
        storage.arcCreationCenterYCU.store(yCU, std::memory_order_release);
        storage.arcCreationStep.store(1, std::memory_order_release);
        return;
    }

    const double centerX = storage.arcCreationCenterXCU.load(std::memory_order_acquire);
    const double centerY = storage.arcCreationCenterYCU.load(std::memory_order_acquire);
    if (step == 1) {
        if (std::hypot(xCU - centerX, yCU - centerY) <= kMinCurveRadiusCU) return;
        storage.arcCreationStartXCU.store(xCU, std::memory_order_release);
        storage.arcCreationStartYCU.store(yCU, std::memory_order_release);
        storage.arcCreationStep.store(2, std::memory_order_release);
        return;
    }

    const double startX = storage.arcCreationStartXCU.load(std::memory_order_acquire);
    const double startY = storage.arcCreationStartYCU.load(std::memory_order_acquire);
    const double radius = std::hypot(startX - centerX, startY - centerY);
    const double endDistance = std::hypot(xCU - centerX, yCU - centerY);
    if (radius <= kMinCurveRadiusCU || endDistance <= kMinCurveRadiusCU) return;

    const double startAngle = std::atan2(startY - centerY, startX - centerX);
    const double endAngle = std::atan2(yCU - centerY, xCU - centerX);
    if (std::abs(std::sin((endAngle - startAngle) * 0.5)) <= 1.0e-7) return;

    Cad2DArcRecordCPU arc{};
    arc.memoryIDContainer = Cad2DFindTargetPage2DMemoryId(tab);
    if (arc.memoryIDContainer == 0) return;
    arc.centerX = centerX;
    arc.centerY = centerY;
    arc.radiusX = radius;
    arc.radiusY = radius;
    arc.startX = startX;
    arc.startY = startY;
    arc.endX = centerX + std::cos(endAngle) * radius;
    arc.endY = centerY + std::sin(endAngle) * radius;
    arc.lineWeight = 1.0f;
    arc.lineWeightMode = Cad2DLineWeightMode::ScreenPixel;
    arc.colorABGR = 0xFF000000u;
    arc.schemaVersion = VishwakarmaStorage::kGeometry2DArcSchemaVersion;
    EnqueueCad2DArc(tab.tabID, arc.memoryIDContainer, arc);

    storage.arcCreationStep.store(0, std::memory_order_release);
}

void HandleTextCreationClick(DATASETTAB& tab, double xCU, double yCU) {
    TabCad2DStorage& storage = *tab.cad2d;
    storage.textCreationXCU.store(xCU, std::memory_order_release);
    storage.textCreationYCU.store(yCU, std::memory_order_release);
    storage.textCreationHasAnchor.store(true, std::memory_order_release);

    std::lock_guard<std::mutex> lock(storage.cpuRecordsMutex);
    if (storage.textCreationObjectId != 0 || !storage.textCreationDraft.empty()) {
        storage.textCreationObjectId = 0;
        storage.textCreationDraft.clear();
    }
}

void EnqueueTextCreationDraft(DATASETTAB& tab, uint64_t page2DMemoryId, uint64_t objectId,
    double xCU, double yCU, std::string textValue) {
    Cad2DTextRecordCPU text{};
    text.memoryID = objectId;
    text.memoryIDContainer = page2DMemoryId;
    text.x = xCU;
    text.y = yCU;
    text.textHeightCU = kDefaultTextHeightCU;
    text.rotationRadians = 0.0f;
    text.colorABGR = 0xFF000000u;
    text.font = 0;
    text.justification = Cad2DTextJustification::Center;
    text.text = std::move(textValue);
    text.schemaVersion = VishwakarmaStorage::kGeometry2DTextSchemaVersion;
    EnqueueCad2DText(tab.tabID, page2DMemoryId, std::move(text));
}

bool HandleTextCreationChar(DATASETTAB& tab, int charCode) {
    TabCad2DStorage& storage = *tab.cad2d;
    if (charCode == '\r' || charCode == '\n') {
        storage.textCreationHasAnchor.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(storage.cpuRecordsMutex);
        storage.textCreationObjectId = 0;
        storage.textCreationDraft.clear();
        return true;
    }

    if (!storage.textCreationHasAnchor.load(std::memory_order_acquire)) {
        return true;
    }

    const uint64_t page2DMemoryId = Cad2DFindTargetPage2DMemoryId(tab);
    if (page2DMemoryId == 0) return true;

    uint64_t objectId = 0;
    std::string draft;
    bool shouldEnqueue = false;
    {
        std::lock_guard<std::mutex> lock(storage.cpuRecordsMutex);
        if (charCode == '\b') {
            if (storage.textCreationDraft.empty()) return true;
            storage.textCreationDraft.pop_back();
            shouldEnqueue = storage.textCreationObjectId != 0;
        }
        else if (charCode == '\t') {
            storage.textCreationDraft.append("    ");
            shouldEnqueue = true;
        }
        else if (charCode >= 32 && charCode <= 126) {
            storage.textCreationDraft.push_back(static_cast<char>(charCode));
            shouldEnqueue = true;
        }
        else {
            return true;
        }

        if (shouldEnqueue && storage.textCreationObjectId == 0) {
            storage.textCreationObjectId = MemoryID::next();
        }
        objectId = storage.textCreationObjectId;
        draft = storage.textCreationDraft;
    }

    if (shouldEnqueue && objectId != 0) {
        const double xCU = storage.textCreationXCU.load(std::memory_order_acquire);
        const double yCU = storage.textCreationYCU.load(std::memory_order_acquire);
        EnqueueTextCreationDraft(tab, page2DMemoryId, objectId, xCU, yCU, std::move(draft));
    }
    return true;
}
}

void EnqueueCad2DLine(uint64_t tabID, uint64_t containerMemoryId, Cad2DLineRecordCPU line) {
    // The `if (objectId == 0) assign` every EnqueueCad2D* used to open with is gone: META_DATA
    // issues the id in its constructor, so every record already carries one. Callers that used to
    // pass 0 to mean "give me a fresh one" now take a fresh id themselves, at their own sites.
    line.memoryIDContainer = containerMemoryId;
#ifdef _DEBUG
    // Corruption checkpoint: coordinates must be sane when the producer hands them over.
    if (std::abs(line.x1) > 1.0e8 || std::abs(line.y1) > 1.0e8 ||
        std::abs(line.x2) > 1.0e8 || std::abs(line.y2) > 1.0e8) {
        std::cout << "[cad2d][dbg] OUTLIER AT ENQUEUE line objectId=" << line.memoryID
                  << " container=" << containerMemoryId << " (" << line.x1 << ", " << line.y1
                  << ") -> (" << line.x2 << ", " << line.y2 << ")" << std::endl;
    }
#endif

    CommandToCopyThread2D command{};
    command.type = CommandToCopyThread2DType::AddLine;
    command.id = line.memoryID;
    command.tabID = tabID;
    command.containerMemoryId = containerMemoryId;
    command.line = line;

    {
        std::lock_guard<std::mutex> lock(gCad2DCopyQueueMutex);
        gCad2DCopyQueue.push(std::move(command));
    }
    toCopyThreadCV.notify_one();
}

void EnqueueCad2DPolyline(uint64_t tabID, uint64_t containerMemoryId, Cad2DPolylineRecordCPU polyline) {
    polyline.memoryIDContainer = containerMemoryId;

    CommandToCopyThread2D command{};
    command.type = CommandToCopyThread2DType::AddPolyline;
    command.id = polyline.memoryID;
    command.tabID = tabID;
    command.containerMemoryId = containerMemoryId;
    command.polyline = std::move(polyline);

    {
        std::lock_guard<std::mutex> lock(gCad2DCopyQueueMutex);
        gCad2DCopyQueue.push(std::move(command));
    }
    toCopyThreadCV.notify_one();
}

void EnqueueCad2DPolygon(uint64_t tabID, uint64_t containerMemoryId, Cad2DPolygonRecordCPU polygon) {
    polygon.memoryIDContainer = containerMemoryId;

    CommandToCopyThread2D command{};
    command.type = CommandToCopyThread2DType::AddPolygon;
    command.id = polygon.memoryID;
    command.tabID = tabID;
    command.containerMemoryId = containerMemoryId;
    command.polygon = polygon;

    {
        std::lock_guard<std::mutex> lock(gCad2DCopyQueueMutex);
        gCad2DCopyQueue.push(std::move(command));
    }
    toCopyThreadCV.notify_one();
}

void EnqueueCad2DCircle(uint64_t tabID, uint64_t containerMemoryId, Cad2DCircleRecordCPU circle) {
    circle.memoryIDContainer = containerMemoryId;

    CommandToCopyThread2D command{};
    command.type = CommandToCopyThread2DType::AddCircle;
    command.id = circle.memoryID;
    command.tabID = tabID;
    command.containerMemoryId = containerMemoryId;
    command.circle = circle;

    {
        std::lock_guard<std::mutex> lock(gCad2DCopyQueueMutex);
        gCad2DCopyQueue.push(std::move(command));
    }
    toCopyThreadCV.notify_one();
}

void EnqueueCad2DEllipse(uint64_t tabID, uint64_t containerMemoryId, Cad2DEllipseRecordCPU ellipse) {
    ellipse.memoryIDContainer = containerMemoryId;

    CommandToCopyThread2D command{};
    command.type = CommandToCopyThread2DType::AddEllipse;
    command.id = ellipse.memoryID;
    command.tabID = tabID;
    command.containerMemoryId = containerMemoryId;
    command.ellipse = ellipse;

    {
        std::lock_guard<std::mutex> lock(gCad2DCopyQueueMutex);
        gCad2DCopyQueue.push(std::move(command));
    }
    toCopyThreadCV.notify_one();
}

void EnqueueCad2DArc(uint64_t tabID, uint64_t containerMemoryId, Cad2DArcRecordCPU arc) {
    arc.memoryIDContainer = containerMemoryId;

    CommandToCopyThread2D command{};
    command.type = CommandToCopyThread2DType::AddArc;
    command.id = arc.memoryID;
    command.tabID = tabID;
    command.containerMemoryId = containerMemoryId;
    command.arc = arc;

    {
        std::lock_guard<std::mutex> lock(gCad2DCopyQueueMutex);
        gCad2DCopyQueue.push(std::move(command));
    }
    toCopyThreadCV.notify_one();
}

void EnqueueCad2DText(uint64_t tabID, uint64_t containerMemoryId, Cad2DTextRecordCPU text) {
    text.memoryIDContainer = containerMemoryId;

    CommandToCopyThread2D command{};
    command.type = CommandToCopyThread2DType::AddText;
    command.id = text.memoryID;
    command.tabID = tabID;
    command.containerMemoryId = containerMemoryId;
    command.text = std::move(text);

    {
        std::lock_guard<std::mutex> lock(gCad2DCopyQueueMutex);
        gCad2DCopyQueue.push(std::move(command));
    }
    toCopyThreadCV.notify_one();
}

void EnqueueCad2DSelectionRefresh(uint64_t tabID, uint64_t containerMemoryId) {
    /* A wake-up, and nothing more. The copy thread diffs the tab's selection set against what it
    last stamped and writes kCad2DSelectedFlag into the affected records - a 4-byte store per
    record - so this command carries no work of its own and its containerMemoryId is diagnostic
    only. */
    CommandToCopyThread2D command{};
    command.type = CommandToCopyThread2DType::SelectionRefresh;
    command.tabID = tabID;
    command.containerMemoryId = containerMemoryId;

    {
        std::lock_guard<std::mutex> lock(gCad2DCopyQueueMutex);
        gCad2DCopyQueue.push(std::move(command));
    }
    toCopyThreadCV.notify_one();
}

void EnqueueCad2DIngestStatsReport(uint64_t tabID, uint64_t containerMemoryId) {
    // Debug diagnostics: FIFO ordering guarantees the copy thread sees this only after
    // ingesting every element queued before it, then prints the container's counts + bbox.
    CommandToCopyThread2D command{};
    command.type = CommandToCopyThread2DType::ReportIngestStats;
    command.tabID = tabID;
    command.containerMemoryId = containerMemoryId;

    {
        std::lock_guard<std::mutex> lock(gCad2DCopyQueueMutex);
        gCad2DCopyQueue.push(std::move(command));
    }
    toCopyThreadCV.notify_one();
}

bool HasPendingCad2DCopyCommands() {
    std::lock_guard<std::mutex> lock(gCad2DCopyQueueMutex);
    return !gCad2DCopyQueue.empty();
}

void PopAllCad2DCopyCommands(std::vector<CommandToCopyThread2D>& outCommands) {
    std::lock_guard<std::mutex> lock(gCad2DCopyQueueMutex);
    while (!gCad2DCopyQueue.empty()) {
        outCommands.push_back(std::move(gCad2DCopyQueue.front()));
        gCad2DCopyQueue.pop();
    }
}

uint64_t Cad2DFindTargetPage2DMemoryId(DATASETTAB& tab) {
    if (!tab.storageObjectsMutex) return 0;

    // Keyed on the input view (inline-active sub-tab, or the extracted view the user last
    // interacted with), so 2D tools follow the view actually receiving the input.
    const uint64_t inputContainerId = InputViewContainerId(tab);

    std::lock_guard<std::mutex> lock(*tab.storageObjectsMutex);
    if (inputContainerId != 0) {
        ResolvedObject active = FindLogicalObject(tab, inputContainerId);
        if (active && active.objectType == VishwakarmaStorage::ObjectType::Page2D) {
            return active.object->memoryID;
        }
    }

    for (const StoredLogicalObject& entry : tab.storageLogicalObjects) {
        if (entry.objectType == VishwakarmaStorage::ObjectType::Page2D && entry.object) {
            return entry.object->memoryID;
        }
    }
    return 0;
}

bool Cad2DIsActivePage2D(DATASETTAB& tab) {
    if (!tab.storageObjectsMutex) return false;

    const uint64_t inputContainerId = InputViewContainerId(tab); // Input view, not just inline-active.

    std::lock_guard<std::mutex> lock(*tab.storageObjectsMutex);
    if (inputContainerId == 0) return false;

    ResolvedObject active = FindLogicalObject(tab, inputContainerId);
    return active && active.objectType == VishwakarmaStorage::ObjectType::Page2D;
}

void Cad2DCancelCreation(DATASETTAB& tab) {
    if (!tab.cad2d) return;
    ClearLineCreationState(*tab.cad2d);
}

void Cad2DBeginLineCreation(DATASETTAB& tab) {
    if (!tab.cad2d || !Cad2DIsActivePage2D(tab)) return;

    ClearLineCreationState(*tab.cad2d);
    tab.cad2d->lineCreationMode.store(true, std::memory_order_release);
    tab.cad2d->lineCreationHasPreviousPoint.store(false, std::memory_order_release);
}

void Cad2DBeginPolylineCreation(DATASETTAB& tab) {
    if (!tab.cad2d || !Cad2DIsActivePage2D(tab)) return;

    ClearLineCreationState(*tab.cad2d);
    tab.cad2d->polylineCreationMode.store(true, std::memory_order_release);
}

void Cad2DBeginPolygonCreation(DATASETTAB& tab) {
    if (!tab.cad2d || !Cad2DIsActivePage2D(tab)) return;

    ClearLineCreationState(*tab.cad2d);
    tab.cad2d->polygonCreationMode.store(true, std::memory_order_release);
    tab.cad2d->polygonCreationHasCenter.store(false, std::memory_order_release);
}

void Cad2DBeginCircleCreation(DATASETTAB& tab) {
    if (!tab.cad2d || !Cad2DIsActivePage2D(tab)) return;

    ClearLineCreationState(*tab.cad2d);
    tab.cad2d->circleCreationMode.store(true, std::memory_order_release);
    tab.cad2d->circleCreationHasCenter.store(false, std::memory_order_release);
}

void Cad2DBeginEllipseCreation(DATASETTAB& tab) {
    if (!tab.cad2d || !Cad2DIsActivePage2D(tab)) return;

    ClearLineCreationState(*tab.cad2d);
    tab.cad2d->ellipseCreationMode.store(true, std::memory_order_release);
    tab.cad2d->ellipseCreationStep.store(0, std::memory_order_release);
}

void Cad2DBeginArcCreation(DATASETTAB& tab) {
    if (!tab.cad2d || !Cad2DIsActivePage2D(tab)) return;

    ClearLineCreationState(*tab.cad2d);
    tab.cad2d->arcCreationMode.store(true, std::memory_order_release);
    tab.cad2d->arcCreationStep.store(0, std::memory_order_release);
}

void Cad2DBeginTextCreation(DATASETTAB& tab) {
    if (!tab.cad2d || !Cad2DIsActivePage2D(tab)) return;

    ClearLineCreationState(*tab.cad2d);
    tab.cad2d->textCreationMode.store(true, std::memory_order_release);
    tab.cad2d->textCreationHasAnchor.store(false, std::memory_order_release);
}

void Cad2DBeginTransform2D(DATASETTAB& tab, Cad2DTransformKind kind) {
    if (!tab.cad2d || kind == Cad2DTransformKind::None || !Cad2DIsActivePage2D(tab)) return;
    {
        std::lock_guard<std::mutex> lock(tab.cad2d->selection2DMutex);
        if (tab.cad2d->selectedObjectIds.empty()) return; // Nothing selected to transform.
    }
    ClearLineCreationState(*tab.cad2d); // Also resets the transform state; step restarts at 0.
    tab.cad2d->transform2DKind.store(static_cast<uint32_t>(kind), std::memory_order_release);
}

// Random user-visible asset number (6 digits), unique within this tab's definitions.
// Caller must hold s.cpuRecordsMutex.
static uint32_t GenerateUniqueAssetNumberLocked(TabCad2DStorage& s) {
    std::mt19937 rng{ std::random_device{}() };
    std::uniform_int_distribution<uint32_t> dist(100000u, 999999u);
    uint32_t assetNumber = 0;
    do {
        assetNumber = dist(rng);
        for (const Cad2DAssetDefinitionRecordCPU& d : s.assetDefinitionRecords) {
            if (!d.isDeleted && d.assetNumber == assetNumber) { assetNumber = 0; break; }
        }
    } while (assetNumber == 0);
    return assetNumber;
}

void Cad2DCreateAssetFromSelection(DATASETTAB& tab) {
    if (!tab.cad2d || !Cad2DIsActivePage2D(tab)) return;
    TabCad2DStorage& s = *tab.cad2d;
    const uint64_t container = Cad2DFindTargetPage2DMemoryId(tab);
    if (container == 0) return;

    std::unordered_set<uint64_t> selected;
    {
        std::lock_guard<std::mutex> lock(s.selection2DMutex);
        selected = s.selectedObjectIds;
    }
    if (selected.empty()) return;

    std::lock_guard<std::mutex> lock(s.cpuRecordsMutex);

    auto wanted = [&](const auto& r) {
        return !r.isDeleted && r.memoryIDContainer == container &&
            selected.count(r.memoryID) != 0;
    };

    // Bounding box of the selected objects; its middle becomes the asset insert base point.
    double minX = 0.0, minY = 0.0, maxX = 0.0, maxY = 0.0;
    bool hasBounds = false;
    auto include = [&](double x, double y) {
        if (!hasBounds) { minX = maxX = x; minY = maxY = y; hasBounds = true; return; }
        minX = (std::min)(minX, x); maxX = (std::max)(maxX, x);
        minY = (std::min)(minY, y); maxY = (std::max)(maxY, y);
    };
    for (const Cad2DLineRecordCPU& r : s.lineRecords) {
        if (!wanted(r)) continue;
        include(r.x1, r.y1); include(r.x2, r.y2);
    }
    for (const Cad2DPolylineRecordCPU& r : s.polylineRecords) {
        if (!wanted(r)) continue;
        for (const Cad2DPoint2D& p : r.points) include(p.x, p.y);
    }
    for (const Cad2DPolygonRecordCPU& r : s.polygonRecords) {
        if (!wanted(r)) continue;
        include(r.centerX - r.radius, r.centerY - r.radius);
        include(r.centerX + r.radius, r.centerY + r.radius);
    }
    for (const Cad2DCircleRecordCPU& r : s.circleRecords) {
        if (!wanted(r)) continue;
        include(r.centerX - r.radius, r.centerY - r.radius);
        include(r.centerX + r.radius, r.centerY + r.radius);
    }
    for (const Cad2DEllipseRecordCPU& r : s.ellipseRecords) {
        if (!wanted(r)) continue;
        const double c = std::cos(r.rotationRadians), sn = std::sin(r.rotationRadians);
        const double hx = std::sqrt(r.radiusX * c * r.radiusX * c + r.radiusY * sn * r.radiusY * sn);
        const double hy = std::sqrt(r.radiusX * sn * r.radiusX * sn + r.radiusY * c * r.radiusY * c);
        include(r.centerX - hx, r.centerY - hy);
        include(r.centerX + hx, r.centerY + hy);
    }
    for (const Cad2DArcRecordCPU& r : s.arcRecords) {
        if (!wanted(r)) continue;
        const double radius = (std::max)(std::abs(r.radiusX), std::abs(r.radiusY));
        include(r.centerX - radius, r.centerY - radius); // Full-ellipse box; conservative for partial arcs.
        include(r.centerX + radius, r.centerY + radius);
    }
    for (const Cad2DTextRecordCPU& r : s.textRecords) {
        if (!wanted(r)) continue;
        include(r.x, r.y); include(r.x, r.y + (double)r.textHeightCU);
    }
    if (!hasBounds) return;

    Cad2DAssetDefinitionRecordCPU definition{};
    definition.memoryID = MemoryID::next();
    definition.assetNumber = GenerateUniqueAssetNumberLocked(s);
    definition.baseX = (minX + maxX) * 0.5;
    definition.baseY = (minY + maxY) * 0.5;
    definition.schemaVersion = VishwakarmaStorage::kAsset2DDefinitionSchemaVersion;

    // The originals become the first placed instance; drawing stays exactly as it is.
    Cad2DAssetInsertRecordCPU firstInsert{};
    firstInsert.memoryID = MemoryID::next();
    firstInsert.memoryIDContainer = container;
    firstInsert.definitionObjectId = definition.memoryID;
    firstInsert.x = definition.baseX;
    firstInsert.y = definition.baseY;
    firstInsert.schemaVersion = VishwakarmaStorage::kAsset2DInsertSchemaVersion;

    // Hidden master copies keep the source page coordinates (inserts translate by click - base).
    // containerMemoryId = 0: no page owns them, so they never render, hit-test or zoom-fit.
    auto convert = [&](auto& records) {
        const size_t originalCount = records.size();
        for (size_t i = 0; i < originalCount; ++i) {
            if (!wanted(records[i])) continue;
            auto master = records[i];
            master.memoryID = MemoryID::next();
            master.persistedId = 0;
            master.persistedParentId = 0;
            master.memoryIDGenerator = definition.memoryID;
            master.memoryIDContainer = 0;
            tab.allIDsInThisTab.push_back(master.memoryID);
            records.push_back(std::move(master)); // May reallocate; re-index the original below.
            Cad2DIndexAppendedRecord(s, records); // Third appender to the record vectors.
            records[i].memoryIDGenerator = firstInsert.memoryID;
        }
    };
    convert(s.lineRecords);
    convert(s.polylineRecords);
    convert(s.polygonRecords);
    convert(s.circleRecords);
    convert(s.ellipseRecords);
    convert(s.arcRecords);
    convert(s.textRecords);

    s.assetDefinitionRecords.push_back(definition);
    Cad2DIndexAppendedRecord(s, s.assetDefinitionRecords);
    s.assetInsertRecords.push_back(firstInsert);
    Cad2DIndexAppendedRecord(s, s.assetInsertRecords);
    tab.allIDsInThisTab.push_back(definition.memoryID);
    tab.allIDsInThisTab.push_back(firstInsert.memoryID);
}

void Cad2DBeginAssetInsert(DATASETTAB& tab) {
    if (!tab.cad2d || !Cad2DIsActivePage2D(tab)) return;
    {
        std::lock_guard<std::mutex> lock(tab.cad2d->cpuRecordsMutex);
        bool anyDefinition = false;
        for (const Cad2DAssetDefinitionRecordCPU& d : tab.cad2d->assetDefinitionRecords) {
            if (!d.isDeleted) { anyDefinition = true; break; }
        }
        if (!anyDefinition) return; // No asset to insert yet.
    }
    ClearLineCreationState(*tab.cad2d);
    tab.cad2d->assetInsertMode.store(true, std::memory_order_release);
}

// --- 2D CPU hit-testing for click-selection (see selection.md) ----------------------------------
namespace {
double DistPointToSegment(double px, double py, double ax, double ay, double bx, double by) {
    const double vx = bx - ax, vy = by - ay;
    const double wx = px - ax, wy = py - ay;
    const double len2 = vx * vx + vy * vy;
    double t = len2 > 1.0e-12 ? (wx * vx + wy * vy) / len2 : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    const double dx = px - (ax + t * vx), dy = py - (ay + t * vy);
    return std::sqrt(dx * dx + dy * dy);
}

double DistPointToCircle(double px, double py, double cx, double cy, double radius) {
    return std::abs(std::sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy)) - radius);
}

// Rough distance to a (possibly rotated) ellipse boundary; adequate for pick tolerance.
double DistPointToEllipse(double px, double py, double cx, double cy, double rx, double ry,
    double rotationRadians) {
    const double sx = (std::max)(std::abs(rx), 1.0e-9);
    const double sy = (std::max)(std::abs(ry), 1.0e-9);
    const double dx = px - cx, dy = py - cy;
    const double c = std::cos(rotationRadians), s = std::sin(rotationRadians);
    const double lx = dx * c + dy * s;  // Un-rotate into the curve's local frame.
    const double ly = -dx * s + dy * c;
    const double nx = lx / sx, ny = ly / sy;
    const double r = std::sqrt(nx * nx + ny * ny);
    return std::abs(r - 1.0) * (std::min)(sx, sy);
}

// --- Snapping (website/content/software/snapping.md) --------------------------------------------
/* What this implements for the Page2D: the priority-then-distance resolution rule of section 5,
the per-record snap point tables of section 7, the derived anchor-relative snaps of section 6, the
ortho constraint of section 11, and the always-on ambient grid that guarantees every click lands on
a defined value (locked decision 5).

The broad phase is the linear scan section 8 knowingly ships: every record of the container is
visited, but a bounding-box reject against the widest aperture means only the handful within reach
of the cursor pay for snap point generation, and only those few enter the pairwise passes.
Section 8's frame-to-frame coherence optimisation is NOT built. */

constexpr double kSnapPi = 3.14159265358979323846;
constexpr double kSnapTwoPi = 2.0 * kSnapPi;
// Below this the two radii of a conic are the same number as far as double arithmetic cares, and
// the closed-form circle solutions apply instead of the bounded numeric ones.
constexpr double kSnapCircleEpsilon = 1.0e-9;

/* Priorities the design document leaves open, because these points are DERIVED - against the
anchor, or against a PAIR of records - rather than exported by one record (section 7 fixes only the
exported ones). An intersection is as meaningful to a draughtsman as a midpoint, so it shares that
level; a perpendicular foot or a tangent point is a construction aid the user aims at deliberately
but which must not outrank a real vertex, so both sit below Quadrant. */
constexpr uint8_t kSnapPriorityIntersection = 12;
constexpr uint8_t kSnapPriorityPerpendicular = 10;
constexpr uint8_t kSnapPriorityTangent = 10;

/* Cap on the curves carried into the pairwise passes. Intersection is O(k^2) and the derived snaps
are O(k), so an unbounded near-set would make a dense drawing cost more the more the user needs it
to cost less. 48 curves within a 24 px aperture is already a congested area. */
constexpr size_t kSnapMaxNearCurves = 48;

struct Cad2DSnapSegment {
    double ax = 0.0, ay = 0.0, bx = 0.0, by = 0.0;
    uint64_t objectId = 0;
};

/* A circle, an ellipse, or an arc of either, in the parameterisation the curve shaders use
(Shader2D_CurveVertex.hlsl): point(t) = center + R(rotation) * (rx cos t, ry sin t), and an arc
keeps the CCW sweep from startAngle to endAngle in that same local frame. Matching the shader
matters - a snap point the renderer does not draw on the curve is a bug the user sees. */
struct Cad2DSnapConic {
    double cx = 0.0, cy = 0.0, rx = 0.0, ry = 0.0, rotation = 0.0;
    bool isArc = false;
    double startAngle = 0.0, endAngle = 0.0;
    uint64_t objectId = 0;

    bool IsCircle() const { return std::abs(rx - ry) <= kSnapCircleEpsilon * (std::max)(std::abs(rx), 1.0); }
    void PointAt(double t, double& x, double& y) const {
        const double lx = rx * std::cos(t), ly = ry * std::sin(t);
        const double c = std::cos(rotation), s = std::sin(rotation);
        x = cx + lx * c - ly * s;
        y = cy + lx * s + ly * c;
    }
    // d/dt of the above. Zero-length only for a degenerate conic, which never gets this far.
    void TangentAt(double t, double& dx, double& dy) const {
        const double lx = -rx * std::sin(t), ly = ry * std::cos(t);
        const double c = std::cos(rotation), s = std::sin(rotation);
        dx = lx * c - ly * s;
        dy = lx * s + ly * c;
    }
    double ParameterOf(double x, double y) const {
        const double sx = (std::max)(std::abs(rx), 1.0e-12);
        const double sy = (std::max)(std::abs(ry), 1.0e-12);
        const double dx = x - cx, dy = y - cy;
        const double c = std::cos(rotation), s = std::sin(rotation);
        return std::atan2((-dx * s + dy * c) / sy, (dx * c + dy * s) / sx);
    }
};

double NormalizeAngleTwoPi(double angle) {
    return std::fmod(std::fmod(angle, kSnapTwoPi) + kSnapTwoPi, kSnapTwoPi);
}

// The shader's AngleInCCWSweep, in double. A full circle / ellipse has no sweep and takes every t.
bool SnapAngleInSweep(const Cad2DSnapConic& conic, double angle) {
    if (!conic.isArc) return true;
    double a = NormalizeAngleTwoPi(angle);
    const double start = NormalizeAngleTwoPi(conic.startAngle);
    double end = NormalizeAngleTwoPi(conic.endAngle);
    if (end < start) end += kSnapTwoPi;
    if (a < start) a += kSnapTwoPi;
    return a <= end;
}

/* Accumulates candidates for one cursor position. Distances go in as SCREEN pixels - never world
units, or the aperture would change meaning with the zoom (section 5).

With ortho armed, a candidate is offered only if it lies ON the constrained ray: section 11 says the
constraint fixes the direction and a higher-priority snap point on that ray still wins, which is
exactly "filter, then resolve normally". */
struct Cad2DSnapGatherer {
    SnapCandidateSet* set = nullptr;
    double cursorXCU = 0.0, cursorYCU = 0.0;
    double zoom = 1.0;      // Pixels per ComputerUnit.
    double reachCU = 0.0;   // Widest aperture in CU: the broad-phase reject radius.
    bool orthoActive = false;
    double anchorXCU = 0.0, anchorYCU = 0.0;
    double orthoDirX = 0.0, orthoDirY = 0.0;  // Unit vector of the locked axis.

    void Offer(double x, double y, SnapKind kind, uint8_t priority, uint64_t objectId,
        uint64_t secondObjectId = 0) const {
        if (orthoActive) {
            // Perpendicular offset from the ray through the anchor; a point off the ray is not
            // reachable while the direction is locked, so it is not a candidate at all.
            const double vx = x - anchorXCU, vy = y - anchorYCU;
            const double offRayCU = std::abs(vx * orthoDirY - vy * orthoDirX);
            if (offRayCU * zoom > 1.0) return;   // One pixel of tolerance for float noise.
        }
        const double dx = x - cursorXCU, dy = y - cursorYCU;
        SnapPoint point{};
        point.x = x;
        point.y = y;
        point.objectId = objectId;
        point.kind = kind;
        point.priority = priority;
        set->Consider(point, std::sqrt(dx * dx + dy * dy) * zoom, secondObjectId);
    }

    bool NearBox(double minX, double minY, double maxX, double maxY) const {
        return cursorXCU >= minX - reachCU && cursorXCU <= maxX + reachCU &&
            cursorYCU >= minY - reachCU && cursorYCU <= maxY + reachCU;
    }
};

// The near-set the pairwise and anchor-relative passes work over.
struct Cad2DSnapScene {
    Cad2DSnapSegment segments[kSnapMaxNearCurves];
    size_t segmentCount = 0;
    Cad2DSnapConic conics[kSnapMaxNearCurves];
    size_t conicCount = 0;

    void AddSegment(double ax, double ay, double bx, double by, uint64_t objectId) {
        if (segmentCount >= kSnapMaxNearCurves) return;
        segments[segmentCount++] = { ax, ay, bx, by, objectId };
    }
    void AddConic(const Cad2DSnapConic& conic) {
        if (conicCount >= kSnapMaxNearCurves) return;
        conics[conicCount++] = conic;
    }
};

// Nearest point on the SEGMENT (clamped): the Nearest snap must land on the drawn stroke.
void ClosestPointOnSegment(double px, double py, const Cad2DSnapSegment& s,
    double& outX, double& outY) {
    const double vx = s.bx - s.ax, vy = s.by - s.ay;
    const double len2 = vx * vx + vy * vy;
    double t = len2 > 1.0e-18 ? ((px - s.ax) * vx + (py - s.ay) * vy) / len2 : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    outX = s.ax + t * vx;
    outY = s.ay + t * vy;
}

/* Roots of a smooth 2pi-periodic objective, found by coarse sampling for sign changes then
bisection. This is the "bounded iteration" section 6 asks for on the ellipse: it cannot diverge and
it cannot loop, unlike a raw Newton step near a curvature extreme. Circles never come here - they
have closed forms above the call sites. */
template <typename Objective>
size_t SnapConicRoots(const Objective& objective, double outRoots[8]) {
    constexpr int kSamples = 96;      // Comfortably above the 4 extrema an ellipse objective has.
    constexpr int kBisections = 48;   // Takes a 2pi/96 bracket to well below double resolution.
    size_t rootCount = 0;
    double previousT = 0.0;
    double previousValue = objective(0.0);
    for (int i = 1; i <= kSamples && rootCount < 8; ++i) {
        const double t = kSnapTwoPi * (double)i / (double)kSamples;
        const double value = objective(t);
        if (previousValue == 0.0) {
            outRoots[rootCount++] = previousT;
        } else if ((previousValue < 0.0) != (value < 0.0)) {
            double lo = previousT, hi = t, loValue = previousValue;
            for (int step = 0; step < kBisections; ++step) {
                const double mid = 0.5 * (lo + hi);
                const double midValue = objective(mid);
                if ((loValue < 0.0) != (midValue < 0.0)) { hi = mid; }
                else { lo = mid; loValue = midValue; }
            }
            outRoots[rootCount++] = 0.5 * (lo + hi);
        }
        previousT = t;
        previousValue = value;
    }
    return rootCount;
}

// Points of `conic` whose tangent is perpendicular to the line from (px, py) - i.e. the feet of the
// perpendicular from that point, which for the cursor are also the nearest points on the curve.
size_t SnapConicPerpendicularFeet(const Cad2DSnapConic& conic, double px, double py,
    double outX[8], double outY[8]) {
    size_t count = 0;
    if (conic.IsCircle()) {
        const double dx = px - conic.cx, dy = py - conic.cy;
        const double length = std::sqrt(dx * dx + dy * dy);
        if (length <= 1.0e-12) return 0;   // At the centre every direction is perpendicular.
        const double ux = dx / length, uy = dy / length;
        outX[count] = conic.cx + conic.rx * ux; outY[count] = conic.cy + conic.rx * uy; ++count;
        outX[count] = conic.cx - conic.rx * ux; outY[count] = conic.cy - conic.rx * uy; ++count;
        return count;
    }
    double roots[8];
    const size_t rootCount = SnapConicRoots([&](double t) {
        double x = 0.0, y = 0.0, dx = 0.0, dy = 0.0;
        conic.PointAt(t, x, y);
        conic.TangentAt(t, dx, dy);
        return (px - x) * dx + (py - y) * dy;   // Zero exactly where the offset is along the normal.
    }, roots);
    for (size_t i = 0; i < rootCount; ++i) conic.PointAt(roots[i], outX[i], outY[i]);
    return rootCount;
}

// Points of `conic` where the line from (px, py) touches without crossing. Empty when the point is
// inside the curve, where no tangent exists.
size_t SnapConicTangentPoints(const Cad2DSnapConic& conic, double px, double py,
    double outX[8], double outY[8]) {
    if (conic.IsCircle()) {
        const double dx = px - conic.cx, dy = py - conic.cy;
        const double distance = std::sqrt(dx * dx + dy * dy);
        const double radius = std::abs(conic.rx);
        if (distance <= radius + 1.0e-12) return 0;   // Inside or on: no tangent from here.
        const double cosA = radius / distance, sinA = std::sqrt((std::max)(0.0, 1.0 - cosA * cosA));
        const double ux = dx / distance, uy = dy / distance;
        outX[0] = conic.cx + radius * (ux * cosA - uy * sinA);
        outY[0] = conic.cy + radius * (ux * sinA + uy * cosA);
        outX[1] = conic.cx + radius * (ux * cosA + uy * sinA);
        outY[1] = conic.cy + radius * (-ux * sinA + uy * cosA);
        return 2;
    }
    double roots[8];
    const size_t rootCount = SnapConicRoots([&](double t) {
        double x = 0.0, y = 0.0, dx = 0.0, dy = 0.0;
        conic.PointAt(t, x, y);
        conic.TangentAt(t, dx, dy);
        return (px - x) * dy - (py - y) * dx;   // Zero exactly where the chord is along the tangent.
    }, roots);
    for (size_t i = 0; i < rootCount; ++i) conic.PointAt(roots[i], outX[i], outY[i]);
    return rootCount;
}

// Segment x segment, closed form. Parallel segments have no single crossing and are skipped.
bool SnapSegmentIntersection(const Cad2DSnapSegment& a, const Cad2DSnapSegment& b,
    double& outX, double& outY) {
    const double r1x = a.bx - a.ax, r1y = a.by - a.ay;
    const double r2x = b.bx - b.ax, r2y = b.by - b.ay;
    const double denominator = r1x * r2y - r1y * r2x;
    if (std::abs(denominator) <= 1.0e-15) return false;
    const double t = ((b.ax - a.ax) * r2y - (b.ay - a.ay) * r2x) / denominator;
    const double u = ((b.ax - a.ax) * r1y - (b.ay - a.ay) * r1x) / denominator;
    // Both parameters must land inside their own segment: an "intersection" off the end of one of
    // them is an apparent intersection, which is phase 2 and deliberately not offered here.
    if (t < 0.0 || t > 1.0 || u < 0.0 || u > 1.0) return false;
    outX = a.ax + t * r1x;
    outY = a.ay + t * r1y;
    return true;
}

/* Segment x conic, closed form: un-rotate and scale the segment into the frame where the conic is
the unit circle, solve the quadratic there, then map the roots back. Exact for circles AND ellipses,
which is why section 6's "line x circle/arc closed form" costs nothing extra here. Conic x conic
stays deferred - that one really does need a solver. */
size_t SnapSegmentConicIntersections(const Cad2DSnapSegment& segment, const Cad2DSnapConic& conic,
    double outX[2], double outY[2]) {
    const double rx = std::abs(conic.rx), ry = std::abs(conic.ry);
    if (rx <= 1.0e-12 || ry <= 1.0e-12) return 0;
    const double c = std::cos(conic.rotation), s = std::sin(conic.rotation);
    auto toUnit = [&](double x, double y, double& ox, double& oy) {
        const double dx = x - conic.cx, dy = y - conic.cy;
        ox = (dx * c + dy * s) / rx;
        oy = (-dx * s + dy * c) / ry;
    };
    double p0x = 0.0, p0y = 0.0, p1x = 0.0, p1y = 0.0;
    toUnit(segment.ax, segment.ay, p0x, p0y);
    toUnit(segment.bx, segment.by, p1x, p1y);

    const double dx = p1x - p0x, dy = p1y - p0y;
    const double a = dx * dx + dy * dy;
    if (a <= 1.0e-18) return 0;
    const double b = 2.0 * (p0x * dx + p0y * dy);
    const double cc = p0x * p0x + p0y * p0y - 1.0;
    const double discriminant = b * b - 4.0 * a * cc;
    if (discriminant < 0.0) return 0;

    const double root = std::sqrt(discriminant);
    const double parameters[2] = { (-b - root) / (2.0 * a), (-b + root) / (2.0 * a) };
    size_t count = 0;
    for (double t : parameters) {
        if (t < 0.0 || t > 1.0) continue;   // Off the drawn segment.
        const double x = segment.ax + t * (segment.bx - segment.ax);
        const double y = segment.ay + t * (segment.by - segment.ay);
        if (!SnapAngleInSweep(conic, conic.ParameterOf(x, y))) continue;   // Off the drawn sweep.
        outX[count] = x;
        outY[count] = y;
        ++count;
    }
    return count;
}

/* The anchor of whichever tool is running: the point every relative snap and both direction
constraints are measured FROM (section 11). Each tool already holds one privately; this is the one
place that knows where they all keep it. False when no tool has taken its first click yet, and the
resolution loop then skips every relative snap because there is nothing to be relative to.

polylineCreationPoints is guarded by cpuRecordsMutex, so this must be called with it held. */
bool Cad2DSnapAnchorLocked(const TabCad2DStorage& s, double& outX, double& outY) {
    auto take = [&](double x, double y) { outX = x; outY = y; return true; };
    if (s.lineCreationMode.load(std::memory_order_acquire) &&
        s.lineCreationHasPreviousPoint.load(std::memory_order_acquire)) {
        return take(s.lineCreationPreviousXCU.load(std::memory_order_acquire),
            s.lineCreationPreviousYCU.load(std::memory_order_acquire));
    }
    if (s.polylineCreationMode.load(std::memory_order_acquire) && !s.polylineCreationPoints.empty()) {
        return take(s.polylineCreationPoints.back().x, s.polylineCreationPoints.back().y);
    }
    if (s.polygonCreationMode.load(std::memory_order_acquire) &&
        s.polygonCreationHasCenter.load(std::memory_order_acquire)) {
        return take(s.polygonCreationCenterXCU.load(std::memory_order_acquire),
            s.polygonCreationCenterYCU.load(std::memory_order_acquire));
    }
    if (s.circleCreationMode.load(std::memory_order_acquire) &&
        s.circleCreationHasCenter.load(std::memory_order_acquire)) {
        return take(s.circleCreationCenterXCU.load(std::memory_order_acquire),
            s.circleCreationCenterYCU.load(std::memory_order_acquire));
    }
    if (s.ellipseCreationMode.load(std::memory_order_acquire) &&
        s.ellipseCreationStep.load(std::memory_order_acquire) > 0) {
        return take(s.ellipseCreationCenterXCU.load(std::memory_order_acquire),
            s.ellipseCreationCenterYCU.load(std::memory_order_acquire));
    }
    if (s.arcCreationMode.load(std::memory_order_acquire)) {
        const uint32_t step = s.arcCreationStep.load(std::memory_order_acquire);
        // Step 2 measures from the sweep start, not from the centre: that is the point the user is
        // dragging away from.
        if (step >= 2) {
            return take(s.arcCreationStartXCU.load(std::memory_order_acquire),
                s.arcCreationStartYCU.load(std::memory_order_acquire));
        }
        if (step == 1) {
            return take(s.arcCreationCenterXCU.load(std::memory_order_acquire),
                s.arcCreationCenterYCU.load(std::memory_order_acquire));
        }
    }
    if (s.transform2DKind.load(std::memory_order_acquire) != 0) {
        const uint32_t step = s.transform2DStep.load(std::memory_order_acquire);
        if (step >= 2) {
            return take(s.transform2DP2XCU.load(std::memory_order_acquire),
                s.transform2DP2YCU.load(std::memory_order_acquire));
        }
        if (step == 1) {
            return take(s.transform2DP1XCU.load(std::memory_order_acquire),
                s.transform2DP1YCU.load(std::memory_order_acquire));
        }
    }
    return false;
}

// Exports one container's snap points into the gatherer and collects its near curves. Caller holds
// cpuRecordsMutex. The record tables are section 7's, priority for priority.
void Cad2DGatherSnapPointsLocked(const TabCad2DStorage& s, uint64_t container,
    const Cad2DSnapGatherer& g, Cad2DSnapScene& scene) {
    auto wanted = [&](const META_DATA& r) { return !r.isDeleted && r.memoryIDContainer == container; };

    for (const Cad2DLineRecordCPU& r : s.lineRecords) {
        if (!wanted(r)) continue;
        if (!g.NearBox((std::min)(r.x1, r.x2), (std::min)(r.y1, r.y2),
            (std::max)(r.x1, r.x2), (std::max)(r.y1, r.y2))) continue;
        g.Offer(r.x1, r.y1, SnapKind::End, 14, r.memoryID);
        g.Offer(r.x2, r.y2, SnapKind::End, 14, r.memoryID);
        g.Offer((r.x1 + r.x2) * 0.5, (r.y1 + r.y2) * 0.5, SnapKind::Mid, 12, r.memoryID);
        scene.AddSegment(r.x1, r.y1, r.x2, r.y2, r.memoryID);
    }

    for (const Cad2DPolylineRecordCPU& r : s.polylineRecords) {
        if (!wanted(r)) continue;
        for (size_t i = 0; i < r.points.size(); ++i) {
            const Cad2DPoint2D& p = r.points[i];
            if (i + 1 < r.points.size()) {
                const Cad2DPoint2D& q = r.points[i + 1];
                if (g.NearBox((std::min)(p.x, q.x), (std::min)(p.y, q.y),
                    (std::max)(p.x, q.x), (std::max)(p.y, q.y))) {
                    g.Offer((p.x + q.x) * 0.5, (p.y + q.y) * 0.5, SnapKind::Mid, 12, r.memoryID);
                    scene.AddSegment(p.x, p.y, q.x, q.y, r.memoryID);
                }
            }
            if (g.NearBox(p.x, p.y, p.x, p.y)) g.Offer(p.x, p.y, SnapKind::End, 14, r.memoryID);
        }
    }

    for (const Cad2DPolygonRecordCPU& r : s.polygonRecords) {
        if (!wanted(r) || r.radius <= 0.0) continue;
        if (!g.NearBox(r.centerX - r.radius, r.centerY - r.radius,
            r.centerX + r.radius, r.centerY + r.radius)) continue;
        g.Offer(r.centerX, r.centerY, SnapKind::Center, 13, r.memoryID);
        // Same vertex parameterisation as the selection hit-test: sin drives x, cos drives y.
        const uint32_t n = std::clamp(r.lineSegmentCount, 3u, 16u);
        const double step = 360.0 / (double)n;
        for (uint32_t i = 0; i < n; ++i) {
            const double a0 = (r.rotationDegrees + step * i) * kSnapPi / 180.0;
            const double a1 = (r.rotationDegrees + step * ((i + 1) % n)) * kSnapPi / 180.0;
            const double x0 = r.centerX + std::sin(a0) * r.radius;
            const double y0 = r.centerY + std::cos(a0) * r.radius;
            const double x1 = r.centerX + std::sin(a1) * r.radius;
            const double y1 = r.centerY + std::cos(a1) * r.radius;
            g.Offer(x0, y0, SnapKind::End, 14, r.memoryID);
            g.Offer((x0 + x1) * 0.5, (y0 + y1) * 0.5, SnapKind::Mid, 12, r.memoryID);
            scene.AddSegment(x0, y0, x1, y1, r.memoryID);
        }
    }

    // Circle, ellipse and arc share the conic parameterisation; only their sweep and radii differ.
    auto addConic = [&](const Cad2DSnapConic& conic, bool exportEnds, double sx, double sy,
        double ex, double ey) {
        const double reachX = std::sqrt(conic.rx * conic.rx + conic.ry * conic.ry);
        if (!g.NearBox(conic.cx - reachX, conic.cy - reachX, conic.cx + reachX, conic.cy + reachX)) {
            return;
        }
        g.Offer(conic.cx, conic.cy, SnapKind::Center, 13, conic.objectId);
        // The four quadrants are the 0/90/180/270 degree points of the curve's OWN rotated frame,
        // which is what a vessel or flange drawing means by a quadrant.
        for (int q = 0; q < 4; ++q) {
            const double t = kSnapPi * 0.5 * (double)q;
            if (!SnapAngleInSweep(conic, t)) continue;
            double x = 0.0, y = 0.0;
            conic.PointAt(t, x, y);
            g.Offer(x, y, SnapKind::Quadrant, 11, conic.objectId);
        }
        if (exportEnds) {
            g.Offer(sx, sy, SnapKind::End, 14, conic.objectId);
            g.Offer(ex, ey, SnapKind::End, 14, conic.objectId);
            // Midpoint of the sweep, walking CCW from start exactly as the shader fills it.
            double start = NormalizeAngleTwoPi(conic.startAngle);
            double end = NormalizeAngleTwoPi(conic.endAngle);
            if (end < start) end += kSnapTwoPi;
            double mx = 0.0, my = 0.0;
            conic.PointAt(0.5 * (start + end), mx, my);
            g.Offer(mx, my, SnapKind::Mid, 12, conic.objectId);
        }
        scene.AddConic(conic);
    };

    for (const Cad2DCircleRecordCPU& r : s.circleRecords) {
        if (!wanted(r) || r.radius <= 0.0) continue;
        Cad2DSnapConic conic{};
        conic.cx = r.centerX; conic.cy = r.centerY;
        conic.rx = r.radius;  conic.ry = r.radius;
        conic.objectId = r.memoryID;
        addConic(conic, false, 0.0, 0.0, 0.0, 0.0);
    }
    for (const Cad2DEllipseRecordCPU& r : s.ellipseRecords) {
        if (!wanted(r) || r.radiusX <= 0.0 || r.radiusY <= 0.0) continue;
        Cad2DSnapConic conic{};
        conic.cx = r.centerX; conic.cy = r.centerY;
        conic.rx = r.radiusX; conic.ry = r.radiusY;
        conic.rotation = r.rotationRadians;
        conic.objectId = r.memoryID;
        addConic(conic, false, 0.0, 0.0, 0.0, 0.0);
    }
    for (const Cad2DArcRecordCPU& r : s.arcRecords) {
        if (!wanted(r) || r.radiusX <= 0.0 || r.radiusY <= 0.0) continue;
        Cad2DSnapConic conic{};
        conic.cx = r.centerX; conic.cy = r.centerY;
        conic.rx = r.radiusX; conic.ry = r.radiusY;
        conic.rotation = r.rotationRadians;
        conic.isArc = true;
        conic.objectId = r.memoryID;
        conic.startAngle = conic.ParameterOf(r.startX, r.startY);
        conic.endAngle = conic.ParameterOf(r.endX, r.endY);
        addConic(conic, true, r.startX, r.startY, r.endX, r.endY);
    }

    // Insertion points are nearly free: both records already carry an explicit one.
    for (const Cad2DTextRecordCPU& r : s.textRecords) {
        if (!wanted(r) || !g.NearBox(r.x, r.y, r.x, r.y)) continue;
        g.Offer(r.x, r.y, SnapKind::Insertion, 13, r.memoryID);
    }
    for (const Cad2DAssetInsertRecordCPU& r : s.assetInsertRecords) {
        if (r.isDeleted || r.memoryIDContainer != container) continue;
        if (!g.NearBox(r.x, r.y, r.x, r.y)) continue;
        g.Offer(r.x, r.y, SnapKind::Insertion, 13, r.memoryID);
    }
}

// The passes that need more than one record, or need the anchor. Runs over the near-set only, so
// its cost is bounded by kSnapMaxNearCurves regardless of drawing size.
void Cad2DGatherDerivedSnaps(const Cad2DSnapScene& scene, const Cad2DSnapGatherer& g,
    bool hasAnchor, double anchorX, double anchorY, uint32_t mask) {
    if (SnapMaskHas(mask, SnapKind::Nearest)) {
        for (size_t i = 0; i < scene.segmentCount; ++i) {
            double x = 0.0, y = 0.0;
            ClosestPointOnSegment(g.cursorXCU, g.cursorYCU, scene.segments[i], x, y);
            g.Offer(x, y, SnapKind::Nearest, 6, scene.segments[i].objectId);
        }
        for (size_t i = 0; i < scene.conicCount; ++i) {
            double xs[8], ys[8];
            const size_t count = SnapConicPerpendicularFeet(scene.conics[i],
                g.cursorXCU, g.cursorYCU, xs, ys);
            for (size_t k = 0; k < count; ++k) {
                if (!SnapAngleInSweep(scene.conics[i], scene.conics[i].ParameterOf(xs[k], ys[k]))) continue;
                g.Offer(xs[k], ys[k], SnapKind::Nearest, 6, scene.conics[i].objectId);
            }
        }
    }

    if (SnapMaskHas(mask, SnapKind::Intersection)) {
        for (size_t i = 0; i < scene.segmentCount; ++i) {
            for (size_t j = i + 1; j < scene.segmentCount; ++j) {
                if (scene.segments[i].objectId == scene.segments[j].objectId) continue;
                double x = 0.0, y = 0.0;
                if (!SnapSegmentIntersection(scene.segments[i], scene.segments[j], x, y)) continue;
                g.Offer(x, y, SnapKind::Intersection, kSnapPriorityIntersection,
                    scene.segments[i].objectId, scene.segments[j].objectId);
            }
            for (size_t j = 0; j < scene.conicCount; ++j) {
                if (scene.segments[i].objectId == scene.conics[j].objectId) continue;
                double xs[2], ys[2];
                const size_t count = SnapSegmentConicIntersections(scene.segments[i],
                    scene.conics[j], xs, ys);
                for (size_t k = 0; k < count; ++k) {
                    g.Offer(xs[k], ys[k], SnapKind::Intersection, kSnapPriorityIntersection,
                        scene.segments[i].objectId, scene.conics[j].objectId);
                }
            }
        }
    }

    // Everything below is measured FROM the anchor, so with no running tool there is nothing to do.
    if (!hasAnchor) return;

    if (SnapMaskHas(mask, SnapKind::Perpendicular)) {
        for (size_t i = 0; i < scene.segmentCount; ++i) {
            const Cad2DSnapSegment& s = scene.segments[i];
            const double vx = s.bx - s.ax, vy = s.by - s.ay;
            const double len2 = vx * vx + vy * vy;
            if (len2 <= 1.0e-18) continue;
            /* Deliberately NOT clamped to the segment: a perpendicular onto the extension of an
            edge is a normal drafting move, and the aperture already rejects a foot the cursor is
            nowhere near. */
            const double t = ((anchorX - s.ax) * vx + (anchorY - s.ay) * vy) / len2;
            g.Offer(s.ax + t * vx, s.ay + t * vy, SnapKind::Perpendicular,
                kSnapPriorityPerpendicular, s.objectId);
        }
        for (size_t i = 0; i < scene.conicCount; ++i) {
            double xs[8], ys[8];
            const size_t count = SnapConicPerpendicularFeet(scene.conics[i], anchorX, anchorY, xs, ys);
            for (size_t k = 0; k < count; ++k) {
                if (!SnapAngleInSweep(scene.conics[i], scene.conics[i].ParameterOf(xs[k], ys[k]))) continue;
                g.Offer(xs[k], ys[k], SnapKind::Perpendicular, kSnapPriorityPerpendicular,
                    scene.conics[i].objectId);
            }
        }
    }

    if (SnapMaskHas(mask, SnapKind::Tangent)) {
        for (size_t i = 0; i < scene.conicCount; ++i) {
            double xs[8], ys[8];
            const size_t count = SnapConicTangentPoints(scene.conics[i], anchorX, anchorY, xs, ys);
            for (size_t k = 0; k < count; ++k) {
                if (!SnapAngleInSweep(scene.conics[i], scene.conics[i].ParameterOf(xs[k], ys[k]))) continue;
                g.Offer(xs[k], ys[k], SnapKind::Tangent, kSnapPriorityTangent,
                    scene.conics[i].objectId);
            }
        }
    }
}

/* Resolves the snap for one cursor pixel, and the single place both the click path and the hover
path go through so a marker can never promise a point the click would not commit.

Returns false only when the pixel is outside the 2D viewport. Otherwise it ALWAYS produces a point:
the ambient grid is priority 0 with an unbounded aperture and cannot be switched off, which is what
makes every click land on a round number rather than on float noise (locked decision 5). */
bool Cad2DResolveSnap(DATASETTAB& tab, const ACTION_DETAILS& input,
    double& outXCU, double& outYCU, SnapResult& outResult) {
    outResult = SnapResult{};
    double rawX = 0.0, rawY = 0.0;
    if (!Page2DCoordinateFromInput(tab, input, rawX, rawY)) return false;

    const Cad2DViewState& view = Cad2DInputView(tab);
    const double zoom = (std::max)(
        (double)view.zoomPixelsPerCU.load(std::memory_order_acquire),
        (double)kCad2DZoomMinPixelsPerCU);
    const double step = Page2DAmbientStepCU(zoom);
    const double dpiScale = SnapDpiScaleForTab(tab);

    /* The master switch clears the whole set rather than the mask, so switching it back on restores
    the user's kind selection instead of a default (section 13). HELD Shift does the same thing
    momentarily - indispensable in a congested area, and the exact inverse of snap-from-object. In
    both cases the ambient grid survives, because it is not an object snap. */
    const bool objectSnapsOn = tab.snapObjectEnabled2D.load(std::memory_order_acquire) &&
        !tab.isShiftDown;
    const uint32_t mask = objectSnapsOn ? tab.snapMask2D.load(std::memory_order_acquire) : 0u;

    double anchorX = 0.0, anchorY = 0.0;
    bool hasAnchor = false;
    const uint64_t container = Cad2DFindTargetPage2DMemoryId(tab);

    SnapCandidateSet candidates(mask, dpiScale);
    Cad2DSnapGatherer gatherer{};
    gatherer.set = &candidates;
    gatherer.zoom = zoom;
    gatherer.reachCU = kApertureMaxPx * dpiScale / zoom;
    // Seeded here, not inside the block below: the ambient fallback reads these, and an empty page
    // (or a tab with no 2D storage at all) would otherwise round every click onto the origin.
    gatherer.cursorXCU = rawX;
    gatherer.cursorYCU = rawY;

    if (tab.cad2d && container != 0) {
        TabCad2DStorage& s = *tab.cad2d;
        Cad2DSnapScene scene;
        {
            std::lock_guard<std::mutex> lock(s.cpuRecordsMutex);
            hasAnchor = Cad2DSnapAnchorLocked(s, anchorX, anchorY);

            /* Ortho, before gathering, because it moves the point the candidates are measured
            from: with the direction locked the user is pointing along the ray, not at the raw
            pixel, and a candidate 30 px off the ray is not one they can reach (section 11). */
            if (hasAnchor && tab.snapOrtho2D.load(std::memory_order_acquire)) {
                const double dx = rawX - anchorX, dy = rawY - anchorY;
                const bool alongX = std::abs(dx) >= std::abs(dy);
                gatherer.orthoActive = true;
                gatherer.anchorXCU = anchorX;
                gatherer.anchorYCU = anchorY;
                gatherer.orthoDirX = alongX ? 1.0 : 0.0;
                gatherer.orthoDirY = alongX ? 0.0 : 1.0;
                gatherer.cursorXCU = alongX ? rawX : anchorX;
                gatherer.cursorYCU = alongX ? anchorY : rawY;
            }

            Cad2DGatherSnapPointsLocked(s, container, gatherer, scene);
            Cad2DGatherDerivedSnaps(scene, gatherer, hasAnchor, anchorX, anchorY, mask);
        }
    }

    if (candidates.Resolve(outResult)) {
        outXCU = outResult.x;
        outYCU = outResult.y;
        return true;
    }

    /* Ambient grid: the guaranteed fallback. Under ortho the off-axis coordinate is the anchor's
    exactly - rounding it would put a kink in a line the user asked to be straight - while the
    along-axis one is still rounded, because an exact direction with a fuzzy length is half a
    result (section 11). */
    outXCU = SnapToStep(gatherer.cursorXCU, step);
    outYCU = SnapToStep(gatherer.cursorYCU, step);
    if (gatherer.orthoActive) {
        if (gatherer.orthoDirX != 0.0) outYCU = anchorY;
        else outXCU = anchorX;
    }
    outResult.hit = true;
    outResult.kind = gatherer.orthoActive ? SnapKind::Ortho : SnapKind::AmbientGrid;
    outResult.priority = 0;
    outResult.x = outXCU;
    outResult.y = outYCU;
    return true;
}

void Cad2DHandleSelectionClick(DATASETTAB& tab, double xCU, double yCU) {
    if (!tab.cad2d) return;
    const uint64_t container = Cad2DFindTargetPage2DMemoryId(tab);
    if (container == 0) return;
    TabCad2DStorage& s = *tab.cad2d;

    const Cad2DViewState& view = Cad2DInputView(tab);
    const double zoom = (std::max)(
        (double)view.zoomPixelsPerCU.load(std::memory_order_acquire),
        (double)kCad2DZoomMinPixelsPerCU);
    const double tolCU = 6.0 / zoom; // ~6 pixel pick tolerance in CAD units.
    uint64_t bestId = 0;
    uint64_t bestParentId = 0;
    double bestDist = tolCU;
    std::vector<uint64_t> hitGroup; // The hit object, expanded to its whole asset instance.

    {
        std::lock_guard<std::mutex> lock(s.cpuRecordsMutex);
        auto consider = [&](double d, uint64_t id, uint64_t parentId) {
            if (d < bestDist) { bestDist = d; bestId = id; bestParentId = parentId; }
        };
        for (const Cad2DLineRecordCPU& r : s.lineRecords) {
            if (r.isDeleted || r.memoryIDContainer != container) continue;
            consider(DistPointToSegment(xCU, yCU, r.x1, r.y1, r.x2, r.y2), r.memoryID, r.memoryIDGenerator);
        }
        for (const Cad2DPolylineRecordCPU& r : s.polylineRecords) {
            if (r.isDeleted || r.memoryIDContainer != container) continue;
            for (size_t i = 1; i < r.points.size(); ++i) {
                consider(DistPointToSegment(xCU, yCU, r.points[i - 1].x, r.points[i - 1].y,
                    r.points[i].x, r.points[i].y), r.memoryID, r.memoryIDGenerator);
            }
        }
        for (const Cad2DPolygonRecordCPU& r : s.polygonRecords) {
            if (r.isDeleted || r.memoryIDContainer != container || r.radius <= 0.0) continue;
            const uint32_t n = std::clamp(r.lineSegmentCount, 3u, 16u);
            const double step = 360.0 / (double)n;
            for (uint32_t i = 0; i < n; ++i) {
                const double a0 = (r.rotationDegrees + step * i) * 3.14159265358979323846 / 180.0;
                const double a1 = (r.rotationDegrees + step * ((i + 1) % n)) * 3.14159265358979323846 / 180.0;
                consider(DistPointToSegment(xCU, yCU,
                    r.centerX + std::sin(a0) * r.radius, r.centerY + std::cos(a0) * r.radius,
                    r.centerX + std::sin(a1) * r.radius, r.centerY + std::cos(a1) * r.radius),
                    r.memoryID, r.memoryIDGenerator);
            }
        }
        for (const Cad2DCircleRecordCPU& r : s.circleRecords) {
            if (r.isDeleted || r.memoryIDContainer != container) continue;
            consider(DistPointToCircle(xCU, yCU, r.centerX, r.centerY, r.radius), r.memoryID,
                r.memoryIDGenerator);
        }
        for (const Cad2DEllipseRecordCPU& r : s.ellipseRecords) {
            if (r.isDeleted || r.memoryIDContainer != container) continue;
            consider(DistPointToEllipse(xCU, yCU, r.centerX, r.centerY, r.radiusX, r.radiusY,
                r.rotationRadians), r.memoryID, r.memoryIDGenerator);
        }
        for (const Cad2DArcRecordCPU& r : s.arcRecords) {
            if (r.isDeleted || r.memoryIDContainer != container) continue;
            consider(DistPointToEllipse(xCU, yCU, r.centerX, r.centerY, r.radiusX, r.radiusY,
                r.rotationRadians), r.memoryID, r.memoryIDGenerator);
        }

        if (bestId != 0) {
            // Parent expansion: when the hit object's parent is a Asset2DInsert, the whole
            // instance is selected - every record sharing that parent, across all record types.
            bool parentIsAssetInsert = false;
            if (bestParentId != 0) {
                for (const Cad2DAssetInsertRecordCPU& insert : s.assetInsertRecords) {
                    if (!insert.isDeleted && insert.memoryID == bestParentId) {
                        parentIsAssetInsert = true;
                        break;
                    }
                }
            }
            if (parentIsAssetInsert) {
                auto gather = [&](const auto& records) {
                    for (const auto& r : records) {
                        if (!r.isDeleted && r.memoryIDGenerator == bestParentId) {
                            hitGroup.push_back(r.memoryID);
                        }
                    }
                };
                gather(s.lineRecords);
                gather(s.polylineRecords);
                gather(s.polygonRecords);
                gather(s.circleRecords);
                gather(s.ellipseRecords);
                gather(s.arcRecords);
                gather(s.textRecords);
            } else {
                hitGroup.push_back(bestId);
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(s.selection2DMutex);
        // Ctrl+click accumulates: toggles the hit object / asset instance in the selection.
        // A plain click replaces the selection (and clears it on an empty click).
        if (!tab.isCtrlDown) s.selectedObjectIds.clear();
        if (!hitGroup.empty()) {
            bool allAlreadySelected = tab.isCtrlDown;
            for (uint64_t id : hitGroup) {
                if (s.selectedObjectIds.count(id) == 0) { allAlreadySelected = false; break; }
            }
            for (uint64_t id : hitGroup) {
                if (allAlreadySelected) s.selectedObjectIds.erase(id);
                else s.selectedObjectIds.insert(id);
            }
        }
    }
    EnqueueCad2DSelectionRefresh(tab.tabID, container);
}


// --- Selection transforms (Commands::EDIT_COPY/OFFSET/MIRROR/ROTATE/MOVE) -----------------------

constexpr double kPi2D = 3.14159265358979323846;

// Rigid point map p' = base + M * (p - pivot); covers translation, rotation and reflection.
struct Cad2DPointMapper {
    double pivotX = 0.0, pivotY = 0.0;
    double baseX = 0.0, baseY = 0.0;
    double m00 = 1.0, m01 = 0.0, m10 = 0.0, m11 = 1.0;

    Cad2DPoint2D Map(double x, double y) const {
        const double vx = x - pivotX, vy = y - pivotY;
        return { baseX + m00 * vx + m01 * vy, baseY + m10 * vx + m11 * vy };
    }
};

// Offset side for a polyline: +1 = left of the direction of travel, -1 = right, decided by which
// side of the segment nearest to the pick point the pick falls on.
bool PolylineOffsetSideFromPick(const std::vector<Cad2DPoint2D>& points, double px, double py,
    double& outSign) {
    double bestDist = 0.0;
    size_t bestSegment = points.size(); // Sentinel: no segment found yet.
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        const double d = DistPointToSegment(px, py, points[i].x, points[i].y,
            points[i + 1].x, points[i + 1].y);
        if (bestSegment == points.size() || d < bestDist) { bestDist = d; bestSegment = i; }
    }
    if (bestSegment == points.size()) return false;
    const Cad2DPoint2D& a = points[bestSegment];
    const Cad2DPoint2D& b = points[bestSegment + 1];
    const double side = (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x);
    if (std::abs(side) <= 1.0e-12) return false; // Pick exactly on the line: side undefined.
    outSign = side > 0.0 ? 1.0 : -1.0;
    return true;
}

std::vector<Cad2DPoint2D> CleanPolylinePoints(const std::vector<Cad2DPoint2D>& points) {
    std::vector<Cad2DPoint2D> cleaned;
    cleaned.reserve(points.size());
    for (const Cad2DPoint2D& p : points) {
        if (!cleaned.empty() &&
            std::hypot(p.x - cleaned.back().x, p.y - cleaned.back().y) <= 1.0e-12) continue;
        cleaned.push_back(p);
    }
    return cleaned;
}

// Offsets an open polyline to one side (sign: +1 left of travel, -1 right) with miter joins.
// Returns false when a segment is degenerate.
bool OffsetPolylinePoints(const std::vector<Cad2DPoint2D>& points, double distance, double sign,
    std::vector<Cad2DPoint2D>& outPoints) {
    if (points.size() < 2) return false;
    const size_t segmentCount = points.size() - 1;
    std::vector<Cad2DPoint2D> dirs(segmentCount);   // Unit segment directions.
    std::vector<Cad2DPoint2D> starts(segmentCount); // Offset segment start points.
    for (size_t i = 0; i < segmentCount; ++i) {
        const double dx = points[i + 1].x - points[i].x, dy = points[i + 1].y - points[i].y;
        const double len = std::hypot(dx, dy);
        if (len <= 1.0e-12) return false;
        dirs[i] = { dx / len, dy / len };
        starts[i] = { points[i].x - dirs[i].y * sign * distance,
                      points[i].y + dirs[i].x * sign * distance };
    }

    outPoints.resize(points.size());
    outPoints.front() = starts.front();
    outPoints.back() = { points.back().x - dirs.back().y * sign * distance,
                         points.back().y + dirs.back().x * sign * distance };
    for (size_t j = 1; j < segmentCount; ++j) {
        const Cad2DPoint2D& d0 = dirs[j - 1];
        const Cad2DPoint2D& d1 = dirs[j];
        const double cross = d0.x * d1.y - d0.y * d1.x;
        if (std::abs(cross) <= 1.0e-9) { // Nearly collinear: plain perpendicular offset.
            outPoints[j] = starts[j];
            continue;
        }
        // Miter join: intersect offset lines (starts[j-1] + t*d0) and (starts[j] + u*d1).
        const double wx = starts[j].x - starts[j - 1].x;
        const double wy = starts[j].y - starts[j - 1].y;
        const double t = (wx * d1.y - wy * d1.x) / cross;
        outPoints[j] = { starts[j - 1].x + d0.x * t, starts[j - 1].y + d0.y * t };
    }
    return true;
}

// Applies the armed transform to every selected object of the active Page2D. Move/Rotate update
// the records in place (ids preserved); Copy/Offset/Mirror enqueue brand-new objects.
void ApplyTransform2DToSelection(DATASETTAB& tab, Cad2DTransformKind kind,
    double p1x, double p1y, double p2x, double p2y, double p3x, double p3y) {
    TabCad2DStorage& s = *tab.cad2d;
    const uint64_t container = Cad2DFindTargetPage2DMemoryId(tab);
    if (container == 0) return;

    std::unordered_set<uint64_t> selected;
    {
        std::lock_guard<std::mutex> lock(s.selection2DMutex);
        selected = s.selectedObjectIds;
    }
    if (selected.empty()) return;

    const bool makesCopy = kind == Cad2DTransformKind::Copy ||
        kind == Cad2DTransformKind::Offset || kind == Cad2DTransformKind::Mirror;
    const double offsetDistance = std::hypot(p2x - p1x, p2y - p1y); // Offset only.

    Cad2DPointMapper map{};
    double rotationDeltaRadians = 0.0;
    double mirrorLineAngleRadians = 0.0;
    if (kind == Cad2DTransformKind::Copy || kind == Cad2DTransformKind::Move) {
        map.pivotX = p1x; map.pivotY = p1y; map.baseX = p2x; map.baseY = p2y;
    } else if (kind == Cad2DTransformKind::Rotate) {
        rotationDeltaRadians = std::atan2(p3y - p1y, p3x - p1x) - std::atan2(p2y - p1y, p2x - p1x);
        const double c = std::cos(rotationDeltaRadians), sn = std::sin(rotationDeltaRadians);
        map.pivotX = p1x; map.pivotY = p1y; map.baseX = p1x; map.baseY = p1y;
        map.m00 = c; map.m01 = -sn; map.m10 = sn; map.m11 = c;
    } else if (kind == Cad2DTransformKind::Mirror) {
        const double len = std::hypot(p2x - p1x, p2y - p1y);
        if (len <= 1.0e-9) return;
        const double ux = (p2x - p1x) / len, uy = (p2y - p1y) / len;
        mirrorLineAngleRadians = std::atan2(uy, ux);
        map.pivotX = p1x; map.pivotY = p1y; map.baseX = p1x; map.baseY = p1y;
        map.m00 = 2.0 * ux * ux - 1.0; map.m01 = 2.0 * ux * uy;
        map.m10 = 2.0 * ux * uy;       map.m11 = 2.0 * uy * uy - 1.0;
    } else if (offsetDistance <= 1.0e-9) {
        return;
    }

    std::vector<Cad2DLineRecordCPU> lines;
    std::vector<Cad2DPolylineRecordCPU> polylines;
    std::vector<Cad2DPolygonRecordCPU> polygons;
    std::vector<Cad2DCircleRecordCPU> circles;
    std::vector<Cad2DEllipseRecordCPU> ellipses;
    std::vector<Cad2DArcRecordCPU> arcs;
    std::vector<Cad2DTextRecordCPU> texts;

    auto asNewObject = [&](auto& record) {
        // A fresh id is taken here rather than left at 0 for EnqueueCad2D* to fill in: META_DATA
        // issues one in its constructor, so "0 means unassigned" is no longer expressible for a
        // migrated record. Save still assigns the persisted ids.
        record.memoryID = MemoryID::next();
        record.persistedId = 0;
        record.persistedParentId = 0;
        record.memoryIDGenerator = 0; // Copies detach from any asset instance (plain page objects).
    };
    auto wanted = [&](uint64_t objectId, bool isDeleted, uint64_t recContainer) {
        return !isDeleted && recContainer == container && selected.count(objectId) != 0;
    };

    {
        std::lock_guard<std::mutex> lock(s.cpuRecordsMutex);
        for (const Cad2DLineRecordCPU& r : s.lineRecords) {
            if (!wanted(r.memoryID, r.isDeleted, r.memoryIDContainer)) continue;
            Cad2DLineRecordCPU out = r;
            if (kind == Cad2DTransformKind::Offset) {
                // Parallel line at the offset distance, toward the side of the second click.
                const double dirX = r.x2 - r.x1, dirY = r.y2 - r.y1;
                const double len = std::hypot(dirX, dirY);
                if (len <= 1.0e-12) continue;
                const double side = dirX * (p2y - r.y1) - dirY * (p2x - r.x1);
                if (std::abs(side) <= 1.0e-12) continue; // Click on the line: side undefined.
                const double sign = side > 0.0 ? 1.0 : -1.0;
                const double nx = -dirY / len * sign * offsetDistance;
                const double ny = dirX / len * sign * offsetDistance;
                out.x1 += nx; out.y1 += ny; out.x2 += nx; out.y2 += ny;
            } else {
                const Cad2DPoint2D a = map.Map(r.x1, r.y1);
                const Cad2DPoint2D b = map.Map(r.x2, r.y2);
                out.x1 = a.x; out.y1 = a.y; out.x2 = b.x; out.y2 = b.y;
            }
            if (makesCopy) asNewObject(out);
            lines.push_back(out);
        }
        for (const Cad2DPolylineRecordCPU& r : s.polylineRecords) {
            if (!wanted(r.memoryID, r.isDeleted, r.memoryIDContainer)) continue;
            Cad2DPolylineRecordCPU out = r;
            if (kind == Cad2DTransformKind::Offset) {
                const std::vector<Cad2DPoint2D> cleaned = CleanPolylinePoints(r.points);
                double sign = 0.0;
                if (!PolylineOffsetSideFromPick(cleaned, p2x, p2y, sign)) continue;
                if (!OffsetPolylinePoints(cleaned, offsetDistance, sign, out.points)) continue;
            } else {
                for (Cad2DPoint2D& p : out.points) p = map.Map(p.x, p.y);
            }
            if (makesCopy) asNewObject(out);
            polylines.push_back(std::move(out));
        }
        for (const Cad2DPolygonRecordCPU& r : s.polygonRecords) {
            if (!wanted(r.memoryID, r.isDeleted, r.memoryIDContainer)) continue;
            Cad2DPolygonRecordCPU out = r;
            if (kind == Cad2DTransformKind::Offset) {
                // Offset the edges: the apothem changes by the distance, so the circumradius
                // changes by distance / cos(pi/n). Second click outside the polygon grows it.
                if (r.radius <= 0.0) continue;
                const uint32_t n = std::clamp(r.lineSegmentCount, 3u, 16u);
                const double cosHalf = (std::max)(std::cos(kPi2D / (double)n), 1.0e-9);
                const double grow =
                    std::hypot(p2x - r.centerX, p2y - r.centerY) > r.radius ? 1.0 : -1.0;
                out.radius = r.radius + grow * offsetDistance / cosHalf;
                if (out.radius <= kMinPolygonRadiusCU) continue;
            } else {
                const Cad2DPoint2D c = map.Map(r.centerX, r.centerY);
                out.centerX = c.x; out.centerY = c.y;
                // Vertex i sits at (center + (sin a, cos a) * radius), a = rotationDegrees + i*step,
                // i.e. param a = 90deg - standard polar angle. A CCW rotation by theta maps a to
                // a - theta; a reflection across a line at standard angle phi maps a to 180 - 2*phi - a.
                if (kind == Cad2DTransformKind::Rotate) {
                    out.rotationDegrees = r.rotationDegrees - rotationDeltaRadians * 180.0 / kPi2D;
                } else if (kind == Cad2DTransformKind::Mirror) {
                    out.rotationDegrees =
                        180.0 - 2.0 * mirrorLineAngleRadians * 180.0 / kPi2D - r.rotationDegrees;
                }
            }
            if (makesCopy) asNewObject(out);
            polygons.push_back(out);
        }
        for (const Cad2DCircleRecordCPU& r : s.circleRecords) {
            if (!wanted(r.memoryID, r.isDeleted, r.memoryIDContainer)) continue;
            Cad2DCircleRecordCPU out = r;
            if (kind == Cad2DTransformKind::Offset) {
                const double grow =
                    std::hypot(p2x - r.centerX, p2y - r.centerY) > r.radius ? 1.0 : -1.0;
                out.radius = r.radius + grow * offsetDistance;
                if (out.radius <= kMinCurveRadiusCU) continue;
            } else {
                const Cad2DPoint2D c = map.Map(r.centerX, r.centerY);
                out.centerX = c.x; out.centerY = c.y;
            }
            if (makesCopy) asNewObject(out);
            circles.push_back(out);
        }
        for (const Cad2DEllipseRecordCPU& r : s.ellipseRecords) {
            if (!wanted(r.memoryID, r.isDeleted, r.memoryIDContainer)) continue;
            Cad2DEllipseRecordCPU out = r;
            if (kind == Cad2DTransformKind::Offset) {
                const double sx = (std::max)(std::abs(r.radiusX), 1.0e-9);
                const double sy = (std::max)(std::abs(r.radiusY), 1.0e-9);
                const double dx = p2x - r.centerX, dy = p2y - r.centerY;
                const double cr = std::cos(r.rotationRadians), sr = std::sin(r.rotationRadians);
                const double nx = (dx * cr + dy * sr) / sx;  // Inside test in the local frame.
                const double ny = (-dx * sr + dy * cr) / sy;
                const double grow = nx * nx + ny * ny > 1.0 ? 1.0 : -1.0;
                out.radiusX = r.radiusX + grow * offsetDistance;
                out.radiusY = r.radiusY + grow * offsetDistance;
                if (out.radiusX <= kMinCurveRadiusCU || out.radiusY <= kMinCurveRadiusCU) continue;
            } else {
                const Cad2DPoint2D c = map.Map(r.centerX, r.centerY);
                out.centerX = c.x; out.centerY = c.y;
                if (kind == Cad2DTransformKind::Rotate) {
                    out.rotationRadians = r.rotationRadians + rotationDeltaRadians;
                } else if (kind == Cad2DTransformKind::Mirror) {
                    out.rotationRadians = 2.0 * mirrorLineAngleRadians - r.rotationRadians;
                }
            }
            if (makesCopy) asNewObject(out);
            ellipses.push_back(out);
        }
        for (const Cad2DArcRecordCPU& r : s.arcRecords) {
            if (!wanted(r.memoryID, r.isDeleted, r.memoryIDContainer)) continue;
            Cad2DArcRecordCPU out = r;
            if (kind == Cad2DTransformKind::Offset) {
                const double sx = (std::max)(std::abs(r.radiusX), 1.0e-9);
                const double sy = (std::max)(std::abs(r.radiusY), 1.0e-9);
                const double cr = std::cos(r.rotationRadians), sr = std::sin(r.rotationRadians);
                const double dx = p2x - r.centerX, dy = p2y - r.centerY;
                const double nx = (dx * cr + dy * sr) / sx;  // Inside test in the local frame.
                const double ny = (-dx * sr + dy * cr) / sy;
                const double grow = nx * nx + ny * ny > 1.0 ? 1.0 : -1.0;
                out.radiusX = r.radiusX + grow * offsetDistance;
                out.radiusY = r.radiusY + grow * offsetDistance;
                if (out.radiusX <= kMinCurveRadiusCU || out.radiusY <= kMinCurveRadiusCU) continue;
                // Rescale the end points per local axis so they stay on the new curve.
                auto rescale = [&](double wx, double wy, double& ox, double& oy) {
                    const double ex = wx - r.centerX, ey = wy - r.centerY;
                    const double lx = (ex * cr + ey * sr) * (out.radiusX / sx);
                    const double ly = (-ex * sr + ey * cr) * (out.radiusY / sy);
                    ox = r.centerX + lx * cr - ly * sr;
                    oy = r.centerY + lx * sr + ly * cr;
                };
                rescale(r.startX, r.startY, out.startX, out.startY);
                rescale(r.endX, r.endY, out.endX, out.endY);
            } else {
                const Cad2DPoint2D c = map.Map(r.centerX, r.centerY);
                const Cad2DPoint2D st = map.Map(r.startX, r.startY);
                const Cad2DPoint2D en = map.Map(r.endX, r.endY);
                out.centerX = c.x; out.centerY = c.y;
                if (kind == Cad2DTransformKind::Rotate) {
                    out.rotationRadians = r.rotationRadians + rotationDeltaRadians;
                }
                if (kind == Cad2DTransformKind::Mirror) {
                    // Arcs sweep CCW from start to end; a reflection reverses the orientation,
                    // so swap the end points to keep the same swept region. The reflected local
                    // frame is rotated to 2*phi - theta.
                    out.rotationRadians = 2.0 * mirrorLineAngleRadians - r.rotationRadians;
                    out.startX = en.x; out.startY = en.y;
                    out.endX = st.x; out.endY = st.y;
                } else {
                    out.startX = st.x; out.startY = st.y;
                    out.endX = en.x; out.endY = en.y;
                }
            }
            if (makesCopy) asNewObject(out);
            arcs.push_back(out);
        }
        if (kind != Cad2DTransformKind::Offset) { // Offset is not defined for text.
            for (const Cad2DTextRecordCPU& r : s.textRecords) {
                if (!wanted(r.memoryID, r.isDeleted, r.memoryIDContainer)) continue;
                Cad2DTextRecordCPU out = r;
                // The rendered origin is (x + xOffsetCU, y + yOffsetCU); map that effective
                // point so the offsets keep working unchanged.
                const Cad2DPoint2D o =
                    map.Map(r.x + (double)r.xOffsetCU, r.y + (double)r.yOffsetCU);
                out.x = o.x - (double)r.xOffsetCU;
                out.y = o.y - (double)r.yOffsetCU;
                if (kind == Cad2DTransformKind::Rotate) {
                    out.rotationRadians = r.rotationRadians + (float)rotationDeltaRadians;
                } else if (kind == Cad2DTransformKind::Mirror) {
                    // Reflect the baseline direction; glyphs stay readable (not mirrored).
                    out.rotationRadians =
                        (float)(2.0 * mirrorLineAngleRadians - (double)r.rotationRadians);
                }
                if (makesCopy) asNewObject(out);
                texts.push_back(std::move(out));
            }
        }
    }

    for (Cad2DLineRecordCPU& r : lines) EnqueueCad2DLine(tab.tabID, container, r);
    for (Cad2DPolylineRecordCPU& r : polylines) EnqueueCad2DPolyline(tab.tabID, container, std::move(r));
    for (Cad2DPolygonRecordCPU& r : polygons) EnqueueCad2DPolygon(tab.tabID, container, r);
    for (Cad2DCircleRecordCPU& r : circles) EnqueueCad2DCircle(tab.tabID, container, r);
    for (Cad2DEllipseRecordCPU& r : ellipses) EnqueueCad2DEllipse(tab.tabID, container, r);
    for (Cad2DArcRecordCPU& r : arcs) EnqueueCad2DArc(tab.tabID, container, r);
    for (Cad2DTextRecordCPU& r : texts) EnqueueCad2DText(tab.tabID, container, std::move(r));
}

void HandleTransform2DClick(DATASETTAB& tab, double xCU, double yCU) {
    TabCad2DStorage& s = *tab.cad2d;
    const auto kind = static_cast<Cad2DTransformKind>(s.transform2DKind.load(std::memory_order_acquire));
    if (kind == Cad2DTransformKind::None) return;
    const uint32_t step = s.transform2DStep.load(std::memory_order_acquire);
    const uint32_t pointsNeeded = kind == Cad2DTransformKind::Rotate ? 3u : 2u;

    if (step == 0) {
        s.transform2DP1XCU.store(xCU, std::memory_order_release);
        s.transform2DP1YCU.store(yCU, std::memory_order_release);
        s.transform2DStep.store(1, std::memory_order_release);
        return;
    }

    const double p1x = s.transform2DP1XCU.load(std::memory_order_acquire);
    const double p1y = s.transform2DP1YCU.load(std::memory_order_acquire);
    if (std::hypot(xCU - p1x, yCU - p1y) <= 1.0e-9) return; // Coincident with the first point.

    if (step == 1 && pointsNeeded == 3) {
        s.transform2DP2XCU.store(xCU, std::memory_order_release);
        s.transform2DP2YCU.store(yCU, std::memory_order_release);
        s.transform2DStep.store(2, std::memory_order_release);
        return;
    }

    double p2x = xCU, p2y = yCU, p3x = 0.0, p3y = 0.0;
    if (pointsNeeded == 3) {
        p2x = s.transform2DP2XCU.load(std::memory_order_acquire);
        p2y = s.transform2DP2YCU.load(std::memory_order_acquire);
        p3x = xCU; p3y = yCU;
    }

    ApplyTransform2DToSelection(tab, kind, p1x, p1y, p2x, p2y, p3x, p3y);
    s.transform2DKind.store(0, std::memory_order_release); // One-shot; re-arm from the ribbon.
    s.transform2DStep.store(0, std::memory_order_release);
}

// Places one instance of the asset chosen in the Insert Asset pane at the clicked point (defaults
// to the first definition). The mode stays armed for repeated placing.
void HandleAssetInsertClick(DATASETTAB& tab, double xCU, double yCU) {
    TabCad2DStorage& s = *tab.cad2d;
    const uint64_t container = Cad2DFindTargetPage2DMemoryId(tab);
    if (container == 0) return;

    uint64_t definitionId = 0;
    {
        std::lock_guard<std::mutex> lock(s.cpuRecordsMutex);
        // The pane's selection when it is still a live definition, else the first available one.
        const uint64_t wantedId = s.assetInsertSelectedDefinitionId.load(std::memory_order_acquire);
        for (const Cad2DAssetDefinitionRecordCPU& d : s.assetDefinitionRecords) {
            if (d.isDeleted) continue;
            if (definitionId == 0) definitionId = d.memoryID;
            if (wantedId != 0 && d.memoryID == wantedId) { definitionId = d.memoryID; break; }
        }
    }
    if (definitionId == 0) return;
    Cad2DInstantiateAsset(tab, container, definitionId, xCU, yCU);
}

// Declared near Page2DCoordinateFromInput; see the contract there.
static bool Page2DSnappedPointFromInput(DATASETTAB& tab, const ACTION_DETAILS& input,
    double& outXCU, double& outYCU, SnapResult* outResult) {
    SnapResult result{};
    if (!Cad2DResolveSnap(tab, input, outXCU, outYCU, result)) return false;
    if (outResult) *outResult = result;
    return true;
}
} // namespace

/* Publishes the hover marker for one cursor position (section 12). Engineering thread writes,
render thread reads, one frame stale and one direction only - a click never reads this back.

Resolution here is the SAME call the click makes, so the marker can never promise a point the click
would not commit. Section 8's other mitigation is the caller's: this runs once per drain of the
input queue, from the coalesced snapHoverInput, not once per mouse report. */
void Cad2DPublishSnapHover(DATASETTAB& tab, const ACTION_DETAILS& input) {
    if (!tab.cad2d || !Cad2DIsActivePage2D(tab) || tab.mouseMiddleDown) {
        tab.snapHover.Clear();
        return;
    }

    double xCU = 0.0, yCU = 0.0;
    SnapResult result{};
    if (!Cad2DResolveSnap(tab, input, xCU, yCU, result) || !result.hit) {
        tab.snapHover.Clear();
        return;
    }

    int viewportWidth = 0, viewportHeight = 0, viewportTop = 0;
    if (!GetVisibleSceneViewportForTab(tab, viewportWidth, viewportHeight, viewportTop)) {
        tab.snapHover.Clear();
        return;
    }
    const Cad2DViewState& view = Cad2DInputView(tab);
    const double zoom = (std::max)(
        (double)view.zoomPixelsPerCU.load(std::memory_order_acquire),
        (double)kCad2DZoomMinPixelsPerCU);
    const double centerX = view.centerXCU.load(std::memory_order_acquire);
    const double centerY = view.centerYCU.load(std::memory_order_acquire);
    // The inverse of Page2DCoordinateFromInput: CU back to the client pixel the marker is drawn at.
    const int screenX = (int)std::lround((xCU - centerX) * zoom + (double)viewportWidth * 0.5);
    const int screenY = (int)std::lround((double)viewportTop + (double)viewportHeight * 0.5 -
        (yCU - centerY) * zoom);

    const uint8_t kind = static_cast<uint8_t>(result.kind);
    // GetTickCount64 is the timestamp this codebase already shares between the engineering and
    // render threads (SelectionState::lastNavInteractionMs). The label delay compares a value
    // written here against one read there, so the two MUST be the same clock.
    const uint64_t nowMs = GetTickCount64();
    // The label delay times how long THIS point has been the answer, so a cursor sweeping a dense
    // drawing does not strobe labels; sliding along one edge still shows one after 400 ms.
    const bool sameAsBefore = tab.snapHover.valid.load(std::memory_order_acquire) &&
        tab.snapHover.kind.load(std::memory_order_acquire) == kind &&
        tab.snapHover.screenX.load(std::memory_order_acquire) == screenX &&
        tab.snapHover.screenY.load(std::memory_order_acquire) == screenY;
    if (!sameAsBefore) tab.snapHover.sinceMs.store(nowMs, std::memory_order_release);

    tab.snapHover.screenX.store(screenX, std::memory_order_release);
    tab.snapHover.screenY.store(screenY, std::memory_order_release);
    tab.snapHover.kind.store(kind, std::memory_order_release);
    tab.snapHover.subTabSlot.store(InputViewSlot(tab), std::memory_order_release);
    tab.snapHover.valid.store(true, std::memory_order_release);
}

bool Cad2DReadPaneSelection(DATASETTAB& tab, Cad2DPaneSelection& out) {
    out = Cad2DPaneSelection{};
    if (!tab.cad2d || !Cad2DIsActivePage2D(tab)) return false; // Not a 2D view: caller uses 3D.
    TabCad2DStorage& s = *tab.cad2d;

    uint64_t singleId = 0;
    {
        std::lock_guard<std::mutex> lock(s.selection2DMutex);
        out.count = s.selectedObjectIds.size();
        if (out.count == 1) singleId = *s.selectedObjectIds.begin();
    }
    if (singleId == 0) return true; // Empty or multiple: the pane draws its count line.
    out.objectId = singleId;

    /* One index lookup, not a scan of up to all seven vectors. This runs
    on the RENDER thread, once per frame per monitor for as long as the pane shows a 2D object,
    and it holds cpuRecordsMutex while it does - so at a million records the scan was blocking the
    engineering thread every frame. Object ids are unique across the process, so the type recorded
    beside the position is enough to pick the vector. */
    std::lock_guard<std::mutex> lock(s.cpuRecordsMutex);
    auto found = s.recordIndex.find(singleId);
    if (found == s.recordIndex.end()) return true;
    const uint32_t at = found->second.index;
    auto take = [&](const auto& records, VishwakarmaStorage::ObjectType type) {
        if (at >= records.size() || records[at].isDeleted) return;
        out.objectType = type;
        // nullptr for POLYLINE2D / POLYGON2D / TEXT2D: variable-arity types have no field
        // table, so the pane shows Type + ID only, as it does for the vertex-list solids.
        out.table = FindPropertyTable(type);
        if (out.table) {
            out.valueCount = out.table->fieldCount;
            ReadPropertyValuesRaw(*out.table, &records[at], out.values);
        }
    };
    switch (found->second.type) {
    case VishwakarmaStorage::ObjectType::Line2D:     take(s.lineRecords, found->second.type); break;
    case VishwakarmaStorage::ObjectType::Polyline2D: take(s.polylineRecords, found->second.type); break;
    case VishwakarmaStorage::ObjectType::Polygon2D:  take(s.polygonRecords, found->second.type); break;
    case VishwakarmaStorage::ObjectType::Circle2D:   take(s.circleRecords, found->second.type); break;
    case VishwakarmaStorage::ObjectType::Ellipse2D:  take(s.ellipseRecords, found->second.type); break;
    case VishwakarmaStorage::ObjectType::Arc2D:      take(s.arcRecords, found->second.type); break;
    case VishwakarmaStorage::ObjectType::Text2D:     take(s.textRecords, found->second.type); break;
    default: break; // The two asset types are in the index but never selectable.
    }
    return true;
}

bool Cad2DInstantiateAsset(DATASETTAB& tab, uint64_t containerMemoryId, uint64_t definitionObjectId,
    double xCU, double yCU, double scaleX, double scaleY, double rotationDegrees) {
    if (!tab.cad2d || containerMemoryId == 0 || definitionObjectId == 0) return false;
    if (!std::isfinite(scaleX) || !std::isfinite(scaleY) || !std::isfinite(rotationDegrees) ||
        std::abs(scaleX) < 1.0e-9 || std::abs(scaleY) < 1.0e-9) {
        return false; // Degenerate transform would collapse the members.
    }
    TabCad2DStorage& s = *tab.cad2d;

    std::vector<Cad2DLineRecordCPU> lines;
    std::vector<Cad2DPolylineRecordCPU> polylines;
    std::vector<Cad2DPolygonRecordCPU> polygons;
    std::vector<Cad2DCircleRecordCPU> circles;
    std::vector<Cad2DEllipseRecordCPU> ellipses;
    std::vector<Cad2DArcRecordCPU> arcs;
    std::vector<Cad2DTextRecordCPU> texts;

    {
        std::lock_guard<std::mutex> lock(s.cpuRecordsMutex);
        double baseX = 0.0, baseY = 0.0;
        bool found = false;
        for (const Cad2DAssetDefinitionRecordCPU& d : s.assetDefinitionRecords) {
            if (!d.isDeleted && d.memoryID == definitionObjectId) {
                baseX = d.baseX; baseY = d.baseY; found = true; break;
            }
        }
        if (!found) return false;

        Cad2DAssetInsertRecordCPU insert{};
        insert.memoryID = MemoryID::next();
        insert.memoryIDContainer = containerMemoryId;
        insert.definitionObjectId = definitionObjectId;
        insert.x = xCU;
        insert.y = yCU;
        insert.scaleX = scaleX;
        insert.scaleY = scaleY;
        insert.rotationDegrees = rotationDegrees;
        insert.schemaVersion = VishwakarmaStorage::kAsset2DInsertSchemaVersion;

        // Point map member = insert + R(theta) * S(scaleX, scaleY) * (master - base).
        const double theta = rotationDegrees * kPi2D / 180.0;
        const double cosT = std::cos(theta), sinT = std::sin(theta);
        const double m00 = cosT * scaleX, m01 = -sinT * scaleY;
        const double m10 = sinT * scaleX, m11 = cosT * scaleY;
        auto mapPoint = [&](double px, double py) -> Cad2DPoint2D {
            const double vx = px - baseX, vy = py - baseY;
            return { xCU + m00 * vx + m01 * vy, yCU + m10 * vx + m11 * vy };
        };

        // Angle fields cannot go through the point map. Decompose the negative-scale part:
        // both axes negative is an extra 180deg rotation; exactly one negative axis is a
        // reflection (across the y-axis for scaleX < 0: line angle phi = 90deg, across the
        // x-axis for scaleY < 0: phi = 0). Then apply the same angle formulas as the
        // EDIT_ROTATE / EDIT_MIRROR selection transforms above. Non-uniform |scale| on the
        // parametric round shapes is an approximation (a squashed circle is not a circle).
        const bool negX = scaleX < 0.0, negY = scaleY < 0.0;
        const bool mirrored = negX != negY;
        const double thetaEff = (negX && negY) ? theta + kPi2D : theta;
        const double mirrorPhi = negX ? kPi2D / 2.0 : 0.0;
        auto mapRotationRadians = [&](double rot) { // Ellipse / arc / text rotation fields.
            return mirrored ? (2.0 * mirrorPhi - rot) + thetaEff : rot + thetaEff;
        };
        auto mapPolygonDegrees = [&](double a) {    // Polygon param angle a = 90deg - polar.
            const double thetaEffDeg = thetaEff * 180.0 / kPi2D;
            return mirrored ? (180.0 - 2.0 * mirrorPhi * 180.0 / kPi2D - a) - thetaEffDeg
                            : a - thetaEffDeg;
        };
        const double absX = std::abs(scaleX), absY = std::abs(scaleY);
        const double radiusScale = (std::max)(absX, absY); // Circle / polygon approximation.

        // Copy every master record of this definition, transform it and re-parent to the insert.
        auto instantiate = [&](const auto& records, auto& out, auto&& transform) {
            for (const auto& r : records) {
                if (r.isDeleted || r.memoryIDGenerator != definitionObjectId) continue;
                auto member = r;
                /* A fresh id is taken HERE rather than left at 0 for EnqueueCad2D* to fill in.
                META_DATA issues an id in its constructor, so a migrated record's memoryID is
                never 0 and "0 means unassigned" has stopped being expressible. Assigning eagerly
                is what both halves can agree on; the Enqueue path's own `== 0` test simply does
                not fire. */
                member.memoryID = MemoryID::next();
                member.persistedId = 0;
                member.persistedParentId = 0;
                member.memoryIDGenerator = insert.memoryID;
                member.memoryIDContainer = containerMemoryId;
                transform(member);
                out.push_back(std::move(member));
            }
        };
        instantiate(s.lineRecords, lines, [&](Cad2DLineRecordCPU& r) {
            const Cad2DPoint2D a = mapPoint(r.x1, r.y1);
            const Cad2DPoint2D b = mapPoint(r.x2, r.y2);
            r.x1 = a.x; r.y1 = a.y; r.x2 = b.x; r.y2 = b.y; });
        instantiate(s.polylineRecords, polylines, [&](Cad2DPolylineRecordCPU& r) {
            for (Cad2DPoint2D& p : r.points) p = mapPoint(p.x, p.y); });
        instantiate(s.polygonRecords, polygons, [&](Cad2DPolygonRecordCPU& r) {
            const Cad2DPoint2D c = mapPoint(r.centerX, r.centerY);
            r.centerX = c.x; r.centerY = c.y;
            r.radius *= radiusScale;
            r.rotationDegrees = mapPolygonDegrees(r.rotationDegrees); });
        instantiate(s.circleRecords, circles, [&](Cad2DCircleRecordCPU& r) {
            const Cad2DPoint2D c = mapPoint(r.centerX, r.centerY);
            r.centerX = c.x; r.centerY = c.y;
            r.radius *= radiusScale; });
        instantiate(s.ellipseRecords, ellipses, [&](Cad2DEllipseRecordCPU& r) {
            const Cad2DPoint2D c = mapPoint(r.centerX, r.centerY);
            r.centerX = c.x; r.centerY = c.y;
            r.radiusX *= absX; // Exact for uniform scale; axis-aligned approximation otherwise.
            r.radiusY *= absY;
            r.rotationRadians = mapRotationRadians(r.rotationRadians); });
        instantiate(s.arcRecords, arcs, [&](Cad2DArcRecordCPU& r) {
            const Cad2DPoint2D c = mapPoint(r.centerX, r.centerY);
            const Cad2DPoint2D st = mapPoint(r.startX, r.startY);
            const Cad2DPoint2D en = mapPoint(r.endX, r.endY);
            r.centerX = c.x; r.centerY = c.y;
            r.radiusX *= absX;
            r.radiusY *= absY;
            r.rotationRadians = mapRotationRadians(r.rotationRadians);
            if (mirrored) { // Reflection reverses the CCW sweep: swap the end points.
                r.startX = en.x; r.startY = en.y;
                r.endX = st.x; r.endY = st.y;
            } else {
                r.startX = st.x; r.startY = st.y;
                r.endX = en.x; r.endY = en.y;
            } });
        instantiate(s.textRecords, texts, [&](Cad2DTextRecordCPU& r) {
            // Map the effective origin (x + offset) like the selection transforms do; glyphs
            // stay readable under mirror (baseline direction reflects, height stays positive).
            const Cad2DPoint2D o = mapPoint(r.x + (double)r.xOffsetCU, r.y + (double)r.yOffsetCU);
            r.x = o.x - (double)r.xOffsetCU;
            r.y = o.y - (double)r.yOffsetCU;
            r.textHeightCU = (float)((double)r.textHeightCU * absY);
            r.rotationRadians = (float)mapRotationRadians((double)r.rotationRadians); });

        s.assetInsertRecords.push_back(insert);
        Cad2DIndexAppendedRecord(s, s.assetInsertRecords);
        tab.allIDsInThisTab.push_back(insert.memoryID);
    }

    for (Cad2DLineRecordCPU& r : lines) EnqueueCad2DLine(tab.tabID, containerMemoryId, r);
    for (Cad2DPolylineRecordCPU& r : polylines) EnqueueCad2DPolyline(tab.tabID, containerMemoryId, std::move(r));
    for (Cad2DPolygonRecordCPU& r : polygons) EnqueueCad2DPolygon(tab.tabID, containerMemoryId, r);
    for (Cad2DCircleRecordCPU& r : circles) EnqueueCad2DCircle(tab.tabID, containerMemoryId, r);
    for (Cad2DEllipseRecordCPU& r : ellipses) EnqueueCad2DEllipse(tab.tabID, containerMemoryId, r);
    for (Cad2DArcRecordCPU& r : arcs) EnqueueCad2DArc(tab.tabID, containerMemoryId, r);
    for (Cad2DTextRecordCPU& r : texts) EnqueueCad2DText(tab.tabID, containerMemoryId, std::move(r));
    return true;
}

uint64_t Cad2DCreateAssetDefinition(DATASETTAB& tab, double baseX, double baseY,
    const std::vector<Cad2DLineRecordCPU>& masterLines,
    const std::vector<Cad2DTextRecordCPU>& masterTexts,
    const std::vector<Cad2DPolygonRecordCPU>& masterPolygons) {
    if (!tab.cad2d) return 0;
    if (masterLines.empty() && masterTexts.empty() && masterPolygons.empty()) return 0;
    TabCad2DStorage& s = *tab.cad2d;

    std::lock_guard<std::mutex> lock(s.cpuRecordsMutex);

    Cad2DAssetDefinitionRecordCPU definition{};
    definition.memoryID = MemoryID::next();
    definition.assetNumber = GenerateUniqueAssetNumberLocked(s);
    definition.baseX = baseX;
    definition.baseY = baseY;
    definition.schemaVersion = VishwakarmaStorage::kAsset2DDefinitionSchemaVersion;

    // Hidden master geometry: containerMemoryId 0 keeps it off every page (never rendered /
    // hit-tested / zoom-fit); parentObjectId links it to the definition. Added directly (not
    // enqueued: the copy thread drops container-0 commands).
    auto addMaster = [&](auto master, auto& records) {
        master.memoryID = MemoryID::next();
        master.persistedId = 0;
        master.persistedParentId = 0;
        master.memoryIDGenerator = definition.memoryID;
        master.memoryIDContainer = 0;
        tab.allIDsInThisTab.push_back(master.memoryID);
        records.push_back(std::move(master));
        Cad2DIndexAppendedRecord(s, records); // Third appender to the record vectors.
    };
    for (const Cad2DLineRecordCPU& r : masterLines) addMaster(r, s.lineRecords);
    for (const Cad2DTextRecordCPU& r : masterTexts) addMaster(r, s.textRecords);
    for (const Cad2DPolygonRecordCPU& r : masterPolygons) addMaster(r, s.polygonRecords);

    s.assetDefinitionRecords.push_back(definition);
    Cad2DIndexAppendedRecord(s, s.assetDefinitionRecords);
    tab.allIDsInThisTab.push_back(definition.memoryID);
    return definition.memoryID;
}

bool Cad2DHandleInput(DATASETTAB& tab, const ACTION_DETAILS& input) {
    if (!tab.cad2d) return false;
    if (!Cad2DIsActivePage2D(tab)) {
        const bool anyCreationMode =
            tab.cad2d->lineCreationMode.load(std::memory_order_acquire) ||
            tab.cad2d->polylineCreationMode.load(std::memory_order_acquire) ||
            tab.cad2d->polygonCreationMode.load(std::memory_order_acquire) ||
            tab.cad2d->circleCreationMode.load(std::memory_order_acquire) ||
            tab.cad2d->ellipseCreationMode.load(std::memory_order_acquire) ||
            tab.cad2d->arcCreationMode.load(std::memory_order_acquire) ||
            tab.cad2d->textCreationMode.load(std::memory_order_acquire) ||
            tab.cad2d->assetInsertMode.load(std::memory_order_acquire) ||
            tab.cad2d->transform2DKind.load(std::memory_order_acquire) != 0;
        if (input.actionType == ACTION_TYPE::KEYDOWN && input.x == VK_ESCAPE && anyCreationMode) {
            ClearLineCreationState(*tab.cad2d);
            return true;
        }
        return false;
    }

    switch (input.actionType) {
    case ACTION_TYPE::KEYDOWN:
        /* The industry-standard snapping function keys, so muscle memory transfers (section 13).
        F3 is the master object-snap switch and F8 is ortho; the ambient grid has no key because it
        has no off state. Checked before the text-creation swallow below - a user typing a text
        string still expects F8 to mean ortho, not a character. */
        if (input.x == VK_F3) {
            const bool wasOn = tab.snapObjectEnabled2D.load(std::memory_order_acquire);
            tab.snapObjectEnabled2D.store(!wasOn, std::memory_order_release);
            return true;
        }
        if (input.x == VK_F8) {
            const bool wasOn = tab.snapOrtho2D.load(std::memory_order_acquire);
            tab.snapOrtho2D.store(!wasOn, std::memory_order_release);
            return true;
        }
        if (input.x == VK_ESCAPE &&
            (tab.cad2d->lineCreationMode.load(std::memory_order_acquire) ||
                tab.cad2d->polylineCreationMode.load(std::memory_order_acquire) ||
                tab.cad2d->polygonCreationMode.load(std::memory_order_acquire) ||
                tab.cad2d->circleCreationMode.load(std::memory_order_acquire) ||
                tab.cad2d->ellipseCreationMode.load(std::memory_order_acquire) ||
                tab.cad2d->arcCreationMode.load(std::memory_order_acquire) ||
                tab.cad2d->textCreationMode.load(std::memory_order_acquire) ||
                tab.cad2d->assetInsertMode.load(std::memory_order_acquire) ||
                tab.cad2d->transform2DKind.load(std::memory_order_acquire) != 0)) {
            ClearLineCreationState(*tab.cad2d);
            return true;
        }
        if (tab.cad2d->textCreationMode.load(std::memory_order_acquire)) {
            return true;
        }
        break;
    case ACTION_TYPE::KEYUP:
        if (tab.cad2d->textCreationMode.load(std::memory_order_acquire)) {
            return true;
        }
        break;
    case ACTION_TYPE::CHAR:
        if (tab.cad2d->textCreationMode.load(std::memory_order_acquire)) {
            return HandleTextCreationChar(tab, input.x);
        }
        break;
    case ACTION_TYPE::MOUSEMOVE:
    {
        const int dx = input.x - tab.lastMouseX;
        const int dy = input.y - tab.lastMouseY;
        if (tab.mouseMiddleDown) {
            Cad2DViewState& view = Cad2DInputView(tab);
            const float zoom = (std::max)(view.zoomPixelsPerCU.load(std::memory_order_acquire),
                kCad2DZoomMinPixelsPerCU);
            const double currentX = view.centerXCU.load(std::memory_order_acquire);
            const double currentY = view.centerYCU.load(std::memory_order_acquire);
            view.centerXCU.store(currentX - (double)dx / (double)zoom, std::memory_order_release);
            view.centerYCU.store(currentY + (double)dy / (double)zoom, std::memory_order_release);
        }
        tab.lastMouseX = input.x;
        tab.lastMouseY = input.y;
        // Coalesced, not resolved here: a 1000 Hz mouse would otherwise run a full candidate
        // scan per report for a marker the screen redraws 60 times a second (section 8).
        tab.snapHoverPending = true;
        tab.snapHoverInput = input;
        return true;
    }
    case ACTION_TYPE::MOUSEWHEEL:
    {
        Cad2DViewState& view = Cad2DInputView(tab);
        const float wheelSteps = input.delta / (float)WHEEL_DELTA;
        const float currentZoom = view.zoomPixelsPerCU.load(std::memory_order_acquire);
        const float zoomFactor = std::pow(1.12f, wheelSteps);
        const float nextZoom = std::clamp(currentZoom * zoomFactor,
            kCad2DZoomMinPixelsPerCU, kCad2DZoomMaxPixelsPerCU);
        const double currentX = view.centerXCU.load(std::memory_order_acquire);
        const double currentY = view.centerYCU.load(std::memory_order_acquire);
        int viewportWidth = 0, viewportHeight = 0, viewportTop = 0;
        if (GetVisibleSceneViewportForTab(tab, viewportWidth, viewportHeight, viewportTop)) {
            const double mouseX = std::clamp((double)input.x, 0.0, (double)viewportWidth);
            const double mouseY = std::clamp((double)(input.y - viewportTop), 0.0, (double)viewportHeight);
            const double offsetX = mouseX - (double)viewportWidth * 0.5;
            const double offsetY = (double)viewportHeight * 0.5 - mouseY;
            const double cursorX = currentX + offsetX / (double)currentZoom;
            const double cursorY = currentY + offsetY / (double)currentZoom;
            view.centerXCU.store(cursorX - offsetX / (double)nextZoom, std::memory_order_release);
            view.centerYCU.store(cursorY - offsetY / (double)nextZoom, std::memory_order_release);
        }
        view.zoomPixelsPerCU.store(nextZoom, std::memory_order_release);
        // The ambient step is derived from the zoom, so a wheel notch moves the marker even though
        // the cursor did not.
        tab.snapHoverPending = true;
        tab.snapHoverInput = input;
        return true;
    }
    case ACTION_TYPE::MBUTTONDOWN:
        tab.mouseMiddleDown = true;
        tab.lastMouseX = input.x;
        tab.lastMouseY = input.y;
        return true;
    case ACTION_TYPE::MBUTTONUP:
        tab.mouseMiddleDown = false;
        tab.lastMouseX = input.x;
        tab.lastMouseY = input.y;
        return true;
    case ACTION_TYPE::LBUTTONDOWN:
        tab.mouseLeftDown = true;
        if (tab.cad2d->lineCreationMode.load(std::memory_order_acquire)) {
            double xCU = 0.0, yCU = 0.0;
            if (Page2DSnappedPointFromInput(tab, input, xCU, yCU)) {
                HandleLineCreationClick(tab, xCU, yCU);
            }
        }
        else if (tab.cad2d->polylineCreationMode.load(std::memory_order_acquire)) {
            double xCU = 0.0, yCU = 0.0;
            if (Page2DSnappedPointFromInput(tab, input, xCU, yCU)) {
                HandlePolylineCreationClick(tab, xCU, yCU);
            }
        }
        else if (tab.cad2d->polygonCreationMode.load(std::memory_order_acquire)) {
            double xCU = 0.0, yCU = 0.0;
            if (Page2DSnappedPointFromInput(tab, input, xCU, yCU)) {
                HandlePolygonCreationClick(tab, xCU, yCU);
            }
        }
        else if (tab.cad2d->circleCreationMode.load(std::memory_order_acquire)) {
            double xCU = 0.0, yCU = 0.0;
            if (Page2DSnappedPointFromInput(tab, input, xCU, yCU)) {
                HandleCircleCreationClick(tab, xCU, yCU);
            }
        }
        else if (tab.cad2d->ellipseCreationMode.load(std::memory_order_acquire)) {
            double xCU = 0.0, yCU = 0.0;
            if (Page2DSnappedPointFromInput(tab, input, xCU, yCU)) {
                HandleEllipseCreationClick(tab, xCU, yCU);
            }
        }
        else if (tab.cad2d->arcCreationMode.load(std::memory_order_acquire)) {
            double xCU = 0.0, yCU = 0.0;
            if (Page2DSnappedPointFromInput(tab, input, xCU, yCU)) {
                HandleArcCreationClick(tab, xCU, yCU);
            }
        }
        else if (tab.cad2d->textCreationMode.load(std::memory_order_acquire)) {
            double xCU = 0.0, yCU = 0.0;
            if (Page2DSnappedPointFromInput(tab, input, xCU, yCU)) {
                HandleTextCreationClick(tab, xCU, yCU);
            }
        }
        else if (tab.cad2d->assetInsertMode.load(std::memory_order_acquire)) {
            double xCU = 0.0, yCU = 0.0;
            if (Page2DSnappedPointFromInput(tab, input, xCU, yCU)) {
                HandleAssetInsertClick(tab, xCU, yCU);
            }
        }
        else if (tab.cad2d->transform2DKind.load(std::memory_order_acquire) != 0) {
            double xCU = 0.0, yCU = 0.0;
            if (Page2DSnappedPointFromInput(tab, input, xCU, yCU)) {
                HandleTransform2DClick(tab, xCU, yCU);
            }
        }
        else {
            // No creation tool active: treat the click as a selection pick.
            double xCU = 0.0, yCU = 0.0;
            if (Page2DCoordinateFromInput(tab, input, xCU, yCU)) {
                Cad2DHandleSelectionClick(tab, xCU, yCU);
            }
        }
        return true;
    case ACTION_TYPE::LBUTTONUP:
        tab.mouseLeftDown = false;
        return true;
    case ACTION_TYPE::RBUTTONDOWN:
        tab.mouseRightDown = true;
        return true;
    case ACTION_TYPE::RBUTTONUP:
        tab.mouseRightDown = false;
        return true;
    case ACTION_TYPE::CAPTURECHANGED:
    case ACTION_TYPE::INPUT:
        tab.mouseLeftDown = false;
        tab.mouseMiddleDown = false;
        tab.mouseRightDown = false;
        // Lost capture means the cursor is no longer ours to reason about; a marker left behind
        // would sit on the drawing claiming a snap that is not being offered.
        tab.snapHover.Clear();
        return true;
    default:
        break;
    }

    return false;
}

void Cad2DAutoGenerateDemoContent(DATASETTAB& tab) {
    if (!tab.cad2d) return;

    const uint64_t page2DMemoryId = Cad2DFindTargetPage2DMemoryId(tab);
    if (page2DMemoryId == 0) return;

    if (!tab.cad2d->demoTextQueued.exchange(true, std::memory_order_acq_rel)) {
        Cad2DTextRecordCPU text{};
        text.memoryIDContainer = page2DMemoryId;
        text.x = -180.0;
        text.y = 110.0;
        text.textHeightCU = 9.0f;
        text.rotationRadians = 0.0f;
        text.colorABGR = 0xFF000000u;
        text.font = 0;
        text.justification = Cad2DTextJustification::Center;
        text.schemaVersion = VishwakarmaStorage::kGeometry2DTextSchemaVersion;
        text.text = "Page2D GPU text - Noto Sans";
        EnqueueCad2DText(tab.tabID, page2DMemoryId, std::move(text));
    }

    const uint32_t n = tab.cad2d->demoLineCounter.fetch_add(1, std::memory_order_acq_rel);
    const double a0 = (double)n * 0.319;
    const double a1 = a0 + 1.15 + (double)(n % 7) * 0.071;
    const double r0 = 45.0 + (double)(n % 23) * 8.0;
    const double r1 = 70.0 + (double)((n * 5) % 29) * 6.0;
    // Subtract in double: in uint32 math (n % 9) - 4 wraps to ~4.29e9 whenever n % 9 < 4,
    // planting demo lines at ~7.7e10 CU and blowing up every zoom-extents fit.
    const double cx = ((double)(n % 9) - 4.0) * 18.0;
    const double cy = ((double)((n / 9) % 7) - 3.0) * 16.0;

    Cad2DLineRecordCPU line{};
    line.memoryIDContainer = page2DMemoryId;
    line.x1 = cx + std::cos(a0) * r0;
    line.y1 = cy + std::sin(a0) * r0;
    line.x2 = cx + std::cos(a1) * r1;
    line.y2 = cy + std::sin(a1) * r1;
    line.lineWeight = 1.0f;
    line.lineWeightMode = Cad2DLineWeightMode::ScreenPixel;
    line.colorABGR = 0xFF000000u;
    EnqueueCad2DLine(tab.tabID, page2DMemoryId, line);
}

void Cad2DGenerateBulkLines(DATASETTAB& tab, uint32_t count) {
    if (!tab.cad2d || count == 0) return;
    const uint64_t page2DMemoryId = Cad2DFindTargetPage2DMemoryId(tab);
    if (page2DMemoryId == 0) return;

    /* A 10 CU cell, 1000 cells per row: a million lines is a 10,000 x 10,000 CU sheet that a human
    can actually look at, and cell n is always the same two points, so two runs produce the same
    drawing. The alternating diagonal is not decoration - it makes a draw-order or an off-by-one in
    the paged rebuild visible as a pattern break rather than as a plausible-looking hairball. */
    constexpr uint32_t kColumns = 1000;
    constexpr double kCellCU = 10.0;
    const uint32_t firstCell = tab.cad2d->bulkLineCounter.fetch_add(count, std::memory_order_acq_rel);
    const auto start = std::chrono::steady_clock::now();

    // Built and pushed inside ONE lock hold: the copy thread cannot drain a partial burst, so a
    // press costs one rebuild instead of one per wake-up. It also keeps only the queue's copy of
    // each command alive, which matters at nearly a kilobyte apiece.
    {
        std::lock_guard<std::mutex> lock(gCad2DCopyQueueMutex);
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t cell = firstCell + i;
            const double cellX = static_cast<double>(cell % kColumns) * kCellCU;
            const double cellY = static_cast<double>(cell / kColumns) * kCellCU;
            const bool backslash = (cell & 1u) != 0;

            Cad2DLineRecordCPU line{};
            line.memoryID = MemoryID::next();
            line.memoryIDContainer = page2DMemoryId;
            line.x1 = cellX + 1.0;
            line.y1 = backslash ? cellY + 9.0 : cellY + 1.0;
            line.x2 = cellX + 9.0;
            line.y2 = backslash ? cellY + 1.0 : cellY + 9.0;
            line.lineWeight = 1.0f;
            line.lineWeightMode = Cad2DLineWeightMode::ScreenPixel;
            line.colorABGR = 0xFF000000u;
            line.schemaVersion = VishwakarmaStorage::kGeometry2DLineSchemaVersion;

            CommandToCopyThread2D command{};
            command.type = CommandToCopyThread2DType::AddLine;
            command.id = line.memoryID;
            command.tabID = tab.tabID;
            command.containerMemoryId = page2DMemoryId;
            command.line = std::move(line);
            gCad2DCopyQueue.push(std::move(command));
        }
    }
    toCopyThreadCV.notify_one();

    const double queueMs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count() / 1000.0;
    std::cout << "[cad2d][stress] queued " << count << " lines, cells " << firstCell << ".."
              << (firstCell + count - 1) << ", in " << queueMs << " ms" << std::endl;
}

void Cad2DModifyBulkLines(DATASETTAB& tab, uint32_t count) {
    if (!tab.cad2d) return;
    const uint64_t page2DMemoryId = Cad2DFindTargetPage2DMemoryId(tab);
    if (page2DMemoryId == 0) return;
    TabCad2DStorage& storage = *tab.cad2d;
    // 0 means EVERY line of the container - the "move the whole drawing" extreme, and the only
    // thing that takes a page to 100% holes in one batch, which is the case where compaction
    // hands a megabyte back instead of only reclaiming draw work.
    if (count == 0) count = UINT32_MAX;

    /* DISTINCT lines, one modify each, taken from the end of the record vector - which is where
    Cad2DGenerateBulkLines put them, so a press edits a contiguous block and the holes it makes land
    on one or two pages rather than smeared over thirty.

    Distinct rather than "one line N times" on purpose. The copy thread collapses repeated modifies
    of ONE id within a batch to a single append, so a burst of them would make no holes at all and
    would test nothing; N different objects is also the shape of the operation a user actually runs
    into - a Move of a large selection. */
    std::vector<Cad2DLineRecordCPU> edited;
    {
        std::lock_guard<std::mutex> lock(storage.cpuRecordsMutex);
        // Reserve against the vector, not against `count`: the sentinel is UINT32_MAX and
        // reserving that many records throws, which on this thread means the engineering thread
        // dies and the window quietly stops responding to every later key.
        edited.reserve((std::min)(static_cast<size_t>(count), storage.lineRecords.size()));
        for (size_t i = storage.lineRecords.size(); i-- > 0 && edited.size() < count; ) {
            const Cad2DLineRecordCPU& record = storage.lineRecords[i];
            if (record.isDeleted || record.memoryIDContainer != page2DMemoryId) continue;
            Cad2DLineRecordCPU moved = record;
            // Half a CU up. Big enough to see at a zoom where individual strokes resolve, small
            // enough that repeated presses keep the drawing recognisable.
            moved.y1 += 0.5;
            moved.y2 += 0.5;
            edited.push_back(std::move(moved));
        }
    }
    if (edited.empty()) return;

    const auto start = std::chrono::steady_clock::now();
    { // One lock hold, exactly as the append harness does it, so the burst arrives as one batch.
        std::lock_guard<std::mutex> lock(gCad2DCopyQueueMutex);
        for (Cad2DLineRecordCPU& line : edited) {
            CommandToCopyThread2D command{};
            command.type = CommandToCopyThread2DType::AddLine;
            command.id = line.memoryID;
            command.tabID = tab.tabID;
            command.containerMemoryId = page2DMemoryId;
            command.line = std::move(line);
            gCad2DCopyQueue.push(std::move(command));
        }
    }
    toCopyThreadCV.notify_one();

    const double queueMs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count() / 1000.0;
    std::cout << "[cad2d][stress] queued " << edited.size() << " line modifies, in "
              << queueMs << " ms" << std::endl;
}

void Cad2DZoomToExtents(DATASETTAB& tab, bool selectedOnly) {
    if (!tab.cad2d) return;
    const uint64_t container = Cad2DFindTargetPage2DMemoryId(tab);
    if (container == 0) {
#ifdef _DEBUG
        std::cout << "[cad2d][dbg] zoom-extents: no target Page2D container." << std::endl;
#endif
        return;
    }

    int viewportWidth = 0, viewportHeight = 0, viewportTop = 0;
    if (!GetVisibleSceneViewportForTab(tab, viewportWidth, viewportHeight, viewportTop) ||
        viewportWidth <= 0 || viewportHeight <= 0) {
#ifdef _DEBUG
        std::cout << "[cad2d][dbg] zoom-extents container=" << container
                  << ": no visible viewport (" << viewportWidth << "x" << viewportHeight
                  << ")." << std::endl;
#endif
        return;
    }

    TabCad2DStorage& s = *tab.cad2d;

    std::unordered_set<uint64_t> selected;
    if (selectedOnly) {
        std::lock_guard<std::mutex> lock(s.selection2DMutex);
        selected = s.selectedObjectIds;
    }
    const bool filterBySelection = selectedOnly && !selected.empty(); // Empty selection = fit all.

    double minX = 0.0, minY = 0.0, maxX = 0.0, maxY = 0.0;
    bool hasBounds = false;
    auto include = [&](double x, double y) {
        if (!hasBounds) { minX = maxX = x; minY = maxY = y; hasBounds = true; return; }
        minX = (std::min)(minX, x); maxX = (std::max)(maxX, x);
        minY = (std::min)(minY, y); maxY = (std::max)(maxY, y);
    };
    auto wanted = [&](uint64_t objectId, bool isDeleted, uint64_t recContainer) {
        if (isDeleted || recContainer != container) return false;
        return !filterBySelection || selected.count(objectId) != 0;
    };

    {
        std::lock_guard<std::mutex> lock(s.cpuRecordsMutex);
        for (const Cad2DLineRecordCPU& r : s.lineRecords) {
            if (!wanted(r.memoryID, r.isDeleted, r.memoryIDContainer)) continue;
            include(r.x1, r.y1); include(r.x2, r.y2);
        }
        for (const Cad2DPolylineRecordCPU& r : s.polylineRecords) {
            if (!wanted(r.memoryID, r.isDeleted, r.memoryIDContainer)) continue;
            for (const Cad2DPoint2D& p : r.points) include(p.x, p.y);
        }
        for (const Cad2DPolygonRecordCPU& r : s.polygonRecords) {
            if (!wanted(r.memoryID, r.isDeleted, r.memoryIDContainer)) continue;
            include(r.centerX - r.radius, r.centerY - r.radius);
            include(r.centerX + r.radius, r.centerY + r.radius);
        }
        for (const Cad2DCircleRecordCPU& r : s.circleRecords) {
            if (!wanted(r.memoryID, r.isDeleted, r.memoryIDContainer)) continue;
            include(r.centerX - r.radius, r.centerY - r.radius);
            include(r.centerX + r.radius, r.centerY + r.radius);
        }
        for (const Cad2DEllipseRecordCPU& r : s.ellipseRecords) {
            if (!wanted(r.memoryID, r.isDeleted, r.memoryIDContainer)) continue;
            // Axis-aligned bounding half-extents of the rotated ellipse.
            const double c = std::cos(r.rotationRadians), sn = std::sin(r.rotationRadians);
            const double hx = std::sqrt(r.radiusX * c * r.radiusX * c + r.radiusY * sn * r.radiusY * sn);
            const double hy = std::sqrt(r.radiusX * sn * r.radiusX * sn + r.radiusY * c * r.radiusY * c);
            include(r.centerX - hx, r.centerY - hy);
            include(r.centerX + hx, r.centerY + hy);
        }
        for (const Cad2DArcRecordCPU& r : s.arcRecords) {
            if (!wanted(r.memoryID, r.isDeleted, r.memoryIDContainer)) continue;
            const double radius = (std::max)(std::abs(r.radiusX), std::abs(r.radiusY));
            include(r.centerX - radius, r.centerY - radius); // Full-ellipse box; conservative for partial arcs.
            include(r.centerX + radius, r.centerY + radius);
        }
        for (const Cad2DTextRecordCPU& r : s.textRecords) {
            if (!wanted(r.memoryID, r.isDeleted, r.memoryIDContainer)) continue;
            include(r.x, r.y); include(r.x, r.y + (double)r.textHeightCU);
        }
    }
    if (!hasBounds) {
#ifdef _DEBUG
        std::cout << "[cad2d][dbg] zoom-extents container=" << container
                  << ": no records to fit (selectedOnly=" << selectedOnly << ")." << std::endl;
#endif
        return;
    }

#ifdef _DEBUG
    // Outlier hunt: a sane drawing never spans 1e8 CU. Name the records that blew up the
    // extents so the producer of corrupt coordinates can be identified.
    if ((maxX - minX) > 1.0e8 || (maxY - minY) > 1.0e8) {
        int reported = 0;
        auto suspicious = [](double v) { return std::abs(v) > 1.0e8; };
        std::lock_guard<std::mutex> lock(s.cpuRecordsMutex);
        for (const Cad2DLineRecordCPU& r : s.lineRecords) {
            if (reported >= 8) break;
            if (r.isDeleted || r.memoryIDContainer != container) continue;
            if (suspicious(r.x1) || suspicious(r.y1) || suspicious(r.x2) || suspicious(r.y2)) {
                std::cout << "[cad2d][dbg] OUTLIER line objectId=" << r.memoryID
                          << " parent=" << r.memoryIDGenerator << " persisted=" << r.persistedId
                          << " (" << r.x1 << ", " << r.y1 << ") -> (" << r.x2 << ", " << r.y2
                          << ")" << std::endl;
                ++reported;
            }
        }
        for (const Cad2DTextRecordCPU& r : s.textRecords) {
            if (reported >= 8) break;
            if (r.isDeleted || r.memoryIDContainer != container) continue;
            if (suspicious(r.x) || suspicious(r.y)) {
                std::cout << "[cad2d][dbg] OUTLIER text objectId=" << r.memoryID
                          << " parent=" << r.memoryIDGenerator << " persisted=" << r.persistedId
                          << " at (" << r.x << ", " << r.y << ") height=" << r.textHeightCU
                          << " text=\"" << r.text.substr(0, 40) << "\"" << std::endl;
                ++reported;
            }
        }
        for (const Cad2DPolygonRecordCPU& r : s.polygonRecords) {
            if (reported >= 8) break;
            if (r.isDeleted || r.memoryIDContainer != container) continue;
            if (suspicious(r.centerX) || suspicious(r.centerY) || suspicious(r.radius)) {
                std::cout << "[cad2d][dbg] OUTLIER polygon objectId=" << r.memoryID
                          << " parent=" << r.memoryIDGenerator << " center(" << r.centerX
                          << ", " << r.centerY << ") radius=" << r.radius << std::endl;
                ++reported;
            }
        }
        for (const Cad2DPolylineRecordCPU& r : s.polylineRecords) {
            if (reported >= 8) break;
            if (r.isDeleted || r.memoryIDContainer != container) continue;
            for (const Cad2DPoint2D& p : r.points) {
                if (suspicious(p.x) || suspicious(p.y)) {
                    std::cout << "[cad2d][dbg] OUTLIER polyline objectId=" << r.memoryID
                              << " parent=" << r.memoryIDGenerator << " point(" << p.x
                              << ", " << p.y << ")" << std::endl;
                    ++reported;
                    break;
                }
            }
        }
        for (const Cad2DCircleRecordCPU& r : s.circleRecords) {
            if (reported >= 8) break;
            if (r.isDeleted || r.memoryIDContainer != container) continue;
            if (suspicious(r.centerX) || suspicious(r.centerY) || suspicious(r.radius)) {
                std::cout << "[cad2d][dbg] OUTLIER circle objectId=" << r.memoryID
                          << " parent=" << r.memoryIDGenerator << " center(" << r.centerX
                          << ", " << r.centerY << ") radius=" << r.radius << std::endl;
                ++reported;
            }
        }
        for (const Cad2DEllipseRecordCPU& r : s.ellipseRecords) {
            if (reported >= 8) break;
            if (r.isDeleted || r.memoryIDContainer != container) continue;
            if (suspicious(r.centerX) || suspicious(r.centerY)) {
                std::cout << "[cad2d][dbg] OUTLIER ellipse objectId=" << r.memoryID
                          << " parent=" << r.memoryIDGenerator << " center(" << r.centerX
                          << ", " << r.centerY << ")" << std::endl;
                ++reported;
            }
        }
        for (const Cad2DArcRecordCPU& r : s.arcRecords) {
            if (reported >= 8) break;
            if (r.isDeleted || r.memoryIDContainer != container) continue;
            if (suspicious(r.centerX) || suspicious(r.centerY)) {
                std::cout << "[cad2d][dbg] OUTLIER arc objectId=" << r.memoryID
                          << " parent=" << r.memoryIDGenerator << " center(" << r.centerX
                          << ", " << r.centerY << ")" << std::endl;
                ++reported;
            }
        }
    }
#endif

    // Recenter the view on the extents, then pick the largest zoom that still fits them.
    Cad2DViewState& view = Cad2DInputView(tab);
    view.centerXCU.store((minX + maxX) * 0.5, std::memory_order_release);
    view.centerYCU.store((minY + maxY) * 0.5, std::memory_order_release);
    const double halfW = (maxX - minX) * 0.5;
    const double halfH = (maxY - minY) * 0.5;
    if (halfW < 1.0e-9 && halfH < 1.0e-9) return; // Single point; recentered, keep the zoom.

    const double margin = 0.95; // Keep a small breathing border around the extents.
    double zoom = (double)kCad2DZoomMaxPixelsPerCU;
    if (halfW > 1.0e-9) zoom = (std::min)(zoom, (double)viewportWidth * 0.5 * margin / halfW);
    if (halfH > 1.0e-9) zoom = (std::min)(zoom, (double)viewportHeight * 0.5 * margin / halfH);
    view.zoomPixelsPerCU.store((float)std::clamp(zoom,
        (double)kCad2DZoomMinPixelsPerCU, (double)kCad2DZoomMaxPixelsPerCU), std::memory_order_release);
#ifdef _DEBUG
    std::cout << "[cad2d][dbg] zoom-extents container=" << container
              << " bounds X[" << minX << " .. " << maxX << "] Y[" << minY << " .. " << maxY
              << "] viewport " << viewportWidth << "x" << viewportHeight
              << " -> view slot " << InputViewSlot(tab)
              << " center(" << (minX + maxX) * 0.5 << ", " << (minY + maxY) * 0.5
              << ") zoom=" << view.zoomPixelsPerCU.load(std::memory_order_acquire) << std::endl;
#endif
}

void Cad2DZoomToWindow(DATASETTAB& tab, int x0, int y0, int x1, int y1) {
    if (!tab.cad2d) return;
    if (Cad2DFindTargetPage2DMemoryId(tab) == 0) return;

    int viewportWidth = 0, viewportHeight = 0, viewportTop = 0;
    if (!GetVisibleSceneViewportForTab(tab, viewportWidth, viewportHeight, viewportTop)) return;
    if (viewportWidth <= 0 || viewportHeight <= 0) return;

    Cad2DViewState& view = Cad2DInputView(tab);
    const double zoom = (std::max)(
        (double)view.zoomPixelsPerCU.load(std::memory_order_acquire),
        (double)kCad2DZoomMinPixelsPerCU);
    const double centerX = view.centerXCU.load(std::memory_order_acquire);
    const double centerY = view.centerYCU.load(std::memory_order_acquire);
    // Same pixel -> CAD-unit mapping as Page2DCoordinateFromInput / the wheel zoom.
    auto toCU = [&](int px, int py, double& outX, double& outY) {
        const double offsetX = (double)px - (double)viewportWidth * 0.5;
        const double offsetY = (double)viewportHeight * 0.5 - (double)(py - viewportTop);
        outX = centerX + offsetX / zoom;
        outY = centerY + offsetY / zoom;
    };
    double ax = 0.0, ay = 0.0, bx = 0.0, by = 0.0;
    toCU(x0, y0, ax, ay);
    toCU(x1, y1, bx, by);

    const double halfW = std::abs(bx - ax) * 0.5;
    const double halfH = std::abs(by - ay) * 0.5;
    if (halfW < 1.0e-9 && halfH < 1.0e-9) return; // Degenerate window; ignore.

    double newZoom = (double)kCad2DZoomMaxPixelsPerCU;
    if (halfW > 1.0e-9) newZoom = (std::min)(newZoom, (double)viewportWidth * 0.5 / halfW);
    if (halfH > 1.0e-9) newZoom = (std::min)(newZoom, (double)viewportHeight * 0.5 / halfH);
    view.centerXCU.store((ax + bx) * 0.5, std::memory_order_release);
    view.centerYCU.store((ay + by) * 0.5, std::memory_order_release);
    view.zoomPixelsPerCU.store((float)std::clamp(newZoom,
        (double)kCad2DZoomMinPixelsPerCU, (double)kCad2DZoomMaxPixelsPerCU), std::memory_order_release);
}
