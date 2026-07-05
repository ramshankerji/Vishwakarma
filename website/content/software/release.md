---
title: "Update and Release Management"
weight: 100208
---
This chapter describes our software installer and self-update mechanism. Earlier we shipped a single raw binary named Vishwakarma.exe; since July 2026 every release instead produces the following 3 files:

## Configurations
Vishwakarma_UserSetup_win10_win11_x64.exe  (self-contained installer, Vishwakarma.exe and its release json embedded inside)  
Vishwakarma_release_details.json  (the release manifest which installed clients poll for updates)  
Vishwakarma_release_details.json.sig  (Ed25519 signature over the manifest bytes)  

The setup filename carries no version suffix: the release URL stays constant so the website Download button is a static link that never breaks. The version number lives only in the manifest `version` field and inside the executable's own version resource (see below).  

In future we will also have: But not now.  
Vishwakarma_UserSetup_win10_win11_ARM64.exe  
Vishwakarma_UserSetup_macXY_x64.exe  
Vishwakarma_UserSetup_ubuntuXY_x64.exe  
Vishwakarma_UserSetup_ubuntuXY_ARM64.exe  

Content of Vishwakarma_release_details.json
{
  "schemaVersion": 1,
  "appId": "MissionVishwakarma",
  "manifestSequence": 20260702-210033-abcdefgh, // YYYMMDD-HHMMSS-8CharCommitHash
  "generatedAt": "2026-07-02T10:30:00Z",
  "validUntil": "2026-08-02T10:30:00Z", //30 days max.
  "signatureScheme": "Ed25519",
  "signingKeyId": "release-key-2026-01",
  "releases": [
    {
      "name": "Vishwakarma",
      "channel": "stable",
      "platform": "windows",
      "osMinVersion": "10.0",
      "osMaxVersion": null,
      "instructionSet": "x64",
      "version": 142,
      "minUpdateFromVersion": 1,
      "packageKind": "full-user-setup-exe",
      "fileName": "Vishwakarma_UserSetup_win10_win11_x64.exe",
      "url": "https://updates.example.com/stable/windows/x64/Vishwakarma_UserSetup_win10_win11_x64.exe",
      "size": 18423822,
      "sha256": "abcdefghijklmnopqrstuvwxyz",
      "authenticodeRequired": true
    }
  ],
  "blockedVersions": [],
  "minimumAllowedVersion": 1
}

## Philosophy

Every time an app launches, it will launch an update service thread. Update thread will calculate and update offset delta time between 10 minutes and 10 hours, when that much time passes,  it will:
-Fetch signed manifest
-Decide whether update applies
-Download package from server or peers
-Verify hash + signature
-Install into staging folder
-Switch active version on restart
-Abort if udpate fails

Running app does: Download setup EXE, Verify SHA-256, Verify Authenticode (ED25519). On app restart, launch:
  Vishwakarma_UserSetup_win10_win11_x64.exe --update --no-launch
Then launch new Vishwakarma.exe

version shall be a simple positive integer, carried in the manifest and the executable version resource (not the filename).
Vishwakarma_release_details.json shall be a simple json file kept at root of the repository.

Peer may announce:  appId, channel, platform, instructionSet, version, sha256, size

Peer should not be the normal source of manifest truth. Target peer should fetch the official manifest from update server.

Only if server is unavailable:   peer-provided signed manifest may be considered,  but only if not expired,  sequence is newer than local state,  signature is valid,  and package hash matches.

Best peer message: "I have artifact sha256=XYZ, size=N, version=142.". Not: "Trust me, version 142 is latest."

Peer-to-peer communication happens using simple binary protocol defined by protocol buffer messages. Peer-to-peer is download optimization only, peers aren't authoritative updater.

Peer discovery mechanism:

Type 1: Local office cache. One machine or service caches updates. Clients download from it after verifying signatures. Simplest.

Type 2: LAN peer discovery. Use mDNS/UDP broadcast to announce cached artifacts. Good for engineering offices.

No chunking of Download binary. We are still very small. Content-Range shall be supported by server anyway. We will Build this in phases:

Installation on windows happens by default (double click by user) on followling location. It  doesn't need any administrative privilege. Since admin access is blocked in most mature enterprise.

"C:\Users\<UserID>\AppData\Local\Programs\Mission Vishwakarma\Vishwakarma.exe"  

However if the installer is provided with a command line option --allUsers than the installation is tried in following folder. If no permission, than installation still happens at above location.

"C:\Program Files\Mission Vishwakarma\Vishwakarma.exe"

A shortcut is always created at the Desktop of the user, and upon completion of installation, application is always launched by default.

Setup has .json file embedded as well, and it overwrites the current installation if current one is older.

We do not support 32bit. All setups or application binaries are 64 bit only.

There is no MSIX installation. It is simply, fully self-contained statically build .exe file. However in future, there will be some CJK fonts, and some .db sqlite database files as well. But not now.

Embedded installer manifest validUntil: 1 year acceptable. root key metadata validUntil: 1 to 3 years acceptable.

The client should persist: lastAcceptedManifestSequence, lastInstalledVersion, lastSuccessfulLaunchVersion, lastRejectedBadVersion

