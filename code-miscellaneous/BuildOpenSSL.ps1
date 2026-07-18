param(
    [Parameter(Mandatory = $true)]
    [string]$SourceDir,

    [Parameter(Mandatory = $true)]
    [string]$BuildDir,

    [Parameter(Mandatory = $true)]
    [ValidateSet("Debug", "Release")]
    [string]$Configuration,

    [Parameter(Mandatory = $true)]
    [string]$VcVarsAll
)

$ErrorActionPreference = "Stop"

$sourcePath = (Resolve-Path -LiteralPath $SourceDir).Path
$buildPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($BuildDir)
$vcVarsAllPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($VcVarsAll)

if (!(Test-Path -LiteralPath (Join-Path $sourcePath "Configure"))) {
    throw "OpenSSL Configure was not found under '$sourcePath'."
}

if (!(Test-Path -LiteralPath $vcVarsAllPath)) {
    throw "Visual Studio vcvarsall.bat was not found at '$vcVarsAllPath'."
}

$perlCommand = Get-Command perl.exe -ErrorAction SilentlyContinue
$perlPath = if ($perlCommand) { $perlCommand.Source } else { $null }

if (!$perlPath) {
    $perlPath = @(
        "C:\Strawberry\perl\bin\perl.exe",
        "C:\Perl64\bin\perl.exe",
        "C:\Perl\bin\perl.exe"
    ) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}

if (!$perlPath) {
    throw "Perl is required to configure OpenSSL, but perl was not found on PATH or in common Windows install locations."
}

New-Item -ItemType Directory -Force -Path $buildPath | Out-Null

$configureMode = if ($Configuration -eq "Debug") { "--debug" } else { "--release" }
$installPath = Join-Path $buildPath "install"
$opensslDir = Join-Path $buildPath "ssl"
$configureScript = Join-Path $sourcePath "Configure"

# Trim OpenSSL to what the app actually uses: TLS 1.3/1.2, RSA/ECDSA/EdDSA cert
# chains, X25519/X448/Ed25519/Ed448, AES + ChaCha20-Poly1305, SHA-2/SHA-3, DH and
# BLAKE2 (kept for compatibility). Each no-* flag drops source from the build, so
# this cuts binary size and OpenSSL's nmake compile time. MD5/SHA-1 are mandatory
# for TLS and intentionally not listed.
$disableFlags = @(
    # Legacy protocols/methods. SSLv2/SSLv3 and the ENGINE API are already gone
    # from OpenSSL 4.0, so no-ssl3/no-engine are intentionally omitted (deprecated
    # no-ops that only add a Configure warning).
    "no-tls1", "no-tls1-method", "no-tls1_1", "no-tls1_1-method",
    "no-dtls", "no-comp",
    # Legacy symmetric ciphers (no-des covers DES + 3DES).
    "no-des", "no-rc2", "no-rc4", "no-rc5", "no-idea", "no-bf", "no-cast",
    "no-seed", "no-camellia", "no-aria",
    # Old hashes.
    "no-md4", "no-mdc2", "no-rmd160", "no-whirlpool",
    # Chinese national algorithms.
    "no-sm2", "no-sm3", "no-sm4",
    # Unused public-key / message layers (DSA certs, S/MIME/CMS, timestamping).
    "no-dsa", "no-cms", "no-ts",
    # Revocation/misc not used by pinning + chain validation.
    "no-ocsp", "no-ct", "no-srp", "no-psk"
) -join " "

$command = @(
    "call `"$vcVarsAllPath`" x64",
    "`"$perlPath`" `"$configureScript`" VC-WIN64A $configureMode no-shared no-pinshared no-asm no-apps no-tests no-makedepend $disableFlags --prefix=`"$installPath`" --openssldir=`"$opensslDir`"",
    "nmake /NOLOGO /S build_libs"
) -join " && "

$localeVariables = @("LC_ALL", "LC_CTYPE", "LANG")
$previousLocaleValues = @{}
foreach ($localeVariable in $localeVariables) {
    $envPath = "Env:$localeVariable"
    $previousLocaleValues[$localeVariable] = (Get-Item -Path $envPath -ErrorAction SilentlyContinue).Value
    Remove-Item -Path $envPath -ErrorAction SilentlyContinue
}

Push-Location $buildPath
try {
    & $env:ComSpec /d /s /c $command
    if ($LASTEXITCODE -ne 0) {
        throw "OpenSSL $Configuration build failed with exit code $LASTEXITCODE."
    }
}
finally {
    foreach ($localeVariable in $localeVariables) {
        $envPath = "Env:$localeVariable"
        if ($null -ne $previousLocaleValues[$localeVariable]) {
            Set-Item -Path $envPath -Value $previousLocaleValues[$localeVariable]
        }
        else {
            Remove-Item -Path $envPath -ErrorAction SilentlyContinue
        }
    }

    Pop-Location
}

foreach ($libraryName in @("libcrypto.lib", "libssl.lib")) {
    $libraryPath = Join-Path $buildPath $libraryName
    if (!(Test-Path -LiteralPath $libraryPath)) {
        throw "OpenSSL $Configuration build completed, but '$libraryPath' was not produced."
    }
}
