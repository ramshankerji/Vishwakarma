---
title: "Undo and Redo"
weight: 100111
---
This page is the Design Document for the Undo/Redo subsystem and the local transaction log it is
built on. To be referred by AI for coding as well as humans. It is a plan; sections marked as
locked are decisions, sections marked **OPEN** are still under debate and must not be treated as
settled by an implementer.

The central claim of this document: **undo is not a feature, it is a read of the transaction log
in reverse.** The same log, read forward, is the collaboration outbox described in
`storage.md` §10. Building two separate mechanisms would mean two conflict
models, two sequence numbers, and two sources of truth about what changed. So we build the log
once and give it two readers. One honest refinement to that claim: the outbox reader consumes the
log forward *plus* the inverse transactions that undo/redo synthesise (§5, §8) — an undo moves
the local cursor, but to a peer it is simply the next forward change. The log is still built
once; the two readers just have slightly different views of it.

## 0. Scope of this iteration

In scope: a per-tab, RAM-only transaction log; undo/redo of property edits, in-place 2D
transforms, object creation, object deletion, and bulk imports; the ribbon buttons and Ctrl+Z /
Ctrl+Y routing.

Out of scope, deliberately: persisting the log to the `.yyy` file (the `object_undo_log` table
specified in `storage.md` §14.10 is **not** implemented by this work), and the network layer
itself. The undo history is born when a tab opens and dies when it closes. Saving a file does
not clear it. The log's *format and retention rules* are nevertheless written so that the future
network layer can attach to it without reshaping it — that is the point of building one log with
two readers (§7).

## 1. What the code looks like today

These facts about the existing code drive every decision below. Each was re-verified against the
source on 2026-07-20.

**1.1 There are two mutation worlds, not one.** 3D engineering objects are `META_DATA`-derived
structs allocated in the `राम` arena under the tab's `memoryGroupNo`, registered in
`tab.storageObjects3D` / `tab.storageLogicalObjects`, mutated on the engineering thread under
`storageObjectsMutex`, and pushed to the GPU as `CommandToCopyThread{ADD|MODIFY|REMOVE}`. 2D CAD
objects are plain `Cad2D*RecordCPU` structs living in `std::vector`s inside `tab.cad2d`, guarded
by `cpuRecordsMutex`. The 2D vectors have a subtlety the transaction log must respect: **new 2D
records are inserted into those vectors by the copy thread**, not the engineering thread. The
`EnqueueCad2D*` functions push commands onto a FIFO queue; the copy thread drains it and upserts
by `objectId` into the vectors (`RenderPage2D-DirectX12.cpp:863`). The engineering thread mutates
*existing* records in place under the mutex (the transform path), but a just-created record may
still be in flight on the queue. §5 and §9 deal with this.

**1.2 There is no delete operation anywhere in the application.** No `Commands::EDIT_DELETE`, no
`ACTION_TYPE` for it, and nothing in the codebase pushes `CommandToCopyThreadType::REMOVE` —
the handler at `RenderScene3D-DirectX12.cpp:787` is complete but unreachable. Since undo of a
create *is* a delete, the delete primitive is on the critical path and is planned here (§6) even
though it ships as a later phase.

**1.3 Every object type already has protobuf Encode/Decode.** `DataStorage.cpp` contains
`EncodeSphere` / `DecodeSphere`, `EncodeLine2D` / `DecodeLine2D` and so on for the full type set,
3D and 2D alike. They are locked inside an anonymous namespace (reopened twice, lines 56 and
1389) and are therefore unreachable from outside that translation unit. Lifting them out is
Phase 0.

**1.4 The arena never frees, and `isDeleted` is a soft flag on both `META_DATA` and every
`Cad2D*RecordCPU`.** The 2D page rebuild already skips deleted records (`wanted()` in
`RenderPage2D-DirectX12.cpp:757`) and the 3D copy thread's REMOVE path already soft-deletes the
GPU record and frees its matrix slot. This means:

> Undo of a create, and redo of a delete, need **no stored payload at all** — only a list of
> object ids and a flag to flip. Only *modify* transactions carry before/after bytes.

That single observation is what makes a 50,000-object DXF import cheap to undo. The same fact
has a sharp edge, though: `राम` exposes exactly `Allocate` and `notifyTabClosed`
(`MemoryManagerCPU.h:303-305`). **There is no per-allocation free.** An earlier draft of this
document placed undo payloads in the arena and "freed them on eviction" — that is
unimplementable. Payload ownership is resolved in §3.

**1.5 Bulk imports are synchronous on the engineering thread.** The DXF and STD importers do run
in a separate worker process (`VishwakarmaExtension.exe`), but
`ExtensionCommunications::RunQueuedStdImport` / `RunQueuedDxfImport` are *blocking* calls: they
return the complete parsed model, and `ImportStdFileIntoTab` / `ImportDxfFileIntoTab`
(`विश्वकर्मा.cpp:1144`, `1244`) then materialise every object in one loop on the engineering
thread. An import is therefore an ordinary transaction — begin before the loop, commit after —
not the streaming special case an earlier draft designed around (§6.4). A worker crash returns
null before anything is materialised, so no transaction is ever left open.

