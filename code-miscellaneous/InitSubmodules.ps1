# Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

# Ensures every git submodule this repository builds is checked out before the build
# consumes its sources. All of our external dependencies (openssl, protobuf, freetype,
# libpng, zlib, lunasvg, cpython, ...) live as submodules under code-external, so a
# fresh clone has empty submodule directories until they are initialized.
#
# Goal: someone can clone the repo, open Vishwakarma.sln and build without any manual
# "git submodule update" step. This is wired as the earliest pre-build step of
# VishwakarmaExternal (see VishwakarmaExternal.vcxproj), the first project in the .sln.
#
# We deliberately do NOT init recursively across the board: openssl carries a large tail
# of test/provider submodules (gost-engine, pkcs11-provider, tlsfuzzer, oqs-provider,
# ... which in turn drag in boringssl-adjacent repos) and freetype carries "dlg", none
# of which our build compiles. Instead we init the top-level submodules shallowly and
# recurse only into the ones whose own nested submodules are actually required.
#
# It is a fast no-op once the submodules are present: it only shells out to git when a
# required submodule is still uninitialized. Assumes git is installed and on PATH.

param(
    [Parameter(Mandatory = $true)]
    [string]$RepoRoot
)

$ErrorActionPreference = "Stop"

# Top-level submodules whose OWN nested submodules must also be checked out to build.
# msdf-atlas-gen needs msdfgen + artery-font-format (neither nests further). Every other
# top-level submodule either has no submodules or only ones our build never touches.
$recursivePaths = @(
    "code-external/msdf-atlas-gen"
)

$repoPath = (Resolve-Path -LiteralPath $RepoRoot).Path

# A source archive (zip download) has no .git, so there is nothing to initialize and
# nothing we could fetch. Warn and let the build proceed / fail on its own.
if (!(Test-Path -LiteralPath (Join-Path $repoPath ".git"))) {
    Write-Warning "No .git found under '$repoPath' - skipping submodule init. If dependencies are missing, clone the repository with git instead of downloading an archive."
    exit 0
}

# "git submodule status" prefixes an uninitialized submodule with '-' (an initialized
# one gets a space or '+'). Returns $true if any listed submodule still needs checkout.
function Test-NeedsInit([string[]]$statusLines) {
    return @($statusLines | Where-Object { $_ -match '^\s*-' }).Count -gt 0
}

Push-Location -LiteralPath $repoPath
try {
    # --- Top-level submodules (all of them, non-recursive) ---
    $status = & git submodule status 2>&1
    if ($LASTEXITCODE -ne 0) { throw "git submodule status failed:`n$status" }
    if (Test-NeedsInit $status) {
        Write-Host "Initializing top-level git submodules (first build after clone)..."
        & git submodule update --init
        if ($LASTEXITCODE -ne 0) { throw "git submodule update --init failed with exit code $LASTEXITCODE." }
    }

    # --- Only the submodules that need their own nested submodules ---
    foreach ($path in $recursivePaths) {
        $subStatus = & git submodule status --recursive -- $path 2>&1
        if ($LASTEXITCODE -ne 0) { throw "git submodule status --recursive -- $path failed:`n$subStatus" }
        if (Test-NeedsInit $subStatus) {
            Write-Host "Initializing nested submodules under $path..."
            & git submodule update --init --recursive -- $path
            if ($LASTEXITCODE -ne 0) { throw "git submodule update --init --recursive -- $path failed with exit code $LASTEXITCODE." }
        }
    }

    Write-Host "Required git submodules are initialized."
}
finally {
    Pop-Location
}
