// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

#include <algorithm>
#include <cmath>
#include <random>
#include <utility>

#include "CommonNamedNumbers.h"
#include "GPUPlatformSelector.h"
#include "RenderPage2D.h"

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

StoredLogicalObject* FindLogicalObjectByIdLocked(DATASETTAB& tab, uint64_t memoryId) {
    for (StoredLogicalObject& entry : tab.storageLogicalObjects) {
        if (entry.object && entry.object->memoryID == memoryId) return &entry;
    }
    return nullptr;
}

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
    return tab.cad2d->views[slot];
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
    line.containerMemoryId = page2DMemoryId;
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

        polyline.objectId = storage.polylineCreationObjectId;
        polyline.containerMemoryId = page2DMemoryId;
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
    polygon.containerMemoryId = page2DMemoryId;
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
    circle.containerMemoryId = page2DMemoryId;
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
    ellipse.containerMemoryId = page2DMemoryId;
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
    arc.containerMemoryId = Cad2DFindTargetPage2DMemoryId(tab);
    if (arc.containerMemoryId == 0) return;
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
    EnqueueCad2DArc(tab.tabID, arc.containerMemoryId, arc);

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
    text.objectId = objectId;
    text.containerMemoryId = page2DMemoryId;
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
    if (line.objectId == 0) line.objectId = MemoryID::next();
    line.containerMemoryId = containerMemoryId;

    CommandToCopyThread2D command{};
    command.type = CommandToCopyThread2DType::AddLine;
    command.id = line.objectId;
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
    if (polyline.objectId == 0) polyline.objectId = MemoryID::next();
    polyline.containerMemoryId = containerMemoryId;

    CommandToCopyThread2D command{};
    command.type = CommandToCopyThread2DType::AddPolyline;
    command.id = polyline.objectId;
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
    if (polygon.objectId == 0) polygon.objectId = MemoryID::next();
    polygon.containerMemoryId = containerMemoryId;

    CommandToCopyThread2D command{};
    command.type = CommandToCopyThread2DType::AddPolygon;
    command.id = polygon.objectId;
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
    if (circle.objectId == 0) circle.objectId = MemoryID::next();
    circle.containerMemoryId = containerMemoryId;

    CommandToCopyThread2D command{};
    command.type = CommandToCopyThread2DType::AddCircle;
    command.id = circle.objectId;
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
    if (ellipse.objectId == 0) ellipse.objectId = MemoryID::next();
    ellipse.containerMemoryId = containerMemoryId;

    CommandToCopyThread2D command{};
    command.type = CommandToCopyThread2DType::AddEllipse;
    command.id = ellipse.objectId;
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
    if (arc.objectId == 0) arc.objectId = MemoryID::next();
    arc.containerMemoryId = containerMemoryId;

    CommandToCopyThread2D command{};
    command.type = CommandToCopyThread2DType::AddArc;
    command.id = arc.objectId;
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
    if (text.objectId == 0) text.objectId = MemoryID::next();
    text.containerMemoryId = containerMemoryId;

    CommandToCopyThread2D command{};
    command.type = CommandToCopyThread2DType::AddText;
    command.id = text.objectId;
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
    // A no-op command: it carries no geometry, but its presence forces the copy thread to rebuild
    // and republish this tab's pages, re-applying selection flags into the GPU records.
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
        StoredLogicalObject* active = FindLogicalObjectByIdLocked(tab, inputContainerId);
        if (active && active->objectType == VishwakarmaStorage::ObjectType::Page2D) {
            return active->object->memoryID;
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

    StoredLogicalObject* active = FindLogicalObjectByIdLocked(tab, inputContainerId);
    return active && active->objectType == VishwakarmaStorage::ObjectType::Page2D;
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
        return !r.isDeleted && r.containerMemoryId == container && selected.count(r.objectId) != 0;
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

    // Random user-visible asset number, unique within this tab's definitions.
    std::mt19937 rng{ std::random_device{}() };
    std::uniform_int_distribution<uint32_t> dist(100000u, 999999u);
    uint32_t assetNumber = 0;
    do {
        assetNumber = dist(rng);
        for (const Cad2DAssetDefinitionRecordCPU& d : s.assetDefinitionRecords) {
            if (!d.isDeleted && d.assetNumber == assetNumber) { assetNumber = 0; break; }
        }
    } while (assetNumber == 0);

    Cad2DAssetDefinitionRecordCPU definition{};
    definition.objectId = MemoryID::next();
    definition.assetNumber = assetNumber;
    definition.baseX = (minX + maxX) * 0.5;
    definition.baseY = (minY + maxY) * 0.5;
    definition.schemaVersion = VishwakarmaStorage::kAsset2DDefinitionSchemaVersion;

    // The originals become the first placed instance; drawing stays exactly as it is.
    Cad2DAssetInsertRecordCPU firstInsert{};
    firstInsert.objectId = MemoryID::next();
    firstInsert.containerMemoryId = container;
    firstInsert.definitionObjectId = definition.objectId;
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
            master.objectId = MemoryID::next();
            master.persistedId = 0;
            master.persistedParentId = 0;
            master.parentObjectId = definition.objectId;
            master.containerMemoryId = 0;
            tab.allIDsInThisTab.push_back(master.objectId);
            records.push_back(std::move(master)); // May reallocate; re-index the original below.
            records[i].parentObjectId = firstInsert.objectId;
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
    s.assetInsertRecords.push_back(firstInsert);
    tab.allIDsInThisTab.push_back(definition.objectId);
    tab.allIDsInThisTab.push_back(firstInsert.objectId);
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
            if (r.isDeleted || r.containerMemoryId != container) continue;
            consider(DistPointToSegment(xCU, yCU, r.x1, r.y1, r.x2, r.y2), r.objectId, r.parentObjectId);
        }
        for (const Cad2DPolylineRecordCPU& r : s.polylineRecords) {
            if (r.isDeleted || r.containerMemoryId != container) continue;
            for (size_t i = 1; i < r.points.size(); ++i) {
                consider(DistPointToSegment(xCU, yCU, r.points[i - 1].x, r.points[i - 1].y,
                    r.points[i].x, r.points[i].y), r.objectId, r.parentObjectId);
            }
        }
        for (const Cad2DPolygonRecordCPU& r : s.polygonRecords) {
            if (r.isDeleted || r.containerMemoryId != container || r.radius <= 0.0) continue;
            const uint32_t n = std::clamp(r.lineSegmentCount, 3u, 16u);
            const double step = 360.0 / (double)n;
            for (uint32_t i = 0; i < n; ++i) {
                const double a0 = (r.rotationDegrees + step * i) * 3.14159265358979323846 / 180.0;
                const double a1 = (r.rotationDegrees + step * ((i + 1) % n)) * 3.14159265358979323846 / 180.0;
                consider(DistPointToSegment(xCU, yCU,
                    r.centerX + std::sin(a0) * r.radius, r.centerY + std::cos(a0) * r.radius,
                    r.centerX + std::sin(a1) * r.radius, r.centerY + std::cos(a1) * r.radius),
                    r.objectId, r.parentObjectId);
            }
        }
        for (const Cad2DCircleRecordCPU& r : s.circleRecords) {
            if (r.isDeleted || r.containerMemoryId != container) continue;
            consider(DistPointToCircle(xCU, yCU, r.centerX, r.centerY, r.radius), r.objectId,
                r.parentObjectId);
        }
        for (const Cad2DEllipseRecordCPU& r : s.ellipseRecords) {
            if (r.isDeleted || r.containerMemoryId != container) continue;
            consider(DistPointToEllipse(xCU, yCU, r.centerX, r.centerY, r.radiusX, r.radiusY,
                r.rotationRadians), r.objectId, r.parentObjectId);
        }
        for (const Cad2DArcRecordCPU& r : s.arcRecords) {
            if (r.isDeleted || r.containerMemoryId != container) continue;
            consider(DistPointToEllipse(xCU, yCU, r.centerX, r.centerY, r.radiusX, r.radiusY,
                r.rotationRadians), r.objectId, r.parentObjectId);
        }

        if (bestId != 0) {
            // Parent expansion: when the hit object's parent is a Asset2DInsert, the whole
            // instance is selected - every record sharing that parent, across all record types.
            bool parentIsAssetInsert = false;
            if (bestParentId != 0) {
                for (const Cad2DAssetInsertRecordCPU& insert : s.assetInsertRecords) {
                    if (!insert.isDeleted && insert.objectId == bestParentId) {
                        parentIsAssetInsert = true;
                        break;
                    }
                }
            }
            if (parentIsAssetInsert) {
                auto gather = [&](const auto& records) {
                    for (const auto& r : records) {
                        if (!r.isDeleted && r.parentObjectId == bestParentId) hitGroup.push_back(r.objectId);
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
        record.objectId = 0; // EnqueueCad2D* assigns a fresh memory id; save assigns persisted ids.
        record.persistedId = 0;
        record.persistedParentId = 0;
        record.parentObjectId = 0; // Copies detach from any asset instance (plain page objects).
    };
    auto wanted = [&](uint64_t objectId, bool isDeleted, uint64_t recContainer) {
        return !isDeleted && recContainer == container && selected.count(objectId) != 0;
    };

    {
        std::lock_guard<std::mutex> lock(s.cpuRecordsMutex);
        for (const Cad2DLineRecordCPU& r : s.lineRecords) {
            if (!wanted(r.objectId, r.isDeleted, r.containerMemoryId)) continue;
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
            if (!wanted(r.objectId, r.isDeleted, r.containerMemoryId)) continue;
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
            if (!wanted(r.objectId, r.isDeleted, r.containerMemoryId)) continue;
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
            if (!wanted(r.objectId, r.isDeleted, r.containerMemoryId)) continue;
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
            if (!wanted(r.objectId, r.isDeleted, r.containerMemoryId)) continue;
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
            if (!wanted(r.objectId, r.isDeleted, r.containerMemoryId)) continue;
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
                if (!wanted(r.objectId, r.isDeleted, r.containerMemoryId)) continue;
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

// Places one instance of the asset chosen in the Insert Asset pane at the clicked point: a new
// Asset2DInsert plus page copies of the definition's master records, translated from the asset
// base point to the click and parented to the insert. The mode stays armed for repeated placing.
void HandleAssetInsertClick(DATASETTAB& tab, double xCU, double yCU) {
    TabCad2DStorage& s = *tab.cad2d;
    const uint64_t container = Cad2DFindTargetPage2DMemoryId(tab);
    if (container == 0) return;

    std::vector<Cad2DLineRecordCPU> lines;
    std::vector<Cad2DPolylineRecordCPU> polylines;
    std::vector<Cad2DPolygonRecordCPU> polygons;
    std::vector<Cad2DCircleRecordCPU> circles;
    std::vector<Cad2DEllipseRecordCPU> ellipses;
    std::vector<Cad2DArcRecordCPU> arcs;
    std::vector<Cad2DTextRecordCPU> texts;
    bool placed = false;

    {
        std::lock_guard<std::mutex> lock(s.cpuRecordsMutex);
        // The pane's selection when it is still a live definition, else the first available one.
        const uint64_t wantedId = s.assetInsertSelectedDefinitionId.load(std::memory_order_acquire);
        const Cad2DAssetDefinitionRecordCPU* definition = nullptr;
        for (const Cad2DAssetDefinitionRecordCPU& d : s.assetDefinitionRecords) {
            if (d.isDeleted) continue;
            if (!definition) definition = &d;
            if (wantedId != 0 && d.objectId == wantedId) { definition = &d; break; }
        }
        if (!definition) return;

        Cad2DAssetInsertRecordCPU insert{};
        insert.objectId = MemoryID::next();
        insert.containerMemoryId = container;
        insert.definitionObjectId = definition->objectId;
        insert.x = xCU;
        insert.y = yCU;
        insert.schemaVersion = VishwakarmaStorage::kAsset2DInsertSchemaVersion;

        const double dx = xCU - definition->baseX;
        const double dy = yCU - definition->baseY;
        const uint64_t definitionId = definition->objectId;

        auto instantiate = [&](const auto& records, auto& out, auto&& translate) {
            for (const auto& r : records) {
                if (r.isDeleted || r.parentObjectId != definitionId) continue;
                auto member = r;
                member.objectId = 0; // EnqueueCad2D* assigns a fresh memory id.
                member.persistedId = 0;
                member.persistedParentId = 0;
                member.parentObjectId = insert.objectId;
                member.containerMemoryId = container;
                translate(member);
                out.push_back(std::move(member));
            }
        };
        instantiate(s.lineRecords, lines, [&](Cad2DLineRecordCPU& r) {
            r.x1 += dx; r.y1 += dy; r.x2 += dx; r.y2 += dy; });
        instantiate(s.polylineRecords, polylines, [&](Cad2DPolylineRecordCPU& r) {
            for (Cad2DPoint2D& p : r.points) { p.x += dx; p.y += dy; } });
        instantiate(s.polygonRecords, polygons, [&](Cad2DPolygonRecordCPU& r) {
            r.centerX += dx; r.centerY += dy; });
        instantiate(s.circleRecords, circles, [&](Cad2DCircleRecordCPU& r) {
            r.centerX += dx; r.centerY += dy; });
        instantiate(s.ellipseRecords, ellipses, [&](Cad2DEllipseRecordCPU& r) {
            r.centerX += dx; r.centerY += dy; });
        instantiate(s.arcRecords, arcs, [&](Cad2DArcRecordCPU& r) {
            r.centerX += dx; r.centerY += dy;
            r.startX += dx; r.startY += dy;
            r.endX += dx; r.endY += dy; });
        instantiate(s.textRecords, texts, [&](Cad2DTextRecordCPU& r) {
            r.x += dx; r.y += dy; });

        s.assetInsertRecords.push_back(insert);
        tab.allIDsInThisTab.push_back(insert.objectId);
        placed = true;
    }

    if (!placed) return;
    for (Cad2DLineRecordCPU& r : lines) EnqueueCad2DLine(tab.tabID, container, r);
    for (Cad2DPolylineRecordCPU& r : polylines) EnqueueCad2DPolyline(tab.tabID, container, std::move(r));
    for (Cad2DPolygonRecordCPU& r : polygons) EnqueueCad2DPolygon(tab.tabID, container, r);
    for (Cad2DCircleRecordCPU& r : circles) EnqueueCad2DCircle(tab.tabID, container, r);
    for (Cad2DEllipseRecordCPU& r : ellipses) EnqueueCad2DEllipse(tab.tabID, container, r);
    for (Cad2DArcRecordCPU& r : arcs) EnqueueCad2DArc(tab.tabID, container, r);
    for (Cad2DTextRecordCPU& r : texts) EnqueueCad2DText(tab.tabID, container, std::move(r));
}
} // namespace

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
            if (Page2DCoordinateFromInput(tab, input, xCU, yCU)) {
                HandleLineCreationClick(tab, xCU, yCU);
            }
        }
        else if (tab.cad2d->polylineCreationMode.load(std::memory_order_acquire)) {
            double xCU = 0.0, yCU = 0.0;
            if (Page2DCoordinateFromInput(tab, input, xCU, yCU)) {
                HandlePolylineCreationClick(tab, xCU, yCU);
            }
        }
        else if (tab.cad2d->polygonCreationMode.load(std::memory_order_acquire)) {
            double xCU = 0.0, yCU = 0.0;
            if (Page2DCoordinateFromInput(tab, input, xCU, yCU)) {
                HandlePolygonCreationClick(tab, xCU, yCU);
            }
        }
        else if (tab.cad2d->circleCreationMode.load(std::memory_order_acquire)) {
            double xCU = 0.0, yCU = 0.0;
            if (Page2DCoordinateFromInput(tab, input, xCU, yCU)) {
                HandleCircleCreationClick(tab, xCU, yCU);
            }
        }
        else if (tab.cad2d->ellipseCreationMode.load(std::memory_order_acquire)) {
            double xCU = 0.0, yCU = 0.0;
            if (Page2DCoordinateFromInput(tab, input, xCU, yCU)) {
                HandleEllipseCreationClick(tab, xCU, yCU);
            }
        }
        else if (tab.cad2d->arcCreationMode.load(std::memory_order_acquire)) {
            double xCU = 0.0, yCU = 0.0;
            if (Page2DCoordinateFromInput(tab, input, xCU, yCU)) {
                HandleArcCreationClick(tab, xCU, yCU);
            }
        }
        else if (tab.cad2d->textCreationMode.load(std::memory_order_acquire)) {
            double xCU = 0.0, yCU = 0.0;
            if (Page2DCoordinateFromInput(tab, input, xCU, yCU)) {
                HandleTextCreationClick(tab, xCU, yCU);
            }
        }
        else if (tab.cad2d->assetInsertMode.load(std::memory_order_acquire)) {
            double xCU = 0.0, yCU = 0.0;
            if (Page2DCoordinateFromInput(tab, input, xCU, yCU)) {
                HandleAssetInsertClick(tab, xCU, yCU);
            }
        }
        else if (tab.cad2d->transform2DKind.load(std::memory_order_acquire) != 0) {
            double xCU = 0.0, yCU = 0.0;
            if (Page2DCoordinateFromInput(tab, input, xCU, yCU)) {
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
        text.containerMemoryId = page2DMemoryId;
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
    const double cx = ((n % 9) - 4) * 18.0;
    const double cy = (((n / 9) % 7) - 3) * 16.0;

    Cad2DLineRecordCPU line{};
    line.containerMemoryId = page2DMemoryId;
    line.x1 = cx + std::cos(a0) * r0;
    line.y1 = cy + std::sin(a0) * r0;
    line.x2 = cx + std::cos(a1) * r1;
    line.y2 = cy + std::sin(a1) * r1;
    line.lineWeight = 1.0f;
    line.lineWeightMode = Cad2DLineWeightMode::ScreenPixel;
    line.colorABGR = 0xFF000000u;
    EnqueueCad2DLine(tab.tabID, page2DMemoryId, line);
}

void Cad2DZoomToExtents(DATASETTAB& tab, bool selectedOnly) {
    if (!tab.cad2d) return;
    const uint64_t container = Cad2DFindTargetPage2DMemoryId(tab);
    if (container == 0) return;

    int viewportWidth = 0, viewportHeight = 0, viewportTop = 0;
    if (!GetVisibleSceneViewportForTab(tab, viewportWidth, viewportHeight, viewportTop)) return;
    if (viewportWidth <= 0 || viewportHeight <= 0) return;

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
            if (!wanted(r.objectId, r.isDeleted, r.containerMemoryId)) continue;
            include(r.x1, r.y1); include(r.x2, r.y2);
        }
        for (const Cad2DPolylineRecordCPU& r : s.polylineRecords) {
            if (!wanted(r.objectId, r.isDeleted, r.containerMemoryId)) continue;
            for (const Cad2DPoint2D& p : r.points) include(p.x, p.y);
        }
        for (const Cad2DPolygonRecordCPU& r : s.polygonRecords) {
            if (!wanted(r.objectId, r.isDeleted, r.containerMemoryId)) continue;
            include(r.centerX - r.radius, r.centerY - r.radius);
            include(r.centerX + r.radius, r.centerY + r.radius);
        }
        for (const Cad2DCircleRecordCPU& r : s.circleRecords) {
            if (!wanted(r.objectId, r.isDeleted, r.containerMemoryId)) continue;
            include(r.centerX - r.radius, r.centerY - r.radius);
            include(r.centerX + r.radius, r.centerY + r.radius);
        }
        for (const Cad2DEllipseRecordCPU& r : s.ellipseRecords) {
            if (!wanted(r.objectId, r.isDeleted, r.containerMemoryId)) continue;
            // Axis-aligned bounding half-extents of the rotated ellipse.
            const double c = std::cos(r.rotationRadians), sn = std::sin(r.rotationRadians);
            const double hx = std::sqrt(r.radiusX * c * r.radiusX * c + r.radiusY * sn * r.radiusY * sn);
            const double hy = std::sqrt(r.radiusX * sn * r.radiusX * sn + r.radiusY * c * r.radiusY * c);
            include(r.centerX - hx, r.centerY - hy);
            include(r.centerX + hx, r.centerY + hy);
        }
        for (const Cad2DArcRecordCPU& r : s.arcRecords) {
            if (!wanted(r.objectId, r.isDeleted, r.containerMemoryId)) continue;
            const double radius = (std::max)(std::abs(r.radiusX), std::abs(r.radiusY));
            include(r.centerX - radius, r.centerY - radius); // Full-ellipse box; conservative for partial arcs.
            include(r.centerX + radius, r.centerY + radius);
        }
        for (const Cad2DTextRecordCPU& r : s.textRecords) {
            if (!wanted(r.objectId, r.isDeleted, r.containerMemoryId)) continue;
            include(r.x, r.y); include(r.x, r.y + (double)r.textHeightCU);
        }
    }
    if (!hasBounds) return;

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
