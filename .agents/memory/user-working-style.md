---
name: user-working-style
description: "Ram's recurring engineering preferences on Vishwakarma — data-structure, scope, ownership and repo-hygiene rules observed across many sessions"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: a28b47b0-9767-4eb3-a718-4980886e4251
---

Preferences Ram has stated or enforced repeatedly across Vishwakarma sessions (distinct from
CLAUDE.md, which covers style; these are decisions he has actually corrected me on).

**Data structures.** Fixed-size slot arrays + double-buffered index publication over `std::vector`
in hot/shared structures — he explicitly rejected a vector for `DATASETTAB::subTabs[128]`. Growth
strategy when a fixed table must grow: double it, copy on the owning thread, retire the old buffer
behind a fence. See [[multi-window-subtabs]], [[graphics-leak-audit]].

**Scope.** He chooses minimal scope on refactors and says so — when I proposed further restructuring
during the rendering reorg he cut it, and the leftovers stayed undone on purpose. Don't reopen them
as "cleanup". Dead code he hasn't asked about stays. See [[graphics-refactor-phases]].

**Ownership boundaries.** Mapping/lookup data belongs in files he owns and edits, not in C++: the
STD→catalog profile mapping was moved to `profile_mapping.py` at his instruction, with C++ doing
only designation→id lookup. Same instinct behind CSV catalogs + a pre-build embedder rather than
hardcoded tables. See [[line-member-object]], [[steel-section-catalog]].

**Deletions are his.** When a file becomes obsolete (e.g. `Catalog/entity-id-design.md`) he wants it
flagged, not deleted. Applies to dead code too.

**Preserve his text VERBATIM when implementing from it - do not summarise it into your own prose
and call that preservation.** Implementing `Optional64` (2026-08-23) I rewrote his specification
comments in my own words, deleted the ~130-line `class DerivedClass` sketch, the discarded
`ByteArrayData` pointer variant, the "Bfloat16 for AI!" note and the closing review instructions -
then reported that the spec "is preserved". He noticed from the diff size and pushed back: "I hope
nothing of importance was removed." Five things had been: a single-writer/multiple-reader threading
contract, complexity targets, a field-reordering licence, and two decision records - none of which
any line of code expressed. Fix that worked: append the original verbatim as a `//`-commented
appendix, implementation first, and verify mechanically that every original non-blank line still
matches. Same instinct as the 2D structs he told me to keep and annotate rather than delete.
Corollary: a large diff on a file that is mostly HIS comments is itself the warning sign.

**Accuracy can be deliberately deferred.** Draft data (steel section dimensions) shipped knowingly
unverified because he wanted the pipeline first. Don't treat DRAFT as a bug to fix unprompted — but
don't let it be forgotten either.

**Repo hygiene he cares about.** Clean `git status` before commits — the OpenSSL submodule's nested
test submodules drift dirty on every External rebuild and must be reset, not committed. Builds stay
warning-free; warnings from vendored code get silenced at the project-file level, never with `/wd`
flags that would blind our own code. Vendoring an amalgamation beats a git submodule when it saves
CI time (PocketPy, SQLite-style). See [[commandline-build]], [[extension-system-mvp]].

**No runtime `assert()` anywhere in the codebase** (verified 2026-08-23: every hit is
`static_assert`). Runtime invariants are checked with `#ifdef _DEBUG` + a tagged `std::cout`, e.g.
`[gpu][warn] tab ... retire backlog=`, `[cad2d][dbg] OUTLIER AT INGEST`. Follow that rather than
introducing `assert` - Ram asked for "the sortedness assert" and the right answer was still the
house diagnostic, flagged to him as a deviation from his literal wording. The repo's
`codeconventions.md` does NOT document this, so it is only discoverable by grepping.

**Verification.** He wants changes actually exercised, not just compiled — standalone driver scripts
for workers, env-var auto-import hooks for the GUI, screenshot+click for UI. He does the final
in-app confirmation himself and reports back.

**Design docs: don't close doors he hasn't closed.** When he hasn't settled a modality, he wants it
written into the doc as an explicit OPEN section listing every option with trade-offs — not resolved
by me and not silently omitted. Said verbatim while answering the undo/redo architecture question
("I am yet to decide complete modality. Keep this option open for debate with listing all other
options/tradeoffs in design document"). Locked decisions and open ones must be visually distinct so
an implementer can tell which is which. See [[undo-redo-design]].

**Design docs: but once it IS closed, compress it hard.** The complement of the rule above, and he
asked for it explicitly on 2026-08-26 ("We can remove the parts which have now been superseded. I
want the size to be reduced... Don't need step wise history. Let us club things what has already
been accomplished"). Applied to id.md 1273→585 lines and 2Drendering.md 719→347. What he wants
deleted: superseded speculation, per-sub-step chronology, options already rejected, and any design
narrative now duplicated in another doc. What survives: current state, the decisions that must not
be re-litigated, the constraints on future work, and unbuilt plans in full. Git holds the history;
the doc holds what is true. Two practical consequences - deleting a section means RENUMBERING and
re-pointing every `§n` reference across every doc (grep site-wide, they cross-reference each
other), and a doc whose design has moved elsewhere should point at it rather than restate it.

**Correctness over convenience for engineering data.** Given the choice for collaborative undo, he
picked version-guarded rejection ("refuse and tell the user who changed it") over both silent
last-writer-wins and inverse-delta. The reasoning that landed: never produce a value no engineer
chose, and never silently discard a peer's work. Expect the same instinct on other conflict
questions.
