// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

#include <algorithm>
#include <cmath>

#include "Snap.h"

namespace {
/* Geometric midpoints of the 1-2-5 ladder, i.e. sqrt(1*2), sqrt(2*5) and sqrt(5*10). A mantissa
   below a threshold is closer - in log space, which is the only meaningful measure on a
   multiplicative sequence - to the value below it than to the one above. */
constexpr double kMidpoint1To2 = 1.4142135623730951;
constexpr double kMidpoint2To5 = 3.1622776601683795;
constexpr double kMidpoint5To10 = 7.0710678118654755;
}

double NiceRound125(double value) {
    if (!(value > 0.0)) return 0.0;   // Written as a negated comparison so NaN takes this branch.

    const double power = std::pow(10.0, std::floor(std::log10(value)));
    const double mantissa = value / power;   // In [1, 10), barring float error at exact decades.

    double nice = 10.0;
    if (mantissa < kMidpoint1To2)       nice = 1.0;
    else if (mantissa < kMidpoint2To5)  nice = 2.0;
    else if (mantissa < kMidpoint5To10) nice = 5.0;
    return nice * power;
}

double SnapToStep(double value, double step) {
    if (!(step > 0.0)) return value;
    return std::round(value / step) * step;
}

double Page2DAmbientStepCU(double zoomPixelsPerCU) {
    if (!(zoomPixelsPerCU > 0.0)) return kPage2DAmbientStepFloorCU;

    const double step = NiceRound125(kAmbientStepTargetPixels / zoomPixelsPerCU);
    // The floor is itself a 1-2-5 value, so clamping to it cannot leave the sequence.
    return (std::max)(step, kPage2DAmbientStepFloorCU);
}
