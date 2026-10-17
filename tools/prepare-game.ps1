<#
.SYNOPSIS
  Developer convenience: unpacks a fake package from game\ (resumable) into the prepared /app0 tree.
.DESCRIPTION
  game\extracted\<folder>                       what each package contains (the .pkg files are only read)
  game\prepared\<folder>                        the /app0 tree (hard links, no copies)
  Needs Python with the cryptography package (tools\setup.ps1 -InstallPython). tools\build.ps1 then builds the boot
  image. Players and the launcher do not use this: Install game... unpacks the .pkg natively.
.PARAMETER Title
  CUSA10454 (USA), CUSA07001 (Japan) or CUSA10209 (Asia, Hong Kong). Empty: the title of the packages.
.PARAMETER Version
  1.00, 1.02 or 1.03 (CUSA07001); 1.00 (CUSA10454); 1.00 or 1.02 (CUSA10209). Empty: the one build the packages complete.
.PARAMETER Scope
  core: executables, modules and metadata only (about 25 MB). full: all game data (about 5.7 GB, the CPK archives).
#>
[CmdletBinding()]
param(
    [ValidateSet('', 'CUSA10454', 'CUSA07001', 'CUSA10209')][string]$Title = '',
    [ValidateSet('', '1.00', '1.02', '1.03')][string]$Version = '',
    [ValidateSet('core', 'full')][string]$Scope = 'full',
    [string]$GameRoot
)

. (Join-Path $PSScriptRoot '_common.ps1')
if (-not $GameRoot) { $GameRoot = Join-Path $script:RepoRoot 'game' }
if (-not (Test-PythonModule 'cryptography')) { Stop-Tool 'cryptography is missing: run tools\setup.ps1 -InstallPython' }

$selectArgs = @()
if ($Title) { $selectArgs += @('--title', $Title) }
if ($Version) { $selectArgs += @('--version', $Version) }
Write-Step "Unpacking and layering ($(if ($Title) { $Title } else { 'the packages' }), $Scope)"
$code = Invoke-Script 'prepare_game.py' (@('--game-root', $GameRoot) + $selectArgs + @('--scope', $Scope))
if ($code -ne 0) { exit $code }
Write-Ok 'Prepared. tools\build.ps1 builds the boot image.'
