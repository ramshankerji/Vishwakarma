// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
#pragma once

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
