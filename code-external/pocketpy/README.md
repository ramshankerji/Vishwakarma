# PocketPy (vendored amalgamation)

PocketPy is a lightweight Python 3.x interpreter in C, used by Vishwakarma as
the **in-process** engine for internal user scripting (parametric templates and
custom block definitions). External extensions use out-of-process CPython
instead; see `website/content/software/extensions.md`.

## Why the amalgamated source is committed directly (not a submodule)

Unlike our other C/C++ dependencies, PocketPy is vendored as its **single-file
amalgamation** (`pocketpy.c` + `pocketpy.h`), committed directly into this
folder rather than pulled as a git submodule. PocketPy ships an `amalgamate.py`
script for exactly this embedding style (the same approach SQLite uses).

This keeps CI fast and self-contained: no submodule clone of PocketPy's full
history/source tree, no Python amalgamation step in the build, and the exact
reviewed bytes are what get compiled. The amalgamated source is portable C —
one file compiles on Windows, macOS and Linux; platform differences are handled
by `#ifdef`s resolved by each compiler.

## Provenance

- Upstream: https://github.com/pocketpy/pocketpy
- Version: **v2.1.8**
- Commit: `a2f16e5f1f5fcc3b3d2cc1bdacfc3a027dfe2d76`
- Generated with the upstream `amalgamate.py` (which runs `prebuild.py` to embed
  the Python standard library, then merges `src/` + `include/` into one file).

## Upgrading

1. Clone the upstream repo at the desired release tag.
2. Run `python amalgamate.py` from its root.
3. Copy `amalgamated/pocketpy.c` and `amalgamated/pocketpy.h` over the files
   here, and update the version/commit above.
4. Rebuild and re-run the in-process VM smoke test.

## Build / hardening flags

`VishwakarmaExternal.vcxproj` compiles `pocketpy.c` with:

- `PK_ENABLE_OS=0` — compiles out PocketPy's filesystem/OS module (capability
  hardening; internal scripts get no filesystem access).
- `PK_ENABLE_THREADS=0` — scripts run single-threaded on the engineering thread;
  also drops PocketPy's C11 `<stdatomic.h>` dependency (which MSVC gates behind
  an experimental flag).

`LICENSE` is PocketPy's upstream MIT license, reproduced verbatim.
