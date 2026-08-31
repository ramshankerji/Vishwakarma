// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
#pragma once

#include <cstdint>

/* Snapping: turning an imprecise cursor pixel into an exact engineering coordinate.
   Design: website/content/software/snapping.md

   This header is the shared vocabulary of both worlds - the snap kinds, the settings mask, the
   priority/aperture ladder and the resolution rule of section 5. The candidate GATHERING lives with
   the data it walks (RenderPage2D.cpp for the 2D records, SnapPointsForObject beside
   GeometryForObject for the 3D objects); only the rule that picks a winner lives here.

   Platform-agnostic and deliberately free of graphics headers, so the resolution rule can be
   compiled and tested without a GPU or a window. */

// Target on-screen size of one ambient grid step, in logical pixels (snapping.md section 16).
constexpr double kAmbientStepTargetPixels = 15.0;

/* Smallest step the ambient grid will ever produce, and the reason the two worlds cannot share one
   constant: Page2D ComputerUnits are millimetres while 3D geometry is SI metres, so a nominal
   "0.001 units" means a micron in one and a millimetre in the other (snapping.md section 10). */
constexpr double kPage2DAmbientStepFloorCU = 0.001;   // 0.001 mm.
constexpr double kScene3DAmbientStepFloorM = 0.001;   // 0.001 m.

/* The rule that produced a point. Also the marker-glyph selector, which is why Ortho appears here
   even though it is a direction constraint rather than a point snap (snapping.md section 4). */
enum class SnapKind : uint8_t {
    None = 0, AmbientGrid, GridObject, Ortho, End, Mid, Center, Quadrant,
    Perpendicular, Parallel, Tangent, Intersection, Nearest, Insertion,
    TextBounds, EdgeMid, FaceCenter, MemberEnd, MemberMid, ObjectDefined
};
constexpr uint8_t kSnapKindCount = 20;   // Values 0..19 above. One mask bit each, 32 available.

// What an object exports. Packed and cheap to generate: a dense drawing produces these by the
// thousand every time the cursor moves.
struct SnapPoint {
    double   x = 0.0, y = 0.0, z = 0.0;   // z unused in 2D.
    uint64_t objectId = 0;
    SnapKind kind = SnapKind::None;
    uint8_t  priority = 0;                // 0 = coarsest (ambient grid) .. 15 = finest.
};

struct SnapResult {
    bool     hit = false;          // false only when every snap including ambient is disabled.
    SnapKind kind = SnapKind::None;
    uint8_t  priority = 0;
    double   x = 0.0, y = 0.0, z = 0.0;
    uint64_t objectId = 0;         // 0 for ambient grid / ortho-only results.
    // Intersection is inferred from TWO objects and the caller (and the marker label) needs both.
    uint64_t secondObjectId = 0;
};

/* Everything a running tool must tell the resolver. The relative snaps and both direction
   constraints are meaningless without a "from" point, so a tool that has not taken its first click
   yet leaves hasAnchor false and the resolution loop skips them (snapping.md section 11). */
struct SnapContext {
    bool     hasAnchor = false;
    double   anchorX = 0.0, anchorY = 0.0, anchorZ = 0.0;
    uint32_t effectiveMask = 0;    // Kind bits currently enabled, master switch already applied.
    bool     ortho = false;        // F8: lock the vector from the anchor onto the nearest axis.
    double   dpiScale = 1.0;       // Logical -> device pixels; apertures are in LOGICAL pixels.
};

// Bit n is SnapKind value n (snapping.md section 12). AmbientGrid's bit is ignored - it is always
// on by locked decision 5 - and Ortho's is unused, ortho being mode state rather than a mask bit.
constexpr uint32_t SnapMaskBit(SnapKind kind) noexcept {
    return 1u << static_cast<uint8_t>(kind);
}
constexpr bool SnapMaskHas(uint32_t mask, SnapKind kind) noexcept {
    return (mask & SnapMaskBit(kind)) != 0u;
}

/* Default masks, stated completely so no kind is left ambiguous (snapping.md section 16): every
   implemented kind on except Nearest, which fires almost everywhere and drowns the finer snaps.
   Parallel, TextBounds and GridObject are absent because they are not implemented - TextBounds
   needs 2D text hit-testing and GridObject needs a grid entity, neither of which exists. */
constexpr uint32_t kDefaultSnapMask2D =
    SnapMaskBit(SnapKind::End) | SnapMaskBit(SnapKind::Mid) | SnapMaskBit(SnapKind::Center) |
    SnapMaskBit(SnapKind::Quadrant) | SnapMaskBit(SnapKind::Intersection) |
    SnapMaskBit(SnapKind::Perpendicular) | SnapMaskBit(SnapKind::Tangent) |
    SnapMaskBit(SnapKind::Insertion);