**1.6 Keyboard shortcuts are mapped on the engineering thread, not in WndProc.** WndProc pushes
raw `KEYDOWN` into the per-tab `userInputQueue`, and already suppresses that push while a UI text
field has focus (`uiKeyboardCaptureCount` gate, `Main.cpp:1087`). The engineering thread's
`KEYDOWN` switch does the mapping — the 'P'-spawns-pyramid example at `विश्वकर्मा.cpp:1930` —
with Ctrl/Shift/Alt state already tracked in `tab.isCtrlDown` etc. Ctrl+Z, Ctrl+Y and Delete
follow this exact pattern; they get text-field suppression for free at the WndProc gate.

**1.7 Ribbon enabled/disabled state is compile-time.** `AllUIControls[]` is `constexpr`
(`UserInterface.h:463`) and `isEnabled` is a static field. There is currently **no mechanism to
grey a button at runtime.** Undo/Redo are the first controls whose enabled state changes moment
to moment, so Phase 2 must add a small runtime-enable hook to the ribbon renderer (§9).

## 2. Locked design decisions

1. **One chokepoint.** Every mutation to engineering data passes through
   `TransactionBegin` / `RecordObjectChange` / `TransactionCommit` on the engineering thread. No
   mutation site writes an object field without an open transaction. Undo reads the log backwards;
   the collaboration outbox reads it forwards. The sole sanctioned exception is
   `Cad2DAutoGenerateDemoContent` and the auto-random-geometry debug scaffolding (§4), which
   bypass the log entirely.
