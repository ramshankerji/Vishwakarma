---
title: "Application Security"
weight: 100112
---
This page is the design document and hardening roadmap for **Vishwakarma** security. Like
`undo-redo.md` and `login.md`, prose sections are **locked decisions**; items marked **OPEN**
are still under debate. Nothing here is an implementation task list yet — it is the ordering,
the reasoning, and the acceptance criteria that a future implementation (human or AI) must meet.

The roadmap below is derived from a full-codebase review (audit date **2026-07-21**) covering
the four subsystems: the C++ desktop application (`code-core`), the build / release tooling
(`code-miscellaneous`), the Django telemetry server (`server`), and the Hugo website
(`website`). Findings are ordered by **risk × exposure**, not by ease of fixing.

---

## 1. Threat Model

Security work is only meaningful against a stated adversary. Vishwakarma is a **source-available
desktop CAD/CAM application** that runs with the logged-in user's full privileges, parses files
authored by third parties, and auto-updates itself over the internet. That shape dictates the
threat model.

### 1.1 Assets worth protecting

1. **The update trust chain.** If an attacker can make `Vishwakarma.exe` accept a malicious
   setup executable, they achieve code execution on *every* installation. This is the highest
   value target in the whole system.
2. **The user's machine.** The application and its extension worker parse untrusted files
   (`.std`, `.dxf`, `.mvdb`, embedded images/fonts) with the user's own token. A parser bug is
   a path to arbitrary code with full access to the user's documents.
3. **The telemetry / account identity.** Each installation *is* an Ed25519 key pair
   (`AccountManager.cpp`, and the future account system in `login.md`). Theft or spoofing of
   that identity pollutes statistics and, once accounts ship, impersonates users.
4. **Server availability.** The telemetry/stats server (`mv-server.ramshanker.in`) runs on a
   single Raspberry Pi behind Cloudflare. Availability, not confidentiality, is its scarce
   resource.
5. **Website integrity.** `mv.ramshanker.in` is where users are told to download the software.
   Defacement or a poisoned download link is a distribution-channel attack.

### 1.2 Adversaries, most-likely first

* **The malicious file author.** Engineers routinely exchange CAD files. A booby-trapped
  `.dxf` or `.std` opened by a victim is the most realistic attack, and it needs no network
  access to the attacker. **This is the primary threat.**
* **The repository scraper.** The repo is public. Anything secret that is committed — now or
  in history — is already compromised.
* **The network attacker (MITM).** Can tamper with update downloads and telemetry in transit.
  Largely defeated today (see §2) but must stay defeated.
* **The telemetry flooder.** Wants to exhaust the Pi's CPU or fill its disk.
* **The future web attacker.** Targets the planned user-edits pipeline (`useredits.md`) and
  login subsystem (`login.md`) once they exist.

### 1.3 Trust boundaries

```text
 UNTRUSTED INPUT                    TRUST BOUNDARY                 TRUSTED CORE
 ------------------                 ---------------                ------------
 .dxf / .std file  ───────►  Extension worker (CPython)  ──────►  validated batch ──► host
 .mvdb file        ───────►  DataStorage protobuf decode ──────►  in-memory model
 signed manifest   ───────►  Ed25519 verify + SHA-256    ──────►  staged installer ──► exec
 telemetry POST    ───────►  Ed25519 chain + rate limit  ──────►  SQLite row
 embedded PNG/font ───────►  libpng / freetype (C)       ──────►  GPU texture / atlas
```

Every arrow crossing into the trusted core is a place where validation must be complete. The
roadmap is essentially a list of arrows that are not yet strong enough, ordered by blast radius.

---

## 2. Current Posture — What Is Already Right

A hardening roadmap that ignores existing defenses invites "fixing" things that are fine and
regressing them. The review found the codebase already does the following well, and these
**must not be weakened**:

* **Signed, hash-verified updates.** `SoftwareUpdate.cpp` downloads a manifest, verifies its
  Ed25519 signature against a source-pinned public key (`VerifyManifestSignature`) *before*
  parsing it, then verifies the downloaded setup's SHA-256 against the signed manifest, plus a
  rollback guard (`manifestSequence`), an expiry (`validUntil`), an `appId` check, and a
  blocked-versions list. The naive JSON scanner only ever runs on already-authenticated bytes.
