---
title: "Model Server (VishwakarmaServer)"
weight: 100114
---
0. Purpose and scope

VishwakarmaServer.exe is the 5th project of the solution, next to Vishwakarma (desktop client), VishwakarmaExternal (static library of external dependencies), VishwakarmaSetup (installer) and VishwakarmaExtension (CPython worker).

It is the server-hosting counterpart of `.zzz` files. Per the storage design document (see *Storage of Data*), a `.zzz` file is administrative / project truth and **is opened only by the server**. The desktop client never opens a `.zzz` directly. VishwakarmaServer opens exactly one root `.zzz` project (which may mount child `.zzz` projects and many `.yyy` files) and becomes the single persistent authority for everything under it.

Division of responsibility, restated from the storage document:

| | .yyy | .zzz |
|---|---|---|
| Truth | Engineering truth | Administrative / project truth |
| Contents | Objects, hierarchy, payloads, versions, tombstones, sync metadata | Project tree, mounts, principals, permissions, policy, project audit |
| Opened by | Desktop client (local mode) or server (mounted under .zzz) | **Server only** |
| Permissions | None inside (single access level) | All real authorization lives here |
| Graphics needed | For viewing/editing on client | **Never** — the server requires no rendering capability |

VishwakarmaServer therefore does three things, and only three things:

1. Opens/maintains the `.zzz` project database and the `.yyy` files it mounts.
2. Serves connected Vishwakarma.exe clients over a binary, compressed TCP protocol: hierarchy, object payloads, subscriptions, transaction commits, published deltas.
3. Serves an embedded web-based **admin module** over HTTP(S) for project administration.

Primary deployment target is servers (Windows Server now, Linux server later), but it must run equally well on an ordinary desktop/laptop for small teams.

1. "Tab 0 capability only"

Tab 0 is the un-closable **Application Tab** defined in the *Tab System and the Application Tab* chapter: no file, no engineering thread, everything owned and serviced by one thread, hosting the 8 fixed application views (Launcher, Profile, Settings, Support, Peer Chat, Documentation, Common Geometry, Stats). VishwakarmaServer is specified as a **tab-0-only application** — the application reduced to exactly that layer:

* No engineering tabs, ever. Tab 1..N never exist; no engineering threads are launched; nothing ever consumes an engineering queue.
* No Scene3D, no Page2D, no GPU, no DirectX/Vulkan dependency, no window, no ribbon, no tab band. The server builds and runs on machines with no graphics stack at all (headless Windows Server Core, Linux VMs).
* What it keeps is the Application-Tab substance: startup/shutdown, application-level settings, account/identity (Ed25519 AccountManager), logging/telemetry/stats, the software-update client, and the data-storage layer (SQLite + Protobuf).
* The server does not render the Application Tab natively — **the admin web module is the server's tab 0**. There is deliberately no native local UI; an administrator on the server machine uses `https://localhost` like any remote administrator. This keeps one configuration code path and keeps the binary GUI-free for Linux. "Configurable by tab 0" (e.g. the admin port itself) therefore means: configurable in the admin module's Settings page, the server-side equivalent of the Application Tab's Settings view.

The correspondence between the desktop Application-Tab views and the server admin module, for orientation (not a 1:1 requirement):

| Desktop tab-0 view | Server equivalent |
|---|---|
| Launcher | Dashboard: the open `.zzz` project and its mounted files |
| Profile | Server identity: Ed25519 server key, TLS certificate |
| Settings | Settings page: ports, proxy mode, paths, retention |
| Stats | Dashboard metrics + Sessions page |
| Support / Peer Chat / Documentation / Common Geometry | Not applicable in v1 (docs are a link to this website) |

On the client side the connection lives in tab 0 too: the desktop **Launcher** view lists recent local files *and online servers* — a VishwakarmaServer entry there is what opens a server-hosted engineering tab.

