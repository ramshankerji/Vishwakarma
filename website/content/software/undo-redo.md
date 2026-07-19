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
once and give it two readers.

## 0. Scope of this iteration

In scope: a per-tab, RAM-only transaction log; undo/redo of property edits, in-place 2D
transforms, object creation, object deletion, and bulk imports; the ribbon buttons and Ctrl+Z /
Ctrl+Y routing.

Out of scope, deliberately: persisting the log to the `.yyy` file. The
`object_undo_log` table specified in `storage.md` §14.10 is **not** implemented by
this work. The undo history is born when a tab opens and dies when it closes. Saving a file does
not clear it.

## 1. What the code looks like today

Three facts about the existing code drive every decision below.

**1.1 There are two mutation worlds, not one.** 3D engineering objects are `META_DATA`-derived
structs allocated in the `राम` arena under the tab's `memoryGroupNo`, registered in
`tab.storageObjects3D` / `tab.storageLogicalObjects`, mutated on the engineering thread under
`storageObjectsMutex`, and pushed to the GPU as `CommandToCopyThread{ADD|MODIFY|REMOVE}`. 2D CAD
objects are plain `Cad2D*RecordCPU` structs living in `std::vector`s inside `tab.cad2d`, guarded
by `cpuRecordsMutex`, and pushed to the GPU through the `EnqueueCad2D*` upsert functions. The
transaction log must span both without pretending they are the same thing.

**1.2 There is no delete operation anywhere in the application.** No `Commands::EDIT_DELETE`, no
`ACTION_TYPE` for it, and nothing in the codebase pushes `CommandToCopyThreadType::REMOVE` —
the handler at `RenderScene3D-DirectX12.cpp:787` is complete but unreachable. Since undo of a
create *is* a delete, the delete primitive is on the critical path and is planned here (§6) even
though it ships as a later phase.

**1.3 Every object type already has protobuf Encode/Decode.** `DataStorage.cpp` contains
`EncodeSphere` / `DecodeSphere`, `EncodeLine2D` / `DecodeLine2D` and so on for the full type set,
3D and 2D alike. They are locked inside an anonymous namespace and are therefore unreachable from
outside that translation unit. Lifting them out is Phase 0.

A fourth fact is a gift rather than an obstacle: **the arena never frees, and `isDeleted` is a
soft flag on both `META_DATA` and every `Cad2D*RecordCPU`.** The 2D page rebuild already skips
deleted records (`wanted()` in `RenderPage2D-DirectX12.cpp:757`) and the 3D copy thread's REMOVE
path already soft-deletes the GPU record and frees its matrix slot. This means:

> Undo of a create, and redo of a delete, need **no stored payload at all** — only a list of
> object ids and a flag to flip. Only *modify* transactions carry before/after bytes.

That single observation is what makes a 50,000-object DXF import cheap to undo.

## 2. Locked design decisions

1. **One chokepoint.** Every mutation to engineering data passes through
   `TransactionBegin` / `RecordObjectChange` / `TransactionCommit` on the engineering thread. No
   mutation site writes an object field without an open transaction. Undo reads the log backwards;
   the future collaboration outbox reads it forwards. (The *degree of validation* inside that
   chokepoint when no peer is connected is **OPEN** — see §7.)
2. **Version-guarded undo.** Each change record stores the object version its edit was based on.
   An undo that would write over a version it does not recognise is refused, and the user is told
   why. Undo never silently discards another author's work. This reuses the
   `current_object_version == base_object_version` rule from `storage.md` §10.3
   rather than inventing a second conflict model.
3. **Protobuf payloads.** Before/after state is captured as protobuf bytes using the existing
   `Encode*` / `Decode*` functions, lifted into a shared header. One byte format for the undo log,
   the `.yyy` file, and future network transport. Schema-versioned for free.
4. **RAM only, per tab.** The log lives in a fixed-slot ring buffer on `DATASETTAB`, with payload
   bytes allocated from the tab's own arena group so that `notifyTabClosed` reclaims everything.
   Nothing is written to disk.
5. **One user gesture, one transaction.** A ribbon click, a committed property edit, or a
   completed modal transform is exactly one undo step, however many objects it touched. A bulk
   import or export is likewise **one** transaction, undone as a single lot.
6. **View state is not undoable.** Camera position, Page2D pan/zoom, selection, sub-tab open/close,
   pane toggles, and visibility flags are not engineering data and never enter the log. This
   matches CAD convention: Ctrl+Z after a zoom must undo the last *edit*, not the zoom.