Use named mutexes: Global\MissionVishwakarmaInstallerLock OR Local\MissionVishwakarmaUpdaterLock OR Local\MissionVishwakarmaAppLaunchLock

Rules: Only one installer runs at a time. Only one updater downloads at a time per user. Do not update while another install is in progress. Do not allow two apply-update operations concurrently.

For per-user install, Local\... is enough. For all-users install, use Global\... if permissions allow.

Update module keeps listening on socket connection on any vacant port between 2^14 to 2^14+16. I expect at least 1 port in this range will be free for peer communication & discovery. Event clients will share 8 nearest IPv4 and IPv6 address with each other to facilitate peer discovery.

## Phased Implementation
* Phase 1: Simple secure updater, Signed latest.json, Full ZIP/MSIX installer package, SHA-256 verification , Code-signed binaries, A/B install folder, Manual + automatic check
* Phase 2: Better reliability, Chunked downloads, Resume support, Staged rollout, Rollback after failed launch, Update health telemetry or local logs
* Phase 3: Server-load reduction, LAN cache, Then LAN peer-to-peer, Peers serve chunks by hash only, HTTP fallback ,always available
* Phase 4: Optimization, Delta updates, Component updates, Per-module packages, More TUF-like key rotation

## Implementation Status and Deviations (July 2026)

Phase 1 is implemented in `code-core/SoftwareUpdate.cpp` / `.h` (single file, compiled into both Vishwakarma.exe and VishwakarmaSetup.exe), packaged by `code-miscellaneous/GenerateRelease.ps1` and published by the nightly GitHub Actions workflow. The following deliberate deviations from the design above exist:

**Manifest is fetched from the release URL, not the repository root.** The client downloads `Vishwakarma_release_details.json` (+ `.sig`) from the nightly GitHub release assets. Committing the freshly generated manifest to the repository root from CI would push to `main` and re-trigger the nightly workflow in an infinite loop. The copy at the repository root is a committed snapshot of the last locally packaged release, kept for transparency and as the spec-mandated record.

**Client does not verify Authenticode; SHA-256 + Ed25519 are the security anchor.** Our code signing certificate (MV-CodeSigner-01) chains to our own self-signed root, which is not in the Microsoft trust store, so WinVerifyTrust would always fail on user machines. Binaries ARE Authenticode-signed (with RFC 3161 timestamp), but the updater trusts the Ed25519 signature over the manifest plus the SHA-256 hash of the downloaded setup. The manifest therefore carries `"authenticodeRequired": false`. This flips to true the day we purchase a certificate chaining to the Microsoft trust store.

**Update apply sequence.** The design said: launch `setup --update --no-launch`, then launch the new Vishwakarma.exe. An exiting application cannot launch the new exe after the install finishes (it would have to outlive the install). Instead the application launches the staged setup with `--update` only and exits; the installer swaps the binary and relaunches the new Vishwakarma.exe itself. `--no-launch` remains available for scripted installs.

**No A/B install folder.** A running exe on Windows cannot be overwritten but can be renamed. The installer parks the old binary as `Vishwakarma.exe.old`, writes the new one, and removes the leftover opportunistically. This gives the same safety as A/B folders (the old file is restored if activation fails) with half the disk footprint and no folder-switching logic.

**versionNo = git commit count.** `git rev-list --count HEAD` gives the simple, monotonically increasing positive integer the spec asks for, with zero bookkeeping. Version 0 (json missing next to the exe) marks a developer build and disables the updater entirely.

**Version-free setup filename.** The setup exe ships as `Vishwakarma_UserSetup_win10_win11_x64.exe` with no version suffix, so the nightly release URL (and the website Download button that links to it) never changes. The manifest's `version` field and the executable's own version resource carry the build number; the filename does not. Earlier we uploaded both a `_v<N>` file and this stable alias — the versioned copy has been dropped.

**Version resource in the executable.** `Vishwakarma.exe` and the setup exe both embed a `VS_VERSION_INFO` resource so that Right-click -> Properties -> Details shows the build metadata: the version number (`git rev-list --count HEAD`) in *File version* / *Product version*, and the 8-char commit hash plus the UTC build date in *Product version* and *Comments*. `code-miscellaneous/GenerateVersionHeader.ps1` writes `VersionGenerated.h` into the project `IntDir` during each project's pre-build event; both `.rc` files include it. The header is only rewritten when its contents change, so unchanged same-day builds don't force a resource recompile.

**Signing keys.** Manifest signing uses Ed25519 key `ManifestSigner-01` (`code-miscellaneous/ManifestSigner-01.key`, password-encrypted; public key hardcoded in SoftwareUpdate.cpp). Authenticode uses `MV-CodeSigner-01.pfx`, which is NOT committed (`.gitignore` excludes `*.pfx`); CI receives it as base64 secret `RELEASE_SIGN_PFX_B64` and the shared password as secret `RELEASE_SIGN_PASSWORD`. Unsigned CI builds (secrets absent) produce no `.sig`, and clients correctly refuse to update from them.

Not yet implemented (later phases as planned): peer-to-peer discovery and the 2^14 port listener, chunked/resumable downloads, rollback after failed launch (`lastRejectedBadVersion` is persisted but nothing sets it yet), delta updates, and key rotation.
