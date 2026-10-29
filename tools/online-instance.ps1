<#
.SYNOPSIS
  Starts one isolated test instance of the game for online testing on a single PC: its own user folder (settings,
  saves, caches, online identity) and its own log folder, pointed at a lobby.
.DESCRIPTION
  Instance "a" uses user\test-client-a and build\logs\test-client-a, instance "b" the same with "b", and so on. The first
  start copies dcp.ini and the saves from the main user folder (-CopyFrom) so both instances can play right away; later
  starts keep the instance's own files. Each instance makes its own online identity on first use, and the second one to
  bind gets another UDP port automatically (the game's port 3658 is taken by the first).
  Runs the launcher; Online play is the instance's own switch unless -ForceOnline. Nothing here is used by normal play.
.EXAMPLE
  tools\online-lobby.ps1                       # window 1: the development lobby
  tools\online-instance.ps1 -Name a            # window 2
  tools\online-instance.ps1 -Name b            # window 3
  tools\online-instance.ps1 -Name a -Lobby wss://dcp-lobby.saulobenigno.workers.dev   # through the production lobby
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidatePattern('^[A-Za-z0-9-]{1,16}$')][string]$Name,
    [string]$Lobby = 'ws://127.0.0.1:8790/',
    [string]$OnlineName = '',
    [string]$JoinCode = '',
    [string]$CopyFrom = '',
    [string]$Executable = '',
    [switch]$ForceOnline
)

. (Join-Path $PSScriptRoot '_common.ps1')

if (-not $Executable) { $Executable = Join-Path $script:RepoRoot 'build\cmake\clang64\bin\dragons_crown_pro.exe' }
if (-not (Test-Path -LiteralPath $Executable)) { Stop-Tool "Not built yet: $Executable" }
if (-not $CopyFrom) { $CopyFrom = Join-Path $script:RepoRoot 'user' }
# The development build loads SDL3, FreeType, libpng, libc++ and the other DLLs from the toolchain, as tools\build.ps1 -Run does.
$tc = Resolve-Toolchain 'auto'
$env:PATH = "$($tc.Bin);$env:PATH"

$user = Join-Path $script:RepoRoot "user\test-client-$Name"
$logs = Join-Path $script:RepoRoot "build\logs\test-client-$Name"
if (-not (Test-Path -LiteralPath $user)) {
    New-Item -ItemType Directory -Force $user | Out-Null
    $settings = Join-Path $CopyFrom 'dcp.ini'
    if (Test-Path -LiteralPath $settings) { Copy-Item -LiteralPath $settings -Destination $user }
    $saves = Join-Path $CopyFrom 'savedata'
    if (Test-Path -LiteralPath $saves) { Copy-Item -LiteralPath $saves -Destination $user -Recurse }
    Write-Ok "New instance folder $user (settings and saves copied from $CopyFrom)"
}
New-Item -ItemType Directory -Force $logs | Out-Null

# Online settings for this process only; the instance's dcp.ini is not changed.
if ($ForceOnline) { $env:DCP_ONLINE = '1' } else { Remove-Item Env:DCP_ONLINE -ErrorAction SilentlyContinue }
$env:DCP_LOBBY = $Lobby
# Without -OnlineName the instance uses the Online name of its own launcher page, as friends see it.
if ($OnlineName) { $env:DCP_ONLINE_NAME = $OnlineName } else { Remove-Item Env:DCP_ONLINE_NAME -ErrorAction SilentlyContinue }
if ($JoinCode) { $env:DCP_JOIN_CODE = $JoinCode } else { Remove-Item Env:DCP_JOIN_CODE -ErrorAction SilentlyContinue }
# New test characters get both offers unless the caller set these for a test.
if (-not $env:DCP_STORY_SKIP) { $env:DCP_STORY_SKIP = '1' }
if (-not $env:DCP_TUTORIAL_SKIP) { $env:DCP_TUTORIAL_SKIP = '1' }

Write-Step "Instance $Name (Client$($Name.ToUpper())) via $Lobby$(if ($OnlineName) { " as $OnlineName" })"
Write-Host "  user folder: $user"
Write-Host "  logs:        $logs"
Start-Process -FilePath $Executable -ArgumentList @('--user', "`"$user`"", '--logs', "`"$logs`"", '--launcher') -WorkingDirectory (Split-Path -Parent $Executable)
