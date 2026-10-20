<#
.SYNOPSIS
  Builds dcp-loader.exe and, on Windows, trg-updater.exe incrementally (MSYS2 CLANG64, CMake + Ninja). -Run launches it.
.DESCRIPTION
  Reuses what is current and updates only what is stale: the video core dependencies (gpu\deps.txt), the CMake
  configuration and, once the game is installed, its boot image (build\image\v1.03). The one build configuration,
  RelWithDebInfo (-O2 -g -DNDEBUG), is what is tested and what a package must contain; nothing else is built.
  The game is installed from the launcher (Play page, Install game...), as in a release.
.PARAMETER Run
  Launches the loader that was just built and stays attached until the game is closed. The launcher opens first
  when its rules say so (docs\LAUNCHER.md). Logs to build\logs\dragons_crown_pro_NNN.log.
.PARAMETER Clean
  Deletes build\cmake first (the installed game, the boot image and the logs are kept).
.PARAMETER CheckImports
  Diagnostic: with -Run, lists the PS4 imports the runtime lacks instead of running the game.
.PARAMETER TimeoutSeconds
  Diagnostic for automated tests only: with -Run, kills the loader after this many seconds (and skips the launcher).
.PARAMETER LoaderArgs
  Extra arguments for dcp-loader.exe (with -Run).
.EXAMPLE
  .\tools\build.ps1
  .\tools\build.ps1 -Run
#>
[CmdletBinding()]
param(
    [switch]$Run,
    [switch]$Clean,
    [switch]$CheckImports,
    [int]$TimeoutSeconds = 0,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$LoaderArgs = @()
)

. (Join-Path $PSScriptRoot '_common.ps1')
$version = '1.03'
$config = 'RelWithDebInfo'
$root = $script:RepoRoot
$buildDir = Join-Path $root 'build\cmake\clang64'
$loader = Join-Path $buildDir 'bin\dcp-loader.exe'
$view = Join-Path $root "game\prepared\v$version"
$marker = Join-Path $view '.dcp_view.json'
$image = Join-Path $root "build\image\v$version"
$loadExecExit = 30 # dcp-loader: sceSystemServiceLoadExec ended the process

# Rebuilt when the game was installed again or the boot image builder changed.
function Test-ImageCurrent {
    $newest = (Get-Item -LiteralPath $marker).LastWriteTime
    foreach ($source in 'launcher\boot_image.cpp', 'launcher\game_profile.h') {
        $time = (Get-Item -LiteralPath (Join-Path $root $source)).LastWriteTime
        if ($time -gt $newest) { $newest = $time }
    }
    foreach ($name in 'boot.bin', 'content.bin', 'image.json') {
        $file = Join-Path $image $name
        if (-not (Test-Path -LiteralPath $file) -or (Get-Item -LiteralPath $file).LastWriteTime -lt $newest) { return $false }
    }
    return $true
}

$tc = Resolve-Toolchain 'auto'
$env:PATH = "$($tc.Bin);$env:PATH"
Write-Step "Toolchain clang64 ($($tc.Bin)), $config"

Sync-GpuDependencies

if ($Clean -and (Test-Path -LiteralPath $buildDir)) {
    Write-Step 'Cleaning build\cmake'
    Remove-Item -LiteralPath $buildDir -Recurse -Force
}
$cache = Join-Path $buildDir 'CMakeCache.txt'
$configured = (Test-Path -LiteralPath (Join-Path $buildDir 'build.ninja')) -and (Test-Path -LiteralPath $cache) -and
    (Select-String -Path $cache -SimpleMatch "CMAKE_BUILD_TYPE:STRING=$config" -Quiet)
if (-not $configured) {
    Write-Step 'Configuring'
    & cmake -S $root -B $buildDir -G Ninja "-DCMAKE_BUILD_TYPE=$config" '-DCMAKE_C_COMPILER=clang'
    if ($LASTEXITCODE -ne 0) { Stop-Tool 'CMake configuration failed' }
}
Write-Step 'Building'
& ninja -C $buildDir dcp-loader trg-updater
if ($LASTEXITCODE -ne 0) { Stop-Tool 'Build failed' }
if (-not (Test-Path -LiteralPath $loader)) { Stop-Tool "Missing $loader" }

if (-not (Test-Path -LiteralPath $marker)) {
    Write-Warn 'The game is not installed yet: start the game and use Install game... on the launcher''s Play page.'
}
elseif (-not (Test-ImageCurrent)) {
    Write-Step 'Updating the boot image'
    & $loader --prepare-image $view $image
    if ($LASTEXITCODE -ne 0) { Stop-Tool 'Boot image build failed' }
}
Write-Host ''
Write-Host "Built $loader" -ForegroundColor Green
if (-not $Run) { return }

$running = @(Get-Process dcp-loader -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $loader })
if ($running.Count -gt 0) {
    Write-Warn "dcp-loader.exe is already running (PID $($running.Id -join ', ')); not launching another copy."
    return
}
# The loader finds the game, the boot image, user\ and build\logs by itself in this tree.
$arguments = @()
if ($CheckImports) { $arguments += '--check-imports' }
if ($TimeoutSeconds -gt 0) { $arguments += '--dcp-skip-launcher' }
$arguments += $LoaderArgs
Push-Location (Split-Path $loader -Parent)
try {
    if ($TimeoutSeconds -gt 0) {
        $process = Start-Process -FilePath $loader -ArgumentList $arguments -NoNewWindow -PassThru
        $null = $process.Handle # keeps the exit code readable after the process ends
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            Write-Warn "Stopping dcp-loader.exe after $TimeoutSeconds seconds"
            $process.Kill()
            $process.WaitForExit()
        }
        $code = $process.ExitCode
    }
    else {
        & $loader @arguments
        $code = $LASTEXITCODE
    }
}
finally {
    Pop-Location
}
Write-Host "dcp-loader.exe exit code $code"
if ($code -eq $loadExecExit) { Write-Host 'The game requested sceSystemServiceLoadExec (see the LoadExec lines above).' }
exit $code
