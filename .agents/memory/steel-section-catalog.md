---
name: steel-section-catalog
description: "Catalog/ steel section CSVs (i/c/l_profiles) + SteelTable.md design doc; DRAFT data from memory, proof-check pending"
metadata: 
  node_type: memory
  type: project
  originSessionId: eb816fb8-de51-4e7d-ba72-e5b54bbe8a26
---

Steel section catalog (July 2026): `Catalog/profiles_hot_<family>.csv` — one CSV per profile
family, country/code/series are columns. Main tables: `_i` (243 rows), `_c` (89), `_l` (129).
Scaffolded with seed rows: `_t` (rolled tees only — split tees derived from _i at runtime),
`_rhs` (SHS+RHS one file), `_chs` (structural only — piping stays a separate entity world,
same size may exist in both with different IDs by design), `_bar` (series doubles as shape
FLAT/ROUND/SQUARE/HEX), `_bulb`, `_rail` (envelope dims only). Cold-formed deferred to
future `profiles_cold_*`. 9 files, 508 ids total.
Design doc: `website/content/civil/SteelTable.md`. Editor: `Catalog/catalog_editor_v2.py`
(defaults to its own folder; enforces immutable random IDs in [2^32, 2^40), supersede-never-delete).

Key decisions: store defining geometry + tabulated mass only (section properties computed in app);
tapered vs parallel encoded via `flange_slope` %; rebadged series (NPB≡IPE, WPB≡HE, CDN W≡US W)
get duplicate rows with `alt_designation`; JIS unequal-thickness angles use extra `t2` column;
`key` = `CODE:DESIGNATION` and files sort by it (clubs rows code-wise).

2026-07-17: catalog now EMBEDDED into the binary — `code-miscellaneous/steel_profile_embedder.py`
(pre-build hook in Vishwakarma.vcxproj, both configs) validates all 9 CSVs (id band/uniqueness,
per-family required dims, status/availability vocab) and emits
`$(IntDir)SteelProfileCatalogEmbedded.generated.h`, included by checked-in
`code-core/SteelProfileCatalog.h` (SteelProfileFamily enum, SteelProfileRecord union-of-columns
struct in mm, id-sorted array + FindSteelProfileById binary search). Consumed by [[line-member-object]].

**All dimension values are DRAFT from Claude's memory — user explicitly deferred accuracy;
proof-checking against the actual standards is a pending task.** ID scheme finalized 2026-07-17
in the "Update ( July 2026 )" section appended to `website/content/software/id.md` (now the sole
authoritative doc): [0, 2^32) permanently invalid; catalog band [2^32, 2^40); top 16 bits stay
reserved-zero (2^48 cap); user/server allocation stays SEQUENTIAL (random user-band idea rejected);
bit 63 always 0 (SQLite sign bit); bit 62 = external-file-reference flag → persisted IDs < 2^62.
`Catalog/entity-id-design.md` discarded by user (deletion is theirs to do). id.md update point 5
records the allocation split: catalog band = RANDOM draw-and-check (avoids dev merge conflicts on
a shared counter, matches catalog_editor_v2.py) vs user IDs = SEQUENTIAL from central server
(disc/RAM cache locality).
