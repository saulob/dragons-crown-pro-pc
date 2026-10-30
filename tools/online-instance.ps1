<#
.SYNOPSIS
  Starts one isolated test instance of the game for online testing on a single PC: its own user folder (settings,
  saves, caches, online identity), its own log folder and, with -Build, its own game build, on the production online
  service.
.DESCRIPTION
  Instance "a" uses user\test-client-a and build\logs\test-client-a, instance "b" the same with "b", and so on. The first
  start copies dcp.ini and the saves from the main user folder (-CopyFrom) so both instances can play right away; later
  starts keep the instance's own files. Each instance makes its own online identity on first use, and the second one to
  bind gets another UDP port automatically (the game's port 3658 is taken by the first).
  -Build pins the instance to one prepared build (DCP_GAME_BUILD for this instance only): its launcher and game use that
  build whatever another instance or the normal launcher activates, and its package selection is off. The prepared
  folders are shared and never copied. Without -Build the instance uses the active build, as the normal launcher does.
  Runs the launcher; Online play is the instance's own switch unless -ForceOnline. Nothing here is used by normal play,
  and the environment of the calling shell is left as it was.
.EXAMPLE
  tools\online-instance.ps1 -Name a -Build CUSA10454-1.00     # terminal 1
  tools\online-instance.ps1 -Name b -Build CUSA10209-1.00     # terminal 2
#>
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [Parameter(Mandatory = $true)][ValidatePattern('^[A-Za-z0-9-]{1,16}$')][string]$Name,
    [string]$Build = '',
    [string]$OnlineName = '',
    [string]$JoinCode = '',
    [string]$CopyFrom = '',
    [string]$Executable = '',
    [switch]$ForceOnline
)

. (Join-Path $PSScriptRoot '_common.ps1')

# Build key (title ID-profile version) -> prepared folder and label; launcher/game_profile.cpp is the source, and
# tests/launcher_tests.cpp checks this table against it.
$GameBuilds = [ordered]@{
    'CUSA10454-1.00' = @{ Folder = 'v1.03';          Label = 'CUSA10454 1.00 USA' }
    'CUSA10454-1.01' = @{ Folder = 'CUSA10454-1.01'; Label = 'CUSA10454 1.01 USA' }
    'CUSA07001-1.00' = @{ Folder = 'CUSA07001-1.00'; Label = 'CUSA07001 1.00 Japan' }
    'CUSA07001-1.02' = @{ Folder = 'CUSA07001-1.02'; Label = 'CUSA07001 1.02 Japan' }
    'CUSA07001-1.03' = @{ Folder = 'CUSA07001-1.03'; Label = 'CUSA07001 1.03 Japan' }
    'CUSA10209-1.00' = @{ Folder = 'CUSA10209-1.00'; Label = 'CUSA10209 1.00 Asia/Hong Kong' }
    'CUSA10209-1.02' = @{ Folder = 'CUSA10209-1.02'; Label = 'CUSA10209 1.02 Asia/Hong Kong' }
}

# The executable's game folders, as launcher/paths.cpp finds them: the repository's for a build-tree executable.
function Get-GameRoots([string]$ExecutablePath) {
    $bin = Split-Path -Parent $ExecutablePath
    $cmake = Split-Path -Parent (Split-Path -Parent $bin)
    $buildDir = Split-Path -Parent $cmake
    if ((Split-Path -Leaf $bin) -eq 'bin' -and (Split-Path -Leaf $cmake) -eq 'cmake' -and (Split-Path -Leaf $buildDir) -eq 'build') {
        $root = Split-Path -Parent $buildDir
        return @{ Prepared = Join-Path $root 'game\prepared'; Image = Join-Path $buildDir 'image' }
    }
    return @{ Prepared = Join-Path $bin 'game\prepared'; Image = Join-Path $bin 'game\image' }
}

# The complete prepared folder of a build key; stops when the key is unknown or the build is not prepared.
function Resolve-GameBuild([string]$Key, [hashtable]$Roots) {
    if (-not $GameBuilds.Contains($Key)) { Stop-Tool "Unknown build $Key. Builds: $($GameBuilds.Keys -join ', ')" }
    $entry = $GameBuilds[$Key]
    $app0 = Join-Path $Roots.Prepared $entry.Folder
    $marker = Join-Path $app0 '.dcp_view.json'
    $scope = ''
    if (Test-Path -LiteralPath $marker) {
        $scopeField = (Get-Content -Raw -LiteralPath $marker | ConvertFrom-Json).PSObject.Properties['scope']
        if ($scopeField) { $scope = $scopeField.Value }
    }
    if ($scope -ne 'full' -or -not (Test-Path -LiteralPath (Join-Path $Roots.Image "$($entry.Folder)\boot.bin"))) {
        Stop-Tool "Build $Key is not prepared."
    }
    return @{ Key = $Key; Label = $entry.Label; App0 = $app0 }
}

