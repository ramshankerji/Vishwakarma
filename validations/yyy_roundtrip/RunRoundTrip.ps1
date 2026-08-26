<#
.SYNOPSIS
    Save/load round-trip test for .yyy project files.

.DESCRIPTION
    For every fixture in SampleFiles\, launches the Debug build with VISHWAKARMA_ROUNDTRIP_IN /
    VISHWAKARMA_ROUNDTRIP_OUT set, waits for the status file the dev hook writes, stops the
    process, and compares the two files field by field with check_roundtrip.py.

    This is the oracle for id.md section 8 step 3. That migration moves the nine Cad2D*RecordCPU
    types onto META_DATA, and section 9 warns that it runs through the save/load path "where a
    defect corrupts user files silently rather than crashing". Run this BEFORE the migration to
    record a green baseline, and after every step of it.

    The application allocates its own console, so its stdout cannot be redirected by a parent
    process. That is why the hook communicates through "<out>.status" instead.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File validations\yyy_roundtrip\RunRoundTrip.ps1

.EXAMPLE
    # One fixture, and leave the application open afterwards to look at what it loaded.
    ... -File validations\yyy_roundtrip\RunRoundTrip.ps1 -Only 02_asset_instance.yyy -KeepOpen
#>
[CmdletBinding()]
param(
    # Run just this fixture (file name inside SampleFiles), instead of all of them.
    [string] $Only,
    # Seconds to wait for one round trip before calling it a failure.
    [int] $TimeoutSeconds = 90,
    # Leave the application running after the round trip, to inspect what it loaded.
    [switch] $KeepOpen,
    # Also run the 90_* known-defect reproducers, which are expected to fail.
    [switch] $IncludeKnownDefects
)

$ErrorActionPreference = 'Stop'

$repoRoot   = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$sampleDir  = Join-Path $repoRoot 'SampleFiles'
$exePath    = Join-Path $repoRoot 'build\Debug\Vishwakarma.exe'
$checker    = Join-Path $PSScriptRoot 'check_roundtrip.py'
$outDir     = Join-Path $sampleDir 'roundtrip-out'

if (-not (Test-Path $exePath)) {
    throw "Debug build not found at $exePath. Build it first: MSBuild code-core\Vishwakarma.vcxproj -p:Configuration=Debug -p:Platform=x64 -p:BuildProjectReferences=false"
}
if (-not (Test-Path $sampleDir)) {
    throw "No SampleFiles directory. Run: python validations\yyy_roundtrip\make_fixtures.py"
}
if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Path $outDir | Out-Null }

$fixtures = Get-ChildItem -Path $sampleDir -Filter '*.yyy' | Where-Object { -not $_.PSIsContainer }
if ($Only) {
    $fixtures = $fixtures | Where-Object { $_.Name -eq $Only }
} elseif (-not $IncludeKnownDefects) {
    # 90_* are quarantined reproducers for defects that are already understood and recorded in
    # the README. They fail by design, so running them by default would train everyone to ignore
    # a red result -- which is the one thing a round-trip oracle must never become.
    $quarantined = $fixtures | Where-Object { $_.Name -like '90_*' }
    foreach ($item in $quarantined) { Write-Host "skipping known-defect reproducer $($item.Name)" }
    $fixtures = $fixtures | Where-Object { $_.Name -notlike '90_*' }
}
if (-not $fixtures) { throw "No fixtures to run. Run make_fixtures.py, or check -Only spelling." }

$passed = 0
$failed = @()

foreach ($fixture in $fixtures) {
    Write-Host ''
    Write-Host ('=' * 78)
    Write-Host "ROUND TRIP  $($fixture.Name)"
    Write-Host ('=' * 78)

    $outFile    = Join-Path $outDir $fixture.Name
    $statusFile = "$outFile.status"
    # Clear anything a previous run left, so a stale status cannot be read as this run's result.
    foreach ($stale in @($outFile, $statusFile, "$outFile-wal", "$outFile-shm")) {
        if (Test-Path $stale) { Remove-Item $stale -Force }
    }

    $env:VISHWAKARMA_ROUNDTRIP_IN  = $fixture.FullName
    $env:VISHWAKARMA_ROUNDTRIP_OUT = $outFile
    $process = Start-Process -FilePath $exePath -PassThru
    Remove-Item Env:\VISHWAKARMA_ROUNDTRIP_IN, Env:\VISHWAKARMA_ROUNDTRIP_OUT

    $status = $null
    for ($waited = 0; $waited -lt ($TimeoutSeconds * 4); $waited++) {
        if (Test-Path $statusFile) {
            # The hook closes the file before we look, but a read that lands mid-write returns
            # empty rather than failing - so keep polling until there is actually content.
            $status = (Get-Content $statusFile -Raw -ErrorAction SilentlyContinue)
            if ($status) { break }
        }
        if ($process.HasExited) { break }
        Start-Sleep -Milliseconds 250
    }

    if (-not $KeepOpen -and -not $process.HasExited) {
        # Ask the window to close before forcing it. This runs once per fixture, and the process
        # holds a D3D12 device -- repeatedly terminating one mid-frame is a good way to leave the
        # driver in a state where the NEXT launch hangs during device/UI initialisation.
        try { $process.CloseMainWindow() | Out-Null } catch {}
        if (-not $process.WaitForExit(8000)) {
            try { Stop-Process -Id $process.Id -Force -ErrorAction Stop } catch {}
        }
        Start-Sleep -Milliseconds 500
    }

    if (-not $status) {
        Write-Host "  no status file after $TimeoutSeconds s - the application did not finish the round trip." -ForegroundColor Red
        $failed += $fixture.Name
        continue
    }

    $status = $status.Trim()
    if ($status -ne 'OK') {
        Write-Host "  application reported: $status" -ForegroundColor Red
        $failed += $fixture.Name
        continue
    }

    & python $checker compare $fixture.FullName $outFile
    if ($LASTEXITCODE -eq 0) {
        $passed++
    } else {
        $failed += $fixture.Name
    }
}

Write-Host ''
Write-Host ('=' * 78)
Write-Host "PASSED $passed / $($fixtures.Count)"
if ($failed.Count -gt 0) {
    Write-Host "FAILED: $($failed -join ', ')" -ForegroundColor Red
    exit 1
}
Write-Host 'All fixtures round-tripped with identical fields.' -ForegroundColor Green
exit 0
