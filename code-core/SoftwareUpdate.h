// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
#pragma once

#include <atomic>

// Release / install / update machinery. Design document: website/content/software/release.md
// SoftwareUpdate.cpp is compiled into two binaries:
// 1. Vishwakarma.exe (the application): the two functions below.
// 2. VishwakarmaSetup.exe (the installer): compiled with VISHWAKARMA_INSTALLER defined,
//    provides its own wWinMain and carries Vishwakarma.exe + release json as resources.

// Called at the very start of wWinMain. If a newer verified setup is staged, it launches
// that setup with --update and returns true: the application must then exit immediately.
bool SoftwareUpdateOnAppLaunch();

// Starts the detached background thread which periodically (random 10 minutes to 4 hours)
// fetches the signed release manifest and stages a newer setup for the next launch.
void StartSoftwareUpdateThread();

// True while a verified newer setup sits staged for the next launch. Written by the update
// thread, read by the render threads to show the "Restart to Update" toast.
extern std::atomic<bool> g_softwareUpdateStagedForRestart;

// Called from the main thread (SOFTWARE_UPDATE_CHECK ribbon button): wakes the update thread
// so it runs its check-and-stage cycle immediately instead of waiting out the random interval.
void RequestImmediateSoftwareUpdateCheck();

// Runs a single, immediate update cycle (download + verify + stage, then apply if no other
// instance is running) and returns a process exit code. Used by the weekly scheduled task
// which launches the application with --background-update. Opens no window and starts none
// of the graphics / copy / engineering threads.
int RunBackgroundUpdate();

// Removes the application: the scheduled task, desktop shortcut, "Apps & features" entry,
// per-user updater data and the install folder. Invoked via --uninstall (the command
// registered as the Windows UninstallString).
void RunUninstall();