* **TLS is never downgraded.** Both `SoftwareUpdate.cpp` and `ImprovementData.cpp` use WinHTTP
  with `WINHTTP_FLAG_SECURE`; no `SECURITY_FLAG_IGNORE_*` or cert-bypass flags exist anywhere
  in the tree. A network attacker cannot inject an update even without pinning, because the
  payload is independently Ed25519-signed.
* **A network-stripped extension worker.** `VishwakarmaExtension.exe` is a frozen, statically
  linked CPython with `socket`/`ssl` never compiled in, `subprocess`/`multiprocessing` not
  frozen, and the `nt` module scrubbed at import — solid defense-in-depth *ahead of* the
  sandbox (documented in `extensions.md`).
* **Host-side validation of all worker output.** `ExtensionCommunications.cpp` enforces count
  caps (`kMaxImportedNodes`, etc.), finite-float checks, per-message size limits, and length
  bounds on every batch the worker returns.
* **A genuinely hardened server.** `improvement_server/settings.py` refuses to boot in
  production without `MV_SECRET_KEY`, pins `ALLOWED_HOSTS`, sets HSTS / secure cookies /
  nosniff / `X-Frame-Options: DENY`, ships a strict CSP via `api/middleware.py`, locks CORS to
  a single origin and a single URL regex, caps upload size, verifies the full Ed25519 signature
  chain on ingestion (`api/crypto.py`), rate-limits per installation key, and exposes **no**
  admin / auth / session surface. The `systemd` unit adds `NoNewPrivileges`,
  `ProtectSystem=strict`, a read-only filesystem, `PrivateTmp`, and kernel-protection knobs.
* **Signing keys are at least encrypted.** The private keys committed to the repo are
  `ENCRYPTED PRIVATE KEY` PKCS#8 PEMs, not plaintext (this is still a finding — see P0).
* **`SDLCheck` is on** for all four `.vcxproj` projects.

The server subsystem in particular is the strongest part of the codebase and needs only minor
edge-DoS hardening (P4).

---

## 3. Prioritized Hardening Roadmap

Each item states the **finding**, the **risk**, the **evidence**, a **design direction**
(what to build — no code here), and the **acceptance criteria** that prove it is done.

### Priority summary

| # | Priority | Item | Subsystem | Blast radius |
|---|----------|------|-----------|--------------|
| P0 | Critical | Signing-key material lives in the public repo | `code-miscellaneous` | All installations |
| P1 | High | Extension worker has no OS-level sandbox | `code-core` | User's machine |
| P2a | High | File parsers are unfuzzed / untested for memory safety | `code-core` | User's machine |
| P2b | Medium | Binary exploit-mitigation flags not fully enabled | `code-core` build | User's machine |
| P3a | Medium | Installation private key stored in plaintext at rest | `code-core` | Telemetry identity |
| P3b | Medium | No Authenticode gate before executing staged installer | `code-core` | Defense-in-depth |
| P3c | Medium | No dependency-CVE / SBOM process | all | Supply chain |
| P4a | Low | Server DoS relies on per-worker in-memory rate limit | `server` | Availability |
| P4b | Low | No `SECURITY.md` / disclosure policy | repo | Process |
| P4c | Low | Static-site security headers undocumented | `website` | Distribution |
| P4d | Future | User-edits & login pipelines need their own threat model | `server`/`website` | TBD |

---

### P0 — Signing-key material is committed to a public repository

**Finding.** `git ls-files` lists `MV-CodeSigner-01.key`, `ManifestSigner-01.key`, and
`RootCA-MV-01.key` (plus the `.crt` public certs) under `code-miscellaneous/`. The `.pfx` is
`.gitignore`d and arrives on CI as the `RELEASE_SIGN_PFX_B64` secret, but the encrypted
**private** `.key` PEMs are tracked in the repository and its history.

**Risk.** These three keys *are* the update trust chain from §1.1(1):

* `ManifestSigner-01.key` signs the release manifest that `VerifyManifestSignature` trusts.
  Whoever holds it can sign a manifest pointing at a malicious setup and every installation
  will accept it — remote code execution on the entire user base.
* `MV-CodeSigner-01.key` is the Authenticode identity users see as the publisher.
* `RootCA-MV-01.key` is the root that signs the code-signer; if that root is ever added to any
  trust store, its compromise forges arbitrary trusted certificates.

