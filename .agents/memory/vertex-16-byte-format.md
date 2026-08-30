---
name: vertex-16-byte-format
description: "Vertex is 16 B (pos+normal only) since 2026-08-05; colour comes per-object from InstanceRecord::packedColor, shaders are split _16/_24 by stride, and the registry colour shadow is load-bearing"
metadata: 
  node_type: memory
  type: project
  originSessionId: c9d7be10-b5bf-43e6-a2e5-0569185d862f
  modified: 2026-08-05T13:29:18.014Z
---

Shipped 2026-08-05: `struct Vertex` (`डेटा.h`) dropped its `XMHALF4 color`, **24 B → 16 B**
(`XMFLOAT3 position` + `XMUBYTE4 normal`, `static_assert`ed). Colour is now one RGBA8 per *object* in
`InstanceRecord::packedColor`, unpacked by the vertex shader. Design is written up in
website/content/software/graphics.md (*Vertex format*, *Vertex format variants*) — read that for the
rationale; what follows is only what the doc does not make obvious.

**Conventions a future change must not break:**

- **Shaders are split by stride, not versioned in place.** `ShaderSceneVertex_16.hlsl` /
  `ShaderScenePickVertex_16.hlsl` are what every PSO binds; `*_24.hlsl` are the parked per-vertex-
  colour twins, still `FxCompile` items with distinct `VariableName`s (`g_sceneVertexShader16` vs
  `…24`) so they cannot bit-rot — nothing includes the `_24` headers. Two `FxCompile` entries with
  the same `VariableName` would collide, which is why the C++ symbols were renamed too.
- **The pixel shaders are shared across strides and deliberately NOT twinned.** That is the reason
  the 16-byte vertex shader does *not* mark its COLOR interpolant `nointerpolation`, even though the
  value is constant per object: the modifier would have to be matched in the shared pixel shader and
  would flat-shade the per-vertex-colour variants. Ram approved `nointerpolation` in the plan; it was
  dropped during implementation for this reason and he was told.
- **`InstanceRegistryEntry::packedColor` is load-bearing, not a convenience.** A transform-only
  MODIFY writes a whole fresh 64-byte record but arrives with empty vertex/index vectors, and the
  arena is device-local so the old record cannot be read back — without the shadow every drag
  repaints the object **black**. It was free: the entry had 36 used bytes of 40, so the assert still
  says 40. See [[graphics-leak-audit]] for the arena/registry it lives in.
- **Per-face colour was knowingly given up, storage untouched.** Every type still stores
  `colorBase`/`colorIncline`/`colorOuter`/… and every `.proto` is unchanged; each `GetGeometry()`
  just nominates a dominant face into `GeometryData::color` (a field that existed with no reader
  until now). Ram accepted this explicitly, pending per-face disaggregation. Do not "fix" it by
  re-adding vertex colour.

**Two things deliberately NOT built** (so a second live format is still real work): `sizeof(Vertex)`
is still one global constant — no `GeometryPage::vertexStride`, `VertexAlign`/`IsFull` still static —
and `RoundUpToMultiple` was kept over the now-valid `AlignUp` mask precisely because the variants
need it. Note also that vertex format is the one axis costing a **draw call**: the input layout is
PSO state and does not ride in the compacted command, so N formats = N `ExecuteIndirect`s per
Viewport, which is what would break [[step7-per-viewport-draw]]'s single call.

**Watch `argGrow`.** Denser vertices mean more objects per 4 MB page and so more 24-byte arguments:
the dense-cuboid case went from ~6,470 objects (155 KB, 59% of the 256 KB reservation) to ~9,200
(216 KB, **84%**). Measured 0 growths, but a non-zero reading now means the reservation is undersized
for the new density, not an exotic workload.
