// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
#pragma once
#include <cstdint>
struct SingleUIWindow;

// Main window class name — wWinMain registers it, CreateSecondaryWindow instantiates it.
inline constexpr wchar_t szWindowClass[] = L"विश्वकर्मा";

// Window lifecycle (moved out of Main.cpp; still called by WndProc / ProcessPendingUIActions).
void CloseSecondaryWindow(uint16_t windowSlot);
// Tears down any window slot, main window included (WndProc's tab-host close path uses it for slot 0).
void TeardownWindowSlot(uint16_t windowSlot);
void ExtractTabToNewWindow(uint16_t tabID);
void ExtractViewToNewWindow(uint16_t tabIndex, uint64_t containerMemoryId);
void CloseViewWindowFor(uint16_t tabIndex, uint16_t subTabSlot);
void HandleSecondaryWindowClose(SingleUIWindow& window);
bool TryMergeWindowOnDrop(SingleUIWindow& dragged);

// Monitor topology & render-thread lifecycle (moved out of Main.cpp).
// FetchAllMonitorDetails / MonitorEnumProc keep their existing declarations in
// MemoryManagerGPU-DirectX12.h, so they are not re-declared here.
void UpdateWindowMonitorAffinity(SingleUIWindow& window, int newMonitorIndex);
void RestartRenderThreads();
