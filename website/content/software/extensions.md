---
title: "Python Extensions and Automation"
weight: 100207
---

This page outlines the design document for the Python-based extension, automation, and scripting system in **Vishwakarma**. It is designed to guide developers and AI systems in implementing the python runtime integration.

---

## Implementation Status & MVP (First Working Slice)

**Finalized decisions:** frozen, statically linked CPython for external extensions; PocketPy strictly for internal scripting; Protobuf Lite on the IPC pipe (no JSON anywhere); Ed25519-signed store packages verified on every load.

The MVP exercises the full pipeline with the `.std` importer as the first real extension:

1.  **`VishwakarmaExtension.exe`** — plain process for now (**no AppContainer yet**), statically linked frozen CPython (implemented — see *Worker Executable* below), launched by the host when the `IMPORT_STD` command fires.
2.  **Anonymous pipe pair** (handle inheritance) carrying length-prefixed Protobuf Lite messages.
3.  **One message pair:** `ImportFileRequest` (raw `.std` file bytes, read and streamed by the host) → `CreateGeometryBatch` (nodes / members) back to the host.
4.  **Host side lives entirely in `ExtensionCommunications.cpp/.h`:** file-open dialog, spawn worker, stream bytes, validate every response (count caps, finite floats), translate into `ACTION_DETAILS`, push to the owning tab's `todoCPUQueue`.
5.  **Worker side:** `main.py` (IPC communicator built on `vishwakarma_api`) imports the refactored .std parser module (bytes-in entry point) and exchanges live Python objects with it in memory.

**Second extension (`Interoperability-DXF`):** the same pipeline imports AutoCAD `.dxf` drawings into the *currently open* Page2D container (the `IMPORT_DXF` ribbon command refuses when no Page2D sub-tab is active). The worker parses model space with the pure-Python DXF reader and streams `CreatePage2DBatch` messages: LINE and (LW)POLYLINE become Page2D lines (bulge arcs tessellated in the worker), CIRCLE becomes a high-segment-count polygon, TEXT/MTEXT become plain-content text rendered with the embedded MSDF font, and DIMENSION is decomposed into lines + measurement text. Blocks, OLE objects, paper-space layouts and layers are discarded by design.

**Implemented since the MVP:** the frozen statically linked CPython worker. `VishwakarmaExtension.exe` (dedicated project `code-core/VishwakarmaExtension.vcxproj`) embeds CPython 3.13 as a /MT static library with a curated frozen stdlib and the `google.protobuf` pure-Python runtime, so extensions run on machines with no Python installed and no pip packages. It is built from the pinned `code-external/cpython` submodule by `code-miscellaneous/BuildCPython.ps1` (CI caches the result like VishwakarmaExternal.lib), signed, embedded into the setup exe, and installed next to `Vishwakarma.exe`.

Immediate follow-ups, in order: AppContainer + Job Object + child-process ban; signed-manifest loading and verification; capability enforcement.

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

Both Python runtimes live in `code-external` and are recompiled only when external dependencies change, but they are vendored and built differently:

*   `code-external/cpython` — https://github.com/python/cpython — a **pinned git submodule** (currently the v3.13.14 release tag); statically linked (`/MT`) into `VishwakarmaExtension.exe` with the curated frozen stdlib set. CPython is a genuine multi-file build, so it stays a submodule (like `protobuf` and `openssl`). It is deliberately **not** part of `VishwakarmaExternal.lib`: the main application embeds PocketPy instead, so CPython is built by `code-miscellaneous/BuildCPython.ps1` into its own cached static library (`build/cpython-x64-release/python313.lib`) consumed only by the worker project.
*   `code-external/pocketpy` — https://github.com/pocketpy/pocketpy — vendored as its **committed single-file amalgamation** (`pocketpy.c` + `pocketpy.h`), not a submodule. PocketPy ships an `amalgamate.py` for exactly this embedding style (the same approach SQLite uses). Committing the amalgamation keeps CI fast and self-contained — no submodule clone, no Python amalgamation step in the build — and compiles the exact reviewed bytes. The amalgamated source is portable C: one file builds on Windows/macOS/Linux via compile-time `#ifdef`s. Provenance (upstream version + commit) and the regenerate-on-upgrade procedure are recorded in `code-external/pocketpy/README.md`. It is compiled with `PK_ENABLE_OS=0` (no filesystem/OS module) and `PK_ENABLE_THREADS=0` (single-threaded, runs on the engineering thread).

