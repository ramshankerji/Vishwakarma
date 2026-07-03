// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
#pragma once

/* Printing of 2D drawings and 3D scene images. See website/content/software/printer.md
Pipeline (Print MVP): CAD model -> D3D12 offscreen render target at print DPI ->
GPU readback -> BGRA bitmap -> GDI StartDoc/StartPage/StretchDIBits -> Windows Print Spooler.
No printer related configuration is maintained in the application. The standard
Windows print dialog is shown and the installed printer driver handles the rest. */

// Prints the currently visible content (Page2D drawing or 3D scene image) of the
// active tab. Must be called from the main UI thread (shows modal print dialog).
void PrintActiveTab();