The only thing standing between a repo scraper and this capability is the **passphrase** on the
encrypted PEMs. The same passphrase (`RELEASE_SIGN_PASSWORD`) protects all of them and is reused
as the manifest-key password (`VW_MANIFEST_PW` in `GenerateRelease.ps1`). Encryption buys time,
not safety: the ciphertext is permanently public, so the keys are only as strong as one shared
password against an *offline* attack with unlimited attempts.

> This also corrects a stale internal assumption that "the signing key is a CI secret, not in
> git." The `.pfx` is; the encrypted `.key` PEMs are not.

**Design direction.**
1. Treat all three keys as **compromised** and plan a rotation: generate new Root CA,
   code-signer, and manifest-signer key pairs offline; ship the new manifest public key in a
   source update; keep an overlap window where the client accepts either the old or the new
   manifest key so in-flight installs can migrate.
2. Purge the `.key` files from the working tree **and** from git history (history rewrite;
   coordinate because it changes commit hashes). Publish only the `.crt`/`.pub` files.
3. Store the new private keys **only** offline (hardware token / HSM / an air-gapped machine)
   and as CI secrets, never as repo files. Encrypted-in-git is not an acceptable resting place
   for the root of the update chain.
4. Give the manifest-signing key its own passphrase, distinct from the PFX password.
5. Document the rotation runbook in `release.md`.

**Acceptance criteria.** `git log --all -- '**/*.key'` shows the keys were removed and history
rewritten; a fresh clone contains no private key; a release built from CI secrets still verifies
end-to-end on a clean install; the client accepts manifests signed by the new key and rejects
the old one after the overlap window.

---

### P1 — The extension worker runs without an OS sandbox

**Finding.** `VishwakarmaExtension.exe` parses untrusted `.std`/`.dxf` files but runs as a
plain child process with the user's full token. `ExtensionCommunications.cpp` launches it with
`CreateProcessW` and `CREATE_NO_WINDOW` only — no AppContainer, no Job Object, no child-process
ban. The code and `extensions.md` both explicitly flag this as the acknowledged next step.

**Risk.** This is the arrow from the **primary threat** (§1.2). Today's defenses are all
*language-level* (no sockets compiled in, `nt` scrubbed). A memory-corruption bug in the
CPython interpreter, in a C extension, or in the pure-Python parsers — or simply a scrubbing
gap — lets attacker-controlled file bytes run code that can read and write every file the user
can, spawn processes, and reach the network via any means the language ban missed. A single
malicious `.dxf` mailed to an engineer is the whole attack.

**Design direction** (defense-in-depth, each layer independently valuable):
* **AppContainer / low-integrity token.** Run the worker in an AppContainer with no
  capabilities, so even arbitrary native code is denied the filesystem, network, and clipboard
  by the OS regardless of what the language allows.
* **Job Object.** Assign the worker to a Job with `JOB_OBJECT_LIMIT_ACTIVE_PROCESS = 1` and
  `KILL_ON_JOB_CLOSE`, plus memory and CPU-time ceilings, so it cannot spawn children or
  outlive the host and cannot be used as a fork bomb.
* **Explicit child-process ban** via `PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY`.
* **File delivery, not path access.** The host already streams file *bytes* over the pipe
  rather than handing the worker a path; keep that invariant so the AppContainer needs zero
  filesystem grants.
* Keep all existing host-side output validation — the sandbox complements it, never replaces it.

**Acceptance criteria.** The worker process token shows an AppContainer SID; a test extension
that attempts to open a file outside its grant, open a socket, or spawn a child fails at the OS
level; killing the host kills the worker via the Job; imports still succeed for the real
`.std`/`.dxf` corpus.

---

### P2a — File parsers are untested for memory safety

**Finding.** Vishwakarma parses several untrusted binary/text formats in memory-unsafe
contexts, with no fuzzing or sanitizer coverage:

* Native `.mvdb` files via protobuf decode in `DataStorage.cpp` (`ParseMessage`, followed by
  `reserve()` on message-supplied counts and `memcpy` into fixed C-string fields in
  `CopyStringToFixedCString`).
* `.dxf` and `.std` in the worker (memory-safe Python, but the *host* then trusts converted
  batches — already validated, keep it).
* Embedded **PNG via libpng**, **JPEG via libjpeg**, **fonts via freetype** — mature C
  libraries with a long CVE history. These inputs are trusted resources *today* (icons, logo,
  bundled font), but the moment a user-supplied image or font can reach them, they become live
  attack surface.

