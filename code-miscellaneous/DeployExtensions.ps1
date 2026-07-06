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

$extensionNames = @("Interoperability-STD", "Interoperability-DXF")

foreach ($extensionName in $extensionNames) {
    $targetDir = Join-Path $OutDir "extensions\$extensionName"
    New-Item -ItemType Directory -Force -Path $targetDir | Out-Null

    Copy-Item (Join-Path $repoRoot "extensions\$extensionName\*.py") $targetDir -Force

    & $resolvedProtocPath "--proto_path=$protoSourceDir" "--python_out=$targetDir" (Join-Path $protoSourceDir "ExtensionIPC.proto")
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    # Package the deployed extension into a zip that VishwakarmaSetup.rc embeds as
    # RCDATA. Only the runtime .py files are needed; selecting them explicitly keeps
    # __pycache__ (created when the worker runs from the build output) out of the archive.
    $zipPath = Join-Path $OutDir "extensions\$extensionName.zip"
    $pyFiles = Get-ChildItem -Path $targetDir -File -Filter *.py
    Compress-Archive -Path $pyFiles.FullName -DestinationPath $zipPath -Force
}
