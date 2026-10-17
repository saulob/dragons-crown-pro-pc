<#
.SYNOPSIS
  Reads the package metadata (read-only), the build each package belongs to, and the executables of the prepared
  folders.
.PARAMETER GameRoot
  Folder holding the .pkg (default: <repo>\game).
.PARAMETER List
  Also prints the package's top-level file summary (needs cryptography; reads only the PFS index).
#>
[CmdletBinding()]
param(
    [string]$GameRoot,
    [switch]$List
)

. (Join-Path $PSScriptRoot '_common.ps1')
if (-not $GameRoot) { $GameRoot = Join-Path $script:RepoRoot 'game' }

$packages = @()
if (Test-Path -LiteralPath $GameRoot) { $packages = @(Get-ChildItem -LiteralPath $GameRoot -Filter '*.pkg') }
if ($packages.Count -eq 0) { Stop-Tool "No .pkg files in '$GameRoot'." }

$python = Get-Python
if (-not $python) { Stop-Tool 'Python 3 not found. Run tools\setup.ps1.' }

$verdict = 0
$infos = @()
foreach ($pkg in $packages) {
    Write-Step $pkg.FullName
    $json = & $python.Exe @($python.Args) '-I' (Join-Path $script:RepoRoot 'scripts\pkg_info.py') $pkg.FullName '--json'
    if ($LASTEXITCODE -ne 0) { Stop-Tool "$($pkg.Name) is not a readable PS4 PKG" }
    $info = ($json -join "`n") | ConvertFrom-Json
    $infos += $info
    $sfo = $info.param_sfo
    Write-Host ("  title id     : {0}" -f $info.title_id)
    Write-Host ("  title        : {0}" -f $sfo.TITLE)
    Write-Host ("  kind         : {0} (CATEGORY={1})" -f $info.kind, $sfo.CATEGORY)
    Write-Host ("  APP_VER      : {0}   VERSION: {1}   SDK (pubtoolinfo): {2}" -f $sfo.APP_VER, $sfo.VERSION, (($sfo.PUBTOOLINFO -split ',' | Where-Object { $_ -like 'sdk_ver=*' }) -replace 'sdk_ver=', ''))
    Write-Host ("  content id   : {0}" -f $info.content_id)
    Write-Host ("  size         : {0:N0} bytes, header agrees: {1}" -f $info.file_size, $info.size_matches_header)
    Write-Host ("  structure    : {0} entries, PFS image inside file: {1}, entries inside file: {2}" -f $info.entry_count, $info.pfs_within_file, $info.entries_within_file)
    Write-Host ("  fake PKG     : {0} (image key index {1})" -f $info.fake_pkg, $info.image_key_index)
    Write-Host ("  delta info   : {0}" -f $info.has_delta_info)
    if ($List) {
        if (-not (Test-PythonModule 'cryptography')) { Stop-Tool 'Listing needs cryptography: run tools\setup.ps1 -InstallPython' }
        & $python.Exe @($python.Args) '-I' (Join-Path $script:RepoRoot 'scripts\pkg_extract.py') $pkg.FullName '--list' |
            Where-Object { $_ -match '^(files|pfs image)|^[d-]\s+\d+\s+[^/]+(/[^/]+)?$' }
    }
}

Write-Step 'Profile'
foreach ($info in $infos) {
    if (-not $info.profile) {
        Write-Warn "$($info.file): not a package of a supported build (APP_VER $($info.param_sfo.APP_VER), $($info.kind)); ignored"
        continue
    }
    $build = $info.profile
    if (-not $info.fake_pkg) { Write-Bad "$($info.file): retail PKG (needs console keys)"; $verdict = 1 }
    if ($build.layer -eq 0) {
        Write-Ok ("{0}: {1} {2}, a complete game" -f $info.file, $build.title_id, $build.version)
        continue
    }
    $base = @($infos | Where-Object { $_.profile -and $_.profile.title_id -eq $build.title_id -and $_.profile.layer -eq 0 })
    if ($base.Count -gt 0) {
        Write-Ok ("{0}: {1} {2}, an update of the base package" -f $info.file, $build.title_id, $build.version)
    }
    else {
        Write-Bad ("{0}: {1} {2} needs the base package of {1}, which is missing" -f $info.file, $build.title_id, $build.version)
        $verdict = 1
    }
}
if (-not ($infos | Where-Object { $_.profile })) { Write-Bad 'no package of a supported build (CUSA10454, CUSA07001 or CUSA10209)'; $verdict = 1 }

$prepared = Join-Path $GameRoot 'prepared'
$views = @(Get-ChildItem -LiteralPath $prepared -Directory -ErrorAction SilentlyContinue)
if ($views.Count -eq 0) { Write-Warn 'nothing prepared yet: run tools\prepare-game.ps1 to read the executables' }
foreach ($view in $views) {
    Write-Step "Executables ($($view.FullName))"
    $code = Invoke-Script 'game_report.py' @($view.FullName)
    if ($code -ne 0) { $verdict = 1 }
}
exit $verdict
