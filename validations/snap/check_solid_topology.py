# Extracts the box topology tables VERBATIM from SnapPointsForObject and checks them as pure
# combinatorics. A wrong index there puts snap points on edges the solid does not have.
# Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
import re, sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
src = (REPO_ROOT / "code-core" / "DataStorage.cpp").read_text(encoding="utf-8")
# Narrow to the function body first: these case labels also appear in three earlier switches.
body = src[src.index("bool SnapPointsForObject("):]
ok = True

def table(anchor, name, width):
    i = body.index(anchor)
    m = re.search(r"static constexpr int %s\[\d+\]\[%d\] = \{(.*?)\};" % (name, width), body[i:], re.S)
    nums = [int(x) for x in re.findall(r"\d+", m.group(1))]
    return [tuple(nums[k:k+width]) for k in range(0, len(nums), width)]

def fail(msg):
    global ok
    print("FAIL " + msg); ok = False

def check_box(label, edges, faces):
    if len(edges) != 12: fail(f"[{label}] {len(edges)} edges, expected 12")
    if len(faces) != 6:  fail(f"[{label}] {len(faces)} faces, expected 6")
    norm = {frozenset(e) for e in edges}
    if len(norm) != 12: fail(f"[{label}] duplicate edges")
    for e in edges:
        if e[0] == e[1] or not all(0 <= v < 8 for v in e): fail(f"[{label}] bad edge {e}")
    deg = {v: 0 for v in range(8)}
    for a, b in edges: deg[a] += 1; deg[b] += 1
    if set(deg.values()) != {3}: fail(f"[{label}] degrees {sorted(deg.values())}; a box graph is 3-regular")
    incidence = {}
    fdeg = {v: 0 for v in range(8)}
    for f in faces:
        if len(set(f)) != 4: fail(f"[{label}] face {f} repeats a vertex")
        for v in f: fdeg[v] += 1
        for k in range(4):
            pair = frozenset((f[k], f[(k + 1) % 4]))
            if pair not in norm: fail(f"[{label}] face {f} is not a cycle: {sorted(pair)} is not an edge")
            incidence[pair] = incidence.get(pair, 0) + 1
    if set(fdeg.values()) != {3}: fail(f"[{label}] corner/face counts {sorted(fdeg.values())}; each corner meets 3 faces")
    if len(incidence) != 12 or set(incidence.values()) != {2}:
        fail(f"[{label}] every edge must be shared by exactly 2 faces")

cub_e = table("case ObjectType::Cuboid:", "kEdges", 2)
cub_f = table("case ObjectType::Cuboid:", "kFaces", 4)
par_e = table("case ObjectType::Parallelepiped:", "kEdges", 2)
par_f = table("case ObjectType::Parallelepiped:", "kFaces", 4)
fru_e = table("case ObjectType::FrustumOfPyramid:", "kEdges", 2)
fru_f = table("case ObjectType::FrustumOfPyramid:", "kFaces", 4)
check_box("Cuboid", cub_e, cub_f)
check_box("Parallelepiped", par_e, par_f)
check_box("FrustumOfPyramid", fru_e, fru_f)

# Cuboid: bit i of the corner index is the sign along axis i.
for a, b in cub_e:
    if bin(a ^ b).count("1") != 1: fail(f"[Cuboid] edge {a}-{b} flips {bin(a^b).count('1')} axes, must flip 1")
for f in cub_f:
    if len([bit for bit in range(3) if len({(v >> bit) & 1 for v in f}) == 1]) != 1:
        fail(f"[Cuboid] face {f} does not hold exactly one axis constant")

# Parallelepiped: vertices are origin, A, B, C, A+B, A+C, B+C, A+B+C (PARALLELEPIPED::Randomize).
subset = {0:frozenset(), 1:frozenset("A"), 2:frozenset("B"), 3:frozenset("C"),
          4:frozenset("AB"), 5:frozenset("AC"), 6:frozenset("BC"), 7:frozenset("ABC")}
for a, b in par_e:
    if len(subset[a] ^ subset[b]) != 1:
        fail(f"[Parallelepiped] edge {a}-{b} differs by {sorted(subset[a]^subset[b])}, must be one vector")
for f in par_f:
    if len([v for v in "ABC" if len({(v in subset[i]) for i in f}) == 1]) != 1:
        fail(f"[Parallelepiped] face {f} does not hold exactly one vector constant")
# The six faces must be exactly GetGeometry's six AddFace quads, as sets.
expected = {frozenset(q) for q in [(0,2,4,1),(0,3,6,2),(0,1,5,3),(7,5,1,4),(7,6,3,5),(7,4,2,6)]}
if {frozenset(f) for f in par_f} != expected:
    fail("[Parallelepiped] faces do not match PARALLELEPIPED::GetGeometry's AddFace list")

# Frustum: 0-3 bottom ring, 4-7 top ring, i joined to i+4.
for k in range(4):
    for e in [(k, (k + 1) % 4), (4 + k, 4 + (k + 1) % 4), (k, k + 4)]:
        if frozenset(e) not in {frozenset(x) for x in fru_e}:
            fail(f"[FrustumOfPyramid] missing edge {e} implied by its index list")

# The tetrahedron's own edge table, checked the same way.
i = body.index("case ObjectType::Pyramid:")
m = re.search(r"static constexpr int kEdges\[6\]\[2\] = \{(.*?)\};", body[i:], re.S)
nums = [int(x) for x in re.findall(r"\d+", m.group(1))]
pyr = [tuple(nums[k:k+2]) for k in range(0, len(nums), 2)]
if {frozenset(e) for e in pyr} != {frozenset(p) for p in [(0,1),(1,2),(2,0),(0,3),(1,3),(2,3)]}:
    fail("[Pyramid] the 6 edges of a tetrahedron (0,1,2 base + 3 apex) are not all present exactly once")

print("box topology:", "ALL PASS" if ok else "FAILURES")
sys.exit(0 if ok else 1)
