---
name: extension-system-mvp
description: "Extension system architecture — protobuf-over-pipes IPC, the two shipped importers, PocketPy in-process VM, and the deploy/bundle paths that must both be wired"
metadata:
  node_type: memory
  type: project
  originSessionId: 315dc281-d69d-4e9d-af56-ead4033e9dbf
  modified: 2026-07-20T19:28:10.258Z
---

Design finalized 2026-07-03 in `website/content/software/extensions.md` (MVP section at top).
Decisions: frozen statically linked CPython for external extensions (curated frozen stdlib =
allowlist, see [[cpython-static-worker]]); PocketPy in-process for internal scripting (recursion
banned, 1024-object cap, transactional commit); Protobuf Lite over anonymous pipes, never JSON; all
host-side code in `ExtensionCommunications.cpp/.h`; extensions live in `%LOCALAPPDATA%`,
Ed25519-verified on every lazy load (reuses [[release-signing-setup]]); dev extensions load
explicitly with explicit reload (deliberate friction).

**Shipped, both end-to-end:**
- `Interoperability-STD` (commit bd05dba, 2026-07-04). `Commands::IMPORT_STD` → host streams .std
  bytes → worker parses via `InteroperabilityWithSTDFile.read_std_bytes` → CreateGeometryBatch →
  host validates (size/count caps, finite coords, node-ref checks) → `ImportStdFileIntoTab` in
  विश्वकर्मा.cpp. Profile carry-over since 2026-07-17, see [[line-member-object]].
- `Interoperability-DXF` — pure-Python hardened reader (size/tag caps, control-char stripping,
  finite-float enforcement) → CreatePage2DBatch. Policy: LINE + (LW)POLYLINE→lines (bulge arcs
  tessellated 15°/seg), CIRCLE→64-gon, TEXT/MTEXT→plain text, DIMENSION→lines+text; ARC/ELLIPSE/
  SPLINE/HATCH skipped; 1 DXF unit = 1 CU. Imports strictly into the ACTIVE Page2D sub-tab
  (`Cad2DIsActivePage2D`), refused with a MessageBox otherwise — checked at both UI dispatch and
  materialization. Host caps: 4M lines / 500k texts / 500k polygons / 4096B per text / 3..512
  polygon segments. Blocks→assets: [[asset2d-system]].

Wire format: `code-core/ExtensionIPC.proto` (HostToWorker/WorkerToHost), added to
`GenerateDataStorageProtobuf.ps1`. `DeployExtensions.ps1` is the post-build deploy and reruns
`protoc --python_out` every time, so the deployed pb2 always matches the .proto.

**LESSON — two separate packaging paths.** `DeployExtensions.ps1` only deploys next to the *local*
build output. The setup exe is built from `VishwakarmaSetup.rc` + `SoftwareUpdate.cpp`
(InstallPayload/wWinMain) and embeds its own copies as RCDATA: 205 = Interoperability-STD, 206 =
Interoperability-DXF, 207 = the worker exe; `InstallBundledExtensions()` extracts via
`%SystemRoot%\System32\tar.exe` (staged to a temp file — zip needs seeking), non-fatal on failure.
Anything the app needs next to the exe at runtime must be wired into **both** paths. Missing this
shipped installed builds that failed with "Extension not found: ...\main.py".

**2D visibility fix (2026-07-07)**, still load-bearing: real DXF content sits 10^5–10^6 units from
origin. Three changes together — `main.py Converter.recenter()`; `Cad2DZoomToExtents` recentring the
view (it used to zoom only, from the current centre); and the 2D zoom floor lowered 0.02→0.0001 via
shared `kCad2DZoomMin/MaxPixelsPerCU` in MemoryManagerGPU2D.h, which replaced ~9 scattered hardcoded
clamps. Those clamps must all agree or stored zoom and rendered zoom diverge.

**PocketPy** vendored 2026-07-04 as its committed single-file amalgamation
(`code-external/pocketpy/pocketpy.c` + .h, v2.1.8, upstream a2f16e5f) — deliberately NOT a submodule,
Ram's call to save CI time; regenerate via upstream `amalgamate.py`. Built by
VishwakarmaExternal.vcxproj with `PK_ENABLE_OS=0` + `PK_ENABLE_THREADS=0` + per-file
`SDLCheck=false` (its intentional C4146 unsigned-negation tricks). Still pending: a minimal hardened
in-process VM smoke test (needs a C++ wrapper + `code-external\pocketpy` on the app include path).

**C++ gotcha:** a `\` at the end of a `//` comment line-continues and swallows the next line — this
silently vanished a struct member in ExtensionCommunications.cpp.

**Next, in order:** (1) AppContainer + Job Object active-process-limit=1 +
PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY on the worker; (2) signed-manifest loading + capability
enforcement. Also flagged to Ram: PSF + protobuf BSD license texts are not yet shipped with the
installer. Diagnostics: [[dxf-import-diagnostics]].

**Store identity design (2026-07-21, PLANNED ONLY):** extensions.md §6 designs the extension-store
↔ login-server integration on top of [[login-design]]: publisher = role on an account (one handle,
immutable, never recycled, no transfer), new Django `store` app (Publisher/Extension/
ExtensionVersion/RevocationEntry), dedicated store Ed25519 signing key sealed with the env KEK,
anonymous consumer endpoints (Cloudflare-cacheable), web-session-only publishing at MVP, yank vs
revoke vs account-closure-freezes-only semantics, monotonic-sequence revocation list, capability-
escalation review gate. Publishers never sign anything (account private key is server-side only).