**Risk.** A crafted file that trips a bounds bug in any native parser yields the same outcome as
P1 — code execution or, at minimum, a denial-of-service crash. CAD files are shared constantly,
so this is high-exposure.

**Design direction.**
* Stand up the **sanitizer / Valgrind testing server** described in §4 as the verification
  engine for this item.
* Build a small, portable, headless harness that feeds a corpus and fuzzer-generated inputs
  through the platform-independent parsers (protobuf decode + geometry validation, the memory
  manager, ID/encoding utilities) under ASan/UBSan and Valgrind memcheck on Linux, and under
  MSVC `/fsanitize=address` + Application Verifier on Windows.
* Keep a regression corpus of every crash ever found; every release must pass it clean.
* Treat libpng/libjpeg/freetype as **untrusted** the day user-supplied assets are allowed, and
  gate that feature on fuzzing those code paths first.

**Acceptance criteria.** A fuzzing target exists for each untrusted format; CI runs the
regression corpus under sanitizers on every PR with zero findings; a documented seed corpus and
crash-triage flow exist.

### P2b — Binary exploit-mitigation flags are not fully enabled

**Finding.** `SDLCheck` is on, but the projects do not explicitly enable Control Flow Guard
(`/guard:cf`), CET shadow stack (`/CETCOMPAT`), or assert high-entropy ASLR / DEP. Some are
linker defaults, but they are not pinned, so a toolchain or setting change can silently drop them.

**Risk.** Without CFG/CET, a memory bug from P2a is far easier to turn into reliable code
execution. These flags are close to free and raise exploitation cost substantially.

**Design direction.** Enable and **pin** `/guard:cf`, `/CETCOMPAT`, `/DYNAMICBASE`,
`/HIGHENTROPYVA`, `/NXCOMPAT`, and `/DEPENDENTLOADFLAG` across `Vishwakarma`,
`VishwakarmaExtension`, and `VishwakarmaSetup`. Add a release-gate check (e.g. `dumpbin
/headers` / BinSkim) that fails if any mitigation is missing from a shipped binary.

**Acceptance criteria.** BinSkim (or equivalent) reports all mitigations present on every
shipped `.exe`; the check runs in CI.

---

### P3a — Installation private key is stored in plaintext

**Finding.** `AccountManager.cpp` writes the installation Ed25519 private key to
`%LOCALAPPDATA%\Mission Vishwakarma\Credentials\InstallationKey.pem` via
`PEM_write_bio_PKCS8PrivateKey(..., nullptr /*cipher*/, ...)` — **unencrypted**. No DPAPI, no
`CryptProtectData`.

**Risk.** Any process or user with read access to the profile can copy the key and impersonate
the installation to the telemetry server (and, once `login.md` ships, to the account system).
Impact is currently limited to identity spoofing / stat pollution — no funds, no PII — hence
Medium, but it grows the day accounts carry authority.

**Design direction.** Wrap the private key at rest with **DPAPI** (`CryptProtectData`, user
scope) so it is bound to the Windows user profile, or store it in the platform credential store.
The public key stays as a convenience copy. Provide a one-time migration that re-wraps an
existing plaintext key on next launch.

**Acceptance criteria.** The on-disk key is unreadable when copied to another user/machine;
telemetry signing still works; a legacy plaintext key is transparently migrated once.

### P3b — No Authenticode gate before executing the staged installer

**Finding.** `SoftwareUpdateOnAppLaunch()` launches the staged setup after verifying its
SHA-256 against the signed manifest, but does **not** call `WinVerifyTrust` to confirm the
binary carries a valid Authenticode signature from the MV code-signer. No `WinVerifyTrust` call
exists in the tree.

**Risk.** Cryptographically the hash-from-signed-manifest chain is already sound *provided the
manifest key is safe* — which is exactly the P0 dependency. An Authenticode check is
**defense-in-depth**: it means an attacker needs to break both the manifest key *and* the
code-signing key, and it catches a locally tampered staging file before it runs.

**Design direction.** Before `LaunchProcess(setupPath, "--update")`, call `WinVerifyTrust` and
require the signer chain to terminate at the pinned MV code-signing certificate. Refuse to
launch on failure and discard the staging area.

**Acceptance criteria.** A staged installer that is unsigned, self-signed, or signed by a
different cert is rejected and deleted; a legitimately signed installer runs.

