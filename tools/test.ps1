<#
.SYNOPSIS
  Builds and runs the focused Windows memory, launcher, cheat, text, cursor, tiling and updater tests: game file validation, the settings file, the
  Play gate, the window placement, the Display options, the Controller prompts patch, the text dialog, the system font
  set, the cursor visibility rules, the GPU tile layouts and the updater (versions, release JSON, SHA-256, ZIP safety,
  staging, install/rollback, helper replacement and restart quoting). They use synthetic files in a temporary folder: no window,
  no game files.
.DESCRIPTION
  Needs the build configured once by tools\build.ps1. The tests are not part of the default build.
#>
[CmdletBinding()]
param()

. (Join-Path $PSScriptRoot '_common.ps1')
$tc = Resolve-Toolchain 'auto'
$env:PATH = "$($tc.Bin);$env:PATH"
$buildDir = Join-Path $script:RepoRoot 'build\cmake\clang64'
if (-not (Test-Path -LiteralPath (Join-Path $buildDir 'build.ninja'))) { Stop-Tool 'Run tools\build.ps1 first: it configures the build.' }

Write-Step 'Building the launcher tests'
& ninja -C $buildDir dcp-memory-windows-tests dcp-launcher-tests dcp-trophy-tests dcp-cheats-tests dcp-text-tests dcp-tiling-tests dcp-cursor-tests dcp-window-tests dcp-present-tests dcp-frame-pacing-tests dcp-amd-water-tests dcp-savedata-tests dcp-voice-tests dcp-system-type-tests dcp-ultrawide-tests dcp-ultrawide-sides-tests dcp-isa-tests dcp-update-tests dcp-update-helper-tests dcp-log-tests dcp-net-tests dcp-online-hle-tests dcp-skip-offers-tests dcp-rtc-tests
if ($LASTEXITCODE -ne 0) { Stop-Tool 'Test build failed' }

Write-Step 'Running the Windows memory tests (sparse backing, commitment, aliases, release)'
& (Join-Path $buildDir 'bin\dcp-memory-windows-tests.exe')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Step 'Running the launcher tests'
& (Join-Path $buildDir 'bin\dcp-launcher-tests.exe')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Step 'Running the trophy tests'
& (Join-Path $buildDir 'bin\dcp-trophy-tests.exe')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Step 'Running the cheat tests'
& (Join-Path $buildDir 'bin\dcp-cheats-tests.exe')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Step 'Running the Japanese NPC voice tests'
& (Join-Path $buildDir 'bin\dcp-voice-tests.exe')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Step 'Running the online System type hook tests'
& (Join-Path $buildDir 'bin\dcp-system-type-tests.exe')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Step 'Running the True Ultrawide hook tests'
& (Join-Path $buildDir 'bin\dcp-ultrawide-tests.exe')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Step 'Running the True Ultrawide side area tests'
& (Join-Path $buildDir 'bin\dcp-ultrawide-sides-tests.exe')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Step 'Running the MOVBE and BMI1 emulation tests'
& (Join-Path $buildDir 'bin\dcp-isa-tests.exe')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Step 'Running the text tests (text dialog, system font set)'
& (Join-Path $buildDir 'bin\dcp-text-tests.exe')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Step 'Running the cursor visibility tests'
& (Join-Path $buildDir 'bin\dcp-cursor-tests.exe')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Step 'Running the window fit tests'
& (Join-Path $buildDir 'bin\dcp-window-tests.exe')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Step 'Running the present tests (swapchain images, present mode, flip pacing)'
& (Join-Path $buildDir 'bin\dcp-present-tests.exe')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Step 'Running the frame pacing tests (Frame rate setting, repeat cadence, limiter)'
& (Join-Path $buildDir 'bin\dcp-frame-pacing-tests.exe')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Step 'Running the AMD water fix tests (vendor defaults, F3 presses)'
& (Join-Path $buildDir 'bin\dcp-amd-water-tests.exe')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Step 'Running the save-data tests (mounts, Japanese startup save hook)'
& (Join-Path $buildDir 'bin\dcp-savedata-tests.exe')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Step 'Running the tiling tests (GPU tile layouts and addresses)'
& (Join-Path $buildDir 'bin\dcp-tiling-tests.exe')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Step 'Running the updater tests'
& (Join-Path $buildDir 'bin\dcp-update-tests.exe')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Step 'Running the updater helper tests'
& (Join-Path $buildDir 'bin\dcp-update-helper-tests.exe')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Step 'Running the RTC tests (Win32 file time)'
& (Join-Path $buildDir 'bin\dcp-rtc-tests.exe')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Step 'Running the Skip Offer tests (synthetic and local boot images)'
& (Join-Path $buildDir 'bin\dcp-skip-offers-tests.exe')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Step 'Running the online tests (JSON, identity, STUN, peer transport over loopback)'
& (Join-Path $buildDir 'bin\dcp-net-tests.exe')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Step 'Running the online HLE tests (needs DCP_TEST_LOBBY; skipped without it)'
& (Join-Path $buildDir 'bin\dcp-online-hle-tests.exe')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Step 'Running the Windows log tests (closed standard streams)'
& (Join-Path $buildDir 'bin\dcp-log-tests.exe')
exit $LASTEXITCODE
