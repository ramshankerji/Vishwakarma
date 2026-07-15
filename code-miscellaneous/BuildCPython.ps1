# Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
# Builds the statically linked CPython used by VishwakarmaExtension.exe (the out-of-process
# extension worker). Design: website/content/software/extensions.md
#
# Products (all under build\cpython-x64-release, cached by CI):
#   python313.lib          - CPython core as a /MT static library (no python313.dll). Built
#                            from the pinned code-external\cpython submodule with the stock
#                            PCbuild\pythoncore.vcxproj, converted to a static library via
#                            CPythonStatic.props. zlib comes from code-external\zlib.
#   include\pyconfig.h     - the generated Windows config header matching that library.
#   frozen_modules\*.h     - curated stdlib allowlist + the google.protobuf pure-Python
#                            runtime (from the code-external\protobuf submodule, so its
#                            version always matches protoc and libprotobuf-lite), frozen to
#                            marshalled bytecode with CPython's own _freeze_module.exe.
#   vk_frozen_modules.c    - generated PyImport_FrozenModules table compiled into the worker.
#
# Security posture (enforced here at build time):
#   - No networking: _socket / select / ssl / asyncio are never compiled or frozen.
#   - No os-level danger beyond boot needs: subprocess, tempfile, socket, ctypes etc. are
#     simply absent from the frozen set; a module that is not frozen in does not exist.
#   - winreg is replaced by a raise-on-use stub (importlib imports it unconditionally on
#     Windows; nothing may actually use it).
# The worker executable additionally curates the builtin module table and scrubs process
# creation primitives - see code-core\VishwakarmaExtension.cpp.
#
# Rebuild is skipped when the products already exist; pass -Force after changing the
# CPython/protobuf submodules or the freeze list below.

param(
    [switch]$Force
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$cpythonDir = Join-Path $repoRoot "code-external\cpython"
$outRoot = Join-Path $repoRoot "build\cpython-x64-release"
$frozenHeaderDir = Join-Path $outRoot "frozen_modules"
$frozenSrcDir = Join-Path $outRoot "frozen-src"
$tableFile = Join-Path $outRoot "vk_frozen_modules.c"
$staticLib = Join-Path $outRoot "python313.lib"

if (-not $Force -and (Test-Path $tableFile) -and (Test-Path $staticLib)) {
    Write-Host "CPython static build already present at $outRoot (use -Force to rebuild)."
    exit 0
}

# ------------------------------------------------------------------ Tool discovery
function Resolve-VsWhere {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) { return $vswhere }
    return $null
}

