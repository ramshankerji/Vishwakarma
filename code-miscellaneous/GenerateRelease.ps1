# Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
# Release packaging of Vishwakarma. Design: website/content/software/release.md
#
# Mode EmbeddedJson : writes build\<Configuration>\EmbeddedReleaseDetails.json and
#                     build\<Configuration>\OpenSourceLicenses.zip which get embedded into
#                     VishwakarmaSetup.exe as resources. Runs as the pre-build event of
#                     VishwakarmaSetup.vcxproj.
# Mode Package      : full release pipeline (default configuration Release):
#                     1. msbuild Vishwakarma.vcxproj
#                     2. Authenticode-sign Vishwakarma.exe        (MV-CodeSigner-01.pfx)
#                     3. generate EmbeddedReleaseDetails.json
#                     4. msbuild VishwakarmaSetup.vcxproj
#                     5. Authenticode-sign + rename the setup exe (stable name)
#                     6. write Vishwakarma_release_details.json at repo root + Ed25519 .sig
#                     Password comes from -PfxPassword or the RELEASE_SIGN_PASSWORD
#                     environment variable. Without it, signing and the .sig are skipped.

param(
    [ValidateSet("EmbeddedJson", "Package")]
    [string]$Mode = "Package",

    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",

    [string]$PfxPassword = ""
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$buildDir = Join-Path $repoRoot "build\$Configuration"
$pfxPath = Join-Path $repoRoot "code-miscellaneous\MV-CodeSigner-01.pfx"
$manifestKeyPath = Join-Path $repoRoot "code-miscellaneous\ManifestSigner-01.key"
$downloadBase = "https://github.com/ramshankerji/Vishwakarma/releases/download/nightly"

if (-not $PfxPassword) { $PfxPassword = $env:RELEASE_SIGN_PASSWORD }

# .gitignore excludes *.pfx, so on CI the PFX arrives as the base64 secret RELEASE_SIGN_PFX_B64.
if (-not (Test-Path $pfxPath) -and $env:RELEASE_SIGN_PFX_B64) {
    $pfxPath = Join-Path $repoRoot "build\MV-CodeSigner-01.pfx"
    New-Item -ItemType Directory -Force -Path (Split-Path $pfxPath) | Out-Null
    [System.IO.File]::WriteAllBytes($pfxPath, [Convert]::FromBase64String($env:RELEASE_SIGN_PFX_B64))
}

# ------------------------------------------------------------------ Shared values
function Get-VersionInfo {
    Push-Location $repoRoot
    try {
        $version = 0
        $hash = "00000000"
        try {
            $version = [int](git rev-list --count HEAD 2>$null)
            $hash = (git rev-parse --short=8 HEAD 2>$null).Trim()
        } catch {}
        $utcNow = [DateTime]::UtcNow
        return [pscustomobject]@{
            Version   = $version
            Hash      = $hash
            UtcNow    = $utcNow
            Sequence  = $utcNow.ToString("yyyyMMdd-HHmmss") + "-" + $hash
            FileName  = "Vishwakarma_UserSetup_win10_win11_x64.exe"
        }
    }
    finally { Pop-Location }
}

# The json embedded inside the setup exe. validUntil: 1 year is acceptable for the embedded
# installer manifest (release.md).
function Write-EmbeddedJson([object]$info) {
    New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
    $generatedAt = $info.UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ")
    $validUntil = $info.UtcNow.AddYears(1).ToString("yyyy-MM-ddTHH:mm:ssZ")
    $json = @"
{
  "schemaVersion": 1,
  "appId": "MissionVishwakarma",
  "manifestSequence": "$($info.Sequence)",
  "generatedAt": "$generatedAt",
  "validUntil": "$validUntil",
  "name": "Vishwakarma",
  "channel": "stable",
  "platform": "windows",
  "osMinVersion": "10.0",
  "instructionSet": "x64",
  "version": $($info.Version),
  "fileName": "$($info.FileName)"
}
"@
    $path = Join-Path $buildDir "EmbeddedReleaseDetails.json"
    [System.IO.File]::WriteAllText($path, $json, (New-Object System.Text.UTF8Encoding $false))
    Write-Host "Wrote $path (version $($info.Version))"
}

# Third-party license texts (OpenSourceLicenses folder + LICENSE.md), embedded into the
# setup exe as one zip and extracted beside Vishwakarma.exe at install time.
function Write-LicensesZip {
    New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
    $zipPath = Join-Path $buildDir "OpenSourceLicenses.zip"
    Compress-Archive -Path (Join-Path $repoRoot "OpenSourceLicenses\*"), (Join-Path $repoRoot "LICENSE.md") `
        -DestinationPath $zipPath -Force
    Write-Host "Wrote $zipPath"
}

$info = Get-VersionInfo

if ($Mode -eq "EmbeddedJson") {
    Write-EmbeddedJson $info
    Write-LicensesZip
    exit 0
}

# ------------------------------------------------------------------ Mode Package
function Find-SignTool {
    $kits = "${env:ProgramFiles(x86)}\Windows Kits\10\bin"
    $candidates = Get-ChildItem -Path $kits -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending |
        ForEach-Object { Join-Path $_.FullName "x64\signtool.exe" } |
        Where-Object { Test-Path $_ }
    if ($candidates) { return $candidates | Select-Object -First 1 }
    return $null
}

function Find-OpenSsl {
    $cmd = Get-Command openssl.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $strawberry = "C:\Strawberry\c\bin\openssl.exe"
    if (Test-Path $strawberry) { return $strawberry }
    return $null
}

function Invoke-Sign([string]$signtool, [string]$file) {
    # Timestamping keeps signatures valid past certificate expiry; retry without it when the
    # timestamp server is unreachable.
    & $signtool sign /f $pfxPath /p $PfxPassword /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 $file
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "Timestamped signing failed for $file, retrying without timestamp."
        & $signtool sign /f $pfxPath /p $PfxPassword /fd SHA256 $file
        if ($LASTEXITCODE -ne 0) { throw "signtool failed for $file" }
    }
    Write-Host "Signed $file"
}

if ($info.Version -le 0) { throw "Could not determine version from git (git rev-list --count HEAD)." }

$signtool = $null
if ($PfxPassword -and -not (Test-Path $pfxPath)) {
    Write-Warning "MV-CodeSigner-01.pfx not found (local file or RELEASE_SIGN_PFX_B64); exes will be UNSIGNED."
} elseif ($PfxPassword) {
    $signtool = Find-SignTool
    if (-not $signtool) { Write-Warning "signtool.exe not found under Windows Kits; exes will be UNSIGNED." }
} else {
    Write-Warning "No signing password (RELEASE_SIGN_PASSWORD / -PfxPassword); exes and manifest will be UNSIGNED."
}

# 1. Build the application first, so the setup exe embeds the final (signed) binary.
msbuild (Join-Path $repoRoot "code-core\Vishwakarma.vcxproj") /p:Configuration=$Configuration /p:Platform=x64 /m
if ($LASTEXITCODE -ne 0) { throw "Vishwakarma.vcxproj build failed." }

$appExe = Join-Path $buildDir "Vishwakarma.exe"
if ($PfxPassword -and $signtool) { Invoke-Sign $signtool $appExe }

# 1b. Extension worker (embedded CPython; its static python313.lib comes from
# BuildCPython.ps1, invoked automatically by the project when missing). Signed and then
# embedded into the setup exe alongside the application.
msbuild (Join-Path $repoRoot "code-core\VishwakarmaExtension.vcxproj") /p:Configuration=$Configuration /p:Platform=x64 /m
if ($LASTEXITCODE -ne 0) { throw "VishwakarmaExtension.vcxproj build failed." }

$workerExe = Join-Path $buildDir "VishwakarmaExtension.exe"
if ($PfxPassword -and $signtool) { Invoke-Sign $signtool $workerExe }

# 2. Setup exe: embeds the app exe + json written above. BuildProjectReferences=false, else
# the app project relinks (its pre-build events always regenerate protobuf sources) and the
# Authenticode signature applied in step 1 is lost.
Write-EmbeddedJson $info
Write-LicensesZip
msbuild (Join-Path $repoRoot "code-core\VishwakarmaSetup.vcxproj") /p:Configuration=$Configuration /p:Platform=x64 /p:BuildProjectReferences=false /m
if ($LASTEXITCODE -ne 0) { throw "VishwakarmaSetup.vcxproj build failed." }

$setupExe = Join-Path $buildDir $info.FileName
Copy-Item (Join-Path $buildDir "VishwakarmaSetup.exe") $setupExe -Force
if ($PfxPassword -and $signtool) { Invoke-Sign $signtool $setupExe }

# 3. Server manifest: 30 days validity maximum (release.md).
$sha256 = (Get-FileHash $setupExe -Algorithm SHA256).Hash.ToLower()
$size = (Get-Item $setupExe).Length
$generatedAt = $info.UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ")
$validUntil = $info.UtcNow.AddDays(30).ToString("yyyy-MM-ddTHH:mm:ssZ")
$manifest = @"
{
  "schemaVersion": 1,
  "appId": "MissionVishwakarma",
  "manifestSequence": "$($info.Sequence)",
  "generatedAt": "$generatedAt",
  "validUntil": "$validUntil",
  "signatureScheme": "Ed25519",
  "signingKeyId": "ManifestSigner-01",
  "releases": [
    {
      "name": "Vishwakarma",
      "channel": "stable",
      "platform": "windows",
      "osMinVersion": "10.0",
      "osMaxVersion": null,
      "instructionSet": "x64",
      "version": $($info.Version),
      "minUpdateFromVersion": 1,
      "packageKind": "full-user-setup-exe",
      "fileName": "$($info.FileName)",
      "url": "$downloadBase/$($info.FileName)",
      "size": $size,
      "sha256": "$sha256",
      "authenticodeRequired": false
    }
  ],
  "blockedVersions": [],
  "minimumAllowedVersion": 1
}
"@
$manifestPath = Join-Path $repoRoot "Vishwakarma_release_details.json"
[System.IO.File]::WriteAllText($manifestPath, $manifest, (New-Object System.Text.UTF8Encoding $false))
Copy-Item $manifestPath (Join-Path $buildDir "Vishwakarma_release_details.json") -Force
Write-Host "Wrote $manifestPath"

# 4. Ed25519 signature over the manifest bytes (raw 64-byte .sig).
if ($PfxPassword) {
    $openssl = Find-OpenSsl
    if (-not $openssl) { throw "openssl.exe not found; cannot Ed25519-sign the manifest." }
    $sigPath = "$manifestPath.sig"
    $env:VW_MANIFEST_PW = $PfxPassword
    try {
        & $openssl pkeyutl -sign -rawin -inkey $manifestKeyPath -passin env:VW_MANIFEST_PW -in $manifestPath -out $sigPath
        if ($LASTEXITCODE -ne 0) { throw "openssl Ed25519 signing failed." }
    }
    finally { Remove-Item Env:VW_MANIFEST_PW -ErrorAction SilentlyContinue }
    Copy-Item $sigPath (Join-Path $buildDir "Vishwakarma_release_details.json.sig") -Force
    Write-Host "Wrote $sigPath"
}

Write-Host "Release packaging complete: version $($info.Version), $($info.FileName)"
