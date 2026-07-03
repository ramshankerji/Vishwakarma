---
title: "Python Extensions and Automation"
weight: 100207
---

This page outlines the design document for the Python-based extension, automation, and scripting system in **Vishwakarma**. It is designed to guide developers and AI systems in implementing the python runtime integration.

---

## Architectural Layout: Python Everywhere

The scripting and automation model divides scripting into two isolated systems based on performance requirements, usage volumes, and security trust levels, both unified under Python:

1.  **External Extensions (Importers, Exporters, Large Plugins):** Out-of-Process Sandboxed CPython.
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
|   - Decoupled CPython Executable      |
+---------------------------------------+
```

---

## 1. External Extensions (Out-of-Process CPython)

External extensions are authored by third-party developers, package creators, and interoperability engineers. These extensions run on standard CPython but must be isolated from the host OS for security.

### Sandboxing & Process Architecture
*   **Worker Executable:** CPython runs inside a separate, lightweight wrapper process (`vk_worker.exe`) distributed in the installation folder.
*   **Sandbox Boundary (AppContainer):** Security isolation is enforced using a Windows **AppContainer** or restricted token. This blocks all network interfaces and filesystem access at the OS kernel level.
*   **Resource Limits (Job Object):** A Windows **Job Object** is used to enforce resource constraints, such as max memory consumption, CPU usage quotas, and execution timeouts.
*   **No Native Binary Modules initially:** To keep security static analysis and cross-machine packaging manageable, the marketplace will **initially support pure Python extensions only**. Native binary wheels (like NumPy/SciPy) are not supported.
*   **File Access for Importers/Exporters:** The main CAD application performs the file I/O (handling open/save dialogs) and streams the raw file bytes via IPC pipes to/from the sandboxed CPython worker. The extension parses/generates the file bytes in-memory and communicates geometry commands. This allows file parsing without giving the extension direct filesystem access.

---

## 2. Internal User Scripting (PocketPy In-Process)

Users can define custom 3D parametric templates and CAD block definitions that execute at model load time. Because a single model may load 10,000+ parametric blocks, out-of-process IPC overhead is too slow. 

These scripts run in-process using **PocketPy** (a lightweight Python 3.x engine written in a header-only C++ format). Because it runs in the main process, this is a **Constrained Trusted Runtime**, not a security sandbox.

### Execution Safety Limits
To prevent user scripts from hanging, leaking memory, or crashing the host:
*   **Instruction Budget:** Expose a step hook in the VM to limit the maximum instructions executed per block call.
*   **Recursion Depth:** Cap recursion depth (e.g. max 50 frames) to prevent stack overflows.
*   **Max Allocations:** Limit the memory allocation size permitted per VM instance.
*   **Max Primitives:** Cap the maximum number of geometry elements a script can generate per invocation.
*   **Time Budget:** Implement a wall-time execution timeout.
*   **Cooperative Cancellation:** Provide flag checks in loops to abort execution if the user cancels or closes the active tab.

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
*   **Out-of-Process Scripting (Under-the-Hood):** Uses a serialized IPC pipe. The initial implementation uses **Google Protocol Buffers (Protobuf Lite)**, and can be optimized to **FlatBuffers** or **Shared Memory** in the future if high-frequency geometry streams require it.

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
*   **Server-Side Ingestion:** The server-side repository performs **static security analysis** (verifying safety, checking for obfuscated imports) and **code signs** verified packages. Users can then safely download signed extensions directly in the CAD interface.
