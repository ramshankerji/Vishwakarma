---
name: yyy-roundtrip-harness
description: "The .yyy save/load round-trip test in validations/yyy_roundtrip - GREEN 5/5 baseline for id.md step 3, how to drive the app headlessly, and the model facts the fixtures had to learn"
metadata:
  node_type: memory
  type: project
---

**Built 2026-08-26 as Phase 0 of id.md step 3** (the 2D object-model migration; the sequencing is
id.md §7 after the 2026-08-26 renumber). Its risks section demands a
save/load oracle BEFORE that migration starts, because a defect there corrupts files silently.
Everything lives in `validations/yyy_roundtrip/` (tracked) and `SampleFiles/` (gitignored, added
to .gitignore this session). **Read its README.md first** - the design rationale is all there and
should not be re-derived here.

**How the app is driven headlessly - this is the reusable part.** A Debug-only hook at the bottom
of `FileInputThread` (`Input_UI_Network_File.h`) reads `VISHWAKARMA_ROUNDTRIP_IN` /
`VISHWAKARMA_ROUNDTRIP_OUT`, loads, waits for the 2D ingest to drain, saves, and writes the
outcome to `<out>.status`. It follows the existing `VISHWAKARMA_AUTO_IMPORT_DXF` precedent.
**A status FILE, not a console line, because Main.cpp calls AllocConsole and reopens stdout onto
CONOUT$ - a parent process cannot redirect and read the app's output.** (Launching from a Bash
console DOES capture it, because the app inherits that console; Start-Process does not.)

**Demo content has to be suppressed or it lands in the file under test.** `RoundTripModeRequested()`
now lives in `विश्वकर्मा.h` and is called from TWO places, and both are required: the flag default
lives in DATASETTAB, `CreateEngineeringTab` covers later tabs, and **the startup tab is spawned
directly from wWinMain (`std::thread t(विश्वकर्मा, 1)`), bypassing CreateEngineeringTab entirely** -
that one cost a full debug cycle to find. Note also that the CREATEPYRAMID handler in
विश्वकर्मा.cpp is an EMPTY STUB, so FileInputThread's 10-command bulk load creates nothing.

**Two storage-layer findings, both recorded in the harness README:**
1. `DecodeText2D` ends `return !text.text.empty();` and the loader treats false as fatal, so ONE
   empty-string Text2D row makes the WHOLE .yyy unopenable. `BuildRowsFromTab` takes the opposite
   view and SKIPS such rows, so the two halves disagree. Not reachable from the app's own save
   (the writer skips it), hence quarantined as fixture `90_known_defect_empty_text.yyy`, which
   RunRoundTrip.ps1 skips unless `-IncludeKnownDefects`. A one-line loader fix would close it.
2. Every `Decode*` sanitises non-positive presentation values (`line_weight`->0.25,
   `text_height_cu`->3.5, `width_mm`->841, `height_mm`->594), so a stored zero CANNOT round-trip.
   Encoded as the `SANITISED` table in check_roundtrip.py, reported as a note not a failure.

**Format facts worth not re-deriving:** object ids SURVIVE a round trip (`row.objectId =
persistedId`, and persistedId is assigned only when 0), which is what makes a by-id comparison
possible. Load reads only `lifecycle_state = 0` and save rewrites the table, so **saving a loaded
file PURGES tombstones**. `file_info` is not validated on load at all - a synthetic .yyy needs only
a valid `object_store`. Rows must be inserted parent-first (`PRAGMA foreign_keys = ON`).

**STATUS: GREEN, AND IT EARNED ITS KEEP.** Baseline `PASSED 5 / 5` on 2026-08-26, then re-run
unchanged after each of the three changes that followed - the line record onto META_DATA, the other
eight, and retiring Cad2DKindOf - `PASSED 5 / 5` every time with the same two expected notes. 234
mechanical renames went through the save/load path and the files came back equivalent field for
field, which reading the diff could not have established. The README records this.
 That is the line step 3 must keep
green; the numbers are in the harness README. The earlier start-up hang WAS the display topology:
with the second monitor back the app reports 2 monitors, reaches "Hello...." and runs fine. Nothing
in the harness was at fault ([[dev-machine-monitors]]).

**Two of the three red rounds were the HARNESS being wrong about the app's model, not defects.**
Worth knowing before trusting a red result during step 3:
1. **A Scene3D appeared from nowhere** in every fixture lacking one. `EnsureDefaultLogicalHierarchy`
   (विश्वकर्मा.cpp) has an `else if (!hasScene3D)` branch that adds one to any tab with logical
   objects but no Scene3D, and it RACED the load. Its guard is
   `autoGenerateRandomGeometry || storageFilePath.empty()` - so the driver's tab must be given a
   **storageFilePath**, which is exactly what CreateEngineeringTab does for File > Open. Fixed in
   the wWinMain round-trip block, which now sets both the flag and the path.
2. **Asset2DDefinition rows are TAB-LEVEL and carry NO parent** - `BuildRowsFromTab` hardcodes
   `row.parentId = 0; // Definitions are tab-level; no page owns them.` Their master members hang
   off the DEFINITION, not off a page. The fixture had parented one to a Page2D.

The runner closes the app with CloseMainWindow before force-killing; repeatedly terminating a
D3D12 process mid-frame is worth avoiding regardless of whether it caused the hang.

Related: [[object-model-unification]], [[yyy-file-inspection]], [[commandline-build]],
[[dev-machine-monitors]].