function Resolve-MSBuild {
    $cmd = Get-Command msbuild.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($cmd) { return $cmd.Source }
    $vswhere = Resolve-VsWhere
    if ($vswhere) {
        $path = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild `
            -find "MSBuild\**\Bin\amd64\MSBuild.exe" | Select-Object -First 1
        if ($path) { return $path }
    }
    throw "MSBuild not found (neither on PATH nor via vswhere)."
}

# CPython's python.props maps VS 2026 (18.0) to the v143 toolset, which VS 2026 does not
# install by default. Pick the toolset that is actually present instead.
function Resolve-PlatformToolset {
    $vswhere = Resolve-VsWhere
    if (-not $vswhere) { return $null }
    $vsRoot = & $vswhere -latest -products * -property installationPath | Select-Object -First 1
    if (-not $vsRoot) { return $null }
    $msvcDir = Join-Path $vsRoot "VC\Tools\MSVC"
    if (-not (Test-Path $msvcDir)) { return $null }
    $latest = Get-ChildItem $msvcDir -Directory | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
    if (-not $latest) { return $null }
    $v = [version]$latest.Name
    if ($v.Major -eq 14 -and $v.Minor -ge 50) { return "v145" }
    return "v143"
}

function Resolve-Protoc {
    $candidates = @(
        (Join-Path $repoRoot "build\protobuf-x64-release\Release\protoc.exe"),
        (Join-Path $repoRoot "build\protobuf-x64-debug\Debug\protoc.exe")
    )
    foreach ($c in $candidates) { if (Test-Path $c) { return $c } }
    $cmd = Get-Command protoc -CommandType Application -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    throw "protoc not found. Build code-external\protobuf first (see nightly.yml)."
}

$msbuild = Resolve-MSBuild
$toolset = Resolve-PlatformToolset
$protoc = Resolve-Protoc

$msbuildArgs = @("/p:Configuration=Release", "/p:Platform=x64", "/m", "/v:m", "/clp:ErrorsOnly", "/nologo")
if ($toolset) { $msbuildArgs += "/p:PlatformToolset=$toolset" }

# ------------------------------------------------------------------ 1. _freeze_module.exe
# Building it also regenerates the standard frozen headers (importlib bootstrap, os, ...)
# that pythoncore's frozen.c compiles in.
Write-Host "Building _freeze_module.exe (also a full static-core compile)..."
& $msbuild (Join-Path $cpythonDir "PCbuild\_freeze_module.vcxproj") @msbuildArgs
if ($LASTEXITCODE -ne 0) { throw "_freeze_module.vcxproj build failed." }
$freezeExe = Join-Path $cpythonDir "PCbuild\amd64\_freeze_module.exe"
if (-not (Test-Path $freezeExe)) { throw "$freezeExe missing after build." }

# ------------------------------------------------------------------ 2. pythoncore as a static library
Write-Host "Building pythoncore as a /MT static library..."
& $msbuild (Join-Path $cpythonDir "PCbuild\pythoncore.vcxproj") @msbuildArgs `
    "/p:ConfigurationType=StaticLibrary" `
    "/p:WholeProgramOptimization=false" `
    "/p:ForceImportBeforeCppTargets=$PSScriptRoot\CPythonStatic.props" `
    "/p:zlibDir=$repoRoot\code-external\zlib"
if ($LASTEXITCODE -ne 0) { throw "pythoncore.vcxproj static build failed." }

New-Item -ItemType Directory -Force -Path $outRoot, (Join-Path $outRoot "include"), $frozenHeaderDir, $frozenSrcDir | Out-Null
Copy-Item (Join-Path $cpythonDir "PCbuild\amd64\python313.lib") $staticLib -Force
Copy-Item (Join-Path $cpythonDir "PCbuild\obj\313amd64_Release\pythoncore\pyconfig.h") (Join-Path $outRoot "include\pyconfig.h") -Force

# ------------------------------------------------------------------ 3. Stage google.protobuf sources
Write-Host "Staging google.protobuf pure-Python runtime..."
$pbPython = Join-Path $repoRoot "code-external\protobuf\python\google"
Remove-Item $frozenSrcDir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path "$frozenSrcDir\google\protobuf\internal" | Out-Null
Copy-Item "$pbPython\__init__.py" "$frozenSrcDir\google\" -Force
Get-ChildItem "$pbPython\protobuf\*.py" -File |
    Where-Object { $_.Name -notmatch "test" } |
    Copy-Item -Destination "$frozenSrcDir\google\protobuf\" -Force
Get-ChildItem "$pbPython\protobuf\internal\*.py" -File |
    Where-Object { $_.Name -notmatch "test|_parameterized" } |
    Copy-Item -Destination "$frozenSrcDir\google\protobuf\internal\" -Force

# Well-known-type *_pb2.py modules are not checked in upstream; generate them with the
# protoc that matches the vendored runtime exactly.
$wellKnown = @("any", "api", "descriptor", "duration", "empty", "field_mask",
               "source_context", "struct", "timestamp", "type", "wrappers") |
    ForEach-Object { "google/protobuf/$_.proto" }
& $protoc "--proto_path=$repoRoot\code-external\protobuf\src" "--python_out=$frozenSrcDir" @wellKnown
if ($LASTEXITCODE -ne 0) { throw "protoc failed generating well-known-type modules." }

# google.protobuf.internal.python_edition_defaults is not checked in either: upstream
# generates it from a template plus the serialized FeatureSetDefaults emitted by protoc
# (bazel rules compile_edition_defaults / embed_edition_defaults). Mirror that here.
$defaultsBin = Join-Path $outRoot "edition_defaults.binpb"
& $protoc "--proto_path=$repoRoot\code-external\protobuf\src" `
    "--edition_defaults_out=$defaultsBin" `
    "--edition_defaults_minimum=PROTO2" "--edition_defaults_maximum=2024" `
    "google/protobuf/descriptor.proto"
if ($LASTEXITCODE -ne 0) { throw "protoc failed generating edition defaults." }
$defaultsEscaped = ([System.IO.File]::ReadAllBytes($defaultsBin) | ForEach-Object { "\x{0:x2}" -f $_ }) -join ""
$defaultsTemplate = Get-Content "$pbPython\protobuf\internal\python_edition_defaults.py.template" -Raw
[System.IO.File]::WriteAllText((Join-Path $frozenSrcDir "google\protobuf\internal\python_edition_defaults.py"),
    $defaultsTemplate.Replace("DEFAULTS_VALUE", $defaultsEscaped))
Remove-Item $defaultsBin -Force

# importlib unconditionally imports winreg on Windows; ship a raise-on-use stub instead of
# the real registry module.
@"
# Auto-generated by BuildCPython.ps1 for the Vishwakarma extension worker.
# The real winreg is not compiled into this interpreter; importlib merely
# requires the module to exist on Windows. Any actual use fails.
def __getattr__(name):
    raise OSError("winreg is not available inside the Vishwakarma extension worker")
"@ | Set-Content -Path (Join-Path $frozenSrcDir "winreg.py") -Encoding Ascii

# ------------------------------------------------------------------ 4. Freeze the curated set
# The frozen set doubles as the stdlib allowlist: a module that is not listed here simply
# does not exist inside the worker. Deliberately absent: socket/select/ssl (no networking),
# subprocess (no process creation), ctypes, multiprocessing, tempfile, sqlite3, ...
# The interpreter's boot set (io, os, stat, ntpath, codecs, importlib bootstrap, ...) is
# already baked into pythoncore's own frozen table and must not be repeated here.
$Lib = Join-Path $cpythonDir "Lib"
$freezeList = New-Object System.Collections.Generic.List[object]
function Add-Freeze([string]$modName, [string]$file, [bool]$isPackage = $false) {
    if (-not (Test-Path $file)) { throw "Freeze source missing: $file ($modName)" }
    $freezeList.Add([pscustomobject]@{ Name = $modName; File = $file; IsPackage = $isPackage })
}

$stdlibModules = @(
    "__future__", "_compat_pickle", "_colorize", "_opcode_metadata", "_threading_local", "_weakrefset",
    "argparse", "ast", "base64", "bisect", "calendar", "contextlib", "copy", "copyreg",
    "csv", "dataclasses", "datetime", "dis", "enum", "fnmatch", "functools", "gettext",
    "glob", "hashlib", "heapq", "inspect", "keyword", "linecache", "locale", "numbers",
    "opcode", "operator", "pickle", "pkgutil", "random", "reprlib", "shutil", "string", "struct",
    "textwrap", "threading", "token", "tokenize", "traceback", "types", "typing",
    "warnings", "weakref"
)
foreach ($m in $stdlibModules) { Add-Freeze $m (Join-Path $Lib "$m.py") }

Add-Freeze "importlib" (Join-Path $Lib "importlib\__init__.py") $true
Add-Freeze "importlib._abc" (Join-Path $Lib "importlib\_abc.py")
Add-Freeze "collections" (Join-Path $Lib "collections\__init__.py") $true
# collections.abc is aliased to the baked-in _collections_abc by collections/__init__.
foreach ($m in @("__init__", "_casefix", "_compiler", "_constants", "_parser")) {
    $name = "re"
    if ($m -ne "__init__") { $name = "re.$m" }
    Add-Freeze $name (Join-Path $Lib "re\$m.py") ($m -eq "__init__")
}
foreach ($m in @("__init__", "decoder", "encoder", "scanner")) {
    $name = "json"
    if ($m -ne "__init__") { $name = "json.$m" }
    Add-Freeze $name (Join-Path $Lib "json\$m.py") ($m -eq "__init__")
}
foreach ($m in @("__init__", "_abc", "_local")) {
    $name = "pathlib"
    if ($m -ne "__init__") { $name = "pathlib.$m" }
    Add-Freeze $name (Join-Path $Lib "pathlib\$m.py") ($m -eq "__init__")
}
foreach ($file in (Get-ChildItem "$Lib\encodings\*.py" -File)) {
    $name = "encodings." + $file.BaseName
    $isPackage = $false
    if ($file.BaseName -eq "__init__") { $name = "encodings"; $isPackage = $true }
    Add-Freeze $name $file.FullName $isPackage
}

Add-Freeze "winreg" (Join-Path $frozenSrcDir "winreg.py")

foreach ($file in (Get-ChildItem "$frozenSrcDir\google" -Recurse -Filter *.py -File)) {
    $relative = $file.FullName.Substring($frozenSrcDir.Length + 1) -replace "\\", "."
    $name = $relative -replace "\.py$", ""
    $isPackage = $false
    if ($name.EndsWith(".__init__")) { $name = $name -replace "\.__init__$", ""; $isPackage = $true }
    Add-Freeze $name $file.FullName $isPackage
}

Write-Host "Freezing $($freezeList.Count) modules..."
Remove-Item "$frozenHeaderDir\*.h" -Force -ErrorAction SilentlyContinue
foreach ($f in $freezeList) {
    $header = Join-Path $frozenHeaderDir ($f.Name + ".h")
    & $freezeExe $f.Name $f.File $header
    if ($LASTEXITCODE -ne 0) { throw "_freeze_module failed for $($f.Name)" }
}

# ------------------------------------------------------------------ 5. Generate the frozen table
$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("/* Auto-generated by code-miscellaneous/BuildCPython.ps1 - do not edit.")
$lines.Add(" * Extra frozen modules for VishwakarmaExtension.exe: curated stdlib allowlist")
$lines.Add(" * plus the google.protobuf pure-Python runtime. Installed into")
$lines.Add(" * PyImport_FrozenModules by code-core/VishwakarmaExtension.cpp. */")
$lines.Add("#include ""Python.h""")
$lines.Add("")
foreach ($f in $freezeList) {
    $lines.Add("#include ""frozen_modules/$($f.Name).h""")
}
$lines.Add("")
$lines.Add("const struct _frozen VkExtraFrozenModules[] = {")
foreach ($f in $freezeList) {
    $symbol = "_Py_M__" + ($f.Name -replace "\.", "_")
    $isPkg = 0
    if ($f.IsPackage) { $isPkg = 1 }
    $lines.Add("    {""$($f.Name)"", $symbol, (int)sizeof($symbol), $isPkg},")
}
$lines.Add("    {0, 0, 0, 0}")
$lines.Add("};")
[System.IO.File]::WriteAllLines($tableFile, $lines)

Write-Host "CPython static build complete: $staticLib"
Write-Host "Frozen table: $tableFile ($($freezeList.Count) modules)"
