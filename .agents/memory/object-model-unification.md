---
name: object-model-unification
description: "2D/3D object-model unification: id.md is the spec, all nine 2D types are on META_DATA, arena residency is next — plus the migration technique and the decisions Ram settled in conversation"
metadata: 
  node_type: memory
  type: project
  originSessionId: f24e80ae-570b-4a96-900b-0d01c14f4018
  modified: 2026-08-26T08:02:15.991Z
---

**The spec is `website/content/software/id.md`. Read it before touching the object model.** Do not
re-derive its content into memory — this file holds only what the repo does not.

**Both design docs were compressed and RENUMBERED on 2026-08-26** (id.md 1273→585 lines,
2Drendering.md 719→347). Anything written before that date citing `id.md §8`/`§10`/`§11` is stale:
§11 was deleted outright (step 2's design and numbers live in 2Drendering.md, "Page2D memory paging
— as built"), and the sections after §4 shifted down by one. Current map: §1 question, §2 what
exists, §3 references/resolver, §4 why residency matters, §5 options, §6 settled decisions,
§7 sequencing, §8 risks, §9 incidental findings. 2Drendering.md now uses NAMED sections, not numbers.
Ram's rule for both: club what is accomplished, drop step-wise history, delete superseded parts.

## Status, end of 2026-08-26 (committed and pushed)

id.md §7 steps 0, 1, 2 shipped. **Step 3 is two migrations wearing one number and only the first is
done:** (A) IDENTITY — all nine `Cad2D*RecordCPU` types derive META_DATA, `Cad2DKindOf` retired in
favour of `dataType`; (B) RESIDENCY — records still live by value in `std::vector` on the heap, not
the arena. **NEXT SESSION: the memoryID mapping / resolver — id.md §3, which is entirely unbuilt.**

**The constraint that governs (B), and Ram's decision on it:** `Optional64::set` calls
`cpu.Allocate(size, cpu.MemoryGroupOf(this))`, and `राम::MemoryGroupOf` **returns 0 for any pointer
outside the chunk pool** — so an Optional64 on a vector-resident record would silently allocate into
group 0, the Application Tab, which never closes. **Decision: no 2D type declares an optional
property until 2D objects are in the arena AND a lookup mechanism exists.** Corollary:
`memoryGroupNo == tabNo` is load-bearing, so 2D must NOT get its own arena group — separate
accounting is worth a counter, not a partition. All in id.md §2.6.

**Idea to weigh before building (B):** arena-allocated FIXED BLOCKS of records rather than one
allocation per record. Keeps streaming contiguity, keeps MemoryGroupOf correct, and makes a 100k
DXF import ~100 allocations instead of 100,000 — the pattern id.md §8 still calls the arena's
untested risk.

## The migration technique that worked — reusable

1. **Build the oracle first** ([[yyy-roundtrip-harness]]), before touching anything.
2. **Pilot ONE type end to end**, then do the rest. The pilot is what reveals the shape.
3. **A phased migration needs an accessor bridge.** Generic code (lambdas taking `auto&`) can name
   neither spelling while types are half-migrated. Getters/setters overloaded on `const META_DATA&`
   versus each unmigrated type phase themselves: migrating a type = change the struct, delete its
   overloads. Delete the bridge when the last type lands.
4. **Let MSVC locate the concrete renames**, then apply them at the exact (line, column) it reports,
   right-to-left per line so earlier edits do not shift later columns. **Fix the GENERIC sites by
   hand FIRST** or the renamer rewrites them and silently breaks the unmigrated types — this bit
   twice during the pilot.
5. **The END of a phased migration is easier than the middle**: once all types want the same rename,
   over-renaming generic code stops being a hazard. Pilot = 56 renames needing constant nursing;
   the other eight = 234 renames, 2 rounds, unattended.
6. **Watch for semantics no compiler checks.** "0 means unassigned" stopped being expressible once
   META_DATA's constructor issued ids: every `== 0` sentinel went unreachable, and three lambdas
   that set the id to 0 for later assignment had to take one eagerly instead.

## Decisions Ram settled in conversation — closed, not up for re-litigation

- One object model, **no META_DATA2D**. References store `memoryID`, never a raw pointer.
  Directories per tab, tab 0 as the shared catalog. Reverse lookup out of scope.
- **KEEP the `RenderPage2D.*` file names.** Ram proposed renaming back to `MemoryManagerGPU2D.*`
  "for consistency with 3D" and withdrew it on the evidence: `MemoryManagerGPU*` is the shared GPU
  foundation, `RenderScene3D*` is the 3D renderer, `RenderPage2D*` is its 2D peer — the current
  names already ARE the consistent pair. He renamed away from MemoryManagerGPU2D himself in 22883d6.
- **PART 2 of `डेटा-सामान्य-2D.h` stays** (the dead 18-type schema): it carries layers, line types,
  indexed colour and draw order that the shipped records lack. Merging is per-field, with step 3.
- The 2D debug/stress keys are now `#ifdef _DEBUG` as a whole block; the `c` camera reset stays out.

## Still true and worth not re-discovering

- **No working memoryID→object index exists anywhere** — resolution is a linear scan, and only 2 of
  the "12 linear scan sites" are actual id lookups; the other ten are legitimate full traversals.
  `राम::id2MemoryMap`, `MemoryIDMap` and `ReferenceID` are three dead mechanisms for that same job.
  This is exactly what the next session builds.
- **`storageObjects3D` is sorted by memoryId** (append-only + monotonic ids), guarded by a `_DEBUG`
  `[3d][warn]` at the append sites. The `.yyy`-load site has still never executed.
- **A third append site exists**, `FlushGeneratedGeometryBatch`, with no sortedness check.
- **2D's render layer structurally depends on the 2D object model** because CommandToCopyThread2D
  carries records and the copy thread generates geometry; 3D's carries pre-baked vertices. That
  command is now **912 bytes** (was 736 before the records gained META_DATA).

## Driving the app by script — traps that all fail silently

- `ShowWindow(SW_MAXIMIZE)` does NOT maximise this window (Chrome-style frameless, handles its own
  sizing). `MoveWindow(h, 0, 0, 1920, 1040, TRUE)` works. `SW_RESTORE` actively un-maximises.
- `SetForegroundWindow` alone silently fails and the click lands in whatever app is foreground. Use
  the AttachThreadInput sandwich, re-assert before EVERY click, abort if the check fails.
- The console holds ~25 lines and heartbeats push diagnostics off within a minute — capture in the
  SAME PowerShell call as the action. Never `GetPixel` over a full screen; use LockBits.
- **The app hangs during GPU/UI init when only ONE monitor is present** (never reaches "Hello....").
  Cost a whole debugging session. See [[dev-machine-monitors]].

Related: [[yyy-roundtrip-harness]], [[properties-pane-plan]], [[undo-redo-design]],
[[snapping-ambient-grid]], [[yyy-file-inspection]], [[commandline-build]], [[user-working-style]].
