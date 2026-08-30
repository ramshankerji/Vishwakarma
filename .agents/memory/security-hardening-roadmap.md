---
name: security-hardening-roadmap
description: "The security.md audit roadmap: what was found, priority order P0-P4, and the Valgrind/sanitizer server plan"
metadata: 
  node_type: memory
  type: project
  originSessionId: cfb8b30a-cd01-4ef4-80e5-3f9ef3378d37
  modified: 2026-07-21T01:37:07.791Z
---

`website/content/software/security.md` was populated (2026-07-21) from a full-codebase security review (DESIGN DOC ONLY, no implementation). Priority order by risk×exposure:

- **P0 Critical:** encrypted signing-key PEMs are committed to the public repo (see [[release-signing-setup]]) — rotate + purge history. Root of the update trust chain.
- **P1 High:** extension worker (`VishwakarmaExtension.exe`) has no OS sandbox yet — AppContainer + Job Object + child-process ban. Primary threat = malicious `.dxf`/`.std` file. Already the acknowledged next step in extensions.md.
- **P2a High:** file parsers (`.mvdb` protobuf in DataStorage.cpp, `.dxf`/`.std`, libpng/libjpeg/freetype) are unfuzzed — verified by the §4 testing server.
- **P2b Medium:** binary mitigation flags (`/guard:cf`, `/CETCOMPAT`, HIGHENTROPYVA) not pinned; only SDLCheck is on.
- **P3a:** installation Ed25519 private key written as PLAINTEXT PKCS8 PEM (AccountManager.cpp, no DPAPI). **P3b:** no WinVerifyTrust/Authenticode gate before launching staged installer (relies solely on manifest hash chain). **P3c:** no dependency-CVE/SBOM process.
- **P4:** server edge-DoS (per-worker locmem rate limit, 2× Ed25519 verify before limiter), no SECURITY.md, undocumented static-site headers, future user-edits/login threat models.

Posture already GOOD (do not weaken): signed+hash-verified updates with signature-checked-before-parse, TLS never downgraded, network-stripped frozen-CPython worker, host-side output validation, hardened Django+systemd server.

**Valgrind server plan (§4):** REALITY CHECK — Valgrind is Linux-only and cannot instrument Win32/DX12. Plan = a Linux memory-safety CI box that fuzzes the PORTABLE CORE subset (parsers, MemoryManagerCPU, ID/CrockfordBase32, geometry/validation) under libFuzzer+ASan/UBSan and nightly Valgrind memcheck/helgrind, plus a Windows counterpart using MSVC `/fsanitize=address` + Application Verifier for the platform code. Separate box from the telemetry Pi; sandboxed fuzzer, non-confidential seed corpus.
