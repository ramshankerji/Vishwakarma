---
name: useredits-design
description: "Wiki-style website edit proposals — DESIGNED ONLY (useredits.md); CM6 vendored, per-proposal branches, bot-created PRs"
metadata: 
  node_type: memory
  type: project
  originSessionId: 88819e92-c35d-49ce-8302-a730028b7b93
  modified: 2026-07-20T19:31:18.815Z
---

`website/content/software/useredits.md` (weight 100210, written 2026-07-21) designs wiki-style
[ edit ] links on every website section: JS bolt-on + Hugo RawMd output format (`<permalink>/index.md`
serves `.RawContent` same-origin) + new Django `edits` app (`POST /api/edits`) that splices the
section server-side and creates a PR via GitHub REST — no local clone, authenticated as the
[[extension-system-mvp]]-unrelated `vishwakarma-bot` GitHub App from contribution.md (website/-only
PRs need no DCO). DESIGNED ONLY — nothing implemented.

Key decisions: CodeMirror 6 vendored as one committed bundle (WYSIWYG rejected: re-serialization
pollutes PR diffs); heading↔source mapping by index with text-equality guard; Ram's literal single
`website-edit-proposals` branch pushed back to namespaced `website-edit-proposals/<NNNN>` per-proposal
branches (open question §12, awaiting his call). Ties into [[login-design]] for future signed
attribution.
