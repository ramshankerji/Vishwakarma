---
name: yyy-file-inspection
description: "How to inspect a saved .yyy project file (SQLite + per-type protobuf blobs) to verify persistence, and the varint trap that makes naive parsers lie"
metadata: 
  node_type: memory
  type: reference
  originSessionId: 04967110-ac87-4666-b641-6320e3c76b1a
  modified: 2026-08-24T17:39:08.792Z
---

A `.yyy` project file is a **SQLite database**, so persistence claims can be checked directly
instead of through the GUI. This turned a whole verification round from "screenshot and squint" into
exact assertions.

```sql
CREATE TABLE object_store (
  object_id INTEGER PRIMARY KEY,  parent_id INTEGER,
  object_type INTEGER NOT NULL,   schema_version INTEGER NOT NULL DEFAULT 1,
  lifecycle_state INTEGER NOT NULL DEFAULT 0,  data BLOB NOT NULL);
```

`data` is ONE type-specific protobuf message (`Sphere`, `Cylinder`, …) with **no shared envelope** —
nothing common wraps it, which is why anything cross-cutting must be added to all 15 3D `.proto`
files. `object_type` maps to `VishwakarmaStorage::ObjectType` in CommonNamedNumbers.h — read it
there, do not guess (Sphere is **6**; 9 is Pipe — I mis-indexed once and drew a wrong conclusion).

**The trap that cost me two wrong answers: protobuf keys are VARINTS.** Field 20 (the placement
convention) encodes as `0xA2 0x01` — two bytes. A parser that does `key = buf[j]; j += 1` extracts
the right field number and wire type by luck, then reads the continuation byte `0x01` as the
submessage length and decodes garbage — in my case a placement that looked like all zeros when it
was really `(0,0,10)`. Always decode keys and lengths with a real varint loop:

```python
def varint(b, j):
    v = s = 0
    while True:
        c = b[j]; j += 1; v |= (c & 0x7f) << s; s += 7
        if not c & 0x80: return v, j
```

Handle wire types 0 (varint), 1 (64-bit), 2 (length-delimited), 5 (32-bit); a `.yyy` with 2D objects
will hit wire type 1 and a parser missing it throws mid-scan.

**Also beware substring searches for a field tag.** Scanning a payload for the bytes `A2 01` to ask
"does this object carry field 20" gives false positives — it matched inside an ELBOW's `center`
float. Confirm structurally (is the claimed submessage length consistent with the bytes remaining?)
or just parse properly.

**Schema versions are recorded but gate nothing** on load, so bumping a `k*SchemaVersion` cannot
break existing files. Since 2026-08-02 one function answers "which version for this type" —
`VishwakarmaStorage::DefaultSchemaVersionForObjectType` in CommonNamedNumbers.h — called by object
creation, file load and serialization alike.

The flip side, which bites when a schema break is *intended*: with no gate, a stale blob is not
rejected, it is **silently misread**. Protobuf merges a repeated field into a singular one by taking
the LAST element, and absent fields default — so repurposing `Cuboid` field 1 from
`repeated Point3F vertices` to a scalar makes an old cuboid load as a zero-size box at its last
corner vertex, with no error anywhere. Give new meanings **new field numbers** and `reserved` the old
one (field 20 is the placement convention — never disturb it), and add a real version check if the
break must be loud. No load-time gate exists in any form today.

Saving is scriptable but note: **"Project Save" only shows a dialog when the tab has no
`storageFilePath`.** On a tab opened from a file it saves silently to that path, so a path typed
afterwards lands in the app as raw keystrokes and fires the single-letter debug keys (`g` bulk
import, `m` move-all, `r`, `c`, `k`, `l` pin instanced LOD, `n`, `v`).

**RE-SAVING A LARGE .yyy USED TO HANG THE APP, and it was a schema defect, not a slow disk**
(found and FIXED 2026-08-24, one added index in `EnsureSchema`, which
runs on save, so old files pick it up). Keep the full `idx_object_parent` index: the partial
`idx_object_parent_live` beside it looks like it covers the same column and cannot.
`foreign_keys = ON` + a self-referencing
`FOREIGN KEY(parent_id)` + the only `parent_id` index being **partial**
(`WHERE lifecycle_state = 0`) = SQLite cannot use it for FK enforcement, which also disables the
truncate optimisation, so `SaveTabToYyy`'s `DELETE FROM object_store;` scans the table once per
deleted row. First save of 300k records into a NEW file: under a minute. Same drawing saved over an
existing 100k-row file: not finished after 15 minutes at 100% of a core.

**Two diagnosis traps that cost ~20 minutes on that hang.** (1) **Windows does not update a file's
size in the directory entry while a handle is open** - `Get-Item bench.yyy-wal | .Length` read 0
throughout, which made "slowly writing" look like "deadlocked before any I/O"; it read 2.4 MB the
instant the process was killed. Never infer progress from the size of a file the target process has
open. (2) Flat `PrivateMemorySize64` over 36 s plus one thread at 100% looked like a spin loop, but
the allocating phase (`BuildRowsFromTab`) had simply already finished and SQLite was reusing its own
page cache. `Get-Process().Threads` state/waitreason is worth dumping early - it showed no thread
blocked on a lock, which is what finally killed the deadlock theory.
Related: [[ribbon-command-recipe]], [[object-model-unification]].
