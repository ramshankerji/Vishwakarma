// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
#pragma once
#include <atomic>
#include <cstdint>

// Anonymous usage statistics + system metrics, stored locally in
// %LOCALAPPDATA%\Mission Vishwakarma\ImprovementStatistics.db (SQLite) and uploaded to the
// telemetry server. No personally identifiable information is collected: only off-the-shelf
// hardware/OS details and aggregate interaction counts. The installation is identified by
// its Ed25519 public key (AccountManager).
//
// Collection cadence (all handled by the dedicated ImprovementDataThread):
// - Every 5 minutes: one UsageLog row (open/focus seconds, click/key deltas, ribbon actions).
// - HardwareStatistics: collected at startup; a new row only when the fingerprint changes.
// - Upload: hardware + installation public key at earliest connectivity; usage logs once per
//   24 hours while the application is open. Rows are deleted after the server acknowledges.
// Debug builds talk to http://127.0.0.1:8000, release builds to the production server.

// Interaction counters. Incremented by the main UI thread (WndProc in Main.cpp); the
// statistics thread records the per-interval delta.
extern std::atomic<uint64_t> g_statLeftClicks;
extern std::atomic<uint64_t> g_statMiddleClicks;
extern std::atomic<uint64_t> g_statRightClicks;
extern std::atomic<uint64_t> g_statKeyPresses;

namespace ImprovementData {

// Called by the UI thread whenever a top ribbon action button dispatches a command.
void RecordRibbonAction(uint32_t commandId);

} // namespace ImprovementData

// Dedicated statistics thread entry point. Started from wWinMain alongside the other
// worker threads; exits when the global shutdownSignal is set (flushing a final partial row).
void ImprovementDataThread();
