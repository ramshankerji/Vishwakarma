---
title: "Steel Section Tables"
weight: 50100
---

Every structural steel design starts from a rolled-section table. This page is the design
document for how Mission Vishwakarma catalogs hot-rolled **I sections**, **Channel sections**
and **Angle sections** from all major international standards — for users who want to know
what is available, and for developers who maintain the catalog files.

> **Status: DRAFT.** The dimension values currently in the catalog CSVs were populated from
> memory to establish the schema and coverage. Every number must be proof-checked against
> the governing standard before release.

## 1. Design decisions

1. **One CSV file per profile family**, not per code, named `profiles_hot_<family>.csv`
   (in the `Catalog/` folder of the repository): `profiles_hot_i.csv`, `profiles_hot_c.csv`,
   `profiles_hot_l.csv`, plus the roadmap families in §6. "Country" and "Code" are just
   columns. This keeps the geometry generator simple: one family = one cross-section
   topology. The `_hot` infix means hot-rolled / hot-finished; cold-formed sections will
   arrive later as a separate `profiles_cold_*` set with their own schema.
2. **Every row carries a permanent 64-bit catalog ID** drawn at random from the catalog band
   `[2^32, 2^40)`. Once released, an ID means the same physical section forever. Rows are
   never deleted, only superseded (`status` / `superseded_by` columns). IDs are allocated
   only through `catalog_editor_v2.py`, never by hand.
3. **We store defining geometry only, not derived section properties.** Area, Ix, Iy, Zx,
   Zpl, rx, ry, J, Cw, centroid and shear-centre positions are all computed by the
   application from the geometry columns. Tabulated properties across codes use
   inconsistent rounding and fillet treatment; computing them ourselves keeps every profile
   self-consistent with the outline we actually draw. The one exception is **mass per
   metre**, which is stored as tabulated — it is the identity check against the source code
   and absorbs density-convention differences (7850 vs 7860 kg/m³).
4. **Units:** millimetres and kg/m everywhere. Imperial sections (US W12X26 etc.) are stored
   in mm with the original designation kept as the display name and the metric designation
   in `alt_designation`.
5. **Rebadged series get their own rows.** Several codes adopt identical geometry (Indian
   NPB ≡ European IPE, Indian WPB ≡ HE B, Canadian W ≡ hard-metric US W, Brazilian W ≡ US W,
   Australian UB descends from British UB). Each country/code combination gets its own row
   and its own ID; `alt_designation` records the equivalence. Duplication is cheap; a
   cross-code aliasing layer is not.

## 2. International codes covered

| # | Country/Region | Code(s) | Covers |
|---|---|---|---|
| 1 | India | **IS 808:2021** (absorbed IS 12778 NPB/WPB); IS 1852 tolerances | I, C, L |
| 2 | USA | **ASTM A6/A6M** + AISC Shapes Database | I, C, L |
| 3 | Canada | **CSA G40.20/G40.21** + CISC Handbook (metric W, welded WWF) | I, C, L |
| 4 | UK (legacy, still rolled) | **BS 4-1:2005**; BS EN 10056-1 angles | I, C, L |
| 5 | Europe | **EN 10365:2017** (consolidated continental + British I/H/U series; replaced DIN 1025-1…5, DIN 1026-1/2, NF A 45-255, Euronorm 19-57/53-62); **EN 10056-1** angles | I, C, L |
| 6 | Japan | **JIS G3192:2021** | I(H), C, L |
| 7 | South Korea | **KS D 3502** (mirrors JIS with additions) | I(H), C, L |
| 8 | China | **GB/T 706-2016** (tapered I, channels, angles); **GB/T 11263-2017** (parallel-flange H) | I, C, L |
| 9 | Russia / CIS | **GOST 8239-89** (tapered I); **GOST R 57837-2017** / GOST 26020-83 (parallel-flange H); **GOST 8240-97** (channels); **GOST 8509-93** / **GOST 8510-86** (equal / unequal angles) | I, C, L |
| 10 | Australia / NZ | **AS/NZS 3679.1** (rolled); **AS/NZS 3679.2** (welded WB/WC) | I, C, L |
| 11 | Brazil | **ABNT NBR 15980** (rolled, AISC-derived); NBR 5884 (welded VS/CS/CVS) | I, C, L |
| 12 | ISO (paper standard, rarely rolled) | **ISO 657** series | I, C, L |

Everything else (Middle East, South-East Asia, Africa, Latin America) adopts one of the
above — no unique geometry.

## 3. Shape families per code (the `series` column)

### I profiles — `profiles_hot_i.csv`

