# Snapping tests that run without Windows

The snapping engine ([Snapping](https://mv.ramshanker.in/software/snapping)) is mostly arithmetic:
a priority ladder, a 1-2-5 decimal sequence, conic intersections, and the edge/face topology of
five solids. None of that needs a GPU, a window, or DirectX — so none of it should have to wait for
a full Windows build to be checked.

These three run on any machine with a C++20 compiler and Python. They cover the parts where a
defect is silent: a snap that returns the *wrong exact number* still looks like a snap.

## Running them

```bat
cd validations\snap
g++ -std=c++20 -Wall -Wextra -I..\..\code-core -o snap_core_test.exe ..\..\code-core\Snap.cpp snap_core_test.cpp
snap_core_test.exe

python extract_2d_geometry.py snap2d_extracted.inc
g++ -std=c++20 -Wall -Wextra -I..\..\code-core -I. -o snap_2d_test.exe ..\..\code-core\Snap.cpp snap_2d_geometry_test.cpp
snap_2d_test.exe

python check_solid_topology.py
```

All three print `ALL PASS` and exit 0, or name the failing line and exit 1.

## What each one is for

**`snap_core_test.cpp`** — the resolution rule of §5, which is the part of the specification that
exists because the obvious implementation is wrong. It asserts that a *nearer* coarse candidate
loses to a *farther* fine one when both are inside their own apertures, that distance only breaks
ties within a level, that a disabled kind never wins however close it is, that the aperture ladder
is monotonic, and that a candidate which projected to nothing (NaN) is discarded rather than ranked
best. Also the 1-2-5 ladder and both ambient-step functions across their whole working range —
0.0001 to 5000 px/CU in 2D, 0.01 m to 100 km of view distance in 3D — checking every step lands on
the sequence and never below the floor.

**`snap_2d_geometry_test.cpp`** — the 2D conic geometry, against hand-computed reference cases:
tangents from (5,0) to a radius-3 circle touching at (1.8, ±2.4), a rotated ellipse cut at ±3
instead of ±8, perpendicular feet that satisfy `dot(P − E(t), E′(t)) = 0` to 1e-6, arc sweeps that
wrap through zero, and the round trip between `PointAt` and `ParameterOf` under rotation.

It `#include`s the geometry **verbatim** from `RenderPage2D.cpp` via `extract_2d_geometry.py`
rather than restating it. A hand-copied version would drift from what the application compiles, and
a geometry test passing against a stale copy is worse than no test at all. The flip side is that
the extractor's two markers are load-bearing: if the section is renamed or moved, it fails loudly
instead of silently testing nothing.

**`check_solid_topology.py`** — the edge and face tables of `SnapPointsForObject`, extracted the
same way and checked as pure combinatorics: a box graph is 3-regular, every face is a real cycle
(consecutive entries are edges, not merely corners of the same face), every edge belongs to exactly
two faces, and every corner meets three. It also checks each solid against its own construction —
the cuboid's corner-index bit scheme, the parallelepiped's origin/A/B/C vertex order taken from
`PARALLELEPIPED::GetGeometry`, and the tetrahedron's six edges.

This is the test that earns its keep: an index typed wrong in a 12-entry table produces snap points
on edges the solid does not have, which is invisible in a screenshot and obvious here.

## What they do NOT cover

Everything that needs the application: the ribbon toggles and their latched state, the hover marker
and its label, the input-loop wiring, the work plane, the 3D object scan, and every threading
contract. Those need a Windows build and the visual-verification recipe in the `ribbon-command`
skill.
