---
title: "Python Extensions and Automation"
weight: 100207
---

This page outlines the design document for the Python-based extension, automation, and scripting system in **Vishwakarma**. It is designed to guide developers and AI systems in implementing the python runtime integration.

---

## Implementation Status & MVP (First Working Slice)

**Finalized decisions:** frozen, statically linked CPython for external extensions; PocketPy strictly for internal scripting; Protobuf Lite on the IPC pipe (no JSON anywhere); Ed25519-signed store packages verified on every load.

The MVP exercises the full pipeline with the `.std` importer as the first real extension:

1.  **`vk_worker.exe`** — plain process for now (**no AppContainer yet**), embedded CPython, launched by the host when the `IMPORT_STD` command fires.
2.  **Anonymous pipe pair** (handle inheritance) carrying length-prefixed Protobuf Lite messages.
3.  **One message pair:** `ImportFileRequest` (raw `.std` file bytes, read and streamed by the host) → `CreateGeometryBatch` (nodes / members) back to the host.
4.  **Host side lives entirely in `ExtensionCommunications.cpp/.h`:** file-open dialog, spawn worker, stream bytes, validate every response (count caps, finite floats), translate into `ACTION_DETAILS`, push to the owning tab's `todoCPUQueue`.
5.  **Worker side:** `main.py` (IPC communicator built on `vishwakarma_api`) imports the refactored .std parser module (bytes-in entry point) and exchanges live Python objects with it in memory.

**Second extension (`Interoperability-DXF`):** the same pipeline imports AutoCAD `.dxf` drawings into the *currently open* Page2D container (the `IMPORT_DXF` ribbon command refuses when no Page2D sub-tab is active). The worker parses model space with the pure-Python DXF reader and streams `CreatePage2DBatch` messages: LINE and (LW)POLYLINE become Page2D lines (bulge arcs tessellated in the worker), CIRCLE becomes a high-segment-count polygon, TEXT/MTEXT become plain-content text rendered with the embedded MSDF font, and DIMENSION is decomposed into lines + measurement text. Blocks, OLE objects, paper-space layouts and layers are discarded by design.

Immediate follow-ups after the MVP, in order: AppContainer + Job Object + child-process ban; signed-manifest loading and verification; capability enforcement.

---

## Architectural Layout: Python Everywhere

The scripting and automation model divides scripting into two isolated systems based on performance requirements, usage volumes, and security trust levels, both unified under Python:

1.  **External Extensions (Importers, Exporters, Large Plugins):** Out-of-Process Sandboxed CPython (statically linked, frozen stdlib — decision finalized).
2.  **Internal User Scripting (Parametric Templates, Custom Blocks):** In-Process Constrained Trusted PocketPy.

```text
+---------------------------------------+
|            Vishwakarma C++            |
|                                       |
|   +-------------------------------+   |
|   |        Data Storage           |   |
|   |    (C++ In-Memory Objects)    |   |
|   +---------------+---------------+   |
|                   ^                   |
|                   | Direct Bindings   |
|                   v                   |
|   +-------------------------------+   |
|   |      PocketPy (In-Process)    |   |
|   |   - No imports / file / eval  |   |
|   |   - Sanitized bindings        |   |
|   +-------------------------------+   |
|                   ^                   |
+-------------------|-------------------+
                    |
                    | IPC (Pipes / Protobuf)
                    v
+---------------------------------------+
|     Out-of-Process CPython Worker     |
|     (Windows AppContainer Sandbox)    |
|                                       |
|   - No Direct File / Network access   |
|   - Frozen Static CPython Executable  |
+---------------------------------------+
```

---

## Build Dependencies (Git Submodules)

Both Python runtimes live in `code-external` and are built statically by the **VishwakarmaExternal** static-library project (recompiled only when external dependencies change), but they are vendored differently:

*   `code-external/cpython` — https://github.com/python/cpython — a **pinned git submodule** at a 3.11+ release tag; statically linked (`/MT`) into `vk_worker.exe` with the curated frozen stdlib set. CPython is a genuine multi-file build, so it stays a submodule (like `protobuf` and `openssl`).
*   `code-external/pocketpy` — https://github.com/pocketpy/pocketpy — vendored as its **committed single-file amalgamation** (`pocketpy.c` + `pocketpy.h`), not a submodule. PocketPy ships an `amalgamate.py` for exactly this embedding style (the same approach SQLite uses). Committing the amalgamation keeps CI fast and self-contained — no submodule clone, no Python amalgamation step in the build — and compiles the exact reviewed bytes. The amalgamated source is portable C: one file builds on Windows/macOS/Linux via compile-time `#ifdef`s. Provenance (upstream version + commit) and the regenerate-on-upgrade procedure are recorded in `code-external/pocketpy/README.md`. It is compiled with `PK_ENABLE_OS=0` (no filesystem/OS module) and `PK_ENABLE_THREADS=0` (single-threaded, runs on the engineering thread).

