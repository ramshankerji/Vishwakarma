---
name: release-signing-setup
description: "Where Vishwakarma's code-signing and manifest-signing keys live and which passwords open what"
metadata: 
  node_type: memory
  type: project
  originSessionId: 3d9949f0-f8e5-4042-b282-964b2b5d55c4
  modified: 2026-07-21T01:36:51.897Z
---

Release signing for Vishwakarma (implemented July 2026):
- `code-miscellaneous/MV-CodeSigner-01.pfx` (self-signed code-signing cert, chains to `RootCA-MV-01.crt`) is used by signtool. It is NOT in git (`.gitignore` has `*.pfx`); CI gets it via base64 secret `RELEASE_SIGN_PFX_B64`.
- SECURITY CAVEAT (confirmed 2026-07-21 via `git ls-files`): the ENCRYPTED private-key PEMs `MV-CodeSigner-01.key`, `ManifestSigner-01.key`, `RootCA-MV-01.key` ARE tracked in the public repo. Only the `.pfx` is gitignored. So the update trust chain rests solely on the passphrase against an offline attack — flagged P0 in [[security-hardening-roadmap]] (website security.md): rotate + purge from history.
- The standalone `MV-CodeSigner-01.key` / `RootCA-MV-01.key` files use a DIFFERENT password than the PFX; nothing in the release pipeline needs them.
- `code-miscellaneous/ManifestSigner-01.key` is the Ed25519 key signing `Vishwakarma_release_details.json`; encrypted with the SAME password as the PFX. Its public key PEM is hardcoded in `code-core/SoftwareUpdate.cpp`.
- One password covers PFX + Ed25519 key; CI secret name: `RELEASE_SIGN_PASSWORD`. Never store the password itself anywhere.
- `code-miscellaneous/GenerateRelease.ps1 -Mode Package` is the whole release pipeline (build → sign app exe → build setup embedding it → sign → manifest + .sig).
