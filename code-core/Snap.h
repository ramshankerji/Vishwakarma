// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
#pragma once

/* Snapping: turning an imprecise cursor pixel into an exact engineering coordinate.
   Design: website/content/software/snapping.md

   This is stage 1 of that plan - the ambient grid only. It is the always-on fallback of the
   resolution rule (locked decision 5): priority 0, unbounded aperture, cannot be switched off, and
   it never fails. Object snap points, apertures, the priority ladder and the settings mask arrive in
   later stages; nothing here anticipates them.

   Platform-agnostic and deliberately free of graphics headers. */

// Target on-screen size of one ambient grid step, in logical pixels (snapping.md section 16).
constexpr double kAmbientStepTargetPixels = 15.0;

/* Smallest step the ambient grid will ever produce, and the reason the two worlds cannot share one
   constant: Page2D ComputerUnits are millimetres while 3D geometry is SI metres, so a nominal
   "0.001 units" means a micron in one and a millimetre in the other (snapping.md section 10). */
constexpr double kPage2DAmbientStepFloorCU = 0.001;   // 0.001 mm.

/* Snaps value to the nearest member of the 1-2-5 decimal sequence (... 0.1, 0.2, 0.5, 1, 2, 5,
   10 ...), nearest measured geometrically since the sequence is multiplicative. Powers of two are
   deliberately not used: engineers read and dimension in decimal. Returns 0 for non-positive input. */
double NiceRound125(double value);

// Rounds one coordinate to the nearest multiple of step. A non-positive step returns value unchanged.
double SnapToStep(double value, double step);

// The ambient grid step for a 2D page, derived from its zoom so one step is about
// kAmbientStepTargetPixels on screen at any zoom level.
double Page2DAmbientStepCU(double zoomPixelsPerCU);
