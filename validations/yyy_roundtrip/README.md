# `.yyy` save/load round-trip test

The oracle for [Internal IDs](https://mv.ramshanker.in/software/id) §8 **step 3** — moving the
nine `Cad2D*RecordCPU` types onto `META_DATA` and, later, into the arena.

§9 of that page names the risk plainly: step 3 runs through the save/load path, "where a defect
corrupts user files silently rather than crashing". A drawing that still *looks* right after a
reload is not evidence. This is.

**Step 3's identity half has since landed and this suite stayed green through it** — see the
baseline below. The residency half has not, so keep running it: before a change, and after every
step of one.

## Running it

```bat
python validations\yyy_roundtrip\make_fixtures.py
powershell -ExecutionPolicy Bypass -File validations\yyy_roundtrip\RunRoundTrip.ps1
```

Needs a Debug x64 build at `build\Debug\Vishwakarma.exe`:

```bat
MSBuild code-core\Vishwakarma.vcxproj -p:Configuration=Debug -p:Platform=x64 -p:BuildProjectReferences=false -m
```

Useful flags: `-Only 02_asset_instance.yyy` for one fixture, `-KeepOpen` to leave the application
running so you can look at what it loaded, `-IncludeKnownDefects` to also run the quarantined
`90_*` reproducers.

Everything lives in `SampleFiles\`, which is gitignored — fixtures are regenerated, never
committed. Outputs land in `SampleFiles\roundtrip-out\`.

## How it works

```text
make_fixtures.py  ->  SampleFiles\NN_name.yyy
                            |
                            v
       Vishwakarma.exe with VISHWAKARMA_ROUNDTRIP_IN / _OUT set
       (Debug-only hook at the bottom of FileInputThread: load, wait
        for the 2D ingest to drain, save, write "<out>.status")
                            |
                            v
     check_roundtrip.py compare BEFORE AFTER   ->  exit 0 / 1
```

Three pieces:

| File | What it is |
|---|---|
| `yyy.py` | Reads and writes `.yyy` — the SQLite schema plus a small protobuf codec. |
| `make_fixtures.py` | Builds the fixtures. Each one targets a field step 3 touches. |
| `check_roundtrip.py` | `compare` (the oracle) and `dump` (normalised, for eyeballing). |
| `RunRoundTrip.ps1` | Drives the application over every fixture and reports pass/fail. |

**Comparison is by decoded field, never by blob bytes.** Two encoders may order or elide protobuf
fields differently and still mean the same thing. Numbers compare exactly, with no epsilon —
doubles and floats both cross protobuf as raw IEEE-754, so any drift at all is a defect rather
than rounding.

**Object ids survive a round trip**, which is what makes a strong comparison possible: a save
reuses `persistedId` and allocates a fresh one only when it is 0.

### Why a hand-rolled protobuf codec

The `.proto` files are compiled `LITE_RUNTIME` for C++ only. Generating Python bindings would put
a `protoc` dependency and a build step between this harness and the thing it is testing. The wire
format needed here is varints, fixed32, fixed64 and length-delimited bytes.

One trap, since it bites everyone once: **the key is a varint, not a byte.** Field numbers above
15 need two key bytes, and a decoder that assumes one byte misreads every field after them without
erroring.

### Why the application writes a status file

`Main.cpp` calls `AllocConsole()` and reopens `stdout` onto `CONOUT$`, so a parent process cannot
redirect and read the application's output. The hook therefore writes its outcome — `OK`, or
`FAILED:` and the storage layer's own message — to `<out>.status`, which the runner polls.

### Why demo content is suppressed

The startup tab generates ten random 3D objects and a 2D line per second. Those land in the tab
being round-tripped and get written into the output file, where the comparison correctly reports
objects that appeared from nowhere. `RoundTripModeRequested()` (`विश्वकर्मा.h`) turns it off in
**two** call sites, and it needs both: `CreateEngineeringTab` covers tabs opened later, while the
startup tab is spawned directly from `wWinMain` and keeps `DATASETTAB`'s defaults until corrected
there. The `wWinMain` site also sets `storageFilePath`, which is what stops
`EnsureDefaultLogicalHierarchy` adding a Scene3D to the tab under test — see the baseline notes
below. Always false in Release.

## Expected transformations

Three, applied as stated rules rather than per-file exceptions. Anything else is a failure.

1. **Tombstones are purged.** The loader selects `lifecycle_state = 0` only and a save rewrites
   the table from live objects, so a soft-deleted row legitimately disappears. Note, not failure.
2. **Non-positive presentation values become the loader's defaults** — `line_weight` → 0.25,
   `text_height_cu` → 3.5, `width_mm` → 841, `height_mm` → 594. Every `Decode*` in
   `DataStorage.cpp` reads these as `value > 0 ? value : <default>`, so a stored zero cannot
   survive a load by construction. Note, not failure — but only when the value lands on exactly
   the documented default; a zero coming back as anything else is still a failure.
3. **Nothing else.**

If a fourth becomes legitimate, add it as a rule with its reason. Never silence a diff.

## Findings

Things this harness found that were not previously recorded.

### An empty `Text2D` string makes a whole file unopenable

`DecodeText2D` (`DataStorage.cpp`) ends `return !text.text.empty();`, and `LoadYyyIntoTab` treats a
false return as fatal — so **one** `Text2D` row carrying an empty string aborts the load of the
entire project file, with "Could not decode Text2D protobuf payload."

The save path takes the opposite view about the same degenerate record: `BuildRowsFromTab` has
`if (text.objectId == 0 || text.text.empty()) continue;` — it skips it. So the two halves of the
storage layer disagree.

**Not reachable from this application's own save today**, precisely because the writer skips such
rows. That is why the reproducer is quarantined as `90_known_defect_empty_text.yyy` rather than
left failing in fixture 04. It is still worth keeping for two reasons: the response is
disproportionate (one degenerate row costs the entire file rather than one object), and a
third-party writer or a future import path could produce one. Making the loader skip the row, the
way the writer already does, would be a one-line change.

Reachable in memory, incidentally, even though it never reaches disk: `HandleTextCreationChar`
enqueues the draft on backspace with `shouldEnqueue = storage.textCreationObjectId != 0`, so
typing one character into a 2D text object and deleting it leaves a live record with an empty
string.

## Baseline — 2026-08-26, PASSED 5 / 5

```text
01_every_type.yyy      OK -  8 objects round-tripped with identical fields.
02_asset_instance.yyy  OK -  8 objects round-tripped with identical fields.
03_two_pages.yyy       OK -  7 objects round-tripped with identical fields.
04_edge_values.yyy     OK - 10 objects round-tripped with identical fields.
                       note: #9 soft-deleted row absent (rule 1)
                       note: #2 line_weight 0.0 -> 0.25 (rule 2)
05_mixed_2d_3d.yyy     OK -  6 objects round-tripped with identical fields.
```

Debug x64, commit following "Page2D Documentation / Progress updates". **This is the line step 3
has to keep green.** The quarantined reproducer was also run and still fails as designed
(`FAILED to load: Could not decode Text2D protobuf payload.`) — worth re-checking occasionally,
because an oracle that cannot go red is worth nothing.

**And it has kept it green.** Re-run unchanged after each of the three changes that followed —
the line record onto `META_DATA`, the other eight onto `META_DATA`, and retiring `Cad2DKindOf` in
favour of `dataType` — `PASSED 5 / 5` every time, with the same two expected notes and nothing
else. That is the harness doing the only job it has: 234 mechanical renames went through the
save/load path and the files came back byte-for-byte equivalent, which is not something reading
the diff could have told anyone.

### What it took to get there, recorded so it is not re-diagnosed

Three rounds of red, and only one of them was an application defect:

1. **Startup demo geometry.** Ten random 3D objects landed in the tab under test and were saved
   into the output. Fixed by `RoundTripModeRequested()`; see "Why demo content is suppressed".
2. **A Scene3D appearing from nowhere** in every fixture that did not already contain one. The
   engineering thread's start-up block calls `EnsureDefaultLogicalHierarchy`, whose
   `else if (!hasScene3D)` branch adds one to any tab holding logical objects but no Scene3D —
   and it was racing the load. The condition guarding that block is
   `autoGenerateRandomGeometry || storageFilePath.empty()`, so the fix is to give the driver's
   tab a `storageFilePath`, which is exactly what `CreateEngineeringTab` does for File > Open.
   **The application was right and the driver was unfaithful to it.**
3. **`Asset2DDefinition` losing its parent.** Also the harness's fault, and a useful thing to have
   learned early: definitions are **tab-level**, and `BuildRowsFromTab` writes
   `row.parentId = 0; // Definitions are tab-level; no page owns them.` The fixture had parented
   one to a Page2D. Their master members hang off the *definition*, not off any page.

Points 2 and 3 are the reason to build this before the migration rather than during it: both
looked like defects and both were the test being wrong about the application's own model. Sorting
that out now means a red result during step 3 can be trusted.

## Adding a fixture

Add a builder to `make_fixtures.py` and list it in `FIXTURES`. Row order matters: the application
sets `PRAGMA foreign_keys = ON`, so a child inserted before its parent is rejected — emit
containers first. `90_*` names are quarantined known-defect reproducers and are skipped by default.

Keep fixtures small. This harness answers "is it correct", not "is it fast" — the performance
question has its own numbers in id.md §11.7 and §11.8.
