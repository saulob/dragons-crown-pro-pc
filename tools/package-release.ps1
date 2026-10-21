<#
.SYNOPSIS
  Prepares a complete release in dist\: the Windows x64 ZIP and the Linux x86_64 AppImage, each with its .sha256 file.
  Local only: nothing is published, tagged or uploaded.
.DESCRIPTION
  Runs tools\package.ps1 for Windows, then tools\package-release-linux.sh through wsl.exe for the AppImage (the Linux
  packaging itself is tools\package-linux.sh). Both run even if the first fails, so one run shows every problem, and the
  release counts as prepared only when both artifacts exist and their .sha256 files match. The version is
  the project VERSION in CMakeLists.txt, the only place it is written.

  The Linux side needs the WSL2 distribution DcpFocalBuild: Ubuntu 20.04, the AppImage's glibc baseline, prepared once
  by packaging/linux/ci-deps.sh (docs/LINUX.md). Nothing is installed or changed in WSL.
.PARAMETER Clean
  Clean build for both platforms.
.PARAMETER SkipBuild
  Package the existing builds of both platforms.
.PARAMETER WslDistro
  The WSL distribution to build in.
.EXAMPLE
  .\tools\package-release.ps1
#>
[CmdletBinding()]
param(
    [switch]$Clean,
    [switch]$SkipBuild,
    [string]$WslDistro = 'DcpFocalBuild'
)

. (Join-Path $PSScriptRoot '_common.ps1')

$root = $script:RepoRoot
$cmake = Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Raw
if ($cmake -notmatch '(?m)^project\(dragons_crown_pro_pc VERSION ([0-9]+\.[0-9]+\.[0-9]+)') { Stop-Tool 'project VERSION not found in CMakeLists.txt' }
$version = $Matches[1]
$dist = Join-Path $root 'dist'
$windowsName = "DragonsCrownProPC-v$version-windows-x64"
$linuxName = "DragonsCrownProPC-v$version-x86_64"
$windowsFiles = @("$windowsName.zip", "$windowsName.zip.sha256")
$linuxFiles = @("$linuxName.AppImage", "$linuxName.AppImage.sha256")

function Test-Checksum([string]$Artifact, [string]$Checksum) {
    $line = (Get-Content -LiteralPath $Checksum -TotalCount 1)
    if ($line -notmatch '^([0-9a-f]{64})  (.+)$') { return "$(Split-Path $Checksum -Leaf) is not '<sha256>  <file name>'" }
    if ($Matches[2] -ne (Split-Path $Artifact -Leaf)) { return "$(Split-Path $Checksum -Leaf) names $($Matches[2])" }
    $actual = (Get-FileHash -LiteralPath $Artifact -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $Matches[1]) { return "$(Split-Path $Checksum -Leaf) does not match the file" }
    return $null
}

function Test-Artifacts([string[]]$Files) {
    $problems = New-Object System.Collections.Generic.List[string]
    foreach ($file in $Files) {
        if (-not (Test-Path -LiteralPath (Join-Path $dist $file) -PathType Leaf)) { $problems.Add("missing: $file") }
    }
    if ($problems.Count -eq 0) {
        $problem = Test-Checksum (Join-Path $dist $Files[0]) (Join-Path $dist $Files[1])
        if ($problem) { $problems.Add($problem) }
    }
    return $problems
}

function Remove-Artifacts([string[]]$Files) {
    foreach ($file in $Files) { Remove-Item -LiteralPath (Join-Path $dist $file) -ErrorAction SilentlyContinue }
}

$results = [ordered]@{ 'Windows package' = $null; 'Linux AppImage' = $null }
$notes = New-Object System.Collections.Generic.List[string]

Write-Step "Dragon's Crown Pro PC $version - release packages (local only)"
New-Item -ItemType Directory -Force $dist | Out-Null