For CPython, pinning to an exact submodule commit gives reproducible builds and an auditable upgrade trail. For PocketPy, the committed amalgamation gives the same reproducibility, and upgrades are a deliberate regenerate-and-commit step rather than a submodule bump.

---

## 1. External Extensions (Out-of-Process CPython)

External extensions are authored by third-party developers, package creators, and interoperability engineers. These extensions run on a frozen, statically linked CPython but must be isolated from the host OS for security.

### Sandboxing & Process Architecture
*   **Worker Executable (Frozen CPython — finalized):** CPython is statically linked into a separate, lightweight wrapper process (`vk_worker.exe`) distributed in the installation folder (same `/MT` static-build approach as OpenSSL). CPython 3.11+ already freezes its bootstrap modules into the binary; we extend that frozen set to a curated stdlib allowlist (`re`, `json`, `databases`, `typing`, `collections`, `math`, ...) so the worker is a single self-contained file — no `python3xx.dll`, no loose stdlib files on disk. The frozen set doubles as the stdlib allowlist: a module that is not frozen in simply does not exist, so anything outside the curated set fails at `import` inside the worker.
*   **Sandbox Boundary (AppContainer):** AppContainer is a **per-process** mechanism: the host creates `vk_worker.exe` with an AppContainer token (`CreateProcess` + `STARTUPINFOEX` / `SECURITY_CAPABILITIES`). A restricted token is NOT an acceptable substitute — it does not block network access. No capabilities are granted (specifically not `internetClient` or `privateNetworkClientServer`), so all network access is denied at the kernel level.
*   **Filesystem Reality:** An AppContainer process can still read paths ACL'd to `ALL APPLICATION PACKAGES` (most of the OS and the installation directory). This is required — the worker must load the Python runtime — and acceptable: the guarantee is *no access to user data*, not "no filesystem access". Future hardening option: LPAC (Less Privileged AppContainer).
*   **No Child Processes:** The worker is created with `PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY = PROCESS_CREATION_CHILD_PROCESS_RESTRICTED`, and the Job Object additionally enforces `JOB_OBJECT_LIMIT_ACTIVE_PROCESS = 1` with kill-on-job-close, so a compromised worker cannot spawn helper processes to escape the sandbox or its resource limits.
*   **Resource Limits (Job Object):** A Windows **Job Object** is used to enforce resource constraints, such as max memory consumption, CPU usage quotas, and execution timeouts.
*   **No Native Binary Modules initially:** To keep security static analysis and cross-machine packaging manageable, the marketplace will **initially support pure Python extensions only**. Native binary wheels (like NumPy/SciPy) are not supported.
*   **File Access for Importers/Exporters:** The main CAD application performs the file I/O (handling open/save dialogs) and streams the raw file bytes via IPC pipes to/from the sandboxed CPython worker. The extension parses/generates the file bytes in-memory and communicates geometry commands. This allows file parsing without giving the extension direct filesystem access.

### Host-Side IPC Command Handler (`ExtensionCommunications.cpp` / `ExtensionCommunications.h`)

All extension-related host code lives in this single translation unit — worker lifecycle, wire protocol, validation, and dispatch. No other file talks to workers. Once the OS sandbox is in place, **the IPC message stream is the real trust boundary**: every message arriving from a worker is treated as hostile input.

Responsibilities:
1.  **Worker Lifecycle:** Spawn `vk_worker.exe` (AppContainer token, child-process ban, Job Object), create the anonymous pipe pair (handle inheritance — no named pipes, no name squatting), monitor worker health, and kill on timeout, user cancel, or shutdown.
2.  **Wire Protocol:** Length-prefixed Protobuf Lite messages. Hard parse limits (max message size, max recursion depth) are applied on every parse.
3.  **Message Validation** (before anything touches the model):
    *   Cap element counts, string lengths, and batch sizes per message and per session.
    *   Reject non-finite floats (NaN / Inf) in coordinates and numeric values.
    *   Validate all object IDs against the live model; reject references to objects the extension did not create or was not granted access to query.
    *   Unknown or malformed messages terminate the worker — they are never "best-effort" interpreted.
4.  **Capability Enforcement:** Every command is checked against the capabilities declared in the extension's signed manifest (e.g. an importer granted `document.create` cannot delete or query unrelated geometry). A command outside the declared capability set terminates the worker.
5.  **Dispatch:** Validated commands are translated into `ACTION_DETAILS` and pushed to the owning tab's Engineering-thread queue (`todoCPUQueue`). The Engineering thread remains the only writer of model data; extensions never get a second path in.