if (-not $Executable) { $Executable = Join-Path $script:RepoRoot 'build\cmake\clang64\bin\dragons_crown_pro.exe' }
if (-not (Test-Path -LiteralPath $Executable)) { Stop-Tool "Not built yet: $Executable" }
if (-not $CopyFrom) { $CopyFrom = Join-Path $script:RepoRoot 'user' }
$pinned = if ($Build) { Resolve-GameBuild $Build (Get-GameRoots $Executable) } else { $null }

$user = Join-Path $script:RepoRoot "user\test-client-$Name"
$logs = Join-Path $script:RepoRoot "build\logs\test-client-$Name"
if (-not (Test-Path -LiteralPath $user)) {
    if ($PSCmdlet.ShouldProcess($user, 'Create the instance folder')) {
        New-Item -ItemType Directory -Force $user | Out-Null
        $settings = Join-Path $CopyFrom 'dcp.ini'
        if (Test-Path -LiteralPath $settings) { Copy-Item -LiteralPath $settings -Destination $user }
        $saves = Join-Path $CopyFrom 'savedata'
        if (Test-Path -LiteralPath $saves) { Copy-Item -LiteralPath $saves -Destination $user -Recurse }
        Write-Ok "New instance folder $user (settings and saves copied from $CopyFrom)"
    }
}
New-Item -ItemType Directory -Force $logs | Out-Null

# Everything below is set for the started instance only, then put back as it was in this shell.
$instanceVariables = 'PATH', 'DCP_ONLINE', 'DCP_ONLINE_NAME', 'DCP_JOIN_CODE', 'DCP_STORY_SKIP', 'DCP_TUTORIAL_SKIP', 'DCP_GAME_BUILD'
$previous = @{}
foreach ($variable in $instanceVariables) { $previous[$variable] = [Environment]::GetEnvironmentVariable($variable, 'Process') }
try {
    # Online settings for this process only; the instance's dcp.ini is not changed.
    if ($ForceOnline) { $env:DCP_ONLINE = '1' } else { Remove-Item Env:DCP_ONLINE -ErrorAction SilentlyContinue }
    # Without -OnlineName the instance uses the Online name of its own launcher page, as friends see it.
    if ($OnlineName) { $env:DCP_ONLINE_NAME = $OnlineName } else { Remove-Item Env:DCP_ONLINE_NAME -ErrorAction SilentlyContinue }
    if ($JoinCode) { $env:DCP_JOIN_CODE = $JoinCode } else { Remove-Item Env:DCP_JOIN_CODE -ErrorAction SilentlyContinue }
    # New test characters get both offers unless the caller set these for a test.
    if (-not $env:DCP_STORY_SKIP) { $env:DCP_STORY_SKIP = '1' }
    if (-not $env:DCP_TUTORIAL_SKIP) { $env:DCP_TUTORIAL_SKIP = '1' }
    if ($pinned) { $env:DCP_GAME_BUILD = $pinned.Key } else { Remove-Item Env:DCP_GAME_BUILD -ErrorAction SilentlyContinue }

    Write-Step "Online test instance $($Name.ToUpper())$(if ($OnlineName) { " as $OnlineName" })"
    # DCP_LOBBY, set by the caller, is the developer override; the script never sets it.
    Write-Host "  Online service: $(if ($env:DCP_LOBBY) { "DCP_LOBBY override $env:DCP_LOBBY" } else { 'production' })"
    if ($pinned) {
        Write-Host "  Game build: $($pinned.Label)"
        Write-Host "  Game root: $($pinned.App0)"
    } else {
        Write-Host '  Game build: the active build (not pinned; use -Build to pin one)'
    }
    Write-Host "  user folder: $user"
    Write-Host "  logs:        $logs"
    # -WhatIf resolves and prints all of the above and starts nothing (tests/online_instance_tests.ps1).
    if ($PSCmdlet.ShouldProcess($Executable, "Start test instance $Name")) {
        # The development build loads SDL3, FreeType, libpng, libc++ and the other DLLs from the toolchain, as tools\build.ps1 -Run does.
        $tc = Resolve-Toolchain 'auto'
        $env:PATH = "$($tc.Bin);$env:PATH"
        Start-Process -FilePath $Executable -ArgumentList @('--user', "`"$user`"", '--logs', "`"$logs`"", '--launcher') -WorkingDirectory (Split-Path -Parent $Executable)
    }
} finally {
    foreach ($variable in $instanceVariables) { [Environment]::SetEnvironmentVariable($variable, $previous[$variable], 'Process') }
}
