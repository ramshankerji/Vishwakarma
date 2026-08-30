---
name: yyy-roundtrip
description: >-
  Use this skill when running, creating, or debugging .yyy save/load round-trip validation tests in Vishwakarma. Covers building the test harness, generating fixtures, expected transformations, and headless driver execution.
---

# `.yyy` Save/Load Round-Trip Validation Harness

The `.yyy` save/load test harness in `validations/yyy_roundtrip/` validates that SQLite schema and protobuf serialization/deserialization maintain data integrity across save/load cycles without silent data corruption.

---

## 1. Running the Test Suite

### Step 1: Build the Debug Binary
The harness requires a Debug x64 build at `build\Debug\Vishwakarma.exe`:
```bat
MSBuild code-core\Vishwakarma.vcxproj -p:Configuration=Debug -p:Platform=x64 -p:BuildProjectReferences=false -m
```

### Step 2: Generate Test Fixtures
```bat
python validations\yyy_roundtrip\make_fixtures.py
```
*Note:* Fixtures are output into gitignored `SampleFiles\`.

### Step 3: Run the Test Runner
```powershell
powershell -ExecutionPolicy Bypass -File validations\yyy_roundtrip\RunRoundTrip.ps1
```

### Useful CLI Flags for `RunRoundTrip.ps1`
- `-Only 02_asset_instance.yyy`: Run a single fixture.
- `-KeepOpen`: Keep the application running post-load to inspect the scene.
- `-IncludeKnownDefects`: Include quarantined `90_*` defect reproducers (e.g. `90_known_defect_empty_text.yyy`).

---

## 2. Architecture & How it Works

```text
make_fixtures.py  ->  SampleFiles\NN_name.yyy
                            |
                            v
       Vishwakarma.exe with VISHWAKARMA_ROUNDTRIP_IN / _OUT set
       (Debug-only hook at the bottom of FileInputThread: loads,
        waits for 2D ingest to drain, saves, writes "<out>.status")
                            |
                            v
     check_roundtrip.py compare BEFORE AFTER   ->  exit 0 / 1
```

- **Field Comparison**: `check_roundtrip.py` decodes protobuf fields and compares exact values (no IEEE-754 epsilon drift allowed).
- **ID Preservation**: Object IDs survive round trips (`persistedId` is reused when non-zero).

---

## 3. Expected Transformations (Valid Rules)

The oracle applies the following 2 rules; any other difference is flagged as a failure:

1. **Tombstone Purging**: The loader selects `lifecycle_state = 0` only and save rewrites live records. Soft-deleted objects are purged.
2. **Presentation Defaults**: Non-positive presentation values are sanitized to defaults on load:
   - `line_weight` → `0.25`
   - `text_height_cu` → `3.5`
   - `width_mm` → `841`
   - `height_mm` → `594`

---

## 4. Key Invariants & Gotchas

- **Status File Communication**: `Main.cpp` allocates a console with `CONOUT$`, preventing standard pipe redirection. The driver polls `<out>.status` for completion (`OK` or `FAILED`).
- **Suppressed Demo Content**: `RoundTripModeRequested()` (`विश्वकर्मा.h`) suppresses random geometry generation and sets `storageFilePath` so `EnsureDefaultLogicalHierarchy` does not race the load.
- **Foreign Key Ordering**: SQLite runs with `PRAGMA foreign_keys = ON`. New fixtures must emit parent containers before children.
- **Tab-Level Definitions**: `Asset2DDefinition` records are tab-level and must have `parentId = 0`.
