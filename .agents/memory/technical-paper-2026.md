---
name: technical-paper-2026
description: "EIL FY2026-27 technical paper — delivered 2026-07-15; what was written, the measured project stats behind it, and why the 2024 entry lost"
metadata:
  node_type: memory
  type: project
  originSessionId: 3c5ca973-88c4-41cb-a825-5b03c345335c
---

Ram's entry for EIL's internal technical-paper competition (FY 2026-27), about Mission Vishwakarma.

**Delivered 2026-07-15** into `build/` at the repo root:
`MissionVishwakarma_TechnicalPaper_FY2026-27_DRAFT.md` (~1,940 body words, 7 sections, 3 inline
tables, appendix chart-data tables) + `Figure1_OpenSourceLeverage_Pie.svg` +
`Figure2_OwnCode_Growth.svg` (render-verified headless; orange #eb6834 sliver / blue #6da7ec mass).
Figures are referenced by placeholder text — Ram embeds them in Word himself. Known open gap: the
Table-1 FY26-27 column is projected from FY24-25 GEM contract values; swap in actuals if he supplies
them.

**Measured project stats as of 2026-07-15** (hard to re-derive, worth keeping): 229 commits
Nov 2022→Jul 2026. Own code ≈37.8k LOC (code-core 30,244; extensions 4,415; tooling 2,312; server
869) vs dependencies ≈4.652M LOC (cpython 1.90M, openssl 1.18M, protobuf 672k, sqlite 322k,
freetype 258k, libpng 92k, DX-Headers 82k, zlib 45k, lunasvg 38k, pocketpy 36k, msdf 18k, fast_float
14k) → ~123:1 leverage. Cumulative own-LOC by month: Dec-25 3,684 / May-26 8,928 / Jun-26 18,013 /
15-Jul-26 32,605. Commits 27/44/41 in May/Jun/half-Jul 2026. AI-tool timeline used in the paper:
Codex first commit 2026-05-08, "Page2D MVP by Codex in 1 night" 2026-06-26, Fable Tasks 1-9 Jul 2026,
Gemini icons, ChatGPT 2D spec. Licensing today: source-available; AGPLv3 intended after EIL approval.
§7 carries Ram's wild estimate: own code stays <5% of shipped lines even at mega-project maturity.

**Why the FY2024-25 entry (build/PreviousPaper.txt) won nothing** — the failure modes to keep
avoiding in any EIL-facing writing: it read as a proposal rather than results; informal tone; GEM
contract disclosure optics; a legal-heavy ask; unsupported extrapolations; no methodology/results
structure. Ram's counter-choices for this one: neutral "this R&D" voice with no biography, ~2000
words, and — overriding my aggregate-only recommendation — a FULL vendor-wise cost table like the
2024 paper.

Related: [[telemetry-system]], [[extension-system-mvp]], [[user-working-style]].
