---
name: hot-drag-placement-design
description: "Why object placement was designed as a composed rigid transform, the alternatives rejected, and the local-space refactor parked for later"
metadata: 
  node_type: memory
  type: project
  originSessionId: 04967110-ac87-4666-b641-6320e3c76b1a
  modified: 2026-08-02T00:14:12.800Z
---

Shipped 2026-08-01/02. **The design itself is now documented in the repo** — see the *Object
placement* section and the updated *World matrix* section of `website/content/software/graphics.md`.
This memory keeps only what the doc does not: the decision history, the parked follow-on, and the
open edges.

**Status.** Phase 5's hot-drag item is done as a DATA PATH: a move rewrites the object's placement
and emits a transform-only MODIFY costing zero geometry page clones (measured twice). What remains
is tooling — gizmo, screen-ray-to-drag-plane, and an undo story ([[undo-redo-design]] is design
only). `EDIT_MOVE`/`EDIT_ROTATE` exist but are Page2D-only ([[page2d-transforms]]).

**Three options were put to Ram; he chose composed placement.**

- *Composed placement* (chosen): stored coordinates become authored space, placement carries the
  move, Properties Pane composes so the user still sees world coordinates. Only option that meets
  the 10M plan's "move 1000 scattered objects clones zero pages" budget.
- *Bake-back at drag end* (rejected): no schema change, drag is smooth, but a committed move of 1000
  scattered objects still costs ~4 GB of clone traffic at mouse-up — defers the budget instead of
  meeting it, wasting what Steps 1-4 bought.
- *Full local-space refactor* (deferred, see below): the correct CAD answer, far larger.

He also chose **data path only** for the round (no interactive tooling), and **leave the Step 7
compute-cull default OFF** until it is one ExecuteIndirect per Viewport.

**Untested: rotation.** Nothing produces a non-identity quaternion yet, so the rotate branch of
`TransformPoint`/`InverseTransformPoint` and of the pane's inverse solve is written but never
exercised. First rotate producer is its real test.

**Parked: the local-space refactor.** Ram raised it unprompted ("why on earth should the sphere's
default geometry be around (-4.568, 1.543, -7.0)") and is right. Agreed conclusion, to revisit:

- Do NOT force all 15 types into origin+quaternion. Split by what naturally defines the type.
  **Point-shaped** (sphere, torus, ellipsoid, cuboid, pyramid, parallelepiped, frustum-of-pyramid,
  elbow, flange) → local shape params + stored placement. **Axis-shaped** (cylinder, cone,
  frustum-of-cone, pipe, tee, line member) → keep the two endpoints as stored truth, generate local
  geometry along +Z, and *derive* the placement from (p1, p2, roll). Ram's own instinct that linear
  members must keep start/end coordinates generalises to all six.
- The free move survives derivation, because **the fast path depends on the local geometry being
  invariant under the move, not on the placement being the stored form.**
- Payoff beyond tidiness: instancing is currently *impossible* (identical bolts produce different
  vertex bytes), and float32 precision is spent encoding world offset rather than shape.
- Acceptance criterion if it happens: the world↔authored conversion layer in PropertyPane gets
  DELETED, not maintained. If it survives, the refactor did not go far enough.
- Open first: how TEE's `branchAngleDegrees` and LINE_MEMBER's section orientation are anchored
  today — if there is an implicit "up is +Z", make roll an explicit stored field.
- Instancing also needs shape-parameter dedup (hash → shared geometry); local space is the
  prerequisite, not the whole win.

**Still latent, Ram said "visit later":** three creation paths in विश्वकर्मा.cpp (~1194, ~1379, ~1718)
call `shape->GetGeometry()` directly, bypassing `GeometryForObject`, so an object CREATED with a
non-identity placement will not show it until reload. Harmless while nothing creates placed objects.

Related: [[properties-pane-plan]], [[graphics-refactor-phases]], [[line-member-object]].
