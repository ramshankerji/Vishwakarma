---
name: devanagari-source-editing
description: "Scripted edits to Vishwakarma sources must be UTF-8 aware — Devanagari filenames and content double-encode silently; also the repo's line-ending policy (LF via .gitattributes since 2026-08-22, was CRLF)"
metadata: 
  node_type: memory
  type: project
  originSessionId: 04967110-ac87-4666-b641-6320e3c76b1a
  modified: 2026-08-22T17:56:36.203Z
---

Vishwakarma has Devanagari **filenames** (`डेटा.h`, `विश्वकर्मा.cpp`, `डेटा-सामान्य-3D.h`, …) and Devanagari
inside comments and `#include` lines, and CLAUDE.md mandates UTF-8 source encoding. Any scripted
multi-file edit has to respect that.

**The trap (hit 2026-08-01, cost a build cycle).** `perl -0pi -e 's/.../.../'` reads bytes as
latin-1 by default. If the *replacement string* contains any character above U+00FF, perl upgrades
the output handle to UTF-8 and re-encodes the ENTIRE buffer — double-encoding every existing
multi-byte sequence in the file, including the UTF-8 BOM. MSVC then fails with

```
error C3872: 'U+bb': this character is not allowed in an identifier
error C1083: Cannot open include file: 'à¤¡à¥‡à¤Ÿà¤¾.h'
```

because it is reading the file as the ANSI codepage. The giveaway is `à¤` sequences in the source
and a leading `c3 af c2 bb c2 bf` instead of `ef bb bf`.

**Fixes, in order of preference:**
1. Keep the replacement **pure ASCII** — refer to "the base data header" rather than pasting `डेटा.h`.
2. If non-ASCII is genuinely needed, use `perl -CSD -0pi` so input and output are both UTF-8.
3. Prefer the Edit tool over scripted regex for anything touching these files.

**`iconv -f UTF-8 -t UTF-8` does NOT detect this.** Double-encoded UTF-8 is still *valid* UTF-8 —
it just means something else. Real checks:

- BOM intact: `head -c 3 file | od -An -tx1` → `ef bb bf` (only some files carry one; compare with
  `git show HEAD:path`).
- No mojibake: `grep -c 'à¤' file` → 0.
- Diff is additions-only: `git diff --numstat -- file` (deletions on lines you did not touch mean the
  whole file got rewritten).

**Line endings — CHANGED 2026-08-22, the two paragraphs that used to be here are now wrong.**
The repo has a `.gitattributes` (`* text=auto eol=lf`) and the working tree is **LF**, not CRLF.
`core.autocrlf=true` still exists but is in the *system* gitconfig
(`Programs/Git/etc/gitconfig`) — not user, not repo — and `.gitattributes` overrides it here.
So a perl pattern spanning multiple lines now matches on plain `\n`; do **not** write `\r\n`
separators, they are what silently matches nothing today. The old advice was the exact inverse.

Three exceptions keep non-LF bytes, deliberately: `*.bat`/`*.cmd` are pinned `eol=crlf`
(cmd.exe mis-parses LF-only labels and multi-line `goto`), and `code-core/Vishwakarma.rc` is
pinned `-text` because it is **UTF-16LE** — marking it, or `*.rc` as a class, as `text` shreds it.
The other two `.rc` files are ASCII and normalize fine.

Why it was done: agent edit tools write LF lines, and into a CRLF file that yields a mixed-EOL
file whose diff renders as a whole-file rewrite. Five files besides the one that triggered the
investigation were already mixed. Migration was free because the index had always been 100% LF
(`git ls-files --eol` showed zero `i/crlf`), so `git add --renormalize .` was a no-op commit.
Recipe gotcha: `git checkout-index -a -f` does **not** re-materialize the working tree — it takes
git's "stat matches index" fast path and silently skips every file. Only
`git rm --cached -r . && git reset --hard` (on a clean tree) actually converts it; that leaves
submodule SHAs untouched, verified against `git submodule status`.

**Still true: use the Edit tool for anything multi-line** (it normalises), keep perl for
single-line substitutions, and always re-grep for the pattern you thought you removed rather than
trusting the exit code — a silent no-match looks identical to success.

Related: [[commandline-build]], [[user-working-style]].