| Country | Series | Notes |
|---|---|---|
| India | ISJB, ISLB, ISMB, ISWB, ISHB, ISSC, **NPB**, **WPB** | JB/LB/MB/WB/HB tapered-flange; NPB ≡ IPE, WPB ≡ HE (parallel) |
| Europe | **IPN** (tapered), **IPE** (+ IPE A/O), **HEA, HEAA, HEB, HEM**, HL, HLZ, HD, HP | HP = bearing pile |
| UK | **UB, UC, UBP, J** | J = joists, tapered, legacy |
| USA | **W, S** (tapered), **M, HP** | |
| Canada | W, WWF (welded), M, S, HP | |
| Japan / Korea | **H** (wide / middle / narrow flange series) | |
| China | **I** (工字钢, tapered, a/b/c web variants), **HW, HM, HN, HT** | |
| Russia | **I** (GOST 8239, tapered); **B** (normal), **Sh** (wide), **K** (column), **D** (additional) | Б/Ш/К/Д |
| Australia | **UB, UC, TFB** (tapered, legacy), WB, WC (welded) | |
| Brazil | W, HP (rolled); VS, CS, CVS (welded) | |

### Channel profiles — `profiles_hot_c.csv`

| Country | Series |
|---|---|
| India | ISJC, ISLC, ISMC (tapered), **ISMCP** (parallel) |
| Europe | **UPN** (tapered), **UPE** (parallel), **U** (small DIN series), **UAP** (French parallel) |
| UK | **PFC** (parallel), CH (tapered, legacy) |
| USA / Canada | **C** (tapered, 1:6 slope), **MC** |
| Japan / Korea | **C** (tapered inner flange) |
| China | **C** (槽钢, tapered, a/b/c variants) |
| Russia | **U** (tapered), **P** (parallel), E (economy), L (light) — GOST 8240 |
| Australia | **PFC**, TFC (tapered, legacy) |

### Angle profiles — `profiles_hot_l.csv`

| Country | Series |
|---|---|
| India | **ISA-E** (equal), **ISA-U** (unequal) |
| Europe / UK | **L-E**, **L-U** (EN 10056-1) |
| USA | **L-E**, **L-U** |
| Japan | L-E, L-U, **L-UT** (unequal leg *and* unequal thickness — two thickness columns) |
| China | L-E (等边), L-U (不等边) |
| Russia | L-E (GOST 8509), L-U (GOST 8510) |
| Australia | **EA**, **UA** |

Deliberately excluded: bulb flats / bulb angles (shipbuilding — a different family), and
cold-formed sections (a future, separate catalog).

## 4. Parameters (the CSV columns)

### Identity and classification — all three files

| Column | Meaning |
|---|---|
| `id` | permanent catalog ID, hex, in `[2^32, 2^40)`; never edited by hand |
| `key` | unique human-readable key `CODE:DESIGNATION`, e.g. `IS808:ISMB 400` — files are sorted by it, which clubs rows country/code-wise |
| `status` | `active` or `superseded` |
| `superseded_by` | ID of the replacement row, if superseded |
| `country` | country / region of the standard |
| `code` | governing standard, e.g. `IS 808:2021`, `EN 10365`, `ASTM A6 / AISC` |
| `series` | type classification within the code: `ISMB`, `W`, `IPE`, `UPN`, `EA`, … |
| `designation` | full canonical name: `ISMB 400`, `HE 200 B`, `W12X26`, `20B1` |
| `alt_designation` | alias in another system (`W12X26` ↔ `W310X38.7`, `NPB 200` ↔ `IPE 200`); blank otherwise |
| `availability` | `current` (still rolled) or `legacy` (obsolete, kept for old drawings and retrofit work) |

### Geometry — I profiles and Channel profiles (identical column set)

| Column | Unit | Meaning |
|---|---|---|
| `h` | mm | overall depth |
| `b` | mm | flange width |
| `tw` | mm | web thickness |
| `tf` | mm | flange thickness; for tapered flanges, at the standard gauge point `(b − tw)/4` |
| `r1` | mm | root (web-to-flange) fillet radius |
| `r2` | mm | flange toe radius — tapered series only; 0 for parallel-flange sections |
| `flange_slope` | % | inner-flange taper: 0 = parallel; 14 = IPN; 16.67 = American S/C; etc. This single column distinguishes tapered from parallel — no separate flag |
| `mass` | kg/m | tabulated mass per metre (identity check, see §1.3) |

### Geometry — Angle profiles

| Column | Unit | Meaning |
|---|---|---|
| `a`, `b` | mm | leg lengths (`a = b` for equal angles — no separate flag needed) |
| `t` | mm | thickness |
| `t2` | mm | second thickness — only for the JIS unequal-thickness series; blank otherwise |
| `r1` | mm | root fillet radius |
| `r2` | mm | toe radius (EN rounds toes; AISC effectively 0) |
| `mass` | kg/m | tabulated mass per metre |

