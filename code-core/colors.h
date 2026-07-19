// Copyright (c) 2025-Present : Ram Shanker: All rights reserved.

// This file hosts all cross-platform colur defintions and constants.
#pragma once

#include <cstdint>

// Label / icon colour for UI controls whose command is not yet wired to any handler.
// Deliberately dull, so an un-implemented button reads as inert without being invisible.
constexpr uint32_t kUIDisabledTextGray = 0xFF9E9E9E; //rgba(158, 158, 158)

// Sky background, drawn as one gradient quad by ClearSceneSkyGradient (Scene3D only).
constexpr float kSceneSkyTopR = 0.62f;
constexpr float kSceneSkyTopG = 0.82f;
constexpr float kSceneSkyTopB = 1.00f;
constexpr float kSceneSkyHorizonR = 0.94f;
constexpr float kSceneSkyHorizonG = 0.98f;
constexpr float kSceneSkyHorizonB = 1.00f;

// Page2D drawing-area background, and the value baked into every window RTT as its optimized clear
// value. Scene3D paints its sky over this, so matching the 2D case keeps the fast clear on both
// paths (a clear to any other color forfeits it and trips CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE).
constexpr float kCad2DBackgroundR = 230.0f / 255.0f;
constexpr float kCad2DBackgroundG = 230.0f / 255.0f;
constexpr float kCad2DBackgroundB = 230.0f / 255.0f;