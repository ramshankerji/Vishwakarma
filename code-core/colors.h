// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.

// This file hosts all cross-platform colur defintions and constants.
#pragma once

// Sky background. The full vertical gradient (ClearSceneSkyGradient) is Scene3D; the
// compositor/window RTT clear reuses the top color so there is no flash before the scene draws.
constexpr float kSceneSkyTopR = 0.62f;
constexpr float kSceneSkyTopG = 0.82f;
constexpr float kSceneSkyTopB = 1.00f;
constexpr float kSceneSkyHorizonR = 0.94f;
constexpr float kSceneSkyHorizonG = 0.98f;
constexpr float kSceneSkyHorizonB = 1.00f;
constexpr int   kSceneSkyGradientBands = 48;