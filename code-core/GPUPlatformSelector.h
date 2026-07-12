// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
#pragma once

// GpuPlatform.h — the ONLY file that names a platform header.
#if defined(_WIN32)
#include "MemoryManagerGPU-DirectX12.h"   // defines PlatformTabGpu, PlatformWindowGpu, ...
#elif defined(__linux__) || defined(__ANDROID__)
#include "MemoryManagerGPU-Vulkan1.1.h"
#elif defined(__APPLE__)
#include "MemoryManagerGPU-Metal.h"
#endif
