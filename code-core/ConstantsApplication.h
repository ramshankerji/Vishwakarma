// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
#pragma once
#include <cstdint>

/*Instead of keeping the software generic, to be able to handle "any" limits, we are setting following hard limits.
These are more than the number, a reasonable human can operate simultaneously.
Putting a hard limit simplifies software development, reduces bugs etc, costs just few kilo bytes of extra memory.
Initially we named it to MAX_MONITORS etc, which was conflicting with some windows headers,
Hence we have decided to prefix all constants below with "MV" to avoid conflict with other libraries.
*/
static const int MV_MAX_MONITORS = 16; // Worst case when the user installs 4 GPUs in same workstation 4x4 = 16.
/*Following is maximum number of independent windows, not tabs. A windows can host 100s of tabs themselves !
So opening a 1000 separate drawing files is still supported. They will simply showup as 1000 tabs in 1 window.*/
static const int MV_MAX_WINDOWS = 128; // Fits in a uint8_t if we ever need compact IDs.
/*I ( Ram ) tried to open 500+ drawing simultaneously at once, while searching for specific drawing.
Current state of art software failed ! Anyway 1000 is far more than normal human would like to see at once.*/
static const int MV_MAX_TABS = 1024;//Maximum number of tabs.
/*Maximum sub-tabs (views such as Page2D / Scene3D containers) open per tab. Fixed array inside
DATASETTAB avoids std::vector reallocation issues and enables delayed slot release after the
respective GPU assets are drained.
Deliberately EQUAL to the 64 bits of the per-object VisibilityMask membership word (10M plan Step 5),
so every open sub-tab is mask-addressable and there is no second class of slot that has to fall back
to "show everything". It was 128 until 2026-08-02; the mismatch bought nothing and made the fallback
path reachable in normal use. Raising it past 64 therefore needs a second mask word, not just a
bigger array here.*/
static const int MV_MAX_SUBTABS = 64;
/*Containers a single sub-tab may draw at once (graphics.md, 10M plan Step 6). A sub-tab holds a SET
of containers of one type, not a single one. Fixed and small on purpose: render threads read the set
lock-free every frame, so it must be inline storage rather than a heap buffer the engineering thread
could reallocate underneath them.*/
static const int MV_MAX_CONTAINERS_PER_SUBTAB = 8;
/*Maximum simultaneously GPU-resident Scene3D GRAPHICS objects in ONE tab - not engineering objects.
One engineering object can emit several graphics objects (a pipe as walls plus end caps, an object
plus its centerline), each with its own gpuInstanceIndex, transform, appearance and visibility, so
the engineering count this supports is this figure divided by the average parts per object.

This is the size of the per-tab instance arena's reserved (tiled) virtual address range: 10485760
records x 64 bytes = 640 MB of GPU virtual address reserved per tab, with physical 64 KB tiles
committed only as the arena grows, so an empty tab still costs zero physical bytes. Lowering it caps
how many objects a tab can hold. Raising it is NOT free above ~33M: D3D12's
MaxGPUVirtualAddressBitsPerResource can be as low as 31 bits (2 GB) on the lowest tier, and
MaxGPUVirtualAddressBitsPerProcess bounds the sum across open tabs. See
website/content/software/graphics.md, 10M plan Step 2 and "Goal and workload".*/
static const uint32_t MV_MAX_INSTANCES_PER_TAB = 10 * 1024 * 1024;
