---
name: cpython-static-worker
description: "VishwakarmaExtension.exe (frozen static CPython worker) IMPLEMENTED 2026-07-06 — build pipeline, hardening tricks, gotchas"
metadata: 
  node_type: memory
  type: project
  originSessionId: d5460b5d-d86a-415d-b7a2-365e5ae750cc
---

Frozen-CPython extension worker implemented 2026-07-06 (replaces system `python -u main.py`; fixes user's install-machine failure "google proto library missing"). Extends [[extension-system-mvp]].

**Pieces:** code-external/cpython = git submodule pinned at v3.13.14 (shallow-cloned). code-miscellaneous/BuildCPython.ps1 builds into build\cpython-x64-release (python313.lib /MT static + include\pyconfig.h + frozen_modules\*.h + generated vk_frozen_modules.c, 234 modules; skip-if-exists, -Force to rebuild). code-core/VishwakarmaExtension.{cpp,rc,vcxproj} = dedicated exe project (~11 MB, both Debug and Release configs build the identical optimized /MT binary; auto-runs BuildCPython.ps1 via BeforeTargets when lib missing). Host spawn switched in ExtensionCommunications.cpp. Setup: RCDATA 207 in VishwakarmaSetup.rc + InstallExtensionWorker() in SoftwareUpdate.cpp (rename-dance like InstallPayload). GenerateRelease.ps1 step 1b builds+signs worker before setup embed. nightly.yml: cache-cpython actions/cache on build/cpython-x64-release keyed on patchlevel.h/script/props/protobuf-python/zlib.

**Key build tricks (hard-won):**
- pythoncore.vcxproj converts to static lib via global props: /p:ConfigurationType=StaticLibrary /p:WholeProgramOptimization=false /p:ForceImportBeforeCppTargets=CPythonStatic.props /p:zlibDir=<repo>\code-external\zlib. The props sets RuntimeLibrary=MultiThreaded + ForcedIncludeFiles=CPythonStaticForceInclude.h (which #undef Py_ENABLE_SHARED / #define Py_NO_ENABLE_SHARED). DO NOT use MSBuild property functions ($([System.String]::Copy(...).Replace(...))) in item-definition metadata — the output gets MSBuild-escaped (';'→%3B) and the whole define list glues into one bogus /D silently.
- CPython's python.props maps VS2026 (18.0) to v143 which isn't installed; BuildCPython.ps1 detects installed MSVC (14.5x→v145, else v143) and passes /p:PlatformToolset.
- _freeze_module.vcxproj build also regenerates the standard frozen headers into PCbuild\obj\313_frozen; pyconfig.h generated into PCbuild\obj\313amd64_Release\pythoncore. Submodule stays git-clean (upstream ignores PCbuild/amd64+obj).
- protobuf python runtime frozen from code-external/protobuf/python/google (v35.1, runtime 7.35.1); two modules are NOT checked in upstream and must be generated: well-known *_pb2.py via protoc --python_out, and google/protobuf/internal/python_edition_defaults.py via protoc --edition_defaults_out=... --edition_defaults_minimum=PROTO2 --edition_defaults_maximum=2024 + \xNN-escaping the blob into the .py.template (mirrors bazel embed_edition_defaults).
- Frozen table: extra modules go into `const struct _frozen VkExtraFrozenModules[]` assigned to PyImport_FrozenModules pre-init (checked BEFORE the baked stdlib table → can override/disable baked entries). Symbol naming: _Py_M__ + name with '.'→'_'. Baked-in set (os, io, codecs, ntpath, site, importlib.*, ...) must NOT be re-frozen (duplicate C symbols). Needed non-obvious freezes: _opcode_metadata (dis chain), pkgutil (google ns __init__), importlib + importlib._abc (not baked), _colorize, winreg raise-on-use stub (importlib._bootstrap_external imports winreg unconditionally on Windows).
- Worker config: PyConfig_InitIsolatedConfig + explicit site_import=0 (isolated does NOT disable site! caused "sys has no attribute winver" fatal), buffered_stdio=0, write_bytecode=0, parse_argv=0, module_search_paths_set=1 with only the script dir.
- nt hardening: post-init inittab edits DON'T work (runtime copies inittab at init). Working approach: inittab entry "nt" → PyInit_vk_nt wrapper that copies posixmodule's PyModuleDef and appends a Py_mod_exec slot deleting system/spawnv/spawnve/execv/execve/startfile/kill/_add_dll_directory/_remove_dll_directory — survives del sys.modules['nt'] + re-import. Builtins dropped from inittab: _winapi, winreg, mmap, msvcrt, _lsprof, xxsubtype, _interpreters/_interpchannels/_interpqueues.

**Testing:** scratchpad vk_e2e.py drives worker exactly like the host (hand-encoded protobuf wire format, no pip deps): STD import 19 nodes/44 members OK, DXF 13757 elements OK. Smoke: socket/ctypes/subprocess/_winapi ImportError, os.system/spawnv/startfile absent, nt reimport scrubbed, winreg stub raises.

**Open items:** AppContainer sandbox still next; PSF + protobuf BSD license texts not yet shipped with the installer (flagged to user).
