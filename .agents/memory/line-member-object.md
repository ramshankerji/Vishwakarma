---
name: line-member-object
description: LINE_MEMBER (ObjectType 27) + STD importer profile carry-over both implemented 2026-07-17 — mapping lives in Python profile_mapping.py (user-owned), C++ only does designation→id lookup
metadata: 
  node_type: memory
  type: project
  originSessionId: ab994074-ccc1-476e-8475-e40d15d3031c
---

LINE_MEMBER = straight structural member: `point1/point2` (XMFLOAT3, SI meters) + `profileId`
(uint64 into [[steel-section-catalog]]) + colorMain/Inner/Cap. Lives in `code-core/डेटा-संरचना.h`
(replaced the dead STRUCTURAL_SIMPLE_MEMBER placeholder; file gained #pragma once + includes).
Persistence: `DataStorage_LINE_MEMBER.proto` + 5 dispatch sites in DataStorage.cpp (ToString,
FromNumber 27, encode, decode, geometry-regen). Ribbon: `CREATE_LINE_MEMBER = 3905024746`;
random-spawn switch case 14 in विश्वकर्मा.cpp (dice bound bumped 13→14).

Geometry (v1, per user decision): sharp corners, flange_slope honored (tf at (b−tw)/4 gauge,
linear inner-face taper, toe clamped ≥ 0.25·tf), fillets r1/r2 ignored, RAIL = envelope approx
(foot 0.20h / head 0.30h), BULB bulb = convex quad approx. Sections centered on bbox center,
not centroid. Default orientation: section +y = world +Z projected ⊥ axis (STAAD BETA=0),
fallback +X for vertical members — `MemberSectionBasis()`. Cross-sections decompose into convex
CCW pieces extruded by `AppendExtrudedConvexPrism` (fan caps); coplanar piece interfaces are
always opposite-facing so back-face culling prevents z-fighting; CHS reuses AppendPipeTube.
Unknown/0 profileId → fallback 114.3×5 tube. Randomize() picks a uniform random catalog row.

**Why:** one family = one topology (SteelTable.md); interpenetrating convex pieces = the TEE
"no CSG" convention.

**How to apply (gotchas met):**
- `IsGeometry3DObjectType` in CommonNamedNumbers.h has RANGE checks (`Elbow..LineMember` now);
  a new 3D ObjectType after 27 must extend it or loading silently fails. Also add
  ObjectTypeDisplayName case there.
- Headers compiled into Windows TUs must use `(std::max)(...)` parenthesized form — max/min
  macros are live project-wide (no NOMINMAX).
- Whole-number float literals from codegen need `.0f` not `f` (steel_profile_embedder.py cpp_float).
- Outline math for all 508 rows re-verifiable via scratchpad-style python mirror (CCW/convex/
  bounds); app smoke = 3 stable launches; save→reopen roundtrip still needs a manual in-app check.

STD importer profile carry-over IMPLEMENTED 2026-07-17 (user amendment: mapping in PYTHON, not
C++). `extensions/Interoperability-STD/profile_mapping.py` = user-owned mapping:
`VISHWAKARMA_DESIGNATION_BY_STD_NAME` (410 entries machine-seeded from catalog: no-space-upper
keys + HEA/HEB/HEM forms; "I10" ambiguity dropped → resolves Chinese GB row) + `designation_for()`
(TABLE only; ST/TC/BC/TB → base name, plate params ignored; T/D/LD/SD/RA/CM/PRIS/UPTABLE → "";
unknown names pass through unchanged so identity designations like W10X49 need no entry).
Known gaps for user: US angles (STAAD L40404 vs L4X4X1/4), STAAD NPB/WPB with mass suffix,
Japanese partial H names. IPC: `StructuralMember.profile_designation = 4`;
vishwakarma_api.send_geometry_batch accepts (id,a,b) or (id,a,b,designation) tuples.
Host: ImportedMember.profileDesignation (>64 bytes → cleared, not an error);
ImportStdFileIntoTab builds designation→record unordered_map (dupes: first row wins, JIS/KS OK),
match → LINE_MEMBER, else PIPE; log line "N profile members and M placeholder pipes".

PARAMETRIC sections ADDED 2026-07-17 (commits d9cdb6e + 54624c1): LINE_MEMBER gained
`userParameter1/2` (float32 **millimeters**, 0 = catalog-row defaults; proto fields 7/8, schema
v2 via `kGeometry3DLineMemberSchemaVersion` — `DefaultSchemaVersionForObjectType` now has a
LineMember case and AppendObjectToTab/save-fallback route through it; loaded rows keep their
stored version, sticky like Ellipse v1). New `SteelProfileFamily::Parametric` +
`Catalog/profiles_parametric.csv` (4 rows RECT/CIRC/OCT/HEX; `series` picks outline like BAR;
embedder validates required default-dims per series). GetGeometry Parametric case: RECT p1=depth
h/p2=width b; CIRC p1=dia (24-gon); OCT p1=across-flats, `Polygon(af/2/cos(π/8), 8, π/8)`; HEX
`Polygon(af/√3, 6, 0)`. Pane: `kLineMemberFields` (8 fields: P1/P2 xyz + params) +
CrossLineMember (points distinct, params ≥ 0); labels PropParameter1/2 = WordID 1434/1435.
STD import: `profile_mapping.parametric_for()` maps PRIS YD+ZD→RECT, YD-only→CIRC, YB/ZB→pipe;
wire `StructuralMember.user_parameter1/2 = 5/6` in SI **meters** (host ×1000 → mm at
materialization); member tuples (id,a,b,designation,p1,p2). DeployExtensions.ps1 reruns protoc
--python_out at every post-build, so the deployed pb2 always matches ExtensionIPC.proto.
Future shapes (custom I, Z): new CSV rows + series branches + user_parameter3/4… (fields 9,10…).

Testing recipe that works: drive the deployed worker exactly like the host —
`VishwakarmaExtension.exe main.py`, cwd=build/Debug/extensions/Interoperability-STD,
length-prefixed protobuf on stdin/stdout (scratchpad drive_std_worker.py pattern; system python
has protobuf and can import the deployed ExtensionIPC_pb2). Full-app e2e: set
`VISHWAKARMA_AUTO_IMPORT_STD=<file.std>` env var + PowerShell Start-Process
-RedirectStandardOutput — the GUI app's std::cout IS captured through the redirected handle,
so "[std-importer] Created ..." is assertable.
