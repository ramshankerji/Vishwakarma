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

$command = @(
    "call `"$vcVarsAllPath`" x64",
    "`"$perlPath`" `"$configureScript`" VC-WIN64A $configureMode no-shared no-pinshared no-asm no-apps no-tests no-makedepend --prefix=`"$installPath`" --openssldir=`"$opensslDir`"",
    "nmake build_libs"
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
