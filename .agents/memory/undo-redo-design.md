---
name: undo-redo-design
description: "Undo/redo is DESIGNED ONLY (website/content/software/undo-redo.md, 2026-07-19) — nothing built; the codebase findings that shaped it and the one section left open"
metadata: 
  node_type: memory
  type: project
  originSessionId: 93e534da-436e-4728-ab2b-f178e59744df
---

Design doc written 2026-07-19 at `website/content/software/undo-redo.md` (Hugo weight 100111).
**Status: nothing is implemented.** Phases 0–7 are all pending. Do not assume undo exists when
touching mutation paths.

Codebase findings that took real digging and drive the whole design:

- **No delete operation exists anywhere in the app.** No `Commands::EDIT_DELETE`, no ACTION_TYPE,
  and nothing pushes `CommandToCopyThreadType::REMOVE` — the copy-thread handler
  (`RenderScene3D-DirectX12.cpp:787`) is complete but unreachable dead code. Undo-of-create *is* a
  delete, so this blocks more than undo.
- **`Encode*`/`Decode*` for every type (3D and 2D) already exist but are trapped in an anonymous
  namespace inside `DataStorage.cpp`.** Lifting them into a shared header is Phase 0 of anything
  that needs object payloads outside that TU.
- **The arena never frees and `isDeleted` is a soft flag in both worlds** (`META_DATA` and every
  `Cad2D*RecordCPU`; the 2D page rebuild's `wanted()` filter already honours it). Consequence:
  undo of a create / redo of a delete need *no payload at all*, only an id list — which is why a
  50k-object DXF import is cheap to undo.
- **`dataVersion` in 2D: NO LONGER A BLOCKER, 2026-08-26.** This design's one required
  existing-struct change is done - all nine `Cad2D*RecordCPU` types derive `META_DATA` and inherit
  `dataVersion` ([[object-model-unification]]). ONE CAVEAT before relying on it: the copy-thread and
  loader upserts do `existing = incoming`, a whole-object copy, so an edit currently OVERWRITES
  dataVersion rather than incrementing it. Deciding that is an open item in id.md §7.

Architecture: one `TransactionBegin/RecordObjectChange/TransactionCommit` chokepoint on the
engineering thread; undo reads the log backwards, the future collaboration outbox reads it
forwards (same log, two readers — deliberately not two subsystems). RAM-only fixed-slot ring on
`DATASETTAB`; the `object_undo_log` table in `storage.md` §14.10 stays unimplemented by choice.

**§7 is deliberately OPEN** — how much protocol runs when no peer is connected (always-full vs
solo fast path vs log-always-defer-network). Ram has not decided; do not resolve it unilaterally.
See [[user-working-style]] on keeping open modalities open. Related: [[graphics-leak-audit]] for
the copy-thread fence discipline any REMOVE path must respect.
