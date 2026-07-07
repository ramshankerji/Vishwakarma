// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

#include "MemoryManagerGPU2D.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "CommonNamedNumbers.h"
#include "MemoryManagerGPU-DirectX12.h"
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
    std::lock_guard<std::mutex> lock(storage.cpuRecordsMutex);
    storage.polylineCreationObjectId = 0;
    storage.polylineCreationPoints.clear();
    storage.textCreationObjectId = 0;
    storage.textCreationDraft.clear();
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

    const double zoom = (std::max)(
        (double)tab.cad2d->view.zoomPixelsPerCU.load(std::memory_order_acquire),
        (double)kCad2DZoomMinPixelsPerCU);
    const double centerX = tab.cad2d->view.centerXCU.load(std::memory_order_acquire);
    const double centerY = tab.cad2d->view.centerYCU.load(std::memory_order_acquire);
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

    std::lock_guard<std::mutex> lock(*tab.storageObjectsMutex);
    if (tab.activeInternalSubTabMemoryId != 0) {
        StoredLogicalObject* active = FindLogicalObjectByIdLocked(tab, tab.activeInternalSubTabMemoryId);
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
    std::lock_guard<std::mutex> lock(*tab.storageObjectsMutex);
    if (tab.activeInternalSubTabMemoryId == 0) return false;

    StoredLogicalObject* active = FindLogicalObjectByIdLocked(tab, tab.activeInternalSubTabMemoryId);
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

// Rough distance to an (axis-aligned) ellipse boundary; adequate for pick tolerance.
double DistPointToEllipse(double px, double py, double cx, double cy, double rx, double ry) {
    const double sx = (std::max)(std::abs(rx), 1.0e-9);
    const double sy = (std::max)(std::abs(ry), 1.0e-9);
    const double nx = (px - cx) / sx, ny = (py - cy) / sy;
    const double r = std::sqrt(nx * nx + ny * ny);
    return std::abs(r - 1.0) * (std::min)(sx, sy);
}

void Cad2DHandleSelectionClick(DATASETTAB& tab, double xCU, double yCU) {
    if (!tab.cad2d) return;
    const uint64_t container = Cad2DFindTargetPage2DMemoryId(tab);
    if (container == 0) return;
    TabCad2DStorage& s = *tab.cad2d;

    const double zoom = (std::max)(
        (double)s.view.zoomPixelsPerCU.load(std::memory_order_acquire),
        (double)kCad2DZoomMinPixelsPerCU);
    const double tolCU = 6.0 / zoom; // ~6 pixel pick tolerance in CAD units.
    uint64_t bestId = 0;
    double bestDist = tolCU;

    {
        std::lock_guard<std::mutex> lock(s.cpuRecordsMutex);
        auto consider = [&](double d, uint64_t id) {
            if (d < bestDist) { bestDist = d; bestId = id; }
        };
        for (const Cad2DLineRecordCPU& r : s.lineRecords) {
            if (r.isDeleted || r.containerMemoryId != container) continue;
            consider(DistPointToSegment(xCU, yCU, r.x1, r.y1, r.x2, r.y2), r.objectId);
        }
        for (const Cad2DPolylineRecordCPU& r : s.polylineRecords) {
            if (r.isDeleted || r.containerMemoryId != container) continue;
            for (size_t i = 1; i < r.points.size(); ++i) {
                consider(DistPointToSegment(xCU, yCU, r.points[i - 1].x, r.points[i - 1].y,
                    r.points[i].x, r.points[i].y), r.objectId);
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
                    r.centerX + std::sin(a1) * r.radius, r.centerY + std::cos(a1) * r.radius), r.objectId);
            }
        }
        for (const Cad2DCircleRecordCPU& r : s.circleRecords) {
            if (r.isDeleted || r.containerMemoryId != container) continue;
            consider(DistPointToCircle(xCU, yCU, r.centerX, r.centerY, r.radius), r.objectId);
        }
        for (const Cad2DEllipseRecordCPU& r : s.ellipseRecords) {
            if (r.isDeleted || r.containerMemoryId != container) continue;
            consider(DistPointToEllipse(xCU, yCU, r.centerX, r.centerY, r.radiusX, r.radiusY), r.objectId);
        }
        for (const Cad2DArcRecordCPU& r : s.arcRecords) {
            if (r.isDeleted || r.containerMemoryId != container) continue;
            consider(DistPointToEllipse(xCU, yCU, r.centerX, r.centerY, r.radiusX, r.radiusY), r.objectId);
        }
    }

    {
        std::lock_guard<std::mutex> lock(s.selection2DMutex);
        s.selectedObjectIds.clear();
        if (bestId != 0) s.selectedObjectIds.insert(bestId); // Single-select; clears on empty click.
    }
    EnqueueCad2DSelectionRefresh(tab.tabID, container);
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
            tab.cad2d->textCreationMode.load(std::memory_order_acquire);
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
                tab.cad2d->textCreationMode.load(std::memory_order_acquire))) {
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
            const float zoom = (std::max)(tab.cad2d->view.zoomPixelsPerCU.load(std::memory_order_acquire),
                kCad2DZoomMinPixelsPerCU);
            const double currentX = tab.cad2d->view.centerXCU.load(std::memory_order_acquire);
            const double currentY = tab.cad2d->view.centerYCU.load(std::memory_order_acquire);
            tab.cad2d->view.centerXCU.store(currentX - (double)dx / (double)zoom, std::memory_order_release);
            tab.cad2d->view.centerYCU.store(currentY + (double)dy / (double)zoom, std::memory_order_release);
        }
        tab.lastMouseX = input.x;
        tab.lastMouseY = input.y;
        return true;
    }
    case ACTION_TYPE::MOUSEWHEEL:
    {
        const float wheelSteps = input.delta / (float)WHEEL_DELTA;
        const float currentZoom = tab.cad2d->view.zoomPixelsPerCU.load(std::memory_order_acquire);
        const float zoomFactor = std::pow(1.12f, wheelSteps);
        const float nextZoom = std::clamp(currentZoom * zoomFactor,
            kCad2DZoomMinPixelsPerCU, kCad2DZoomMaxPixelsPerCU);
        const double currentX = tab.cad2d->view.centerXCU.load(std::memory_order_acquire);
        const double currentY = tab.cad2d->view.centerYCU.load(std::memory_order_acquire);
        int viewportWidth = 0, viewportHeight = 0, viewportTop = 0;
        if (GetVisibleSceneViewportForTab(tab, viewportWidth, viewportHeight, viewportTop)) {
            const double mouseX = std::clamp((double)input.x, 0.0, (double)viewportWidth);
            const double mouseY = std::clamp((double)(input.y - viewportTop), 0.0, (double)viewportHeight);
            const double offsetX = mouseX - (double)viewportWidth * 0.5;
            const double offsetY = (double)viewportHeight * 0.5 - mouseY;
            const double cursorX = currentX + offsetX / (double)currentZoom;
            const double cursorY = currentY + offsetY / (double)currentZoom;
            tab.cad2d->view.centerXCU.store(cursorX - offsetX / (double)nextZoom, std::memory_order_release);
            tab.cad2d->view.centerYCU.store(cursorY - offsetY / (double)nextZoom, std::memory_order_release);
        }
        tab.cad2d->view.zoomPixelsPerCU.store(nextZoom, std::memory_order_release);
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
            include(r.centerX - r.radiusX, r.centerY - r.radiusY);
            include(r.centerX + r.radiusX, r.centerY + r.radiusY);
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
    s.view.centerXCU.store((minX + maxX) * 0.5, std::memory_order_release);
    s.view.centerYCU.store((minY + maxY) * 0.5, std::memory_order_release);
    const double halfW = (maxX - minX) * 0.5;
    const double halfH = (maxY - minY) * 0.5;
    if (halfW < 1.0e-9 && halfH < 1.0e-9) return; // Single point; recentered, keep the zoom.

    const double margin = 0.95; // Keep a small breathing border around the extents.
    double zoom = (double)kCad2DZoomMaxPixelsPerCU;
    if (halfW > 1.0e-9) zoom = (std::min)(zoom, (double)viewportWidth * 0.5 * margin / halfW);
    if (halfH > 1.0e-9) zoom = (std::min)(zoom, (double)viewportHeight * 0.5 * margin / halfH);
    s.view.zoomPixelsPerCU.store((float)std::clamp(zoom,
        (double)kCad2DZoomMinPixelsPerCU, (double)kCad2DZoomMaxPixelsPerCU), std::memory_order_release);
}