7. **Redo is cleared by new work.** Committing a new transaction while the redo stack is non-empty
   discards the redo stack. No undo trees, no branching history.

## 3. Data structures

Two records, mirroring `transaction_log` and `object_change_log` from
`storage.md` §14.7–14.8 so the eventual persisted form is a field-for-field subset.

```cpp
// One entry per changed object within a transaction.
struct OBJECT_CHANGE_RECORD {
    uint64_t objectId = 0;        // memoryID. Process-scoped, which is all a RAM-only log needs.
    uint8_t  operationType = 0;   // storage.md §14.8: 1=insert 2=update 3=move 4=soft_delete 8=restore
    uint8_t  world = 0;           // 0 = 3D META_DATA object, 1 = 2D Cad2D record. Selects the apply path.
    uint16_t objectType = 0;      // VishwakarmaStorage::ObjectType, so the decoder knows which Decode* to call.
    uint64_t baseVersion = 0;     // Object version this edit was based on. The undo guard reads this.

    // Payload offsets into the tab arena. Both zero for insert/soft_delete/restore - see §1.
    std::byte* beforePayload = nullptr;
    std::byte* afterPayload = nullptr;
    uint32_t   beforeSize = 0;
    uint32_t   afterSize = 0;
};

// One entry per user gesture.
struct TRANSACTION_RECORD {
    uint64_t transactionSeq = 0;  // Provisional while solo; the authority reassigns on connect. See §7.
    uint16_t transactionKind = 1; // storage.md §14.7: 1=user_edit 3=import 5=delete ...
    uint32_t commandId = 0;       // Commands:: enum value, so the UI can label the button "Undo Move".
    uint64_t commitTimeMs = 0;    // GetTickCount64(). Display and diagnostics only, never ordering.

    OBJECT_CHANGE_RECORD* changes = nullptr; // Arena-allocated array, tab memoryGroupNo.
    uint32_t changeCount = 0;
    uint32_t payloadBytes = 0;    // Sum of before+after sizes, for the eviction budget.
};
```

On `DATASETTAB`, a fixed-slot ring in the house style (no `std::vector` growth in the hot path):

```cpp
static constexpr uint32_t MV_MAX_UNDO_TRANSACTIONS = 128; // ConstantsApplication.h
TRANSACTION_RECORD undoRing[MV_MAX_UNDO_TRANSACTIONS];
uint32_t undoRingHead = 0;    // Next slot to write.
uint32_t undoRingCount = 0;   // Live entries, <= MV_MAX_UNDO_TRANSACTIONS.
uint32_t undoCursor = 0;      // Entries undone but not yet re-done, counted back from head.
uint64_t undoPayloadBytes = 0;
```

Two independent bounds, whichever binds first: `MV_MAX_UNDO_TRANSACTIONS` slots, and a payload
budget (proposed 256 MB per tab). Evicting the oldest transaction frees its payloads back to the
arena and permanently shortens the reachable history — which is correct behaviour, not an error.

**Ownership and threading.** The ring is written and read *only* by that tab's engineering thread.
No mutex is required for the ring itself. The UI thread never touches it; it posts
`ACTION_TYPE::UNDO` / `REDO` into `todoCPUQueue` like every other command, and reads a published
atomic pair (`canUndo` / `canRedo`) to grey out the ribbon buttons.

