// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
#pragma once

// Release / install / update machinery. Design document: website/content/software/release.md
// SoftwareUpdate.cpp is compiled into two binaries:
// 1. Vishwakarma.exe (the application): the two functions below.
// 2. VishwakarmaSetup.exe (the installer): compiled with VISHWAKARMA_INSTALLER defined,
//    provides its own wWinMain and carries Vishwakarma.exe + release json as resources.

// Called at the very start of wWinMain. If a newer verified setup is staged, it launches
// that setup with --update and returns true: the application must then exit immediately.
bool SoftwareUpdateOnAppLaunch();

// Starts the detached background thread which periodically (random 10 minutes to 10 hours)
// fetches the signed release manifest and stages a newer setup for the next launch.
void StartSoftwareUpdateThread();
