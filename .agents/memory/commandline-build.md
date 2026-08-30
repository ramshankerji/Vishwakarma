---
name: commandline-build
description: "How to build Vishwakarma from the command line on this machine, and the two project-file mechanisms that keep the build warning-free"
metadata:
  node_type: memory
  type: project
  originSessionId: 075934ed-0938-4e81-8b74-68b852a5fd30
  modified: 2026-08-01T06:42:29.818Z
---

To verify the app compiles (VS 18 Community MSBuild at
`D:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe`):

`MSBuild code-core/Vishwakarma.vcxproj -p:Configuration=Debug -p:Platform=x64 -p:BuildProjectReferences=false -m`

`BuildProjectReferences=false` is the important part: the `VishwakarmaExternal` ProjectReference
otherwise re-runs BuildOpenSSL.ps1, which fails here (perl `OpenSSL::fallback` missing, `vswhere` not
on PATH). The prebuilt `build/Debug/External/VishwakarmaExternal.lib` already exists so linking still
works, and this matches CLAUDE.md — external deps compile only when they change. Pre/post build
events are fine to leave on. Incremental builds are ~8–90 s. `VishwakarmaSetup.vcxproj` builds the
same way.

**Release|x64 cannot build on this machine**: a project guard errors out because the one-time
prebuilt `build/protobuf-x64-release/Release/libprotobuf-lite.lib` was never built here (only the
Debug protobuf tree exists). Remedy if ever needed: one-time CMake build of code-external/protobuf
into `build/protobuf-x64-release`. CI does this in GenerateRelease.ps1.

**Warning hygiene — three mechanisms, do not "tidy" any away** (verified 0 warnings, 2026-07-18;
FxCompile one added 2026-08-01):

1. **LNK4098** (LIBCMT vs LIBCMTD): `IgnoreSpecificDefaultLibraries=LIBCMT` in the *Debug* Link
   block. Needed because freetype.lib is a Release /MT build deliberately linked into the /MTd app.
2. **protobuf/absl header warnings** (C4267/C4244/C4018, all theirs): `<ExternalIncludePath>` +
   `<ExternalWarningLevel>TurnOffAllWarnings</ExternalWarningLevel>` + `/external:templates-`.
   code-core stays at Level3.
3. **FXC shader-PDB warning** ("no output provided for debug - embedding PDB in shader container"),
   one per shader (20) on any FULL shader rebuild in Debug: `<FxCompile><AdditionalOptions>
   -Qembed_debug %(AdditionalOptions)</AdditionalOptions></FxCompile>` in the *Debug* x64
   ItemDefinitionGroup only (Release shaders carry no debug info). Only visible on a full shader
   rebuild — editing the .vcxproj (e.g. adding an FxCompile item) forces one, so an incremental
   build that skipped shaders wrongly looked clean before this landed.

Why it's shaped that way: MSVC's external-headers feature only works if a directory is reached via
`ExternalIncludePath`, **not** `/I` — a dir in AdditionalIncludeDirectories is treated as internal
even when also marked external. So `code-external\protobuf\src`, `protobuf\third_party\utf8_range`
and both `build\protobuf-x64-{debug,release}\_deps\absl-src` were *moved out of*
AdditionalIncludeDirectories into ExternalIncludePath (Microsoft.Cpp.Current.targets appends it to
IncludePath, so headers still resolve — just searched later; verified no name collisions).
`/external:templates-` is needed on top of /external:W0 or warnings surfacing through template
instantiation chains get attributed to our code. Property names confirmed in
`MSBuild\Microsoft\VC\v180\1033\cl.xml`. Both are VS2019 16.10+, so GitHub Actions windows-latest
honours them.

**VishwakarmaExternal.vcxproj is a different case**: its vendored files (pocketpy.c, sqlite3.c,
lunasvg/plutovg) are *compiled as source*, not included as headers, so ExternalIncludePath does not
apply. The lever there is per-file ClCompile metadata — `pocketpy.c` carries
`<WarningLevel>TurnOffAllWarnings</WarningLevel>` (+ pre-existing `<SDLCheck>false</SDLCheck>`), and
lunasvg+plutovg (21 files) are silenced as a group via `<ClCompile Update="lunasvg\**\*.cpp;...">`
placed **after** the ItemGroup that defines them — Update only reaches already-defined items, so
order matters. Deliberately not project-level: sqlite3.c, libpng, zlib keep Level3, and so does
`fast_float_wrapper.cpp`, which is *our* code sitting in code-external.

**How to apply:** if a future external dependency spams warnings, add its dir to ExternalIncludePath
in *both* config PropertyGroups and remove it from AdditionalIncludeDirectories. Don't add `/wd`
flags — those would blind code-core too.

**Standalone probe TUs that include code-core headers (2026-08-23).** Useful for measuring
`sizeof` or asserting an invariant against the REAL structs instead of a retyped copy. Two ordering
traps, both of which look like a broken toolchain: (a) vcvars64 cannot find vswhere here, so it sets
the MSVC include but NOT the Windows SDK - add `Windows Kits\10\Include\10.0.26100.0\{ucrt,shared,um}`
and the matching Lib dirs explicitly; and (b) **the DirectX-Headers submodule is NEWER than the
installed SDK**, so `code-external\DirectX-Headers\include\directx` must come BEFORE the SDK in
INCLUDE. Reversed, `d3dx12_core.h` fails on `D3D12_RESOURCE_FLAG_USE_TIGHT_ALIGNMENT` undeclared -
which reads like a corrupt submodule, not an ordering bug. `डेटा.h` pulls in `d3dx12.h`, so anything
including it hits this. Working invocation shape:
`cmd /v:on /c "vcvars64.bat >nul && set INCLUDE=<dxheaders>;<code-core>;!INCLUDE!;<sdk>&& cl /std:c++20 /utf-8 probe.cpp"`.
Two more traps found 2026-08-23 doing this for real: (c) the SDK list needs **`winrt`** as well as
ucrt/shared/um, because `d3dx12_state_object.h` includes `wrl/client.h` - without it the failure
reads as a broken DirectX-Headers submodule; and (d) anything including `डेटा-संरचना.h` needs
`build\Debug\Intermediate` on INCLUDE for the generated steel catalog header. Also: quoting a
vcvars path with spaces inside `cmd /c "..."` from the Bash tool drops into an interactive shell -
write the whole thing to a .bat with the Write tool and invoke that instead. Probes live in the
session scratchpad, not the repo.

Two build-system quirks worth remembering: editing VishwakarmaExternal.vcxproj re-triggers the
BuildOpenSSL target (`$(MSBuildProjectFullPath)` is one of its Inputs) — cheap while
`build/openssl-x64-*` is intact, a ~15-minute rebuild only if that directory is deleted. And
actions/cache preserves old mtimes, so an Inputs/Outputs check can see freshly checked-out submodule
sources as newer than cache-restored .libs and rebuild OpenSSL despite a cache hit; fixed both ways
(GenerateRelease.ps1 auto-adds BuildProjectReferences=false when the lib exists; nightly.yml touches
restored libs on an exact hit). Related: [[extension-system-mvp]], [[user-working-style]].
