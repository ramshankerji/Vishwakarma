// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

#include "MemoryManagerGPU2D.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "MemoryManagerGPU-DirectX12.h"
#include "विश्वकर्मा.h"
#include "ID.h"

namespace {
std::mutex gCad2DCopyQueueMutex;
std::queue<CommandToCopyThread2D> gCad2DCopyQueue;

StoredLogicalObject* FindLogicalObjectByIdLocked(DATASETTAB& tab, uint64_t memoryId) {
    for (StoredLogicalObject& entry : tab.storageLogicalObjects) {
        if (entry.object && entry.object->memoryID == memoryId) return &entry;
    }
    return nullptr;
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

bool Cad2DHandleInput(DATASETTAB& tab, const ACTION_DETAILS& input) {
    if (!tab.cad2d || !Cad2DIsActivePage2D(tab)) return false;

    switch (input.actionType) {
    case ACTION_TYPE::MOUSEMOVE:
    {
        const int dx = input.x - tab.lastMouseX;
        const int dy = input.y - tab.lastMouseY;
        if (tab.mouseMiddleDown) {
            const float zoom = (std::max)(tab.cad2d->view.zoomPixelsPerCU.load(std::memory_order_acquire), 0.01f);
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
        const float nextZoom = std::clamp(currentZoom * zoomFactor, 0.02f, 5000.0f);
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
        text.justification = Cad2DTextJustification::BottomLeft;
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
