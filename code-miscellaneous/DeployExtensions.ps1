# Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
# Post-build step: deploy the bundled extensions next to the executable
# ($(OutDir)extensions\...) and generate their Python protobuf bindings.

param(
    [Parameter(Mandatory = $true)][string]$OutDir,
    [string]$ProtocPath = ""
)

$ErrorActionPreference = "Stop"

# MSBuild passes $(OutDir) with a trailing backslash that can escape the
# closing quote of the argument; strip any stray quote characters.
$OutDir = $OutDir.Trim('"')

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$protoSourceDir = Join-Path $repoRoot "code-core"

$candidateProtocPaths = @()
if (![string]::IsNullOrWhiteSpace($ProtocPath)) {
    $candidateProtocPaths += $ProtocPath
}
$globalProtoc = Get-Command "protoc" -CommandType Application -ErrorAction SilentlyContinue
if ($globalProtoc) {
    $candidateProtocPaths += $globalProtoc.Source
}
$candidateProtocPaths += @(
    (Join-Path $repoRoot "build\protobuf-x64-debug\Debug\protoc.exe"),
    (Join-Path $repoRoot "build\protobuf-x64-release\Release\protoc.exe")
)

$resolvedProtocPath = $candidateProtocPaths |
    Where-Object { ![string]::IsNullOrWhiteSpace($_) -and (Test-Path -LiteralPath $_) } |
    Select-Object -First 1

if ([string]::IsNullOrWhiteSpace($resolvedProtocPath)) {
    Write-Error "protoc was not found. Build protobuf first or install protoc 35.1."
    exit 1
}

$targetDir = Join-Path $OutDir "extensions\std-importer"
New-Item -ItemType Directory -Force -Path $targetDir | Out-Null

Copy-Item (Join-Path $repoRoot "extensions\std-importer\*.py") $targetDir -Force
Copy-Item (Join-Path $repoRoot "code-miscellaneous\InteroperabilityWithSTDFile.py") $targetDir -Force

& $resolvedProtocPath "--proto_path=$protoSourceDir" "--python_out=$targetDir" (Join-Path $protoSourceDir "ExtensionIPC.proto")
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