# Windows x64
Write-Step 'Windows x64 ZIP (tools\package.ps1)'
Remove-Artifacts $windowsFiles
Remove-Item -LiteralPath (Join-Path $dist "$windowsName.sha256") -ErrorAction SilentlyContinue
$windowsArgs = @()
if ($Clean) { $windowsArgs += '-Clean' }
if ($SkipBuild) { $windowsArgs += '-SkipBuild' }
$powershell = (Get-Process -Id $PID).Path
& $powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'package.ps1') @windowsArgs
$windowsProblems = @()
if ($LASTEXITCODE -ne 0) { $windowsProblems += "tools\package.ps1 failed (exit $LASTEXITCODE)" }
else { $windowsProblems = @(Test-Artifacts $windowsFiles) }
$results['Windows package'] = ($windowsProblems.Count -eq 0)
foreach ($problem in $windowsProblems) { $notes.Add("Windows: $problem") }

# Linux x86_64 through WSL
Write-Step 'Linux x86_64 AppImage (tools\package-release-linux.sh in WSL)'
Remove-Artifacts $linuxFiles
$wslArgs = @('-d', $WslDistro)
$wsl = Get-Command wsl.exe -ErrorAction SilentlyContinue
$linuxProblems = @()
if (-not $wsl) {
    $linuxProblems += 'wsl.exe was not found. The AppImage is built in WSL2, in an Ubuntu 20.04 distribution prepared as docs/LINUX.md describes.'
} else {
    $probe = & wsl.exe @wslArgs --exec /bin/uname -sm 2>&1
    if ($LASTEXITCODE -ne 0 -or "$probe" -notmatch 'Linux x86_64') {
        $linuxProblems += "WSL distribution $WslDistro is not usable ($(("$probe" -replace '\0', '').Trim())). Create it as docs/LINUX.md describes, or name another Ubuntu 20.04 one with -WslDistro."
    } else {
        $repoWsl = (& wsl.exe @wslArgs --exec /usr/bin/wslpath -a -u $root | Out-String).Trim()
        if ($LASTEXITCODE -ne 0 -or -not $repoWsl) {
            $linuxProblems += 'wslpath could not convert the repository path.'
        } else {
            $linuxArgs = @()
            if ($Clean) { $linuxArgs += '--clean' }
            if ($SkipBuild) { $linuxArgs += '--skip-build' }
            & wsl.exe @wslArgs --exec /bin/bash "$repoWsl/tools/package-release-linux.sh" @linuxArgs
            if ($LASTEXITCODE -ne 0) {
                $linuxProblems += "tools/package-release-linux.sh failed (exit $LASTEXITCODE). If it listed missing tools, prepare WSL once with: wsl -d $WslDistro -u root --exec /bin/bash $repoWsl/packaging/linux/ci-deps.sh"
            } else {
                $linuxProblems = @(Test-Artifacts $linuxFiles)
            }
        }
    }
}
$results['Linux AppImage'] = ($linuxProblems.Count -eq 0)
foreach ($problem in $linuxProblems) { $notes.Add("Linux: $problem") }

Write-Host ''
foreach ($key in $results.Keys) {
    if ($results[$key]) { Write-Host ("{0}: OK" -f $key) -ForegroundColor Green }
    else { Write-Host ("{0}: FAILED" -f $key) -ForegroundColor Red }
}
if ($results.Values -contains $false) {
    Write-Host ''
    foreach ($note in $notes) { Write-Host "  $note" -ForegroundColor Red }
    Write-Host ''
    Write-Host 'RELEASE NOT PREPARED' -ForegroundColor Red
    exit 1
}

Write-Host ''
Write-Host 'RELEASE PREPARED (nothing was published)' -ForegroundColor Green
foreach ($file in $windowsFiles + $linuxFiles) { Write-Host "  $(Join-Path $dist $file)" }
Write-Host "  Symbols (not for release): $(Join-Path $dist "$windowsName-symbols"), $(Join-Path $dist "$linuxName-symbols")"