For CPython, pinning to an exact submodule commit gives reproducible builds and an auditable upgrade trail. For PocketPy, the committed amalgamation gives the same reproducibility, and upgrades are a deliberate regenerate-and-commit step rather than a submodule bump.

---

## 1. External Extensions (Out-of-Process CPython)

External extensions are authored by third-party developers, package creators, and interoperability engineers. These extensions run on a frozen, statically linked CPython but must be isolated from the host OS for security.

### Sandboxing & Process Architecture
*   **Worker Executable (Frozen CPython — implemented):** CPython 3.13 is statically linked into a separate, lightweight wrapper process (`VishwakarmaExtension.exe`, ~11 MB) distributed in the installation folder (same `/MT` static-build approach as OpenSSL). CPython already freezes its bootstrap modules into the binary; we extend that frozen set to a curated stdlib allowlist (`re`, `json`, `typing`, `collections`, `dataclasses`, `pathlib`, `argparse`, `datetime`, `threading`, full `encodings`, ...) plus the **`google.protobuf` pure-Python runtime** (frozen from the `code-external/protobuf` submodule, so its version always matches protoc and the host's libprotobuf-lite — extension developers get protobuf without pip). The worker is a single self-contained file — no `python3xx.dll`, no loose stdlib files on disk, no dependence on any Python installed on the machine. The frozen set doubles as the stdlib allowlist: a module that is not frozen in simply does not exist, so anything outside the curated set fails at `import` inside the worker.
*   **OS / networking removal (implemented):** networking cannot exist in the worker — `_socket`, `select` and `_ssl` are never compiled and `socket.py` / `ssl.py` are not frozen in. The builtin module table also drops `_winapi`, `mmap` and `msvcrt`; `winreg` is a raise-on-use stub (the import machinery merely requires it to exist on Windows). Process-creation primitives (`nt.system`, `spawn*`, `exec*`, `startfile`, `kill`, DLL-directory manipulation) are deleted by an extra `Py_mod_exec` slot spliced into the `nt` module definition, so even a fresh re-import of `nt` comes up scrubbed; `subprocess` / `multiprocessing` / `ctypes` are absent outright. File I/O intentionally remains (`_io` drives the IPC pipes and module loading) — confining *what* is reachable on disk is the AppContainer's job below.
*   **Sandbox Boundary (AppContainer):** AppContainer is a **per-process** mechanism: the host creates `VishwakarmaExtension.exe` with an AppContainer token (`CreateProcess` + `STARTUPINFOEX` / `SECURITY_CAPABILITIES`). A restricted token is NOT an acceptable substitute — it does not block network access. No capabilities are granted (specifically not `internetClient` or `privateNetworkClientServer`), so all network access is denied at the kernel level.
*   **Filesystem Reality:** An AppContainer process can still read paths ACL'd to `ALL APPLICATION PACKAGES` (most of the OS and the installation directory). This is required — the worker must load the Python runtime — and acceptable: the guarantee is *no access to user data*, not "no filesystem access". Future hardening option: LPAC (Less Privileged AppContainer).
*   **No Child Processes:** The worker is created with `PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY = PROCESS_CREATION_CHILD_PROCESS_RESTRICTED`, and the Job Object additionally enforces `JOB_OBJECT_LIMIT_ACTIVE_PROCESS = 1` with kill-on-job-close, so a compromised worker cannot spawn helper processes to escape the sandbox or its resource limits.
*   **Resource Limits (Job Object):** A Windows **Job Object** is used to enforce resource constraints, such as max memory consumption, CPU usage quotas, and execution timeouts.
*   **No Native Binary Modules initially:** To keep security static analysis and cross-machine packaging manageable, the marketplace will **initially support pure Python extensions only**. Native binary wheels (like NumPy/SciPy) are not supported.
*   **File Access for Importers/Exporters:** The main CAD application performs the file I/O (handling open/save dialogs) and streams the raw file bytes via IPC pipes to/from the sandboxed CPython worker. The extension parses/generates the file bytes in-memory and communicates geometry commands. This allows file parsing without giving the extension direct filesystem access.

### Host-Side IPC Command Handler (`ExtensionCommunications.cpp` / `ExtensionCommunications.h`)

All extension-related host code lives in this single translation unit — worker lifecycle, wire protocol, validation, and dispatch. No other file talks to workers. Once the OS sandbox is in place, **the IPC message stream is the real trust boundary**: every message arriving from a worker is treated as hostile input.

Responsibilities:
1.  **Worker Lifecycle:** Spawn `VishwakarmaExtension.exe` (AppContainer token, child-process ban, Job Object), create the anonymous pipe pair (handle inheritance — no named pipes, no name squatting), monitor worker health, and kill on timeout, user cancel, or shutdown.
2.  **Wire Protocol:** Length-prefixed Protobuf Lite messages. Hard parse limits (max message size, max recursion depth) are applied on every parse.
3.  **Message Validation** (before anything touches the model):
    *   Cap element counts, string lengths, and batch sizes per message and per session.
    *   Reject non-finite floats (NaN / Inf) in coordinates and numeric values.
    *   Validate all object IDs against the live model; reject references to objects the extension did not create or was not granted access to query.
    *   Unknown or malformed messages terminate the worker — they are never "best-effort" interpreted.
4.  **Capability Enforcement:** Every command is checked against the capabilities declared in the extension's signed manifest (e.g. an importer granted `document.create` cannot delete or query unrelated geometry). A command outside the declared capability set terminates the worker.
5.  **Dispatch:** Validated commands are translated into `ACTION_DETAILS` and pushed to the owning tab's Engineering-thread queue (`todoCPUQueue`). The Engineering thread remains the only writer of model data; extensions never get a second path in.

### Extension Package Layout & In-Worker Imports

Each extension is a plain Python package: an entry module (`main.py`) plus any number of sibling modules. Inside the worker, sibling modules are imported normally (e.g. `import std_reader`) and exchange live Python objects in memory — no serialization exists *inside* the worker. Serialization happens only at the IPC boundary, encapsulated by `vishwakarma_api`. For importers this means the parser module exposes a bytes-in entry point (e.g. `parse(data: bytes)`) with no direct file I/O of its own; the entry module feeds it the bytes streamed from the host. (Both bundled importers follow this shape: `extensions/Interoperability-STD/InteroperabilityWithSTDFile.py` and `extensions/Interoperability-DXF/InteroperabilityWithDXFFile.py` live as sibling modules of their `main.py` and expose bytes-in entry points.)

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

---

## 6. Extension Store Identity (Django Login-Server Integration)

**Status: PLANNED — nothing in this section is implemented.** It concretizes the
"server-side repository" of §5 on top of the account system designed in `login.md`, which is
itself not yet built. The Django server in `./server` today ships only telemetry
(`/api/logs`, `/api/stats`); implementation order is: `accounts` app first (per `login.md`
§17), then the `store` app below. Until then, the only extensions are the two bundled
importers, which ship inside the installer and need no store at all.

### 6.1 Identity Mapping: Publisher = Account

A publisher is not a new kind of identity — it is a **role claimed by an existing account**
from `login.md`: one immutable Ed25519 account key pair, with all its properties inherited
unchanged (no merger, disable-only credentials, append-only event chain).

*   **One account, at most one publisher role.** Claiming the role writes a `Publisher` row
    binding a **handle** to the account. The handle is the `<publisher>` half of the on-disk
    id `<publisher>.<name>` of §5 (*Storage on Disk*): lowercase `[a-z0-9-]`, 3–32 chars,
    chosen once, immutable forever.
*   **Handles are never recycled.** Not after account closure, not after publisher
    suspension. A freed handle is a supply-chain attack (register the abandoned name, ship a
    hostile "update") — the npm/PyPI name-resurrection lesson. The uniqueness constraint
    covers all rows regardless of status.
*   **No transfer.** Consistent with the no-merger rule: a publisher who loses their account
    loses the handle forever. The extensions can be re-reviewed and re-published under a new
    handle; installed copies keep working (revocation is an explicit security act, §6.5,
    never an automatic side effect of account loss).
*   The public identity shown in the store UI is the handle plus the account public-key
    prefix (`ram-shanker · Ab3…`), same convention as telemetry's `Installation.__str__`.

### 6.2 Server: New `store` Django App

Third app beside `api` (telemetry-only) and the planned `accounts`. Models in the concise
style of `api/models.py` / `login.md` §6:

```python
class Publisher(models.Model):
    account = models.OneToOneField("accounts.Account", on_delete=models.PROTECT)
    handle = models.CharField(max_length=32, unique=True)     # Immutable, never recycled.
    status = models.IntegerField(default=1)                   # 1 ACTIVE, 2 SUSPENDED.
    created_utc = models.DateTimeField(auto_now_add=True)

class Extension(models.Model):
    publisher = models.ForeignKey(Publisher, on_delete=models.PROTECT,
                                  related_name="extensions")
    name = models.CharField(max_length=32)                    # <publisher>.<name> is the id.
    display_name = models.CharField(max_length=64)
    summary = models.CharField(max_length=256, blank=True, default="")

    class Meta:
        constraints = [models.UniqueConstraint(fields=["publisher", "name"],
                                               name="unique_extension_per_publisher")]

class ExtensionVersion(models.Model):
    extension = models.ForeignKey(Extension, on_delete=models.PROTECT,
                                  related_name="versions")
    version = models.CharField(max_length=24)                 # Monotonic per extension.
    status = models.IntegerField(default=1)   # 1 SUBMITTED, 2 REJECTED, 3 PUBLISHED,
                                              # 4 YANKED, 5 REVOKED.
    capabilities = models.JSONField(default=list)             # Declared capability strings.
    package_sha256 = models.CharField(max_length=64)          # Zip as uploaded.
    manifest = models.JSONField()                             # Exactly what gets signed.
    manifest_signature = models.CharField(max_length=96)      # Store Ed25519 key; "" until signed.
    submitted_utc = models.DateTimeField(auto_now_add=True)
    published_utc = models.DateTimeField(null=True)

    class Meta:
        constraints = [models.UniqueConstraint(fields=["extension", "version"],
                                               name="unique_version_per_extension")]

class RevocationEntry(models.Model):
    """Append-only. The signed revocation list of §5 is generated from these rows."""
    extension = models.ForeignKey(Extension, on_delete=models.PROTECT)
    version_range = models.CharField(max_length=64)           # "*" or "min-max".
    reason_code = models.IntegerField()                       # Enum, no free text to clients.
    created_utc = models.DateTimeField(auto_now_add=True)
```

Package zips live as files in the writable state directory (`/var/lib/…`), keyed by
SHA-256, not as database blobs — the Pi's SQLite stays small. Pure-Python packages are
tiny; ingestion enforces a **hard 10 MB cap per package** and per-file/count caps mirroring
the host-side IPC philosophy: the server, like the host, treats every upload as hostile
input.

The **store signing key** is a dedicated Ed25519 key pair, sealed at rest with the same
environment KEK that seals account private keys (`login.md` §2), distinct from both the
release-manifest key and every account key. Its public key is embedded in the executable
(§5, *Signing & Load-Time Verification* — unchanged).

### 6.3 URL Map and Authentication

Same `/api/` prefix, routed to the `store` app. Two auth classes, both inherited from
`login.md` rather than invented here:

*   **Publisher endpoints** — require a login: a `WebSession` cookie (dashboard on the
    static site, `login.md` §8) or, later, the desktop delegation chain
    (`account key → installation key → session key → body`, `login.md` §7.4). The MVP
    accepts **web sessions only**; in-app publishing is deferred (§6.7).
*   **Consumer endpoints** — **anonymous, always.** Browsing, downloading, and revocation
    checks never require login and never carry the installation key: what a user installs
    is nobody's business, matching the telemetry stance of "no PII, ever". Anonymous GETs
    are also exactly what Cloudflare can cache for free in front of the Pi.

| Method | URL | Auth | Purpose |
|--------|-----|------|---------|
| GET  | `/api/store/index`                    | none | Catalog: ids, versions, manifest hashes (signed, Cloudflare-cached) |
| GET  | `/api/store/download/<id>/<version>`  | none | Package zip + detached signed manifest |
| GET  | `/api/store/revocations`              | none | Signed revocation list (§6.5) |
| POST | `/api/store/publisher/register`       | session + Turnstile | Claim handle, append `AccountEvent` |
| POST | `/api/store/submit`                   | session + Turnstile | Upload a version (zip + declared capabilities) |
| GET  | `/api/store/publisher/status`         | session | Own extensions, versions, review states |
| POST | `/api/store/yank`                     | session | Publisher withdraws a version (§6.5) |

Every publisher-side mutation appends to the account's signed `AccountEvent` chain
(`login.md` §6) with new event types — publisher registered, version submitted, version
yanked — so a publisher's publishing history is as tamper-evident as their credential
history. Rate limits extend the existing cache-based limiter: per-account submission caps
(e.g. 10/day) beside the per-IP caps of `login.md` §11.

### 6.4 Submission → Review → Signing Pipeline

1.  **Upload.** The dashboard posts the zip and the declared capability list. The server
    validates structure (package layout of §1, entry `main.py`, file caps), stores the zip
    by hash, writes `ExtensionVersion(status=SUBMITTED)`.
2.  **Static analysis** (§5) runs server-side: import scan against the frozen-stdlib
    allowlist, obfuscation heuristics, size anomalies. As §5 already states, this is a
    review *filter*, not a security control — the sandbox and host-side capability
    enforcement remain the actual controls.
3.  **Human approval.** Initially every publication is manually approved (volume will be
    tiny); the automated scan only pre-sorts. Rejection stores a reason visible via
    `publisher/status`.
4.  **Signing.** On approval the server assembles the canonical manifest — extension id,
    version, declared capabilities, entry module, SHA-256 of every file, **publisher handle
    and publisher account public key**, signing timestamp — signs it with the store key,
    and flips the version to PUBLISHED. The manifest format of §5 gains exactly those
    provenance fields; client-side verification (`EVP_DigestVerify` on every load) is
    unchanged.
5.  **Capability escalation gate.** A version whose capability set exceeds its
    predecessor's is flagged for mandatory human review, and the client surfaces the
    delta at update time ("this update adds `document.query`") before the new version is
    used — the Android-permission-escalation lesson, applied at the store *and* the host.

The publisher never signs anything: their account private key is server-side only by
design (`login.md` §2), so publisher provenance is asserted by the store signature over
the manifest's publisher fields plus the account event chain — one signer (the store key),
one verification path in the client.

### 6.5 Yank, Revoke, and Account-Lifecycle Interactions

Three distinct removal semantics, deliberately mirroring crates.io's yank-vs-revoke split:

*   **Yank (publisher-initiated):** stops new installs and update offers; already-installed
    copies keep loading. Not on the revocation list. For "this version has a bad bug".
*   **Revoke (store-initiated, security):** appended to `RevocationEntry`, served in the
    signed revocation list, and — per §5 — disables installed copies at next load even
    though their signature remains valid. For malware, compromised publisher, or a
    sandbox-relevant vulnerability.
*   **Account closure / publisher suspension:** freezes all publishing (no new versions,
    no new extensions) but revokes nothing automatically. Installed software keeps working
    unless a human decides it is actually dangerous. Automatic mass-revocation on account
    loss would let an account-takeover-then-close attack brick every user of a popular
    extension.

The revocation list is JSON, signed by the store key, and carries a **monotonic sequence
number**; the client persists the highest sequence seen and rejects any older list, so a
network attacker cannot replay a pre-revocation list to resurrect a revoked extension. It
is fetched alongside software-update checks (§5, unchanged) — anonymous, cacheable, cheap.

### 6.6 Trust Display in the Application

The in-app store browser and the installed-extensions list show, for every extension: the
publisher handle, the account public-key prefix, the declared capabilities in plain
language, and the review/signing date — all read from the verified manifest, never from
mutable server responses. Developer-mode extensions (§5) show none of this and keep their
persistent warning badge; the signed-store path is the only way to acquire a trust line.

### 6.7 Explicitly Out of Scope (This Iteration)

*   **Paid extensions / licensing** — needs the licensing design first (`login.md` §15
    names it as a future per-account feature); the store launches free-only.
*   **Ratings, comments, download counts** — social features are out (`login.md` §15);
    aggregate install counts, if ever wanted, would ride the existing anonymous telemetry.
*   **In-app publishing over the delegation chain** — designed for (§6.3) but deferred;
    the web dashboard is the only submission surface at launch.
*   **Organization / team publishers** — one account, one human, one handle. Multi-owner
    publishing would reintroduce exactly the shared-credential ambiguity the account
    design forbids.

### 6.8 OPEN Items

*   **Publisher vetting depth** — whether claiming a handle requires ≥2 active credentials
    on the account (recovery-hardened accounts only), or any account may publish.
*   **Manifest transparency log** — whether signed manifests should also be appended to a
    public hash chain (a miniature certificate-transparency analogue) so the store cannot
    sign secretly-targeted packages. Cheap to add later because manifests are already
    canonical signed records.
*   **Package storage growth** — the Pi's disk is finite; policy for pruning old REJECTED
    and superseded package blobs.

### 6.9 Implementation Order

1.  `store` app skeleton: models + migrations, package storage by hash → verify: unit
    tests for handle immutability, no-recycle constraint, version-state transitions.
2.  Signed index + download + revocation endpoints serving the two bundled importers as
    the first store-hosted packages → verify: client installs, verifies, and loads a
    store-downloaded package end-to-end; a revocation-list entry disables it at next load.
3.  Publisher registration + submission + review dashboard (requires `accounts` app
    steps 1–2 of `login.md` §17) → verify: full submit→approve→sign→install round trip by
    a second account; `AccountEvent` rows appear for every publisher mutation.
4.  Capability-escalation gate and update-time delta prompt → verify: an update adding a
    capability is held for review and prompts in-app before first use.