Consequence for code layout: any code that VishwakarmaServer consumes must live below the graphics boundary. The sync-protocol module, storage module and account module are platform-agnostic files compiled into **both** Vishwakarma.exe and VishwakarmaServer.exe — the same single-source-two-binaries pattern already used by `SoftwareUpdate.cpp` (compiled into both the app and the setup exe). The desktop client needs the same protocol code anyway for its local `.yyy` virtual-server mode (storage doc §3.2), so the protocol implementation is written once, hosted twice:

| Binary | Role of the shared sync module |
|---|---|
| VishwakarmaServer.exe | Authority for server-hosted `.zzz` mode |
| Vishwakarma.exe | Client side; also authority when acting as local `.yyy` virtual server |

2. Process model

* Single process, single instance per machine (named mutex `Global\MissionVishwakarmaServerLock`, following the release-management mutex conventions).
* Fully self-contained, statically linked (`/MT`) executable, exactly like Vishwakarma.exe. Depends on VishwakarmaExternal.lib; the linker drops the graphics-only objects. External dependencies actually used: sqlite, protobuf, zlib, openssl, fast_float. Explicitly not used: DirectX-Headers, freetype, libpng, lunasvg, msdf-atlas-gen, cpython, pocketpy.
* Two run modes:
  * **Console mode** — launched from a terminal, logs to stdout + log file. Default when not installed as a service. Used for local hosting and development.
  * **Service mode** — Windows Service `VishwakarmaServer` (systemd unit on Linux later), auto-start, running as the per-service virtual account `NT SERVICE\VishwakarmaServer`. Activation (§8) grants that SID access to the server data directory.
