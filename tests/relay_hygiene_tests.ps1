# Source hygiene of the online relay work: static checks over the uncommitted tree, no build and no network.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$failures = 0

function Check([bool]$ok, [string]$what) {
    if (-not $ok) {
        $script:failures++
        Write-Host "FAIL: $what" -ForegroundColor Red
    }
}

function Text([string]$path) {
    $full = Join-Path $root $path
    if (Test-Path -LiteralPath $full) { return [IO.File]::ReadAllText($full) }
    return ''
}

$relayFiles = @(
    'src/net/relay_frame.h', 'src/net/relay_frame.cpp', 'src/net/lobby_link.h', 'src/net/lobby_link.cpp',
    'src/net/websocket.h', 'src/net/websocket_win.cpp', 'src/net/websocket_posix.cpp', 'src/net/peer_socket.h',
    'src/net/peer_socket.cpp', 'src/net/connection_test.h', 'src/net/connection_test.cpp', 'src/runtime_matching2.cpp',
    'server/lobby/src/relay.js', 'server/lobby/src/lobby.js', 'server/lobby/src/worker.js'
)

foreach ($file in $relayFiles) {
    $text = Text $file
    Check ($text.Length -gt 0) "$file is missing"
    Check ($text -notmatch 'DEBUG_SAULO') "$file contains DEBUG_SAULO (production relay logs must not)"
    Check ($text -notmatch '[A-Za-z]:\\(Users|VisualStudio)|/mnt/[a-z]/|/home/[a-z]') "$file contains a local absolute path"
}
foreach ($file in $relayFiles | Where-Object { $_ -notmatch 'websocket_win|peer_socket\.cpp|peer_socket\.h' }) {
    Check ((Text $file) -notmatch 'CUSA\d') "$file depends on a game build"
}
foreach ($file in 'src/net/relay_frame.h', 'src/net/relay_frame.cpp', 'src/net/lobby_link.h', 'src/net/lobby_link.cpp',
                  'src/net/websocket.h', 'src/runtime_matching2.cpp') {
    Check ((Text $file) -notmatch '#\s*include\s*<(windows|winsock2|winhttp)\.h>|_WIN32|HINTERNET') "$file uses a Windows-only API"
}

$optionPattern = 'force[_ ]?relay|DCP_RELAY|relay[_ ]?only|RELAY_FORCE'
$scanned = @(git ls-files -- 'src' 'launcher' 'gpu/shim' 'tools' 'server/lobby/src' 'server/lobby/local') |
    Where-Object { $_ -notmatch 'tools/test\.ps1' }
foreach ($file in $scanned) {
    Check ((Text $file) -notmatch "(?i)$optionPattern") "$file exposes a force-relay option"
}

$protocols = @(
    @{ File = 'src/net/online_version.h'; Pattern = 'kOnlineProtocolVersion\s*=\s*(\d+)' },
    @{ File = 'src/net/lobby_link.h'; Pattern = 'kLobbyProtocol\s*=\s*(\d+)' },
    @{ File = 'server/lobby/src/lobby.js'; Pattern = 'export const PROTOCOL\s*=\s*(\d+)' },
    @{ File = 'src/net/peer_socket.cpp'; Pattern = 'constexpr uint8_t kVersion\s*=\s*(\d+)' }
)
foreach ($entry in $protocols) {
    $now = [regex]::Match((Text $entry.File), $entry.Pattern)
    $before = [regex]::Match(((git show "HEAD:$($entry.File)") -join "`n"), $entry.Pattern)
    Check ($now.Success -and $before.Success -and $now.Groups[1].Value -eq $before.Groups[1].Value) "$($entry.File): protocol constant differs from HEAD (a bump must be intentional)"
}

$changed = @(git diff --name-only HEAD) + @(git ls-files --others --exclude-standard)
foreach ($file in $changed) {
    Check ($file -notmatch '^(gpu/|android/)') "$file is a renderer or unrelated change"
    Check ($file -notmatch '(?i)water|swapchain|present|tiling|ultrawide|cheat') "$file touches an unrelated feature"
}

$whitespace = git diff --check HEAD 2>&1
Check ($LASTEXITCODE -eq 0) "git diff --check found whitespace errors:`n$($whitespace -join "`n")"

$tests = Text 'server/lobby/test/relay.test.js'
Check ($tests -match 'rate limits' -and $tests -match 'backpressure' -and $tests -match 'resumed seat') 'the Worker relay tests lost a required case'

if ($failures -gt 0) {
    Write-Host "relay hygiene: $failures failure(s)" -ForegroundColor Red
    exit 1
}
Write-Host 'relay hygiene: OK'
exit 0