### Extension Package Layout & In-Worker Imports

Each extension is a plain Python package: an entry module (`main.py`) plus any number of sibling modules. Inside the worker, sibling modules are imported normally (e.g. `import std_reader`) and exchange live Python objects in memory — no serialization exists *inside* the worker. Serialization happens only at the IPC boundary, encapsulated by `vishwakarma_api`. For importers this means the parser module exposes a bytes-in entry point (e.g. `parse(data: bytes)`) with no direct file I/O of its own; the entry module feeds it the bytes streamed from the host. (The existing `code-miscellaneous/InteroperabilityWithSTDFile.py` will be refactored to this shape — file I/O split from parsing — when this design is implemented.)

---

## 2. Internal User Scripting (PocketPy In-Process)

Users can define custom 3D parametric templates and CAD block definitions that execute at model load time. Because a single model may load 10,000+ parametric blocks, out-of-process IPC overhead is too slow. 

These scripts run in-process using **PocketPy** (a lightweight Python 3.x engine written in a header-only C++ format). Because it runs in the main process, this is a **Constrained Trusted Runtime**, not a security sandbox.

### Execution Safety Limits
To prevent user scripts from hanging, leaking memory, or crashing the host:
*   **Instruction Budget:** Expose a step hook in the VM to limit the maximum instructions executed per block call.
*   **Recursion Ban:** Recursion is not permitted in block scripts. Enforced cheaply at call time: a small VM frame cap (e.g. 16 frames — enough for nested helper functions) plus rejecting any call whose function is already on the call stack.
*   **Max Allocations:** Limit the memory allocation size permitted per VM instance.
*   **Max Primitives (Hard Cap 1024):** A script may generate at most **1024 engineering objects per invocation** — real parametric templates and blocks never approach this. Generation is transactional: objects accumulate in a scratch buffer and are committed only if the script completes within all budgets; exceeding any cap aborts and rolls back. Unlimited object generation is available only to external extensions over the IPC interface.
*   **Time Budget:** Implement a wall-time execution timeout.
*   **Cooperative Cancellation:** Provide flag checks in loops to abort execution if the user cancels or closes the active tab.

### Provenance Gating for Embedded Scripts

The budgets above stop resource abuse; they do not stop a memory-safety bug in the VM itself, where opening a hostile model file could become code execution. Model files that carry embedded scripts and originate from an untrusted source (Mark-of-the-Web on downloaded files) trigger a **one-time trust prompt** before any embedded script executes; the user's decision is remembered per file. This is the lesson Office (default macro blocking for internet-sourced files) and Professional CAD Software (`SECURELOAD` / `TRUSTEDPATHS` after the `acad.lsp` worms) each learned retroactively — Vishwakarma bakes it in from the start.

### Performance & Caching
*   **Compile-Once:** Parse and compile PocketPy scripts to bytecode (AST/IR) once at load time, rather than parsing string scripts repeatedly.
*   **Dirty-Instance Re-evaluation:** Store the generated geometry in cache. Only re-evaluate scripts for dirty instances when their input parameters or dependencies change.

### Capability Hardening Constraints
*   **No Imports:** All `import` and `from ... import` statements are disabled by clearing module search paths, omitting the importer, and overriding the `__import__` builtin to throw a `PermissionError`.
*   **No Dynamic Code Execution:** The `eval` and `exec` builtins are explicitly removed from `vm->builtins` to prevent executing obfuscated or dynamic strings.
*   **No Filesystem Access:** The `open` builtin is completely removed. No filesystem modules (like `os` or `pathlib`) are loaded.
*   **No Native Modules:** Unsafe native libraries are excluded. Only a sanitized math library is exposed.
*   **Sanitized C++ Bindings:** PocketPy directly binds to C++ structures (points, colors, transforms) using copy-by-value or strictly bound wrapper classes. No raw pointers or host-internal object metadata are accessible.
*   **Reflection Constraints:** Introspective builtins such as `globals()`, `locals()`, `getattr()`, `setattr()`, and `dir()` are stripped or restricted to prevent scripts from searching or modifying host-bound internals.

---

## 3. Serialization & IPC

*   **In-Process Scripting:** Uses **Zero Serialization**. PocketPy binds directly to C++ memory structures, allowing direct reads and writes with zero overhead.
*   **Out-of-Process Scripting (Under-the-Hood):** Uses a serialized IPC pipe. The initial implementation uses **Google Protocol Buffers (Protobuf Lite)**, and can be optimized to **FlatBuffers** or **Shared Memory** in the future if high-frequency geometry streams require it. Protobuf is also the application's persistence format (SQLite database / saved files), so schemas are declared statically and shared — no JSON anywhere on the IPC path. Messages arriving from a worker are untrusted input: hard size and recursion limits are applied at parse time (see `ExtensionCommunications.cpp`).

