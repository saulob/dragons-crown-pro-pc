<#
.SYNOPSIS
  Builds and runs the focused launcher tests: game file validation, the settings file, the Play gate, the window
  placement, the Display options and the Controller prompts patch. They use synthetic files in a temporary folder: no
  window, no game files.
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
& ninja -C $buildDir dcp-launcher-tests
if ($LASTEXITCODE -ne 0) { Stop-Tool 'Test build failed' }

Write-Step 'Running the launcher tests'
& (Join-Path $buildDir 'bin\dcp-launcher-tests.exe')
exit $LASTEXITCODE