constexpr uint32_t kDefaultSnapMask3D =
    SnapMaskBit(SnapKind::MemberEnd) | SnapMaskBit(SnapKind::MemberMid) |
    SnapMaskBit(SnapKind::End) | SnapMaskBit(SnapKind::EdgeMid) |
    SnapMaskBit(SnapKind::FaceCenter) | SnapMaskBit(SnapKind::Center) |
    SnapMaskBit(SnapKind::Quadrant) | SnapMaskBit(SnapKind::ObjectDefined);

/* Aperture by priority, in LOGICAL pixels (snapping.md section 5). Kept as a computed function
   rather than a hand-tuned sixteen-entry table until measurement on real drawings justifies one -
   the two constants are the whole model. Level 0 is the ambient grid: unbounded, so it is the
   guaranteed fallback and never appears here. */
constexpr double kApertureMaxPx = 24.0;   // Level 1, the coarsest bounded snap.
constexpr double kApertureMinPx = 10.0;   // Level 15, a member end or a piping connection point.
double SnapAperturePx(uint8_t priority);

/* The resolution rule of section 5: DESCENDING PRIORITY, first level with a hit wins - not global
   nearest. Distance breaks ties only WITHIN a level.

   The naive "nearest candidate inside its own radius" comparison is the trap this exists to avoid:
   with apertures widening as priority falls, a grid point 20 px away would beat an endpoint 10 px
   away and the finest snaps would become unreachable exactly when the drawing is dense.

   Feed it candidates in any order, each with its distance from the cursor IN SCREEN PIXELS - never
   in world units, or the aperture becomes zoom-dependent, which is precisely wrong. */
class SnapCandidateSet {
public:
    SnapCandidateSet(uint32_t mask, double dpiScale);

    /* Offers one candidate. Ignored unless its kind is enabled in the mask and it lies inside the
       aperture of its own priority level. secondObjectId is for Intersection, which is inferred
       from two objects; every other kind leaves it 0. */
    void Consider(const SnapPoint& point, double screenDistancePx, uint64_t secondObjectId = 0);

    // The winner, or false when no bounded level was hit and the caller must fall back to ambient.
    bool Resolve(SnapResult& out) const;

private:
    static constexpr uint8_t kLevels = 16;   // Priorities 0..15; level 0 is never stored here.
    uint32_t  mask_;
    double    dpiScale_;
    SnapPoint best_[kLevels];
    uint64_t  bestSecondId_[kLevels];
    double    bestDistancePx_[kLevels];
};

/* Snaps value to the nearest member of the 1-2-5 decimal sequence (... 0.1, 0.2, 0.5, 1, 2, 5,
   10 ...), nearest measured geometrically since the sequence is multiplicative. Powers of two are
   deliberately not used: engineers read and dimension in decimal. Returns 0 for non-positive input. */
double NiceRound125(double value);

// Rounds one coordinate to the nearest multiple of step. A non-positive step returns value unchanged.
double SnapToStep(double value, double step);

// The ambient grid step for a 2D page, derived from its zoom so one step is about
// kAmbientStepTargetPixels on screen at any zoom level.
double Page2DAmbientStepCU(double zoomPixelsPerCU);

/* The same for the 3D world, where the on-screen size of a world unit is not a stored zoom but a
   consequence of the perspective projection (snapping.md section 10):
       pixelsPerUnit = viewportHeight / (2 * distanceToWorkPlane * tan(fov/2))
   Metres, not millimetres - the two worlds share the ladder and nothing else. */
double Scene3DAmbientStepM(double viewportHeightPx, double distanceToPlane, double fovRadians);

/* The 3D work plane: a screen ray does not determine a point, and the ambient grid can only round
   WITHIN a plane, so the plane is explicit state rather than something inferred from the camera
   (snapping.md section 9). Axis-aligned, named by its NORMAL axis plus an offset along that normal.

   It belongs to the VIEW, not to the Scene3D - a view will eventually host several Scene3D
   containers at once - so it lives in Viewport beside that view's CameraState, and is never
   written to file. */
constexpr uint8_t kWorkPlaneAxisX = 0;
constexpr uint8_t kWorkPlaneAxisY = 1;
constexpr uint8_t kWorkPlaneAxisZ = 2;   // Default: the horizontal plane. Structural and plant work
                                         // is floor-based and the camera is Z-up.
struct SnapWorkPlane {
    uint8_t axis = kWorkPlaneAxisZ;   // Which axis the plane's normal points along.
    double  offset = 0.0;             // Signed distance from the origin along that normal, metres.
};
