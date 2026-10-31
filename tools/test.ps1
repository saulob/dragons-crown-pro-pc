<#
.SYNOPSIS
  Builds and runs the focused headless tests, and prints the supported-build parity summary. It is the gate to pass before
  committing parity work: no window, no game launch, no network.
.DESCRIPTION
  Needs the build configured once by tools\build.ps1. The tests are not part of the default build; only the focused test
  targets are built, incrementally.

  Every test runs even after one fails; the exit code is non-zero when any of them failed. The tests that decide the
  supported-build parity contract (the profile and capability matrix, update archives and languages, render size, True
  Ultrawide, LZCNT, System type, Skip Offers, startup save and cheat mappings, Online Play structure, unknown builds) are
  marked in the summary: if one fails, no build is reported as passing. Every supported build must offer every GLOBAL
  feature of CUSA10454 1.00; the global feature summary counts the builds that do.

  Local boot images (build\image) and prepared folders (game\prepared) are read only. A build without one is proven by
  synthetic images and reported as "PASS synthetic" (no real executable was inspected); "PASS real" means its
  executable image was inspected.
#>
[CmdletBinding()]
param()

. (Join-Path $PSScriptRoot '_common.ps1')
$tc = Resolve-Toolchain 'auto'
$env:PATH = "$($tc.Bin);$env:PATH"
$buildDir = Join-Path $script:RepoRoot 'build\cmake\clang64'
if (-not (Test-Path -LiteralPath (Join-Path $buildDir 'build.ninja'))) { Stop-Tool 'Run tools\build.ps1 first: it configures the build.' }

function Test-Step([string]$Title, [string]$Exe = '', [string]$Script = '', [switch]$Parity, [switch]$Capture) {
    [pscustomobject]@{ Title = $Title; Exe = $Exe; Script = $Script; Parity = [bool]$Parity; Capture = [bool]$Capture }
}

$tests = @(
    (Test-Step 'Windows memory tests (sparse backing, commitment, aliases, release)' 'dcp-memory-windows-tests'),
    (Test-Step 'launcher tests (profiles, install, update archives, languages, prompts, settings)' 'dcp-launcher-tests' -Parity),
    (Test-Step 'unsupported-build diagnostics tests (direct package analysis, one-shot report, loader validators, edge cases)' 'dcp-build-diagnostics-tests' -Parity),
    (Test-Step 'build feature parity tests (contract, update parity, mappings, unknown builds)' 'dcp-build-parity-tests' -Parity -Capture),
    (Test-Step 'feature locator tests (Cheats, Japanese NPC Voices, True Ultrawide: real images, synthetic images, known builds only)' 'dcp-feature-sites-tests' -Parity),
    (Test-Step 'trophy tests' 'dcp-trophy-tests'),
    (Test-Step 'cheat tests' 'dcp-cheats-tests' -Parity),
    (Test-Step 'Japanese NPC voice tests' 'dcp-voice-tests' -Parity),
    (Test-Step 'online System type hook tests' 'dcp-system-type-tests' -Parity),
    (Test-Step 'True Ultrawide hook tests' 'dcp-ultrawide-tests' -Parity),
    (Test-Step 'True Ultrawide side area tests' 'dcp-ultrawide-sides-tests'),
    (Test-Step 'module history tests (crash report)' 'dcp-module-history-tests'),
    (Test-Step 'MOVBE, BMI1 and LZCNT tests' 'dcp-isa-tests' -Parity),
    (Test-Step 'text tests (text dialog, system font set)' 'dcp-text-tests'),
    (Test-Step 'cursor visibility tests' 'dcp-cursor-tests'),
    (Test-Step 'window fit tests' 'dcp-window-tests'),
    (Test-Step 'present tests (swapchain images, present mode, flip pacing)' 'dcp-present-tests'),
    (Test-Step 'frame pacing tests (Frame rate setting, repeat cadence, limiter)' 'dcp-frame-pacing-tests'),
    (Test-Step 'Options menu tests (chord, close gate, live and restart settings, dcp.ini, Online Play lock)' 'dcp-options-menu-tests'),
    (Test-Step 'AMD water workaround tests (vendor defaults, no hotkey)' 'dcp-amd-water-tests'),
    (Test-Step 'water diagnostics tests (AMD only, deduplication, cap)' 'dcp-water-diag-tests'),
    (Test-Step 'save-data tests (mounts, Japanese startup save hook)' 'dcp-savedata-tests' -Parity),
    (Test-Step 'tiling tests (GPU tile layouts and addresses)' 'dcp-tiling-tests'),
    (Test-Step 'updater tests' 'dcp-update-tests'),
    (Test-Step 'updater helper tests' 'dcp-update-helper-tests'),
    (Test-Step 'RTC tests (Win32 file time)' 'dcp-rtc-tests'),
    (Test-Step 'Skip Offer tests (synthetic and local boot images)' 'dcp-skip-offers-tests' -Parity),
    (Test-Step 'online tests (JSON, identity, STUN, peer transport over loopback)' 'dcp-net-tests'),
    (Test-Step 'online build tests (every supported build''s online imports, one lobby game, cheat lock)' 'dcp-online-builds-tests' -Parity),
    (Test-Step 'online test instance script tests (-Build, with -WhatIf only)' -Script 'tests\online_instance_tests.ps1'),
    (Test-Step 'online HLE tests (needs DCP_TEST_LOBBY; skipped without it)' 'dcp-online-hle-tests'),
    (Test-Step 'Windows log tests (closed standard streams)' 'dcp-log-tests')
)

