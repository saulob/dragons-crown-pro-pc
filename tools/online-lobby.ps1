<#
.SYNOPSIS
  Runs the development lobby (server/lobby/local/server.js) in this window until Ctrl+C. No Cloudflare account needed.
.DESCRIPTION
  Clients reach it at ws://127.0.0.1:<Port>/ (two game instances on this PC), or with -Lan at ws://<this PC's address>:<Port>/
  from other PCs of the home network. Accounts, friend codes and friends are kept in user\online-test-lobby\lobby.sqlite
  across restarts (-Database for another file, -Memory for none); rooms of a previous run are removed at start.
  Needs Node.js 22.13 or later.
.PARAMETER Reset
  Deletes the local test lobby's database (accounts, friend codes, friends) and stops. Nothing else is touched.
.PARAMETER SeedTestFriends
  Makes test instances a and b (user\test-client-a, user\test-client-b) friends before starting.
#>
[CmdletBinding()]
param(
    [int]$Port = 8790,
    [string]$Database = '',
    [switch]$Memory,
    [switch]$Lan,
    [switch]$ShowTraffic,
    [switch]$Reset,
    [switch]$SeedTestFriends
)

. (Join-Path $PSScriptRoot '_common.ps1')

$stateDir = Join-Path $script:RepoRoot 'user\online-test-lobby'
if (-not $Database) { $Database = Join-Path $stateDir 'lobby.sqlite' }

if ($Reset) {
    $files = @($Database, "$Database-wal", "$Database-shm") | Where-Object { Test-Path -LiteralPath $_ }
    if (-not $files) {
        Write-Ok "No local test lobby state at $Database"
        return
    }
    try { Remove-Item -LiteralPath $files -Force -ErrorAction Stop }
    catch { Stop-Tool "Cannot delete $Database (is the lobby still running?): $($_.Exception.Message)" }
    Write-Ok "Local test lobby state deleted ($Database)"
    return
}

$node = Get-Command node -ErrorAction SilentlyContinue
if (-not $node) { Stop-Tool 'Node.js was not found. Install Node.js 22.13 or later (https://nodejs.org).' }
$version = [version]((& $node.Source --version).TrimStart('v'))
if ($version -lt [version]'22.13.0') { Stop-Tool "Node.js $version is too old: the lobby needs 22.13 or later (node:sqlite)." }

$server = Join-Path $script:RepoRoot 'server\lobby\local\server.js'
$arguments = @($server, '--port', $Port, '--host', $(if ($Lan) { '0.0.0.0' } else { '127.0.0.1' }))
if (-not $Memory) {
    New-Item -ItemType Directory -Force (Split-Path -Parent ([IO.Path]::GetFullPath($Database))) | Out-Null
    $arguments += @('--db', $Database)
}
if ($SeedTestFriends) {
    $identities = @('a', 'b') | ForEach-Object { Join-Path $script:RepoRoot "user\test-client-$_\online_identity.ini" }
    $missing = @($identities | Where-Object { -not (Test-Path -LiteralPath $_) })
    if ($missing) { Stop-Tool "Start each test instance once first (no identity yet: $($missing -join ', '))" }
    foreach ($identity in $identities) { $arguments += @('--seed-friends', $identity) }
}
if ($ShowTraffic) { $arguments += '--verbose' }
Write-Step "Development lobby on port $Port (Ctrl+C stops it)"
& $node.Source --no-warnings @arguments
