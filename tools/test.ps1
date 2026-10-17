<#
.SYNOPSIS
  Builds and runs the focused launcher, cheat, text, cursor and tiling tests: game file validation, the settings file, the
  Play gate, the window placement, the Display options, the Controller prompts patch, the text dialog, the system font
  set, the cursor auto-hide timer and the GPU tile layouts. They use synthetic files in a temporary folder: no window,
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
& ninja -C $buildDir dcp-launcher-tests dcp-trophy-tests dcp-cheats-tests dcp-text-tests dcp-tiling-tests dcp-cursor-tests dcp-window-tests dcp-savedata-tests
if ($LASTEXITCODE -ne 0) { Stop-Tool 'Test build failed' }

Write-Step 'Running the launcher tests'
& (Join-Path $buildDir 'bin\dcp-launcher-tests.exe')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Step 'Running the trophy tests'
& (Join-Path $buildDir 'bin\dcp-trophy-tests.exe')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Step 'Running the cheat tests'
& (Join-Path $buildDir 'bin\dcp-cheats-tests.exe')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Step 'Running the text tests (text dialog, system font set)'
& (Join-Path $buildDir 'bin\dcp-text-tests.exe')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Step 'Running the cursor auto-hide tests'
& (Join-Path $buildDir 'bin\dcp-cursor-tests.exe')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Step 'Running the window fit tests'
& (Join-Path $buildDir 'bin\dcp-window-tests.exe')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Step 'Running the save-data tests (mounts, Japanese startup save hook)'
& (Join-Path $buildDir 'bin\dcp-savedata-tests.exe')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Step 'Running the tiling tests (GPU tile layouts and addresses)'
& (Join-Path $buildDir 'bin\dcp-tiling-tests.exe')
exit $LASTEXITCODE