Write-Step 'Building the focused tests'
& ninja -C $buildDir dcp-memory-windows-tests dcp-launcher-tests dcp-build-diagnostics-tests dcp-build-parity-tests dcp-feature-sites-tests dcp-trophy-tests dcp-cheats-tests dcp-text-tests dcp-tiling-tests dcp-cursor-tests dcp-window-tests dcp-present-tests dcp-frame-pacing-tests dcp-options-menu-tests dcp-amd-water-tests dcp-water-diag-tests dcp-savedata-tests dcp-voice-tests dcp-system-type-tests dcp-ultrawide-tests dcp-ultrawide-sides-tests dcp-isa-tests dcp-module-history-tests dcp-update-tests dcp-update-helper-tests dcp-log-tests dcp-net-tests dcp-online-hle-tests dcp-online-builds-tests dcp-skip-offers-tests dcp-rtc-tests
if ($LASTEXITCODE -ne 0) { Stop-Tool 'Test build failed' }
foreach ($test in $tests) {
    if ($test.Exe -and -not (Test-Path -LiteralPath (Join-Path $buildDir "bin\$($test.Exe).exe"))) { Stop-Tool "$($test.Exe) is not built: add it to the ninja line above." }
}

$failed = New-Object System.Collections.Generic.List[string]
$parityFailed = New-Object System.Collections.Generic.List[string]
$parityOutput = @()
foreach ($test in $tests) {
    Write-Step "Running the $($test.Title)"
    if ($test.Script) {
        & powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $script:RepoRoot $test.Script)
    } elseif ($test.Capture) {
        $parityOutput = @(& (Join-Path $buildDir "bin\$($test.Exe).exe") 2>&1 | ForEach-Object { $line = "$_"; Write-Host $line; $line })
    } else {
        & (Join-Path $buildDir "bin\$($test.Exe).exe")
    }
    if ($LASTEXITCODE -ne 0) {
        $name = if ($test.Exe) { $test.Exe } else { $test.Script }
        $failed.Add($name)
        if ($test.Parity) { $parityFailed.Add($name) }
        Write-Host "FAILED: $name (exit code $LASTEXITCODE)" -ForegroundColor Red
    }
}

Write-Host ''
Write-Host 'Build parity:' -ForegroundColor Cyan
$rows = @($parityOutput | Where-Object { $_ -like 'PARITY-BUILD|*' } | ForEach-Object { ,$_.Split('|') })
if ($rows.Count -eq 0) {
    Write-Host '  no result from dcp-build-parity-tests' -ForegroundColor Red
    if (-not $failed.Contains('dcp-build-parity-tests')) { $failed.Add('dcp-build-parity-tests') }
}
foreach ($row in $rows) {
    $status = $row[2]
    $good = $status -like 'PASS*' -and $parityFailed.Count -eq 0
    if ($status -like 'PASS*' -and -not $good) { $status = 'FAIL (a shared parity test failed)' }
    Write-Host ('  {0,-34} {1}' -f $row[1], $status) -ForegroundColor $(if ($good) { 'Green' } else { 'Red' })
}
Write-Host ''
Write-Host 'Global feature parity:' -ForegroundColor Cyan
$features = @($parityOutput | Where-Object { $_ -like 'PARITY-FEATURE|*' } | ForEach-Object { ,$_.Split('|') })
if ($features.Count -eq 0) {
    Write-Host '  no result from dcp-build-parity-tests' -ForegroundColor Red
    if (-not $failed.Contains('dcp-build-parity-tests')) { $failed.Add('dcp-build-parity-tests') }
}
foreach ($row in $features) {
    $status = $row[2]
    $good = $status -like 'PASS*' -and $parityFailed.Count -eq 0
    if ($status -like 'PASS*' -and -not $good) { $status = 'FAIL (a shared parity test failed)' }
    Write-Host ('  {0,-34} {1}' -f $row[1], $status) -ForegroundColor $(if ($good) { 'Green' } else { 'Red' })
}
Write-Host ''
if ($failed.Count -gt 0) {
    Write-Host ('FAILED: ' + ($failed -join ', ')) -ForegroundColor Red
    exit 1
}
Write-Host 'All tests passed.' -ForegroundColor Green
exit 0