**2D records need a version field.** `META_DATA::dataVersion` already exists and already
increments on modify. The `Cad2D*RecordCPU` structs have `schemaVersion` (the *schema's* version)
but no object version. Phase 1 must add a `dataVersion` field to each 2D record struct — this is
the only change to an existing data structure that this work requires.

## 4. The chokepoint

```cpp
// All three run on the engineering thread that owns the tab.
void TransactionBegin(DATASETTAB& tab, Commands commandId, uint16_t transactionKind);
void RecordObjectChange(DATASETTAB& tab, uint64_t objectId, uint8_t operationType,
                        uint8_t world, uint16_t objectType);
void TransactionCommit(DATASETTAB& tab);   // Empty transactions are dropped, not committed.
void TransactionAbort(DATASETTAB& tab);    // Modal gesture cancelled by ESC.
```

`RecordObjectChange` is always called **before** the mutation, while the object still holds its
old state: it encodes `beforePayload` and captures `baseVersion` there and then.
`TransactionCommit` afterwards walks its own change list and encodes the after-payloads from the
now-mutated objects. Call sites therefore need one line before the edit and nothing after it,
and there is no way to accidentally record a "before" that is already modified.

Existing mutation sites and where they hook in:

| Site | File | Transaction shape |
| --- | --- | --- |
| `ModifyObjectProperty` | `विश्वकर्मा.cpp:711` | 1 update. The cleanest site; Phase 2 wires it first. |
| `ApplyTransform2DToSelection` (Move, Rotate) | `RenderPage2D.cpp:1065` | N updates. |
| `ApplyTransform2DToSelection` (Copy, Offset, Mirror) | `RenderPage2D.cpp:1065` | N inserts; sources untouched. |
| `RegisterGeneratedGeometryElement` | `विश्वकर्मा.cpp:681` | 1 insert per 3D primitive. |
| `CreateLogicalElement` | `विश्वकर्मा.cpp` | 1 insert (Folder / Page2D / Scene3D). |
| `Cad2DHandleInput` creation paths | `RenderPage2D.cpp` | 1 insert per completed shape. |
| `Cad2DInstantiateAsset` | `RenderPage2D.cpp` | 1 insert for the insert record + N for its member copies. |
| `Cad2DCreateAssetFromSelection` | `RenderPage2D.cpp` | Mixed: inserts (definition, insert, masters) + updates (re-parented sources). |
| `ImportStdFileIntoTab` / `ImportDxfFileIntoTab` | `विश्वकर्मा.cpp` | 1 transaction, N inserts. See §6.4. |
| `Cad2DAutoGenerateDemoContent` | `RenderPage2D.cpp` | **Excluded.** Debug scaffolding, not user data. |

The demo/random-geometry generator is deliberately left outside the log. It fires continuously on
a timer; letting it in would flood the ring and make Ctrl+Z useless during development.

## 5. Executing an undo

Undo pops the transaction at `undoRingHead - undoCursor - 1`, walks its changes **in reverse
order**, and applies the inverse of each:

| operationType | Undo does | Redo does |
| --- | --- | --- |
| insert (1) | `isDeleted = true`; 3D: push `REMOVE`. 2D: page rebuild. | `isDeleted = false`; re-push geometry. |
| update (2) | `Decode*(beforePayload)` into the live object; bump version; regenerate geometry; push `MODIFY`. | Same with `afterPayload`. |
| soft_delete (4) | `isDeleted = false`; re-push geometry. | `isDeleted = true`; push `REMOVE`. |

Reverse order matters within a transaction: `Cad2DCreateAssetFromSelection` creates a definition,
then an insert, then re-parents members. Undoing forward would orphan the members before their
parent is removed.

**The version guard.** Before applying an update, compare the live object's `dataVersion` against
the change record's `baseVersion`. In single-user linear undo these always match by construction —
undo pops the most recent transaction, so nothing can have changed underneath it. So locally the
guard is an assertion that catches bugs, not a gate that fires. It becomes a real gate the moment
a peer connects (§8), and costs one atomic read either way, which is why it is captured
unconditionally from day one rather than retrofitted.

**Geometry regeneration.** Undo of a 3D update must call `GeometryForObject` and push a `MODIFY`
exactly as `ModifyObjectProperty` does — decoding the payload only restores the CPU-side struct.
Undo of a 2D change must re-enqueue the record (the `EnqueueCad2D*` functions are upserts) or, for
pure delete/restore, enqueue a `SelectionRefresh` to force the page rebuild.

**Undo is itself a transaction, but not a logged one.** An undo does not push a new entry onto the
ring; it moves `undoCursor`. When peers are connected the undo *is* published as a new forward
transaction to them, because the peer has no concept of our local cursor — see §8.

## 6. Delete and restore (planned, phased)

Undo of a create requires a delete path, which does not exist. It is designed here and built in
Phase 4.

**6.1 The command.** A new `Commands::EDIT_DELETE` (random unused 10-digit id) plus
`ACTION_TYPE::DELETE_SELECTED`, wired through the eight-file ribbon recipe: `ListOfCommands.h`,
the translation CSV + compiler, `AllUIControls[]` (with `isEnabled = true`), an SVG icon +
`SVGIconManifest.h` entry, dispatch in `ProcessPendingUIActions`, and a handler in the
`todoCPUQueue` loop. Also bound to the Delete key in `WndProc`, suppressed while
`uiKeyboardCaptureCount != 0` so it does not fire during text-field editing.

**6.2 The mechanics.** 2D is nearly free: set `isDeleted = true` under `cpuRecordsMutex` and
enqueue a selection refresh; the page rebuild's `wanted()` filter already excludes deleted
records. 3D needs `isDeleted = true` on the `META_DATA` plus a `CommandToCopyThreadType::REMOVE`
push — the copy thread's handler is already written and correct, including freeing the matrix
slot and erasing from `objectLocation`, which is precisely what a later redo-of-create needs in
order to re-`ADD` under the same id.

**6.3 Consequence: soft-deleted objects accumulate.** Nothing is ever freed from the arena. A
long session of create-then-undo leaves dead objects occupying arena bytes and matrix slots. This
is an accepted cost of the design, bounded in practice by the ring eviction policy. The eventual
fix is the lifecycle compaction described in `storage.md` §9 (`tombstone_retained` →
`purged_stub`), which is a file-level operation and out of scope here. It should be recorded as a
known cost, not discovered later as a leak.

**6.4 Bulk import as one transaction.** The DXF and STD importers run in a separate worker process
(`VishwakarmaExtension.exe`) and stream records in asynchronously over several seconds. Holding a
transaction open across that is fragile — a worker crash would leave it open forever. Instead the
ingest path accumulates created ids into a pending-import list, and a transaction is *synthesised
at completion* from that list. The ingest hot path stays untouched, and a crashed import simply
produces no transaction. Since an import is insert-only, its undo record is `N × 8` bytes of ids
and no payloads: a 50,000-object DXF costs 400 KB and undoes with one page rebuild.

## 7. OPEN: how much protocol applies when nobody is connected

**This section is not decided.** It is recorded with its trade-offs so the debate can be had
against something concrete.

The question: when a tab has no connected peer, should mutations still run the full transaction
protocol, or take a reduced fast path?

**Option A — always full protocol.** One code path, no modes. Every mutation pays for payload
encoding, a ring slot, base-version capture, and whatever hashing/stamping the protocol
specifies. *For:* impossible to have a solo/collaborative divergence bug, because there is no
divergence. *Against:* a "change all 50,000 beams to ISMB300" operation pays 50,000 protobuf
before-encodes, and pays them even for a user who will never connect to anything.

**Option B — solo fast path.** When `peerCount == 0`, mutate directly: skip conflict validation,
author/device/session stamping, transaction hashing, and outbox queueing. Still record the undo
entry, because undo needs it regardless. *For:* the cheapest possible path for what will be the
overwhelmingly common case — one engineer, one laptop, no network. *Against:* the solo →
collaborative transition becomes a genuine correctness boundary. Entries logged in fast mode have
no authority-assigned sequence and no validated base version, so on connect one must choose
between clearing the undo stack, marking pre-connect entries non-propagatable (undoable locally,
never sent), or forcing a full-state upload. Every one of those is a user-visible behaviour that
has to be specified and tested.

**Option C — always log, defer only the network work.** Always capture the full transaction
including `baseVersion`; skip only the parts that exist purely to serve a peer (hashing, outbox,
ack tracking, authority round-trip). *For:* this is where the cost actually is. Capturing
`baseVersion` is a single atomic read of `dataVersion`, which the code already reads and bumps —
effectively free. Payload encoding is needed for undo whether or not a peer exists, so it is not
a collaboration cost at all. The expensive, genuinely skippable work is all network work. This
gets ~all of Option B's savings with ~all of Option A's uniformity, and the solo→collaborative
transition needs only sequence-number reassignment rather than a semantic rescue.

**Recommendation: C**, on the grounds that B's headline saving is largely illusory — measure
before assuming otherwise. The two costs people intuitively attribute to "the collaboration
protocol" (payload encoding, log slots) are undo costs that survive removing collaboration
entirely. What is left to skip is exactly the network work, which Option C already skips.

**The sequence-number wrinkle, whichever option wins.** While solo, the tab assigns
`transactionSeq` locally and monotonically. On connect, the authority is the only body that may
assign real sequence numbers. Local sequence values must therefore be understood as *provisional*
throughout, and any code that compares sequence numbers across the connect boundary is wrong.
This is worth stating in the code, not just here.

**Also open:** whether *export* deserves a transaction at all. Export does not mutate engineering
data, so under §2.6 it should not be undoable. It is grouped with import in the current
instruction; the distinction is worth resolving before Phase 6.

## 8. What changes when peers connect

Walking the motivating example. The beam is at 3 m. I move it to 5 m; my client records
`{objectId, update, baseVersion: 7}` and the object becomes version 8. A peer moves it 5 m → 6 m;
the authority commits their change as version 9 and publishes the delta, which my client applies.
I now press Ctrl+Z.

My change record says "restore the payload where length was 3 m, based on version 7". The live
object is version 9. `9 != 7`, so the guard fires and **the undo is refused**. The user sees
something like *"Cannot undo: BEAM-104 was changed by Ram Shanker."* The transaction is marked
non-undoable and the cursor skips past it; the rest of the history remains reachable.

This is the honest outcome. The two alternatives are both worse for engineering data: forcing
3 m silently destroys the peer's work, and applying an inverse delta (−2 m, giving 4 m) produces
a beam length that no engineer ever chose and nobody reviewed.

When the guard *passes* — the common case, where nobody else touched my object — the undo is
published to the authority as an ordinary forward transaction (a new update whose payload happens
to be an older state). Peers have no notion of my local cursor and must not need one. This means
an undo, once accepted, is itself a committed change in the shared history, which is the correct
semantics: the model moved, everyone sees it move, and the audit trail records who moved it and
when.

**Deferred to the collaboration work, not solved here:** what happens to in-flight local
transactions during a disconnect (storage.md §5 grants a 24-hour editing grace period), and
whether an undo may be attempted against a transaction that has not yet been acknowledged by the
authority. Both need the network layer to exist before they can be specified usefully.

## 9. Hazards

**Object identity across redo.** Redo of a create must reuse the *original* `memoryID`, not
allocate a fresh one from `MemoryID::next()`. Any other object that referenced it — an
Asset2DInsert's members via `parentObjectId`, a 3D child's `memoryIDParent` — would otherwise
point at a corpse. Since the object is never freed from the arena, reusing its id is natural: the
undo path flips a flag rather than destroying and recreating.

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
`ACTION_TYPE::UNDO` on `todoCPUQueue`.

**Ctrl+Z during text editing.** `SingleUIWindow::uiKeyboardCaptureCount` is non-zero while a
property-pane text field has focus. Undo must be suppressed there, or Ctrl+Z inside a half-typed
field will revert the last *model* edit instead of the last keystroke — the same trap the 'P'
key (spawn pyramid) already hit during the properties-pane work.

## 10. Implementation phases

Each phase is independently shippable and independently verifiable.

| Phase | Work | Verify |
| --- | --- | --- |
| 0 | Lift `Encode*` / `Decode*` out of `DataStorage.cpp`'s anonymous namespace into a shared header/namespace. No behaviour change. | Save a tab to `.yyy`, reload, byte-identical round trip; build stays warning-free. |
| 1 | `TRANSACTION_RECORD` / `OBJECT_CHANGE_RECORD`, the ring on `DATASETTAB`, the four chokepoint functions, `dataVersion` on the 2D record structs. No call sites yet. | Synthetic test: commit 200 transactions into a 128-slot ring; oldest evict correctly, `undoPayloadBytes` returns to zero after eviction. |
| 2 | Wire `ModifyObjectProperty`. Add `Commands::EDIT_UNDO` / `EDIT_REDO`, `ACTION_TYPE::UNDO` / `REDO`, ribbon buttons, Ctrl+Z / Ctrl+Y in `WndProc` (suppressed under keyboard capture). | Edit a sphere radius in the properties pane → Ctrl+Z visibly restores the old radius and the pane re-reads it; Ctrl+Y re-applies; buttons grey out at both ends of the history. |
| 3 | 2D in-place transforms: Move and Rotate. | Move a 5-object selection, Ctrl+Z returns all 5 to their original coordinates in one step. |
| 4 | Delete + restore (§6): `EDIT_DELETE`, 3D `REMOVE` push, 2D `isDeleted` + rebuild. | Delete a mixed 2D/3D selection → gone from screen; Ctrl+Z → all back with identical ids; Ctrl+Y → gone again. |
| 5 | Create transactions: all `CREATE_*`, Copy/Offset/Mirror, asset insert, asset-from-selection. | Create a sphere → Ctrl+Z removes it → Ctrl+Y restores it **with the same memoryID**; asset-from-selection undoes without orphaning members. |
| 6 | Bulk import as one synthesised transaction (§6.4). | Import a DXF of N objects → Ctrl+Z clears the page in a single step → Ctrl+Y restores all N. |
| 7 | Activate the version guard and its rejection UX. Deferred until the collaboration layer exists. | Two clients, the §8 beam scenario: the undo is refused with the peer's name, and history beyond it stays reachable. |

## 11. Explicit non-goals

Listed so each is a conscious cut rather than an oversight: persistence of the undo log across
sessions (the `object_undo_log` table stays unimplemented); undo trees or branching history;
selective / out-of-order undo of an arbitrary older transaction; cross-tab undo; merging adjacent
transactions (e.g. coalescing a drag into one step — every gesture is already one step, but a
sequence of small typed edits is not coalesced); undo of view state, visibility, or selection;
and automatic property-level merge of conflicting concurrent edits, which
`storage.md` §10.3 already advises against for engineering objects.