### P3c — No dependency-CVE / SBOM process

**Finding.** The app statically links a substantial C/C++ supply chain (OpenSSL, protobuf,
libpng, libjpeg, freetype, CPython, and the rest of `code-external`). There is no documented
process for tracking CVEs in these, no SBOM, and no update cadence.

**Risk.** A published CVE in any linked library (OpenSSL and freetype especially) ships in
Vishwakarma until someone notices by hand.

**Design direction.** Generate an SBOM (CycloneDX) at release; subscribe the pinned submodule
versions to a vulnerability feed / `osv-scanner`; define a patch SLA for critical CVEs; record
the policy in `release.md`.

**Acceptance criteria.** Each release ships an SBOM; a scanner runs in CI against the pinned
dependency set; a documented owner and SLA exist.

---

### P4 — Lower-priority and future items

* **P4a — Server edge DoS.** Rate limiting is per-gunicorn-worker `LocMemCache` (acknowledged
  as coarse in `settings.py`), and every request pays two Ed25519 verifications *before* the
  limiter. An unauthenticated flood of bad-signature requests still burns CPU on the Pi.
  *Direction:* add a Cloudflare edge rate-limit rule and an IP-based pre-auth throttle so
  bad-signature traffic is dropped before signature math. Low priority — Cloudflare already
  fronts the origin and body size is capped at 1 MB.
* **P4b — No `SECURITY.md` / disclosure policy.** A source-available app that parses untrusted
  files needs a stated way to report vulnerabilities. *Direction:* add `SECURITY.md` with a
  contact and a coordinated-disclosure window.
* **P4c — Static-site headers undocumented.** `hugo.toml` does not enable goldmark `unsafe`
  HTML (good — raw HTML is escaped), but the security headers for `mv.ramshanker.in` (CSP,
  HSTS, `X-Content-Type-Options`) are set at the Cloudflare/host layer and are not captured in
  the repo. *Direction:* document the required response headers alongside the deploy config so
  they survive a hosting change.
* **P4d — Future subsystems.** The planned user-edits pipeline (`useredits.md`: wiki `[edit]`
  → Django `/api/edits` → bot PRs) and the login subsystem (`login.md`) each introduce new
  untrusted-input boundaries (content injection, PR spam, OTP abuse, PII at rest). Each must
  carry its own threat-model section in its own design doc **before** implementation, and be
  linked back here.

---

## 4. Verification Infrastructure: the Sanitizer / Valgrind Testing Server

P2a is only credible if there is a machine that continuously proves the parsers are memory-safe.
This section is the plan for that machine. **It is a plan, not an implementation.**

### 4.1 The reality check first

Vishwakarma is a **Windows + DirectX 12** application, and **Valgrind does not run on Windows
and cannot instrument Direct3D**. Any plan that claims to "run Vishwakarma under Valgrind" is
wrong. The honest architecture splits the codebase by portability:

| Layer | Examples | Portable to Linux? | Tooling |
|-------|----------|--------------------|---------|
| **Portable core** | `DataStorage` protobuf decode & validation, `MemoryManagerCPU`, `ID`/`CrockfordBase32`, geometry math, extension-batch validators | **Yes** — no Win32/DX12 | Valgrind + libFuzzer/AFL++ + ASan/UBSan on Linux |
| **Windows platform** | DX12 renderers, WinHTTP, Win32 UI, `SoftwareUpdate` process logic | No | MSVC `/fsanitize=address`, Application Verifier, Dr. Memory on Windows |
| **Server** | Django app | N/A (already Linux) | `pytest`, `bandit`, `pip-audit` — not Valgrind |

So the "Valgrind server" is really a **Linux memory-safety CI box that exercises the portable
core and the file parsers**, complemented by a Windows-side sanitizer job. The highest-value
target — untrusted-file parsing — is almost entirely in the portable core, which is exactly what
Valgrind and a fuzzer *can* reach. That is why this is worth building despite the platform split.

### 4.2 What the server does

1. **Builds a headless Linux harness.** A small CMake target links the portable-core subset
   (no Win32, no DX12) plus a `main()` that reads one input file and drives it through a parser
   entry point. One target per format: `.mvdb` protobuf decode, the `.std` parser module, the
   `.dxf` parser module, and (once user assets are allowed) libpng/libjpeg/freetype wrappers.
