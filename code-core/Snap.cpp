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

double Scene3DAmbientStepM(double viewportHeightPx, double distanceToPlane, double fovRadians) {
    const double tanHalfFov = std::tan(fovRadians * 0.5);
    // A degenerate camera (zero height, the plane through the eye, a 0 or 180 degree field of view)
    // has no defined scale, so there is nothing to derive a step from. Fall back to the floor
    // rather than dividing by zero and rounding every coordinate to NaN.
    if (!(viewportHeightPx > 0.0) || !(distanceToPlane > 0.0) || !(tanHalfFov > 0.0)) {
        return kScene3DAmbientStepFloorM;
    }

    const double pixelsPerUnit = viewportHeightPx / (2.0 * distanceToPlane * tanHalfFov);
    if (!(pixelsPerUnit > 0.0)) return kScene3DAmbientStepFloorM;

    const double step = NiceRound125(kAmbientStepTargetPixels / pixelsPerUnit);
    return (std::max)(step, kScene3DAmbientStepFloorM);
}

double SnapAperturePx(uint8_t priority) {
    // Level 0 is the ambient grid: unbounded, so it catches everything the bounded levels missed.
    if (priority == 0) return HUGE_VAL;
    const uint8_t level = (std::min)(priority, static_cast<uint8_t>(15));
    // Linear from kApertureMaxPx at level 1 down to kApertureMinPx at level 15. Monotonically
    // decreasing is the property that matters: fine snaps must be approached closely, coarse ones
    // catch you from far away.
    return kApertureMaxPx -
        (double)(level - 1) * (kApertureMaxPx - kApertureMinPx) / 14.0;
}

SnapCandidateSet::SnapCandidateSet(uint32_t mask, double dpiScale)
    : mask_(mask), dpiScale_(dpiScale > 0.0 ? dpiScale : 1.0) {
    for (uint8_t level = 0; level < kLevels; ++level) {
        bestSecondId_[level] = 0;
        bestDistancePx_[level] = HUGE_VAL;
    }
}

void SnapCandidateSet::Consider(const SnapPoint& point, double screenDistancePx,
    uint64_t secondObjectId) {
    if (point.kind == SnapKind::None) return;
    if (!SnapMaskHas(mask_, point.kind)) return;
    // NaN takes this branch: a candidate that projected to nothing is not a candidate.
    if (!(screenDistancePx >= 0.0)) return;

    const uint8_t level = (std::min)(point.priority, static_cast<uint8_t>(15));
    if (level == 0) return;   // Priority 0 is the ambient grid, which is not a candidate.

    // Apertures are quoted in logical pixels; on a 4K panel a fixed device-pixel aperture would
    // feel half the size (snapping.md section 5).
    if (screenDistancePx > SnapAperturePx(level) * dpiScale_) return;
    if (screenDistancePx >= bestDistancePx_[level]) return;

    best_[level] = point;
    bestSecondId_[level] = secondObjectId;
    bestDistancePx_[level] = screenDistancePx;
}

bool SnapCandidateSet::Resolve(SnapResult& out) const {
    // Descending priority, first level with a hit wins. Distance already broke ties within a level.
    for (uint8_t level = 15; level >= 1; --level) {
        if (bestDistancePx_[level] == HUGE_VAL) continue;

        out.hit = true;
        out.kind = best_[level].kind;
        out.priority = best_[level].priority;
        out.x = best_[level].x;
        out.y = best_[level].y;
        out.z = best_[level].z;
        out.objectId = best_[level].objectId;
        out.secondObjectId = bestSecondId_[level];
        return true;
    }
    return false;
}
