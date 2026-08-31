// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
// Verifies the snapping.md section 5 resolution rule and the section 10 ambient ladder.
#include "Snap.h"
#include <cstdio>
#include <cmath>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

int main() {
    // --- aperture ladder: strictly decreasing, level 0 unbounded, endpoints as documented -------
    CHECK(std::isinf(SnapAperturePx(0)));
    CHECK(std::abs(SnapAperturePx(1) - kApertureMaxPx) < 1e-9);
    CHECK(std::abs(SnapAperturePx(15) - kApertureMinPx) < 1e-9);
    for (uint8_t l = 1; l < 15; ++l) CHECK(SnapAperturePx(l) > SnapAperturePx(l + 1));

    // --- the rule is PRIORITY-first, not global nearest --------------------------------------
    // Both inside their OWN apertures (level 13 reaches 12 px, level 14 reaches 11 px): the
    // nearer coarse centre at 8 px must still lose to the farther endpoint at 10 px.
    {
        SnapCandidateSet set(kDefaultSnapMask2D, 1.0);
        SnapPoint coarse{}; coarse.kind = SnapKind::Center; coarse.priority = 13; coarse.x = 1.0;
        SnapPoint fine{};   fine.kind = SnapKind::End;      fine.priority = 14; fine.x = 2.0;
        set.Consider(coarse, 8.0);
        set.Consider(fine, 10.0);
        CHECK(SnapAperturePx(13) > 8.0 && SnapAperturePx(14) > 10.0);
        SnapResult r{};
        CHECK(set.Resolve(r));
        CHECK(r.kind == SnapKind::End);
        CHECK(r.x == 2.0);
    }
    // Within one level, distance decides.
    {
        SnapCandidateSet set(kDefaultSnapMask2D, 1.0);
        SnapPoint a{}; a.kind = SnapKind::End; a.priority = 14; a.x = 1.0;
        SnapPoint b{}; b.kind = SnapKind::End; b.priority = 14; b.x = 2.0;
        set.Consider(a, 9.0);
        set.Consider(b, 3.0);
        SnapResult r{};
        CHECK(set.Resolve(r) && r.x == 2.0);
    }
    // A disabled kind is never returned, however close it is.
    {
        SnapCandidateSet set(kDefaultSnapMask2D, 1.0);   // Nearest is off by default (section 16).
        SnapPoint n{}; n.kind = SnapKind::Nearest; n.priority = 6; n.x = 5.0;
        set.Consider(n, 0.5);
        SnapResult r{};
        CHECK(!set.Resolve(r));
    }
    // Outside its own aperture a candidate does not count: level 15 reaches only kApertureMinPx.
    {
        SnapCandidateSet set(kDefaultSnapMask3D, 1.0);
        SnapPoint m{}; m.kind = SnapKind::MemberEnd; m.priority = 15;
        set.Consider(m, kApertureMinPx + 0.5);
        SnapResult r{};
        CHECK(!set.Resolve(r));
        set.Consider(m, kApertureMinPx - 0.5);
        CHECK(set.Resolve(r) && r.kind == SnapKind::MemberEnd);
    }
    // DPI scales the aperture: the same candidate that missed at 1.0 hits at 2.0.
    {
        SnapPoint m{}; m.kind = SnapKind::MemberEnd; m.priority = 15;
        SnapResult r{};
        SnapCandidateSet lo(kDefaultSnapMask3D, 1.0); lo.Consider(m, 15.0); CHECK(!lo.Resolve(r));
        SnapCandidateSet hi(kDefaultSnapMask3D, 2.0); hi.Consider(m, 15.0); CHECK(hi.Resolve(r));
    }
    // Intersection carries both participants through to the result.
    {
        SnapCandidateSet set(kDefaultSnapMask2D, 1.0);
        SnapPoint i{}; i.kind = SnapKind::Intersection; i.priority = 10; i.objectId = 7;
        set.Consider(i, 4.0, 9);
        SnapResult r{};
        CHECK(set.Resolve(r) && r.objectId == 7 && r.secondObjectId == 9);
    }
    // A NaN distance (a candidate that projected behind the camera) is discarded, not ranked best.
    {
        SnapCandidateSet set(kDefaultSnapMask3D, 1.0);
        SnapPoint m{}; m.kind = SnapKind::MemberEnd; m.priority = 15;
        set.Consider(m, std::nan(""));
        SnapResult r{};
        CHECK(!set.Resolve(r));
    }

    // --- the 1-2-5 ladder ---------------------------------------------------------------------
    CHECK(NiceRound125(0.0) == 0.0);
    CHECK(NiceRound125(-3.0) == 0.0);
    CHECK(std::abs(NiceRound125(1.3) - 1.0) < 1e-12);
    CHECK(std::abs(NiceRound125(1.5) - 2.0) < 1e-12);
    CHECK(std::abs(NiceRound125(3.0) - 2.0) < 1e-12);
    CHECK(std::abs(NiceRound125(4.0) - 5.0) < 1e-12);
    CHECK(std::abs(NiceRound125(8.0) - 10.0) < 1e-12);
    CHECK(std::abs(NiceRound125(3000.0) - 2000.0) < 1e-9);
    CHECK(std::abs(NiceRound125(0.00031) - 0.0002) < 1e-15);

    CHECK(SnapToStep(1.234, 0.0) == 1.234);      // Non-positive step is a no-op.
    CHECK(std::abs(SnapToStep(1.234, 0.5) - 1.0) < 1e-12);
    CHECK(std::abs(SnapToStep(-1.4, 1.0) + 1.0) < 1e-12);

    // 2D ambient: every step is on the ladder and never below the floor, across the whole
    // zoom range the view clamps to (0.0001 .. 5000 px/CU).
    for (double zoom = 0.0001; zoom <= 5000.0; zoom *= 1.07) {
        const double step = Page2DAmbientStepCU(zoom);
        CHECK(step >= kPage2DAmbientStepFloorCU);
        CHECK(std::abs(NiceRound125(step) - step) < step * 1e-9);
    }
    CHECK(Page2DAmbientStepCU(0.0) == kPage2DAmbientStepFloorCU);
    CHECK(Page2DAmbientStepCU(-1.0) == kPage2DAmbientStepFloorCU);

    // 3D ambient: same properties, and degenerate cameras fall back rather than produce NaN.
    CHECK(Scene3DAmbientStepM(0.0, 10.0, 1.047) == kScene3DAmbientStepFloorM);
    CHECK(Scene3DAmbientStepM(1080.0, 0.0, 1.047) == kScene3DAmbientStepFloorM);
    CHECK(Scene3DAmbientStepM(1080.0, 10.0, 0.0) == kScene3DAmbientStepFloorM);
    for (double d = 0.01; d < 100000.0; d *= 1.3) {
        const double step = Scene3DAmbientStepM(1080.0, d, 60.0 * 3.14159265358979 / 180.0);
        CHECK(step >= kScene3DAmbientStepFloorM);
        CHECK(std::abs(NiceRound125(step) - step) < step * 1e-9);
    }
    // At a 10 m viewing distance on a 1080 px viewport the step should be a human-scale one.
    {
        const double step = Scene3DAmbientStepM(1080.0, 10.0, 60.0 * 3.14159265358979 / 180.0);
        std::printf("3D ambient step at 10 m / 1080 px / 60 deg = %g m\n", step);
        CHECK(step >= 0.05 && step <= 0.5);
    }

    // --- default masks match section 16 exactly -----------------------------------------------
    CHECK(SnapMaskHas(kDefaultSnapMask2D, SnapKind::End));
    CHECK(SnapMaskHas(kDefaultSnapMask2D, SnapKind::Intersection));
    CHECK(!SnapMaskHas(kDefaultSnapMask2D, SnapKind::Nearest));
    CHECK(!SnapMaskHas(kDefaultSnapMask2D, SnapKind::Parallel));
    CHECK(!SnapMaskHas(kDefaultSnapMask2D, SnapKind::TextBounds));
    CHECK(SnapMaskHas(kDefaultSnapMask3D, SnapKind::MemberEnd));
    CHECK(SnapMaskHas(kDefaultSnapMask3D, SnapKind::ObjectDefined));
    CHECK(!SnapMaskHas(kDefaultSnapMask3D, SnapKind::Nearest));
    // Every kind fits in the 32-bit mask.
    CHECK(kSnapKindCount <= 32);
    CHECK(SnapMaskBit(SnapKind::ObjectDefined) == (1u << 19));

    std::printf(failures == 0 ? "snap core: ALL PASS\n" : "snap core: %d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