void Cad2DZoomToWindow(DATASETTAB& tab, int x0, int y0, int x1, int y1) {
    if (!tab.cad2d) return;
    if (Cad2DFindTargetPage2DMemoryId(tab) == 0) return;

    int viewportWidth = 0, viewportHeight = 0, viewportTop = 0;
    if (!GetVisibleSceneViewportForTab(tab, viewportWidth, viewportHeight, viewportTop)) return;
    if (viewportWidth <= 0 || viewportHeight <= 0) return;

    TabCad2DStorage& s = *tab.cad2d;
    const double zoom = (std::max)(
        (double)s.view.zoomPixelsPerCU.load(std::memory_order_acquire),
        (double)kCad2DZoomMinPixelsPerCU);
    const double centerX = s.view.centerXCU.load(std::memory_order_acquire);
    const double centerY = s.view.centerYCU.load(std::memory_order_acquire);
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
    s.view.centerXCU.store((ax + bx) * 0.5, std::memory_order_release);
    s.view.centerYCU.store((ay + by) * 0.5, std::memory_order_release);
    s.view.zoomPixelsPerCU.store((float)std::clamp(newZoom,
        (double)kCad2DZoomMinPixelsPerCU, (double)kCad2DZoomMaxPixelsPerCU), std::memory_order_release);
}