2. **Fuzzes continuously.** Each harness is also compiled as a **libFuzzer** (or AFL++) target
   with `-fsanitize=address,undefined,fuzzer`. The fuzzer runs against a seed corpus of real
   engineering files and mutates toward crashes.
3. **Runs deep Valgrind passes.** On a slower schedule (nightly), the regression corpus runs
   under `valgrind --tool=memcheck --leak-check=full --error-exitcode=1` and `--tool=helgrind`
   for the threaded memory manager. Valgrind catches uninitialized reads and leaks that ASan
   misses; ASan catches overflows Valgrind is slower on. They are complementary, so run both.
4. **Triages and reports.** Any crash/leak is minimized, added to the permanent regression
   corpus, and filed. A release is blocked until the regression corpus passes clean.

### 4.3 Suggested provisioning (design, not scripts)

* **Host.** A cheap always-on Linux box or VM (4+ cores, 8 GB+ RAM, SSD for corpus I/O).
  Ubuntu LTS. This can be a container on the same infrastructure family as the telemetry Pi,
  but should be a **separate machine** — fuzzing is CPU-hungry and must not starve the
  production server.
* **Toolchain.** `clang`/`llvm` (libFuzzer, sanitizers), `valgrind`, `cmake`, `afl++`
  optional, `git`. No network egress needed except to pull the repo.
* **Isolation.** The fuzzer executes attacker-shaped inputs, so run it as an unprivileged user
  inside a container with no secrets mounted, read-only root, and a `tmpfs` scratch — the same
  posture as the telemetry `systemd` unit. A parser crash must never reach anything valuable.
* **Corpus store.** A versioned directory (or small git-LFS repo) holding: `seeds/` (real
  files, scrubbed of any confidential project data), `crashes/` (regressions), and
  `minimized/`. Seeds must be non-confidential — never commit a customer's drawing.
* **CI wiring.** A GitHub Actions job builds the sanitizer harnesses and runs the regression
  corpus on every PR (fast, minutes). The full mutation fuzzing and nightly Valgrind runs
  happen on the dedicated box on a timer, reporting new crashes back as issues.
* **Windows counterpart.** A parallel Windows CI job builds with MSVC `/fsanitize=address` and
  runs the same corpus through the real Windows parsers under Application Verifier, covering the
  platform code Valgrind cannot.

### 4.4 What "done" looks like

* A documented list of portable-core files that compile clean on Linux with no Win32/DX12.
* At least one fuzz target per untrusted format, each reaching the actual parser.
* A seed corpus and a growing regression corpus, both non-confidential.
* Nightly Valgrind memcheck + helgrind on the regression corpus, exit-code-gated.
* CI that fails a PR on any sanitizer or Valgrind finding.
* A one-page runbook (in `release.md` or a `testing.md`) for adding a new fuzz target and
  triaging a crash.

### 4.5 Sequencing

The server underpins **P2a**, so it is built alongside it, after **P0** (never expose signing
keys to a fuzzing box) and can proceed in parallel with **P1**. Order: rotate keys (P0) →
stand up the Linux harness + seed corpus → wire regression-corpus CI → enable continuous
fuzzing + nightly Valgrind → add the Windows sanitizer counterpart.

---

## 5. Non-Goals

* **Not** a penetration-test report or a compliance certification (SOC 2, etc.). This is an
  engineering roadmap.
* **Not** DRM or anti-tamper. The software is source-available by design.
* **Not** confidentiality of telemetry content — it is aggregate and non-PII by design
  (`login.md`, `telemetry`).
* **Not** a rewrite in a memory-safe language. Mitigation here is sandboxing + fuzzing +
  exploit mitigations, not rewriting the DX12 engine.

## 6. Open Questions (OPEN)

* **OPEN:** Does the Root CA (`RootCA-MV-01`) get installed into any user or OS trust store, or
  is it used purely as an internal signing anchor? The answer changes P0 from "rotate a signing
  key" to "rotate a trusted root," which is far more urgent.
* **OPEN:** Should the extension worker sandbox target AppContainer specifically, or the newer
  Win32 app-isolation / process-mitigation policies? Decide before P1 implementation.
* **OPEN:** Where does the fuzzing box live physically, and who owns crash triage?
* **OPEN:** Timeline for the P0 key rotation overlap window — how long must clients accept both
  the old and new manifest keys before the old one is retired?
