# Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
# Builds the static protobuf-lite library and protoc.exe that Vishwakarma and
# VishwakarmaExtension link against. Products land under <BuildDir>\<Configuration>\:
#   libprotobuf-lited.lib / libprotobuf-lite.lib  - the runtime the app links.
#   protoc.exe                                    - compiler used by BuildCPython.ps1
#                                                   and to regenerate *_pb2 modules.
# Abseil and utf8_range are pulled in by protobuf's own CMake (FetchContent, landing
# under <BuildDir>\_deps and <BuildDir>\third_party).
#
# This mirrors the "Build Protobuf" step of .github\workflows\nightly.yml, but takes
# the configuration from the caller so a Debug app build produces the Debug protobuf
# and a Release build produces the Release one (same /MTd vs /MT split the app uses).
# It is wired as a post-build step of VishwakarmaExternal (see VishwakarmaExternal.vcxproj),
# which itself only rebuilds when external dependencies change.
#
# Skipped when the products already exist; pass -Force to rebuild.

param(
    [Parameter(Mandatory = $true)]
    [string]$SourceDir,

    [Parameter(Mandatory = $true)]
    [string]$BuildDir,

    [Parameter(Mandatory = $true)]
    [ValidateSet("Debug", "Release")]
    [string]$Configuration,

    [ValidateSet("x64")]
    [string]$Platform = "x64",

    [switch]$Force
)

$ErrorActionPreference = "Stop"

$sourcePath = (Resolve-Path -LiteralPath $SourceDir).Path
$buildPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($BuildDir)

if (!(Test-Path -LiteralPath (Join-Path $sourcePath "CMakeLists.txt"))) {
    throw "protobuf CMakeLists.txt was not found under '$sourcePath'."
}

# Visual Studio (multi-config) generator layout: products land in <BuildDir>\<Configuration>.
$outputDir = Join-Path $buildPath $Configuration
$libName = if ($Configuration -eq "Debug") { "libprotobuf-lited.lib" } else { "libprotobuf-lite.lib" }
$libPath = Join-Path $outputDir $libName
$protocPath = Join-Path $outputDir "protoc.exe"

if (!$Force -and (Test-Path -LiteralPath $libPath) -and (Test-Path -LiteralPath $protocPath)) {
    Write-Host "protobuf $Configuration already present at $outputDir (use -Force to rebuild)."
    exit 0
}

# ------------------------------------------------------------------ Tool discovery
function Resolve-CMake {
    $cmd = Get-Command cmake.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($cmd) { return $cmd.Source }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vsRoot = & $vswhere -latest -products * -property installationPath | Select-Object -First 1
        if ($vsRoot) {
            $cmakeExe = Join-Path $vsRoot "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
            if (Test-Path $cmakeExe) { return $cmakeExe }
        }
    }
    throw "cmake not found (neither on PATH nor in the Visual Studio install)."
}

$cmake = Resolve-CMake

# /MTd for Debug, /MT for Release - keeps the static runtime identical to
# VishwakarmaExternal and the app across the link.
$runtime = if ($Configuration -eq "Debug") { "MultiThreadedDebug" } else { "MultiThreaded" }

# ------------------------------------------------------------------ 1. Configure
# Mirrors nightly.yml: protobuf_MSVC_STATIC_RUNTIME + CMAKE_MSVC_RUNTIME_LIBRARY force the
# static CRT; CMP0091=NEW is what lets CMAKE_MSVC_RUNTIME_LIBRARY take effect.
$configureArgs = @(
    "-S", $sourcePath,
    "-B", $buildPath,
    "-A", $Platform,
    "-Dprotobuf_BUILD_TESTS=OFF",
    "-DCMAKE_POLICY_DEFAULT_CMP0091=NEW",
    "-DCMAKE_BUILD_TYPE=$Configuration",
    "-Dprotobuf_MSVC_STATIC_RUNTIME=ON",
    "-DCMAKE_MSVC_RUNTIME_LIBRARY=$runtime",
    "-DCMAKE_CXX_STANDARD=20",
    "-DCMAKE_CXX_FLAGS=/std:c++20"
)
& $cmake @configureArgs
if ($LASTEXITCODE -ne 0) { throw "protobuf $Configuration CMake configure failed with exit code $LASTEXITCODE." }

# ------------------------------------------------------------------ 2. Build
& $cmake --build $buildPath --config $Configuration -- /m /v:q /clp:ErrorsOnly
if ($LASTEXITCODE -ne 0) { throw "protobuf $Configuration build failed with exit code $LASTEXITCODE." }

# ------------------------------------------------------------------ 3. Verify products
foreach ($product in @($libPath, $protocPath)) {
    if (!(Test-Path -LiteralPath $product)) {
        throw "protobuf $Configuration build completed, but '$product' was not produced."
    }
}

Write-Host "protobuf $Configuration build complete: $libPath"
Write-Host "protoc: $protocPath"