2. **Optimistic local-first commit.** Every user action mutates local RAM state immediately and
   unconditionally, in every mode — solo, offline, or connected. Committing to the upstream
   authority is asynchronous: the network layer (future) reads committed transactions from the
   log and submits them; the user is never blocked waiting for the authority. Only when the
   authority *rejects* a transaction is the user interrupted: they are notified and the rejected
   work is rolled back on screen (§8). This is what makes the client fully usable as a RAM-only
   offline client of a centralised data server — disconnected editing is not a special mode, it
   is just an outbox that has not drained yet (`storage.md` §5's 24-hour grace period bounds it).
3. **Version-guarded undo.** Each change record stores a guard version (§3, §5). An undo that
   would write over a version it does not recognise is refused, and the user is told why. Undo
   never silently discards another author's work. This reuses the
   `current_object_version == base_object_version` rule from `storage.md` §10.3
   rather than inventing a second conflict model.
4. **Protobuf payloads.** Before/after state is captured as protobuf bytes using the existing
   `Encode*` / `Decode*` functions, lifted into a shared header. One byte format for the undo log,
   the `.yyy` file, and future network transport. Schema-versioned for free.
5. **RAM only, per tab, heap-owned payloads.** The log lives in a fixed-slot ring on
   `DATASETTAB`. Payload bytes and change arrays are **plain heap allocations owned by their ring
   slot** — not arena allocations, because `राम` cannot free (§1.4) and a churning 256 MB payload
   budget would otherwise leak without bound over a long session. They are freed at eviction,
   redo-stack discard, rejection rollback, and tab close (the engineering thread's shutdown path
   walks the ring). This is a deliberate, documented exception to the arena house style;
   transaction commit is not a per-frame hot path. Engineering objects themselves stay in the
   arena. Nothing is written to disk.
6. **One user gesture, one transaction.** A ribbon click, a committed property edit, or a
   completed modal transform is exactly one undo step, however many objects it touched. A bulk
   import is likewise **one** transaction, undone as a single lot. Export is *not* a transaction:
   it mutates nothing, so under rule 7 it never enters the log (this resolves an inconsistency in
   an earlier draft that grouped export with import).
7. **View state is not undoable.** Camera position, Page2D pan/zoom, selection, sub-tab
   open/close, pane toggles, and visibility flags are not engineering data and never enter the
   log. This matches CAD convention: Ctrl+Z after a zoom must undo the last *edit*, not the zoom.
8. **Redo is cleared by new local gestures only.** Committing a new *user-initiated* transaction
   while the redo stack is non-empty discards the redo stack. Two things explicitly do **not**
   clear it: the inverse transaction an undo publishes to the outbox when connected (§5), and
   incoming remote deltas (rule 9). No undo trees, no branching history.
9. **Remote deltas never enter the undo ring.** Changes published by the authority on behalf of
   other authors are applied to local state but are not undoable here. Ctrl+Z operates on *my*
   gestures only; another author's change can only be reversed by editing it, which creates a new
   transaction of mine. This matches the guard model of §8.

## 3. Data structures

Two records, mirroring `transaction_log` and `object_change_log` from
`storage.md` §14.7–14.8 so the eventual persisted form is a field-for-field subset.

```cpp
// One entry per changed object within a transaction.
struct OBJECT_CHANGE_RECORD {
    uint64_t objectId = 0;          // memoryID. Process-scoped, which is all a RAM-only log needs.
    uint64_t containerMemoryId = 0; // 2D: owning Page2D (0 for asset masters). 3D: memoryIDParent.
                                    // The apply path and the copy-thread commands need it (§5).
    uint8_t  operationType = 0;     // storage.md §14.8: 1=insert 2=update 4=soft_delete.
                                    // 3=move and 8=restore are reserved and unused in this
                                    // iteration; re-parenting travels inside update payloads.
    uint8_t  world = 0;             // 0 = 3D META_DATA object, 1 = 2D Cad2D record. Selects the apply path.
    uint16_t objectType = 0;        // VishwakarmaStorage::ObjectType, so the decoder knows which Decode* to call.
    uint64_t guardVersion = 0;      // The dataVersion the next traversal (undo or redo) must find
                                    // on the live object. Written at commit as the post-edit
                                    // version; re-stamped by every undo/redo application (§5).

    // Heap-owned payload bytes (§2.5). Both null for insert/soft_delete — see §1.4.
    std::byte* beforePayload = nullptr;
    std::byte* afterPayload = nullptr;
    uint32_t   beforeSize = 0;
    uint32_t   afterSize = 0;
};

// One entry per user gesture.
struct TRANSACTION_RECORD {
    uint64_t transactionSeq = 0;  // Provisional, tab-local, monotonic. Never a real transaction_seq:
                                  // the authority assigns those on accept. This value is only the
                                  // outbox correlation handle. See §7.
    uint16_t transactionKind = 1; // storage.md §14.7: 1=user_edit 3=import 5=delete ...
    uint8_t  outboxState = 0;     // 0=solo/no session, 1=pending send, 2=sent unacked, 3=acked,
                                  // 4=rejected. Always 0 in this iteration; the field exists so the
                                  // eviction rule below is written against it from day one.
    uint32_t commandId = 0;       // Commands:: enum value, so the UI can label the button "Undo Move".
    uint64_t commitTimeMs = 0;    // GetTickCount64(). Display and diagnostics only, never ordering.

    OBJECT_CHANGE_RECORD* changes = nullptr; // Heap-owned array, freed with its payloads.
    uint32_t changeCount = 0;
    uint32_t payloadBytes = 0;    // Sum of before+after sizes, for the eviction budget.
};
```

On `DATASETTAB`, a fixed-slot ring in the house style (no `std::vector` growth in the hot path):

```cpp
static constexpr uint32_t MV_MAX_UNDO_TRANSACTIONS = 128;            // ConstantsApplication.h
static constexpr uint64_t MV_UNDO_PAYLOAD_BUDGET = 256ull << 20;     // 256 MB per tab. ConstantsApplication.h
TRANSACTION_RECORD undoRing[MV_MAX_UNDO_TRANSACTIONS];
uint32_t undoRingHead = 0;    // Next slot to write.
uint32_t undoRingCount = 0;   // Live entries, <= MV_MAX_UNDO_TRANSACTIONS.
uint32_t undoCursor = 0;      // Entries undone but not yet re-done, counted back from head.
uint64_t undoPayloadBytes = 0;
std::atomic<bool> canUndo{ false }, canRedo{ false }; // Published for the ribbon (§1.7).
```

**Bounds and eviction.** Two independent bounds, whichever binds first: `MV_MAX_UNDO_TRANSACTIONS`
slots, and the payload budget. Evicting the oldest transaction `delete[]`s its payloads and change
array and permanently shortens the reachable history — which is correct behaviour, not an error.
Three refinements:

- **The ack frontier.** Once a collaboration session exists, eviction may not reclaim a
  transaction whose `outboxState` is pending or sent-unacked — those entries are the offline
  work waiting to reach the server, and evicting them would silently lose it. (At minimum the
  `afterPayload` and guard metadata must survive; the `beforePayload` serves only undo and may be
  freed under budget pressure, sacrificing undo-reachability but never the outbox.) A tab that
  has never connected has no frontier and evicts freely. Extended offline editing therefore grows
  RAM with unsent transactions; that is the accepted cost of offline-first working, bounded by
  the 24-hour editing grace period of `storage.md` §5.
- **Oversized transactions.** A single transaction larger than the whole payload budget still
  commits — it simply evicts everything else and becomes the only reachable undo step.
- **Redo discard arithmetic.** When a new gesture commits while `undoCursor > 0` (§2.8), the
  slots in `[head - undoCursor, head)` are freed, `undoRingHead -= undoCursor`,
  `undoRingCount -= undoCursor`, `undoCursor = 0`, and only then is the new entry written.
  `undoPayloadBytes` is adjusted at every free.

**Ownership and threading.** The ring is written and read *only* by that tab's engineering thread.
No mutex is required for the ring itself. The UI thread never touches it; it posts
`ACTION_TYPE::UNDO` / `REDO` into `todoCPUQueue` like every other command, and reads the published
`canUndo` / `canRedo` atomics to grey out the ribbon buttons.

**2D records need a version field.** `META_DATA::dataVersion` already exists (`डेटा.h:103`) and
already increments on modify (`विश्वकर्मा.cpp:742`). The `Cad2D*RecordCPU` structs have
`schemaVersion` (the *schema's* version) but no object version. Phase 1 must add a `dataVersion`
field to each 2D record struct — all nine: the seven shape records plus
`Cad2DAssetDefinitionRecordCPU` and `Cad2DAssetInsertRecordCPU`. This is the only change to an
existing data structure that this work requires. Who bumps it is specified in §4.

## 4. The chokepoint

```cpp
// All four run on the engineering thread that owns the tab.
void TransactionBegin(DATASETTAB& tab, Commands commandId, uint16_t transactionKind);
void RecordObjectChange(DATASETTAB& tab, uint64_t objectId, uint8_t operationType,
                        uint8_t world, uint16_t objectType);
void TransactionCommit(DATASETTAB& tab);   // Empty transactions are dropped, not committed.
void TransactionAbort(DATASETTAB& tab);    // Modal gesture cancelled by ESC.
```

Timing rules, by operation:

- **update / soft_delete:** `RecordObjectChange` is called **before** the mutation, while the
  object still holds its old state: it encodes `beforePayload` there and then. There is no way to
  accidentally record a "before" that is already modified.
- **insert:** called **after** the object exists under its final id (there is no before-state to
  capture and no id until creation). No payloads are stored.
- Calling `RecordObjectChange` twice for the same object inside one transaction is a no-op the
  second time: the first capture wins, and the after-payload encoded at commit reflects the final
  state. One change record per object per transaction.

`TransactionCommit` walks its own change list, encodes the after-payloads of *update* records
from the now-mutated objects, and stamps each record's `guardVersion` with the object's post-edit
`dataVersion`. Call sites therefore need one line before the edit and nothing after it.

**Version-bump responsibility.** Every update site must leave `dataVersion` incremented before
`TransactionCommit` runs. `ModifyObjectProperty` already does this for 3D
(`विश्वकर्मा.cpp:742`); the 2D mutation sites gain their bump (under `cpuRecordsMutex`)
alongside the Phase 1 `dataVersion` field. Insert sites initialise it to 1.

**2D id materialisation.** The `EnqueueCad2D*` functions assign fresh ids *internally* to
by-value copies — a call site that passes `objectId = 0` never learns the id it just created
(see the comment at `RenderPage2D.cpp:1113`). Insert records need that id. Phase 1 therefore
changes the `EnqueueCad2D*` functions to **return the assigned `uint64_t` id**. Call sites that
do not record transactions simply ignore the return value; nothing else changes.

Existing mutation sites and where they hook in:

| Site | File | Transaction shape |
| --- | --- | --- |
| `ModifyObjectProperty` | `विश्वकर्मा.cpp:711` | 1 update. The cleanest site; Phase 2 wires it first. |
| `ApplyTransform2DToSelection` (Move, Rotate) | `RenderPage2D.cpp:1065` | N updates. |
| `ApplyTransform2DToSelection` (Copy, Offset, Mirror) | `RenderPage2D.cpp:1065` | N inserts; sources untouched. |
| `RegisterGeneratedGeometryElement` | `विश्वकर्मा.cpp:681` | 1 insert per 3D primitive. |
| `CreateLogicalElement` | `विश्वकर्मा.cpp:524` | 1 insert (Folder / Page2D / Scene3D). |
| `Cad2DHandleInput` creation paths | `RenderPage2D.cpp:1550` | 1 insert per completed shape. |
| `Cad2DInstantiateAsset` | `RenderPage2D.cpp:1369` | 1 insert for the insert record + N for its member copies. |
| `Cad2DCreateAssetFromSelection` | `RenderPage2D.cpp:711` | Mixed: inserts (definition, insert, masters) + updates (re-parented sources). |
| `ImportStdFileIntoTab` / `ImportDxfFileIntoTab` | `विश्वकर्मा.cpp:1144` / `1244` | 1 transaction, N inserts, begin/commit around the materialisation loop (§1.5, §6.4). |
| `Cad2DAutoGenerateDemoContent` | `RenderPage2D.cpp:1736` | **Excluded.** Debug scaffolding, not user data. |

The demo/random-geometry generator is deliberately left outside the log. It fires continuously on
a timer; letting it in would flood the ring and make Ctrl+Z useless during development.

## 5. Executing an undo

Undo pops the transaction at `undoRingHead - undoCursor - 1`, walks its changes **in reverse
order**, and applies the inverse of each:

| operationType | Undo does | Redo does |
| --- | --- | --- |
| insert (1) | `isDeleted = true`; 3D: push `REMOVE`. 2D: `SoftDelete2D` command. | `isDeleted = false`; 3D: re-`ADD` same id. 2D: `Restore2D` command. |
| update (2) | `Decode*(beforePayload)` into the live object; bump version; regenerate geometry; push `MODIFY` / re-enqueue. | Same with `afterPayload`. |
| soft_delete (4) | `isDeleted = false`; 3D: re-`ADD`. 2D: `Restore2D`. | `isDeleted = true`; 3D: `REMOVE`. 2D: `SoftDelete2D`. |

Reverse order matters within a transaction: `Cad2DCreateAssetFromSelection` creates a definition,
then an insert, then re-parents members. Undoing forward would orphan the members before their
parent is removed.

**The version guard, both directions.** Before applying a change record (either direction),
compare the live object's `dataVersion` against the record's `guardVersion`. On refusal the
transaction is marked non-undoable, the user is told why, and the cursor skips past it (§8). On
success the apply *bumps* the live `dataVersion` (versions are history — they never decrement)
and **re-stamps `guardVersion` with the version it just produced**, which is exactly what the
opposite traversal must find. Without the re-stamp, redo would have no valid guard: undo bumps
the version, so the commit-time value could never match again. In single-user linear undo the
guard always matches by construction — locally it is an assertion that catches bugs, not a gate
that fires. It becomes a real gate the moment a peer connects (§8), and costs one atomic read
either way, which is why it is captured unconditionally from day one rather than retrofitted.

**3D apply path.** Undo of a 3D update must call `GeometryForObject` and push a `MODIFY`
exactly as `ModifyObjectProperty` does — decoding the payload only restores the CPU-side struct.
Undo of a 3D insert pushes `REMOVE`; redo re-pushes `ADD` with the same geometry and the same
`memoryID` (§9).

**2D apply path — always through the copy-thread queue.** All 2D undo/redo mutations flow through
the same FIFO queue as forward edits, never as direct flag-flips on the vectors:

- *update:* decode the payload into the live record under `cpuRecordsMutex`, bump `dataVersion`,
  and re-enqueue it via the existing `Add*` upsert — the pattern the transform path already uses
  (`RenderPage2D.cpp:1300`).
- *insert-undo / delete / restore:* two new command types, `CommandToCopyThread2DType::SoftDelete2D`
  and `Restore2D`, carrying only object ids + container. The copy thread's ingest switch flips
  `isDeleted` on the stored records; the page rebuild's `wanted()` filter does the rest.

The queue is mandatory because of §1.1: a record created moments ago may not have been ingested
into the vectors yet. A direct flag-flip on the engineering thread could miss it, and the
in-flight `Add`'s upsert would then resurrect it. FIFO ordering makes the race impossible — the
`SoftDelete2D` is always processed after the `Add` it reverses. (The engineering thread's own
CPU-truth view lags by at most one copy-thread batch; acceptable, and no different from forward
edits.)

**Bookkeeping that must follow every delete/undo-of-create:** remove the affected ids from the 3D
selection (`tab.selection`) and the 2D selection set (`selectedObjectIds` under
`selection2DMutex`), and note that the data tree builder (`UserInterface.cpp:1557`) currently has
**no `isDeleted` filter** — without adding one, soft-deleted 3D objects remain as ghost nodes in
the tree (§9, §12).

**Undo is itself a transaction, but not a logged one.** An undo does not push a new entry onto the
ring; it moves `undoCursor`. When peers are connected the undo *is* published to the authority as
a synthesised forward transaction — a new update whose payload happens to be an older state —
because the peer has no concept of our local cursor (§8). That publication does **not** clear the
redo stack (§2.8); only a new local gesture does.

## 6. Delete and restore (planned, phased)

Undo of a create requires a delete path, which does not exist. It is designed here and built in
Phase 4.

**6.1 The command.** A new `Commands::EDIT_DELETE` (random unused 10-digit id) plus
`ACTION_TYPE::DELETE_SELECTED`, wired through the standard ribbon recipe: `ListOfCommands.h`,
the translation CSVs + compiled header, `AllUIControls[]` (with `isEnabled = true`), an SVG icon
+ `SVGIconManifest.h` entry, dispatch in `ProcessPendingUIActions` (`Main.cpp:365`), and a
handler in the `todoCPUQueue` loop (`विश्वकर्मा.cpp:1986`). The Delete key is mapped in the
engineering thread's `KEYDOWN` switch (`विश्वकर्मा.cpp:1930`), the same pattern as the 'P' key —
*not* in WndProc. Text-field suppression comes free: WndProc already withholds `KEYDOWN` from the
tab while `uiKeyboardCaptureCount != 0` (`Main.cpp:1087`). Ctrl+Z / Ctrl+Y take the identical
route, using the already-tracked `tab.isCtrlDown`.

**6.2 The mechanics.** 2D: enqueue `SoftDelete2D` with the selected ids (§5); the page rebuild's
`wanted()` filter already excludes deleted records. 3D: `isDeleted = true` on the `META_DATA`
plus a `CommandToCopyThreadType::REMOVE` push — the copy thread's handler is already written and
correct, including freeing the matrix slot and erasing from `objectLocation`, which is precisely
what a later redo-of-create needs in order to re-`ADD` under the same id. In both worlds, clear
the deleted ids from the selection sets and let the data tree filter hide them.

**6.3 Scope of v1 delete: geometry only.** `EDIT_DELETE` applies to selected 2D records and 3D
geometry objects. Logical containers — Folder, Page2D, Scene3D — are **not deletable in v1**:
deleting a container implies cascade semantics (what happens to its children, its open sub-tab,
its view state) that deserve their own design pass. The linear undo discipline itself never needs
container deletion: a container's children were created after it, so undo always reaches them
first.

**6.4 Consequence: soft-deleted objects accumulate.** Nothing is ever freed from the arena. A
long session of create-then-undo leaves dead objects occupying arena bytes and matrix slots. This
is an accepted cost of the design, bounded in practice by the ring eviction policy. The eventual
fix is the lifecycle compaction described in `storage.md` §9 (`tombstone_retained` →
`purged_stub`), which is a file-level operation and out of scope here. It should be recorded as a
known cost, not discovered later as a leak.

**6.5 Bulk import as one transaction.** Because imports are synchronous on the engineering
thread (§1.5), an import is an ordinary transaction: `TransactionBegin` before the
materialisation loop, one insert record per created object (ids now available — §4), and
`TransactionCommit` after. A worker crash returns before anything is created, so no transaction
opens at all. Since an import is insert-only, its undo record is `N × sizeof(OBJECT_CHANGE_RECORD)`
with no payloads: a 50,000-object DXF costs a few megabytes and undoes with one `SoftDelete2D`
batch and one page rebuild. (Should imports ever become streaming/asynchronous, the fallback
design is to accumulate created ids in a pending list and synthesise the transaction at
completion — recorded here so it is not re-invented, but not needed today.)

## 7. Solo and connected sessions: one capture path

**Locked.** An earlier draft debated three options for how much of the transaction protocol
should run when no peer is connected: (A) always everything, (B) a solo fast path that skips
capture, (C) always capture, defer only the network work. **Option C is adopted**, for the reason
the analysis already showed: the two costs people intuitively attribute to "the collaboration
protocol" — payload encoding and log slots — are *undo* costs that survive removing
collaboration entirely. Capturing the guard version is one atomic read of a field the code
already bumps. What is genuinely skippable when solo is exactly the network work: transaction
hashing, outbox packet building, ack tracking, authority round-trips. So:

- **Always:** full transaction capture — payloads, guard versions, ring entry. One code path, no
  solo/collaborative divergence bug surface.
- **Only when a session exists:** the outbox reader wakes up, `outboxState` starts moving, the
  ack frontier constrains eviction (§3), and the version guard becomes a live gate (§8).

This dovetails with the optimistic-commit decision (§2.2): the local mutation and the log entry
are synchronous and unconditional; upstream submission is asynchronous and additive. Connecting
to a server changes *what happens to the log afterwards*, never *how mutations are made*.

**The sequence-number rule.** While solo, the tab assigns `transactionSeq` locally and
monotonically — it is a correlation handle, nothing more. The authority is the only body that may
assign real sequence numbers (`storage.md` §10.1), delivered in the accept message alongside the
client's provisional handle. Any code that compares local sequence values across the connect
boundary is wrong. This is worth stating in the code, not just here. The same applies to
`META_DATA::dataVersion`: today it starts at 1 per process; when the load/connect layer arrives
it must be seeded from the persisted `object_version` so guards and server checks speak the same
numbers (deferred, §8).

## 8. What changes when peers connect

Walking the motivating example. The beam is at 3 m. I move it to 5 m; my client records
`{objectId, update, guardVersion: 8}` (the post-edit version) and publishes the transaction. A
peer moves it 5 m → 6 m; the authority commits their change as version 9 and publishes the delta,
which my client applies (without logging it — §2.9). I now press Ctrl+Z.

My change record says "restore the payload where length was 3 m, expecting to find version 8".
The live object is version 9. `9 != 8`, so the guard fires and **the undo is refused**. The user
sees something like *"Cannot undo: BEAM-104 was changed by another user."* The transaction is
marked non-undoable and the cursor skips past it; the rest of the history remains reachable.

This is the honest outcome. The two alternatives are both worse for engineering data: forcing
3 m silently destroys the peer's work, and applying an inverse delta (−2 m, giving 4 m) produces
a beam length that no engineer ever chose and nobody reviewed.

When the guard *passes* — the common case, where nobody else touched my object — the undo is
published to the authority as an ordinary forward transaction (a new update whose payload happens
to be an older state). Peers have no notion of my local cursor and must not need one. This means
an undo, once accepted, is itself a committed change in the shared history, which is the correct
semantics: the model moved, everyone sees it move, and the audit trail records who moved it and
when.

**Rejection rollback (the flip side of optimistic commit).** When the authority rejects my
transaction T — its base version no longer matches, because a peer's change was committed first —
my client must reconcile the screen with the truth. The rule: **roll back T and every later
pending-local transaction, in reverse order, using their before-payloads; remove them from the
ring; notify the user once** with the author and objects involved; then apply the authority's
published deltas and continue. Rolling back the whole optimistic suffix rather than just T needs
no dependency analysis and is always correct — a later local transaction may have built on T's
state. It is deliberately conservative; a refinement that rolls back only transactions whose
object sets transitively intersect T's is possible later without changing the log format,
because every transaction already carries its full object list. Rejections are rare (they require
two authors editing the *same objects* within one round-trip window), so the conservative rule
costs little in practice.

**Undo of a not-yet-acknowledged transaction is allowed.** The undo passes the local guard by
construction and publishes its inverse like any other; the authority sees two transactions that
cancel. If the original is still unsent, the outbox may cancel the pair locally as an
optimisation — deferred until the outbox exists.

**Deferred to the collaboration work, not solved here:** the mechanics of the 24-hour disconnect
grace period (`storage.md` §5); seeding `dataVersion` from persisted `object_version` on
load/connect (§7); and the outbox transport itself.

## 9. Hazards

**Object identity across redo.** Redo of a create must reuse the *original* `memoryID`, not
allocate a fresh one from `MemoryID::next()`. Any other object that referenced it — an
Asset2DInsert's members via `parentObjectId`, a 3D child's `memoryIDParent` — would otherwise
point at a corpse. Since the object is never freed from the arena, reusing its id is natural: the
undo path flips a flag rather than destroying and recreating.

**The 2D ingestion race.** New 2D records enter the vectors on the *copy thread* (§1.1). Any undo
path that flips `isDeleted` directly on the engineering thread can miss a record still in flight
on the queue — and the in-flight `Add`'s upsert would then resurrect it. This is why every 2D
delete/restore travels through the queue as `SoftDelete2D` / `Restore2D` (§5). Do not "optimise"
this into a direct flag write.

**Asset2D transactions are multi-object and order-sensitive.** `Cad2DCreateAssetFromSelection`
produces a definition, hidden master copies, an insert, and re-parented source records in one
gesture. Its undo must run in strict reverse order (§5), and its change list must include the
re-parent updates, not just the inserts, or undo will leave the sources pointing at a deleted
insert.

**Copy-thread ordering.** Undo pushes GPU commands onto the same queue as normal edits. They are
processed in order, so an undo cannot overtake the edit it reverses. But undo must push *the same
kind* of command the forward path would have — pushing `MODIFY` for an object the copy thread has
already REMOVEd is a no-op that leaves the screen stale. The `operationType` table in §5 is the
contract; deviating from it is how ghost geometry appears.

**The engineering thread is the sole writer, and must stay so.** The ring is lock-free precisely
because only one thread touches it. A future "undo from the network thread" or "undo from the
extension worker" would break this silently. Undo requests must always arrive as
`ACTION_TYPE::UNDO` on `todoCPUQueue` — including the future rejection-rollback path (§8), which
the network layer must post as a queue action, never execute itself.

**Modal gestures and undo must not interleave.** If an `UNDO` action arrives while a modal
gesture is armed or mid-flight (a transform awaiting its second click, a polyline under
construction), the gesture is cancelled first — the same path ESC takes, i.e. `TransactionAbort`
plus the existing cancel functions — and only then does the undo execute. Undoing underneath a
half-finished gesture would let the gesture commit against state that no longer exists.

**Ctrl+Z during text editing.** WndProc already withholds `KEYDOWN` from the tab while
`uiKeyboardCaptureCount != 0` (`Main.cpp:1087`), so shortcuts cannot fire from inside a text
field — the same trap the 'P' key (spawn pyramid) already hit and solved during the
properties-pane work. The hazard now is architectural: keep Ctrl+Z / Ctrl+Y / Delete on that
gated route (engineering-thread `KEYDOWN` mapping). Any second route that bypasses the gate
reintroduces the bug.

**The ribbon cannot grey buttons at runtime yet.** `AllUIControls[]` is `constexpr`; `isEnabled`
is compile-time (§1.7). Phase 2 must add a small runtime-enable check — the draw loop and the
click hit-test (`UserInterface.cpp:484`, `1299`) consult the active tab's `canUndo` / `canRedo`
atomics for these two controls. Without it, Undo/Redo render permanently active and clicking them
at history ends is a silent no-op at best.

**Data-tree ghosts.** The tree builder (`UserInterface.cpp:1557`) iterates `storageObjects3D`
with no `isDeleted` filter. Undo of a 3D create (or a Phase 4 delete) removes the object from
screen but would leave its node in the tree. The filter is one line; it must land with Phase 4.

**Eviction versus the outbox.** The eviction policy serves undo; the ack frontier (§3) serves the
outbox. They must be implemented as one function with two constraints, not two functions — a
refactor that "simplifies" eviction to pure slot-count reasoning will silently drop unsent
offline work the day the network layer arrives.

## 10. Implementation phases

Each phase is independently shippable and independently verifiable.

| Phase | Work | Verify |
| --- | --- | --- |
| 0 | Lift `Encode*` / `Decode*` out of `DataStorage.cpp`'s anonymous namespaces (lines 56, 1389) into a shared header/namespace. No behaviour change. | Save a tab to `.yyy`, reload, byte-identical round trip; build stays warning-free. |
| 1 | `TRANSACTION_RECORD` / `OBJECT_CHANGE_RECORD`, the ring on `DATASETTAB`, the four chokepoint functions, heap payload ownership, `dataVersion` on the nine 2D record structs, `EnqueueCad2D*` return the assigned id. No call sites yet. | Synthetic test: commit 200 transactions into a 128-slot ring; oldest evict correctly; `undoPayloadBytes` returns to zero after ring clear; no heap leaks under CRT debug heap. |
| 2 | Wire `ModifyObjectProperty`. Add `Commands::EDIT_UNDO` / `EDIT_REDO`, `ACTION_TYPE::UNDO` / `REDO`, ribbon buttons + the runtime-enable hook (§1.7), Ctrl+Z / Ctrl+Y mapping in the engineering thread's `KEYDOWN` switch. | Edit a sphere radius in the properties pane → Ctrl+Z visibly restores the old radius and the pane re-reads it; Ctrl+Y re-applies; buttons grey out at both ends of the history; Ctrl+Z inside a text field does nothing to the model. |
| 3 | 2D in-place transforms: Move and Rotate (N-update transactions, `dataVersion` bumps at the 2D sites). | Move a 5-object selection, Ctrl+Z returns all 5 to their original coordinates in one step; Ctrl+Y re-applies. |
| 4 | Delete + restore (§6): `EDIT_DELETE`, Delete-key mapping, 3D `REMOVE` push, `SoftDelete2D` / `Restore2D` commands, selection cleanup, data-tree `isDeleted` filter. | Delete a mixed 2D/3D selection → gone from screen and tree; Ctrl+Z → all back with identical ids; Ctrl+Y → gone again. |
| 5 | Create transactions: all `CREATE_*`, Copy/Offset/Mirror, asset insert, asset-from-selection. | Create a sphere → Ctrl+Z removes it → Ctrl+Y restores it **with the same memoryID**; asset-from-selection undoes without orphaning members. |
| 6 | Bulk import as one transaction (§6.5). | Import a DXF of N objects → Ctrl+Z clears the page in a single step → Ctrl+Y restores all N. |
| 7 | Activate the version guard as a live gate and its rejection UX; rejection rollback (§8). Deferred until the collaboration layer exists. | Two clients, the §8 beam scenario: the undo is refused naming the other author, and history beyond it stays reachable; a rejected transaction rolls back the optimistic suffix with one notification. |

## 11. Explicit non-goals

Listed so each is a conscious cut rather than an oversight: persistence of the undo log across
sessions (the `object_undo_log` table stays unimplemented); the network/outbox transport itself
(this iteration ships the log format, `outboxState`, and the eviction frontier rule, but no code
reads the log forward yet); undo trees or branching history; selective / out-of-order undo of an
arbitrary older transaction (the §8 rejection rollback is not selective undo — it is a
last-in-first-out rollback of the optimistic suffix, which preserves linearity); cross-tab undo;
delete of logical containers (§6.3); merging adjacent transactions (e.g. coalescing a drag into
one step — every gesture is already one step, but a sequence of small typed edits is not
coalesced); undo of view state, visibility, or selection; and automatic property-level merge of
conflicting concurrent edits, which `storage.md` §10.3 already advises against for engineering
objects.

## 12. Where the code changes

The complete file map for Phases 0–6, so an implementing session can navigate directly. "New"
marks files that do not exist yet.

| File | Phase | Change |
| --- | --- | --- |
| `code-core/DataStorage.cpp` | 0 | Move `Encode*` / `Decode*` out of the anonymous namespaces (lines 56, 1389); no behaviour change. |
| `code-core/DataStorage.h` (or a new `DataStorageCodec.h`) | 0 | Public declarations for the lifted codecs. |
| `code-core/TransactionLog.h` / `.cpp` — **new** | 1 | `OBJECT_CHANGE_RECORD`, `TRANSACTION_RECORD`, ring operations, the four chokepoint functions, the undo/redo executors, eviction + budget + redo-discard logic. |
| `code-core/ConstantsApplication.h` | 1 | `MV_MAX_UNDO_TRANSACTIONS`, `MV_UNDO_PAYLOAD_BUDGET`. |
| `code-core/विश्वकर्मा.h` | 1 | `DATASETTAB` gains the ring members, `canUndo` / `canRedo` atomics, and the open-transaction state; ring cleanup in the engineering-thread shutdown path. |
| `code-core/RenderPage2D.h` | 1 | `dataVersion` on the nine `Cad2D*RecordCPU` structs; `EnqueueCad2D*` signatures return `uint64_t`; declare `EnqueueCad2DSoftDelete` / `EnqueueCad2DRestore`. |
| `code-core/डेटा.h` | — | **No change.** `META_DATA::dataVersion` already exists (line 103). |
| `code-core/RenderScene3D-DirectX12.cpp` | — | **No change.** The `REMOVE` handler (line 787) is already correct; Phase 4 merely starts pushing to it. |
| `code-core/विश्वकर्मा.cpp` | 2,4,5,6 | Wire `ModifyObjectProperty` (711); `RegisterGeneratedGeometryElement` inserts (681); `CreateLogicalElement` (524); import loops (1144, 1244); Ctrl+Z / Ctrl+Y / Delete in the `KEYDOWN` switch (1930); `UNDO` / `REDO` / `DELETE_SELECTED` handlers in the `todoCPUQueue` loop (1986); publish `canUndo` / `canRedo` after every commit/undo/redo. |
| `code-core/UserInputProcessing.h` | 2,4 | `ACTION_TYPE::UNDO = 30031`, `REDO = 30032`, `DELETE_SELECTED = 30033` (next free values after 30030). |
| `code-core/ListOfCommands.h` | 2,4 | `Commands::EDIT_UNDO` / `EDIT_REDO` / `EDIT_DELETE`, random unused 10-digit ids. |
| `code-core/UserInterface.h` | 2,4 | `AllUIControls[]` entries for the three commands (`isEnabled = true`); the runtime-enable hook for Undo/Redo. |
| `code-core/UserInterface.cpp` | 2,4 | Ribbon draw + click hit-test honour `canUndo` / `canRedo` (484, 1299); data-tree builder skips `isDeleted` objects (1557). |
| `code-miscellaneous/UserInterfaceTranslation*.csv` + `code-core/UserInterfaceTranslationCompiled.h/.cpp` | 2,4 | New `UITextID`s: Undo, Redo, Delete, and the §8 refusal/rollback message strings; regenerate the compiled pair. |
| `code-core/SVGIconManifest.h` + icon assets | 2,4 | Undo / redo / delete icons. |
| `code-core/Main.cpp` | 2,4 | `ProcessPendingUIActions` dispatch for the three commands (365). |
| `code-core/RenderPage2D.cpp` | 3,4,5 | Transform, creation-commit, and asset paths call the chokepoint; 2D `dataVersion` bumps; implement the two new enqueue functions; selection-set cleanup on delete. |
| `code-core/RenderPage2D-DirectX12.h` | 4 | `CommandToCopyThread2DType::SoftDelete2D` / `Restore2D` and their id payload. |
| `code-core/RenderPage2D-DirectX12.cpp` | 4 | Ingest switch handles the two new commands (flip `isDeleted` in the record vectors); the rebuild filter (757) already does the rest. |