### What is deliberately not stored

- **Section properties** (A, Ix, Iy, Zx, Zpl, rx, ry, J, Cw, centroid, shear centre) —
  computed by the application from geometry.
- **Detailing data** (AISC `k`/`k1`, bolt gauge lines `g`) — derivable or workflow-specific;
  can be added as columns later without breaking anything.
- `d` (clear web depth between fillets) — derivable: `h − 2·tf − 2·r1` for parallel flanges.

## 5. Implementation notes for developers

- The catalog lives in `Catalog/` and is edited only through `Catalog/catalog_editor_v2.py`,
  which enforces the invariants: locked `id`/`status`/`superseded_by` columns, random
  draw-and-check ID allocation across *all* files, supersede-never-delete.
- The fiddly part of outline generation is the **tapered flange**: each convention family
  (American 1:6, European IPN, Indian IS, GOST) defines the gauge point for `tf` and the
  toe treatment slightly differently. Implement one tapered-flange construction per
  convention family, driven by `flange_slope`/`r2`, not one per code.
- A geometry hash test should eventually verify that rows claiming equivalence via
  `alt_designation` (NPB ↔ IPE, Canadian W ↔ US W) actually carry identical numbers.

## 6. Roadmap: other shape families

The codes above standardize more than I/C/L. The following families are **in scope** and
already have scaffolded CSVs (schema fixed, a few DRAFT seed rows each; population pending).
All share the identity columns of §4 and differ only in geometry columns:

| File | Family | Geometry columns | Sources |
|---|---|---|---|
| `profiles_hot_t.csv` | **Rolled tees** | `h, b, tw, tf, r1, r2, flange_slope` (same set as I/C) | EN 10055 T; IS 808 / IS 1173 ISNT/ISJT/ISLT/ISST/ISHT |
| `profiles_hot_rhs.csv` | **SHS + RHS**, hot-finished (SHS is just RHS with `h = b`) | `h, b, t, r_out, r_in` | EN 10210; IS 4923 (HF grades); ASTM A501; GOST 8639/8645; JIS G3466 |
| `profiles_hot_chs.csv` | **CHS**, hot-finished round hollow | `d, t` | EN 10210; IS 1161 (NB tubes); ASTM A501; GOST 8732 |
| `profiles_hot_bar.csv` | **Bar stock** — `series` doubles as the shape: FLAT (`a`=width, `b`=thickness), ROUND (`a`=dia), SQUARE (`a`=side), HEX (`a`=across flats) | `a, b` | EN 10058/10059/10060/10061; IS 1730/1731/1732; GOST 103/2590/2591; ASTM A6 bars |
| `profiles_hot_bulb.csv` | **Bulb flats** (ship/offshore stiffeners) | `h, t, bulb_h, r1` | EN 10067 HP; IS 1863 |
| `profiles_hot_rail.csv` | **Crane & railway rails** — envelope dims only; the full head/web/foot outline is a per-series construction in code, not CSV data | `h, b_head, b_foot, tw` | DIN 536 A-series; EN 13674 (60E1…); ASCE/AREMA; IS 3443 CR |

**Split tees are not stored.** WT/MT/ST (AISC), CT (JIS), TW/TM/TN (GB), BT/CT (AS) are half
of a parent I section; the application derives them from `profiles_hot_i.csv` at runtime.
Only genuinely rolled tees earn rows in `profiles_hot_t.csv`.

Note on CHS: structural CHS overlaps the piping catalog (pipes have their own component
pipeline). `profiles_hot_chs.csv` holds *structural* hollow sections only; a pipe used as
a pipe stays a piping entity. The same physical size may legitimately exist in both worlds
with different IDs — that is intentional, they are different products with different
tolerance and material standards.

**Deferred** (standardized, but not scheduled):

- **Bulb angles** (IS 1252, legacy ASTM) — nearly extinct.
- **Sheet piles** (EN 10248, ASTM A857, GOST 4781) — interlock geometry is vendor-specific
  (ArcelorMittal AZ/AU/GU…); more a manufacturer catalog than a code table.
- **Rolled Z sections** — effectively dead as hot-rolled.
- **Double angles / built-up combos** (AISC 2L, star angles) — combinations of catalog
  items; belongs in the modeling layer, never in these files.
- **Cold-formed thin-gauge** (lipped C/Z purlins, sigma, top-hat — IS 811, AISI S100,
  EN 10162, AS/NZS 4600) — different parameter world (base thickness + bend radii + lips)
  and largely manufacturer-specific; will get its own `profiles_cold_*` design pass.