* Threading, phase 1: one accept thread per listener, one blocking I/O thread per client connection, **one commit thread** that owns all SQLite writes (server is the single writer; SQLite WAL mode). Sub-tab/OS-event machinery from the desktop app is not used. IOCP (Windows) / epoll (Linux) replace thread-per-connection only if telemetry shows connection counts demanding it — engineering offices are tens of clients, not thousands.
* Server data directory (config, logs, certificates — not project data): `C:\ProgramData\Mission Vishwakarma\Server\` in service mode; overridable everywhere with `--config <path>`. Project `.zzz`/`.yyy` files live wherever the administrator mounts them from.
* Server-local settings (ports, TLS, paths, retention overrides, admin bootstrap) live in `server_config.sqlite` in the data directory. Settings that are project truth stay in the `.zzz`; settings that are machine truth stay in `server_config.sqlite`. Nothing is stored in the registry.

3. Client sync protocol (TCP)

The client-facing protocol is a length-prefixed binary framing over one TCP connection per client session. Design goals: compact on the wire, cheap to encode/decode, versioned from day one, and identical whether the authority is VishwakarmaServer or a desktop client in virtual-server mode.

3.1 Transport and framing

* One TCP connection per session. Default port **17017** (configurable; registered in `server_config.sqlite` and advertised in the admin UI). All integers little-endian.
* Connection opens with a fixed 16-byte hello: magic `VWKS`, u32 highest protocol version supported, u32 feature flags (bit 0: zlib, bit 1: TLS required by server, rest reserved), u32 reserved. Server replies with the chosen version + flags. Version negotiation is min(client, server); a server may refuse clients below `minimumAllowedVersion` (same philosophy as the update manifest).
* After hello, everything is frames:

```
u32  payloadLength          (compressed length if compressed; max 64 MB)
u8   frameKind              (message type registry, see 3.3)
u8   flags                  (bit 0: payload is zlib-compressed)
u16  reserved               (0)
[payload]
```

* Compression: zlib (already vendored). Frames under 512 bytes are never compressed. Interactive frames use a fast level; bulk snapshot streaming uses a higher level. zstd is the known upgrade path if telemetry justifies a new dependency; the per-frame flag byte has room for it.
* Heartbeat: ping/pong every 15 s of idle; a session silent for 60 s is dropped (the client then follows the network-interruption rules of the storage doc §5).
* TLS: OpenSSL (vendored), server certificate auto-generated at activation (self-signed). Clients pin the fingerprint on first connect (trust-on-first-use, SSH style); a changed fingerprint blocks the connection until the user explicitly re-accepts. TLS is mandatory for any non-loopback bind; loopback may run plaintext.

3.2 Payload encoding

The storage document (§12.2) finalizes **FlatBuffers** for sync transport (Protobuf remains persistence-only). This gives zero-copy reads on the hot delta-publish path. FlatBuffers is not yet vendored in code-external; adopting it means: a `flatbuffers` submodule, and a `GenerateSyncFlatbuffers.ps1` generation step parallel to `GenerateDataStorageProtobuf.ps1`.

The simpler alternative — Protobuf Lite for the wire too, zero new dependencies, one serialization system — was considered. Decision: **follow the storage document, FlatBuffers**, because the wire format is the hardest thing to migrate later and the zero-copy property matters exactly on the highest-volume message (published deltas fanned out to N subscribers, encoded once). Note that the object *payload* inside a delta remains an opaque Protobuf blob (it is the persisted `object_store.data` bytes); FlatBuffers carries the envelope, not the engineering payload.

3.3 Message registry (frameKind)

Grouped by phase of a session. Each message is a FlatBuffers table; fields follow the storage doc's transaction/sync model (§10) verbatim.

| # | Message | Direction | Purpose |
|---|---|---|---|
| 1 | Hello / HelloAck | both | version + feature negotiation (pre-TLS) |
| 2 | AuthChallenge | S→C | 32-byte nonce |
| 3 | AuthResponse | C→S | principal id + Ed25519 signature over (nonce ‖ session info) |
| 4 | AuthResult | S→C | accepted / rejected / pending-approval + session id + effective permission summary |
| 5 | ProjectTreeRequest / ProjectTree | C→S / S→C | `.zzz` folder tree, mounted `.yyy` / child `.zzz` entries the principal may view |
| 6 | SubscribeRequest / SubscribeResult | C→S / S→C | subscribe: whole `.yyy` / folder subtree / object set (storage §10.4); result carries snapshot boundary change_seq |
| 7 | SnapshotChunk | S→C | streamed initial state: object rows (id, type, version, parent, payload blob), 1–4 MB per frame, backpressure via TCP |
| 8 | ProposeTransaction | C→S | client-proposed transaction: operations with object_id, base_object_version, operation_type, new_payload |
| 9 | CommitResult | S→C | accept/reject per §10.3; on accept: transaction_id, transaction_seq, change_seq, assigned persistent IDs (temporary→persistent remap), new object versions |
| 10 | DeltaPublish | S→C | fan-out to other subscribers: changed/deleted/tombstoned IDs, new versions, affected parents |
| 11 | ResyncRequest / ResyncResult | C→S / S→C | reconnect with last known change_seq; server replays from object_change_log or answers FULL_RELOAD_REQUIRED (§11) |
| 12 | Ping / Pong | both | keep-alive |
| 13 | Error | S→C | protocol / permission / validation error with stable error code |
| 14 | Bye | both | orderly close |

Authority rules enforced at the protocol layer, from storage doc §18.1: clients never receive write handles to files; a client session may not re-serve (no nested peers); every commit is validated against effective permissions before touching a `.yyy`.

3.4 Authentication

Client identity is the existing AccountManager Ed25519 keypair. Challenge-response (never a password on this port). The signed identity is matched against the `principal` table of the `.zzz`. Unknown principals land in a **pending-approval queue** visible in the admin module; an administrator maps them to a principal (or rejects). This gives closed-project security with zero pre-provisioning friction.

4. Admin module (embedded web server)

4.1 Server choice

The admin module is served by an embedded C++ HTTP(S) server. Candidates considered: in-house minimal HTTP/1.1 server, cpp-httplib, Boost.Beast, Drogon/uWebSockets. Decision: **cpp-httplib**, vendored as its committed single-header amalgamation in code-external — exactly the PocketPy vendoring pattern (committed reviewed bytes, no submodule, portable C++, builds on Windows/Linux/Mac, MIT license). It integrates with our vendored OpenSSL for TLS, and HTTP/1.1 is entirely sufficient for an admin plane serving one-digit concurrent administrators. HTTP/2 or websockets would be reasons to revisit; neither is needed (the dashboard polls).

4.2 Ports and reverse proxy

* Defaults: **443** (TLS, same auto-generated certificate as the sync port until the administrator uploads a real one) and **80** (redirect-to-443 only).
* Both ports are configurable in the admin module itself (i.e. by "tab 0"), because the intended enterprise deployment is VishwakarmaServer bound to e.g. `127.0.0.1:8443` behind the organization's main reverse proxy. A `behindProxy` setting makes the server honor `X-Forwarded-For` / `X-Forwarded-Proto` (only when set — never trusted otherwise).
* Until activation completes (§8), all listeners bind loopback only.

4.3 In-house HTML/CSS/JavaScript, embedded

The admin UI is hand-written HTML/CSS/JS — no framework, no npm, no build step, no CDN, no external request of any kind. Vanilla JS with `fetch`. This is the same self-contained philosophy as the rest of the application, applied to the web.

All assets are embedded into the executable at compile time, reusing the established embedding pattern (steel-profile CSV embedder, icon/SVG manifest): a generation script walks `code-core/ServerAdminWeb/` (html/css/js/svg source files), gzip-compresses each, and emits a generated header of byte arrays + a lookup table (path, content-type, ETag = content hash, compressed bytes). The server serves them with `Content-Encoding: gzip` directly — assets are stored pre-compressed once, never compressed at runtime. A developer-mode flag (`--webroot <dir>`) serves from disk instead, for UI iteration without recompiling.

4.4 Admin plane API

The browser JS talks to `/api/…` endpoints. Wire format: **JSON**. This is a deliberate, contained exception to the "no JSON anywhere" IPC rule (extensions doc): the peer here is a browser, JSON is browser-native, the rates are human-scale, and request/response bodies double as human-readable audit evidence. The exception ends at the admin plane — nothing on the sync port or in persistence is JSON. Every mutating endpoint requires an authenticated admin session and lands in `project_admin_transaction_log` / `project_audit_summary` (storage doc §15.9–15.10).

Admin authentication: local admin account, created at activation with a one-time bootstrap password printed exactly once to the activating console. Password storage: scrypt via vendored OpenSSL (`EVP_PBE_scrypt`), per-account salt. Session cookie (HttpOnly, Secure, SameSite=Strict), login rate-limited. Admin accounts are separate from engineering principals; an engineering lead may hold both.

4.5 Admin module pages

| Page | Content |
|---|---|
| Dashboard | server version, uptime, open project, mounted file states, live sessions, commit rate, staged update |
| Project tree | `.zzz` folder hierarchy; create/rename/move folders; mount/unmount `.yyy` and child `.zzz`; mount state and load policy per entry |
| Principals | users/groups/service accounts, group membership, pending-approval queue from §3.4 |
| Permissions | permission_rule CRUD per folder / mounted file / child project; effective-permission preview for a chosen principal |
| Sessions | connected clients: principal, address, subscriptions, traffic, last activity; force-disconnect |
| Audit | project_admin_transaction_log + audit_summary browsing, filter by principal/time/kind |
| Maintenance | checkpoints, log-retention settings (storage doc §11), integrity_issue review, backup trigger |
| Settings | ports, TLS certificate upload, behindProxy, data paths, update channel |

5. What the server does to .yyy and .zzz files

Nothing in this project invents new storage semantics — it implements the server-side rules already fixed in the storage document:

* Opens the `.zzz` with the §15 schema; enforces tree/DAG mounting (no cycles).
* Mounts `.yyy` files per `mounted_yyy` load_policy; opens them with the §14 schema and recommended pragmas (`synchronous = FULL` for authoritative writes).
* Is the sole writer. Assigns persistent object IDs (40-bit local IDs), transaction_id/seq, change_seq, object_version, server_version. Never recycles IDs.
* Conflict detection per §10.3 (`current_object_version == base_object_version`), no automatic property-level merge.
* Computes effective permissions (§6.2), maintains `effective_permission_cache`, enforces at subscribe and commit time.
* Maintains transaction/change/undo logs, checkpoints and retention per §11.
* AutoSave semantics: every accepted commit is durable before CommitResult is sent. There is no "save" on the server.

6. Software update behavior

The server reuses `SoftwareUpdate.cpp` (manifest fetch, Ed25519 + SHA-256 verification, staging) with one policy difference: **a server never self-restarts**. The update thread downloads, verifies and stages; the admin dashboard then shows "update staged — restart service to apply", and applying happens on the next service restart (or an explicit admin-triggered restart from the Maintenance page). The scheduled-task mechanism of the desktop app is not used in service mode; the service process is long-lived and its own update thread suffices.

7. Packaging, signing, installation

* Built by the same pipeline: `GenerateVersionHeader.ps1` pre-build (VS_VERSION_INFO with git-count version, commit hash, build date), `GenerateRelease.ps1` packaging, nightly CI.
* Authenticode-signed with the same certificate as Vishwakarma.exe (MV-CodeSigner-01) and RFC 3161 timestamped. Covered by the same Ed25519-signed release manifest; the setup exe's sha256 in the manifest covers the embedded server binary too.
* **One setup file installs both.** `Vishwakarma_UserSetup_win10_win11_x64.exe` carries Vishwakarma.exe, VishwakarmaExtension.exe and VishwakarmaServer.exe, and writes all three side by side into the install directory.
* The server is **installed dormant**: no desktop shortcut, no start-menu entry, no service registration, no firewall rule, no scheduled task, no process launch. A file on disk, nothing more. Regular users never notice it exists.

8. Activation (explicit, administrator-only)

The server comes alive only when an administrator runs, from an **elevated** prompt:

```
VishwakarmaServer.exe --activate
```

Activation performs, in order:

1. Verify elevation; refuse otherwise with a one-line explanation.
2. Create the data directory `C:\ProgramData\Mission Vishwakarma\Server\`, ACL'd to administrators + the service SID.
3. Create `server_config.sqlite` with defaults (sync 17017, admin 443/80, loopback-only until first admin login changes it).
4. Generate the Ed25519 server identity and the self-signed TLS certificate.
5. Register the Windows Service `VishwakarmaServer` (auto-start, `NT SERVICE\VishwakarmaServer`).
6. Add inbound firewall rules for the configured ports (`netsh advfirewall`).
7. Create the bootstrap admin account; print its one-time password to the console — the only time it is ever shown.
8. Start the service and print the admin URL.

`--deactivate` (also elevated) stops and removes the service, firewall rules and scheduled state but **keeps** the data directory and all project files — deactivation must never destroy engineering or administrative data. Uninstalling the application leaves server data in ProgramData for the same reason.

Console mode (`VishwakarmaServer.exe --console`) needs no elevation and no activation; it binds loopback (or fails gracefully on ports < 1024 without privilege) using a config next to it or `--config`. This is the "hosted on local machine" path for a small team lead running the server on their workstation.

Command-line summary:

| Flag | Elevation | Effect |
|---|---|---|
| `--console` (or none) | no | run in foreground, log to stdout |
| `--activate` | yes | install service + firewall + data dir + bootstrap admin, start service |
| `--deactivate` | yes | remove service + firewall; data preserved |
| `--config <path>` | no | explicit config database path |
| `--webroot <dir>` | no | developer mode: serve admin UI from disk |
| `--version` | no | print version and exit |

9. Project and build integration

* New project file `code-core/VishwakarmaServer.vcxproj`, added to Vishwakarma.sln (Debug/Release, x64 only), depending on VishwakarmaExternal — never triggering its rebuild (same `BuildProjectReferences=false` command-line discipline as the main project). Warning-free like the rest.
* Server-only sources live in code-core with a `Server` prefix (`ServerMain.cpp`, `ServerSync*.cpp/h`, `ServerAdmin*.cpp/h`, `ServerAdminWeb/` assets). Shared sources (storage, sync protocol, account, update) are the existing platform-agnostic files compiled into both binaries.
* Platform separation follows the graphics-doc convention: `<Module>.cpp` platform-agnostic, `<Module>-Windows.cpp` / `<Module>-Linux.cpp` per platform (sockets, service integration, daemonization). The Linux build system (likely CMake, matching a future Linux client) is out of scope here; the code is written to be ready for it.

10. Phased implementation plan

Each phase ends with a verifiable state; no phase begins before the previous one's check passes.

* **Phase 0 — scaffolding.** vcxproj + sln entry; `--version`; signed; embedded in setup. Verify: command-line build of all projects is warning-free; fresh install puts VishwakarmaServer.exe on disk, creates no shortcut/service/process.
* **Phase 1 — config + admin skeleton.** server_config.sqlite; embedded-asset pipeline; cpp-httplib serving the dashboard on loopback; bootstrap admin auth. Verify: browser login on a machine with no graphics stack; all assets served from the exe with correct ETags; `--webroot` round-trip.
* **Phase 2 — .zzz core.** Create/open `.zzz` (storage §15 schema); project-tree and mount CRUD from the admin UI; admin transaction log + audit. Verify: sqlite3 inspection matches the schema; every mutation produces an audit row.
* **Phase 3 — sync read path.** TCP listener, hello/TLS/auth, pending-approval queue; client entry via the tab-0 Launcher view's online-servers list (opens a server-hosted engineering tab); ProjectTree, mount `.yyy`, SubscribeRequest + SnapshotChunk streaming. Verify: a client on another machine loads and renders a mounted model; Wireshark shows compressed frames, nothing readable.
* **Phase 4 — commit authority.** ProposeTransaction → validate → commit → CommitResult + ID remap → DeltaPublish; Resync + FULL_RELOAD_REQUIRED. Verify: two clients see each other's edits live; base-version conflict is rejected cleanly; hard-killing the server mid-commit loses nothing (WAL recovery) and clients resync.
* **Phase 5 — permissions.** Principals/groups/rules CRUD, effective-permission computation + cache, enforcement on subscribe and commit. Verify: a deny rule blocks exactly the denied subtree; the admin preview matches enforcement.
* **Phase 6 — hardening.** TLS mandatory off-loopback + client pinning; service mode polish; staged-update-on-restart; metrics into the telemetry pipeline; load test with simulated clients. Verify: 30-day soak on a test project.

11. Open questions

* The tab-0 definition follows the *Tab System and the Application Tab* chapter (tabs.md). One dependency flows back to it: the Launcher view's "online servers" list is the client-side entry point to this server, so its persistence format (recent-servers list, pinned TLS fingerprints per §3.1) should be settled together with that chapter's "recent-files persistence" open question.
* FlatBuffers vendoring (§3.2) is a new submodule + generation script; final go/no-go before Phase 3.
* Default sync port 17017 is a proposal; confirm before clients ship pinned defaults.
* The JSON admin-plane exception (§4.4) — confirm or veto.
* Remote child `.zzz` mounts (federation across two VishwakarmaServer instances) are explicitly out of scope for v1: child `.zzz` files must be local paths. The mount tables already carry `canonical_uri` for the future.
* Linux packaging (systemd unit, port-binding capabilities, CMake) is acknowledged but deferred; nothing in this design is Windows-only except §8's concrete mechanics.