---

## 4. Stable Python API Abstraction Layer (`vishwakarma_api.v1`)

To insulate extension developers from the binary wire layout (whether Protobuf, FlatBuffers, or Shared Memory), they write extensions against a stable Python API wrapper.

*   **Decoupled Interface:** Extensions target the stable `vishwakarma_api.v1` interface and import standard namespaces. They do not directly import or manipulate raw Protobuf classes or FlatBuffer schema-compiled modules:
    ```python
    import vishwakarma as vk

    # Create geometry
    vk.create_line(x1=0.0, y1=0.0, x2=10.0, y2=10.0, color=0xFF0000FF)

    # Batch operations
    vk.batch([
        vk.Line2D(0, 0, 5, 5),
        vk.Text2D("Sample", 5, 5)
    ])

    # Query elements
    elements = vk.query(parent_id)
    ```
*   **Encapsulated Serialization:** The `vishwakarma` Python API module is responsible for translating these high-level calls into the active wire format (e.g. packing data into Protobuf / FlatBuffers and sending over pipes). This allows the application's underlying IPC channel to be fully optimized or changed without breaking third-party extension code.

---

## 5. Extension Distribution & Compatibility

*   **Rules for Schema Evolution:** Schema compatibility is not automatic. Both Protobuf and FlatBuffers are backward/forward compatible **only if strict rules are followed**:
    1. Never change the tag numbers or field IDs of existing fields.
    2. Never change the type of an existing field.
    3. Only add new fields as optional/with defaults, and never reuse deprecated tag numbers.
*   **Pure Python Submissions:** Developers submit pure Python source code to the extension library registry.
*   **Server-Side Ingestion:** The server-side repository performs **static security analysis** (verifying safety, checking for obfuscated imports) and **code signs** verified packages. Users can then download signed extensions directly in the CAD interface. Static analysis is a review *filter*, not a security control — Python is too dynamic for import-scanning to be reliable against a determined author. The sandbox and host-side capability enforcement are the actual controls; passing review never relaxes them.

### Storage on Disk

*   **Installed extensions (store downloads):** `%LOCALAPPDATA%\Vishwakarma\Extensions\<publisher>.<name>\<version>\` — one folder per extension, one subfolder per version. Per-user and writable without elevation (the installation directory under Program Files is admin-writable only). A user-writable location is safe because trust comes from the signature verified at every load, not from directory ACLs.
*   **Contents:** the Python package files, plus a `manifest` (extension id, version, declared capabilities, entry module, SHA-256 of every file) and `manifest.sig` (Ed25519 signature over the manifest bytes).

### Signing & Load-Time Verification

*   Packages are signed server-side with the store's **Ed25519** key. The corresponding public key is embedded in the main executable (same pattern as the software-update manifest). Verification uses the statically linked OpenSSL (`EVP_DigestVerify`).
*   **Verified on every load:** extensions are lazy-loaded on first invocation per session. At that moment the host verifies `manifest.sig`, then checks the SHA-256 of each package file against the manifest as the file is read. Any mismatch disables and quarantines the extension. Ed25519 verification plus hashing a pure-Python package costs microseconds to milliseconds — trust is never cached across sessions.
*   **Revocation:** the store serves a revocation list (extension id + version ranges), fetched alongside software-update checks. Revoked extensions are disabled even though their signature remains cryptographically valid.

### Developer Mode

*   During development, extensions live in an `extensions` folder beside the executable (portable/dev builds) or any user-configured folder — one subfolder per extension, same package layout as store extensions, no signature required.
*   Loading unsigned extensions requires an explicit **developer-mode opt-in** (per machine, off by default) and shows a persistent warning badge in the UI. Without opt-in, an auto-loading unsigned-code folder would be a drop-point for any malware able to write files as the user.
*   **Explicit load, explicit reload:** the Dev folder is never scanned or auto-loaded — the user explicitly loads each dev extension, and must explicitly reload it after every change (no file watching, no hot reload, not remembered across sessions). This friction is deliberate: it prevents "just install it as a developer extension" from becoming a bypass of the signed store for distributing extensions to end users.
*   Extensions are **never** auto-loaded from document/model directories — auto-loading code that travels alongside documents is exactly how the `acad.lsp` worms (e.g. ACAD/Medre.A, which mass-exfiltrated drawings) spread.
