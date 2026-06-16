# Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

param(
    [string]$ProtocPath = "",
    [string]$OutputDir = ""
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = Join-Path $PSScriptRoot "..\build\GeneratedProtobuf"
}

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$protoSourceDir = Join-Path $repoRoot "code-core"
$candidateProtocPaths = @()
if (![string]::IsNullOrWhiteSpace($ProtocPath)) {
    $candidateProtocPaths += $ProtocPath
}

# Check if protoc is installed globally (like via Chocolatey) and accessible via system PATH
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


$resolvedProtocPath = $candidateProtocPaths |
    Where-Object { ![string]::IsNullOrWhiteSpace($_) -and (Test-Path -LiteralPath $_) } |
    Select-Object -First 1

if ([string]::IsNullOrWhiteSpace($resolvedProtocPath)) {
    Write-Error "protoc was not found. Build protobuf first or install protoc 35.1."
    exit 1
}

$ProtocPath = $resolvedProtocPath

$protoFiles = Get-ChildItem -LiteralPath $protoSourceDir -Filter "DataStorage_*.proto" |
    Sort-Object Name |
    ForEach-Object { $_.FullName }

if ($protoFiles.Count -eq 0) {
    Write-Warning "No DataStorage_*.proto files found in '$protoSourceDir'."
    exit 0
}

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

& $ProtocPath "--proto_path=$protoSourceDir" "--cpp_out=$OutputDir" @protoFiles
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
