<#
.SYNOPSIS
  Builds and runs the focused headless tests, and prints the supported-build parity summary. It is the gate to pass before
  committing parity work: no window, no game launch, no production network (a development lobby on loopback at most).
.PARAMETER SkipLinux
  Does not run the Linux network tests through WSL.
.DESCRIPTION
  Needs the build configured once by tools\build.ps1. The dependencies in gpu\deps.txt are refreshed as in build.ps1 when
  their pin or patches changed. The tests are not part of the default build; only the focused test targets are built,
  incrementally.

  Every test runs even after one fails; the exit code is non-zero when any of them failed. The tests that decide the
  supported-build parity contract (the profile and capability matrix, update archives and languages, render size, True
  Ultrawide, LZCNT, System type, Skip Offers, startup save and cheat mappings, Online Play structure, unknown builds) are
  marked in the summary: if one fails, no build is reported as passing. Every supported build must offer every GLOBAL
  feature of CUSA10454 1.00; the global feature summary counts the builds that do.

  Local boot images (build\image) and prepared folders (game\prepared) are read only. A build without one is proven by
  synthetic images only, which prove the checks and not the build. A feature that depends on the executable or the game
  archives counts as PASS only with a real boot image, a prepared folder or a reporter's scan of the real executable behind
  it; otherwise it is PARTIAL with DATA UNAVAILABLE, which names the builds and is no failure.

  Online relay: the net tests (frame codec, direct regression, automatic fallback, mixed rooms, failure paths, timeouts,
  compatibility, Connection test) run against loopback sockets and an in-process stand-in for the lobby; with Node.js
  22.13 or later a development lobby is started on loopback for the real WebSocket tests, the Worker relay tests run
  through node --test, and with WSL the same net tests run on Linux (POSIX WebSocket). Whatever is skipped is named, with
  the reason. Structural build parity (same code path, no build-specific relay code) is reported apart from runtime
  validation, which needs the local boot images; no game is ever started.

  Launcher UI: dcp-launcher-tests is relinked on every run, so an executable from an older tree can never be the one that
  runs, and it reports each launcher group (SVG icons, icon and NanoSVG licenses, header art, DPI layout, scrollbar gutter,
  separators and row spacing, sidebar in English and Portuguese, page navigation, shared source) as GROUP|launcher-*|PASS or
  FAIL. tests\launcher_gate_tests.ps1 adds the static groups: source hygiene (with git diff --check), the TRG patch chain
  applied to a scratch clone of the pinned revision and compared with the checkout the tests read, a syntax-only compile of
  the launcher sources with the build's own warning flags (zero warnings), and the portable sources compiled with g++ in
  WSL. A group that prints nothing is a FAIL; only a missing WSL reports SKIPPED. The checks are geometric and structural: no
  window, no screenshots, no pixel comparison. How the launcher looks stays a manual check.

  Public endpoint: dcp-online-hle-tests also runs the game's endpoint report against that lobby acting as a public one, which
  observes a chosen global IPv6 or public IPv4 address, with a loopback stand-in for STUN. Its sockets are bound to
  loopback, so it needs no firewall rule and nothing is sent to the addresses it reports. A required Worker test that is
  missing fails the run like one that fails.
#>
[CmdletBinding()]
param([switch]$SkipLinux)

. (Join-Path $PSScriptRoot '_common.ps1')
$tc = Resolve-Toolchain 'auto'
$env:PATH = "$($tc.Bin);$env:PATH"
$buildDir = Join-Path $script:RepoRoot 'build\cmake\clang64'
if (-not (Test-Path -LiteralPath (Join-Path $buildDir 'build.ninja'))) { Stop-Tool 'Run tools\build.ps1 first: it configures the build.' }

function Test-Step([string]$Title, [string]$Exe = '', [string]$Script = '', [switch]$Parity, [switch]$Capture,
                   [switch]$Quiet, [switch]$Lobby, [scriptblock]$Block = $null, [string]$Key = '', [string[]]$Arguments = @(),
                   [string[]]$Requires = @(), [string[]]$Forbids = @()) {
    [pscustomobject]@{ Title = $Title; Exe = $Exe; Script = $Script; Parity = [bool]$Parity; Capture = [bool]$Capture
                       Quiet = [bool]$Quiet; Lobby = [bool]$Lobby; Block = $Block; Key = $Key; Arguments = $Arguments
                       Requires = $Requires; Forbids = $Forbids }
}

function Get-Node {
    $node = Get-Command node -ErrorAction SilentlyContinue
    if (-not $node) { return $null }
    $version = [version]((& $node.Source --version).TrimStart('v'))
    if ($version -lt [version]'22.13.0') { return $null }
    return $node.Source
}

function Format-ExitCode([long]$Code) {
    'exit code {0}, 0x{1:X8}' -f $Code, [uint32]($Code -band 0xFFFFFFFFL)
}

function Skipped([string]$Reason) { [pscustomobject]@{ Code = 0; Skip = $Reason; Lines = @() } }

# Loopback only: the repository's development lobby, in memory, on a free port. Ready means its HTTP health answer came back.
function Start-DevLobby {
    $node = Get-Node
    if (-not $node) { return [pscustomobject]@{ Url = ''; Process = $null; Skip = 'Node.js 22.13 or later was not found'; Error = ''; Files = @() } }
    $probe = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $probe.Start()
    $port = $probe.LocalEndpoint.Port
    $probe.Stop()
    $stem = Join-Path ([IO.Path]::GetTempPath()) "dcp-test-lobby-$PID"
    $files = @("$stem.out", "$stem.err")
    $server = Join-Path $script:RepoRoot 'server\lobby\local\server.js'
    $process = Start-Process -FilePath $node -WindowStyle Hidden -PassThru -RedirectStandardOutput $files[0] -RedirectStandardError $files[1] `
        -ArgumentList @('--no-warnings', "`"$server`"", '--host', '127.0.0.1', '--port', $port, '--db', ':memory:', '--test-observed-address')
    $url = "ws://127.0.0.1:$port/"
    $lobby = [pscustomobject]@{ Url = ''; Process = $process; Skip = ''; Error = ''; Files = $files }
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($process.HasExited) {
            $lobby.Error = "the development lobby exited with code $($process.ExitCode): $((Get-Content -LiteralPath $files[1] -ErrorAction SilentlyContinue) -join ' ')"
            return $lobby
        }
        try {
            $health = (Invoke-WebRequest -Uri "http://127.0.0.1:$port/" -UseBasicParsing -TimeoutSec 2).Content | ConvertFrom-Json
            if ("$($health.service)" -like 'dcp-lobby*') {
                $lobby.Url = $url
                return $lobby
            }
        } catch { Start-Sleep -Milliseconds 100 }
    }
    $lobby.Error = "the development lobby did not answer its health check on port $port within 20 s: $((Get-Content -LiteralPath $files[1] -ErrorAction SilentlyContinue) -join ' ')"
    return $lobby
}

function Invoke-WorkerTests {
    $node = Get-Node
    if (-not $node) { return Skipped 'Node.js 22.13 or later was not found' }
    Push-Location (Join-Path $script:RepoRoot 'server\lobby')
    try {
        $lines = @(& $node --no-warnings --test --test-reporter=tap 'test/*.test.js' 2>&1 | ForEach-Object { "$_" })
        $code = $LASTEXITCODE
    } finally { Pop-Location }
    [pscustomobject]@{ Code = $code; Skip = ''; Lines = $lines }
}

# A log line the player's report depends on, missing or present where it must not be.
function Get-LogProblems($Test, [string[]]$Lines) {
    foreach ($text in $Test.Requires) {
        if (-not ($Lines | Where-Object { $_.Contains($text) })) { "missing log line: $text" }
    }
    foreach ($text in $Test.Forbids) {
        if ($Lines | Where-Object { $_.Contains($text) }) { "forbidden log line: $text" }
    }
}

function Find-LinuxDistro {
    if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) { return $null }
    foreach ($name in @(& wsl.exe -l -q 2>$null | ForEach-Object { ($_ -replace "`0", '').Trim() } | Where-Object { $_ })) {
        & wsl.exe -d $name --exec bash -c 'command -v g++ >/dev/null && test -f /usr/include/openssl/ssl.h' 2>$null
        if ($LASTEXITCODE -eq 0) { return $name }
    }
    return $null
}

function Invoke-LinuxNetTests {
    if ($SkipLinux) { return Skipped 'skipped with -SkipLinux' }
    if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) { return Skipped 'WSL is not installed' }
    $distro = Find-LinuxDistro
    if (-not $distro) { return Skipped 'no WSL distribution has g++ and the OpenSSL headers' }
    $root = (& wsl.exe -d $distro --exec wslpath -u $script:RepoRoot | Select-Object -First 1).Trim()
    $lines = @(& wsl.exe -d $distro --exec bash "$root/tools/test-linux-net.sh" $root 2>&1 | ForEach-Object { "$_" })
    [pscustomobject]@{ Code = $LASTEXITCODE; Skip = ''; Lines = $lines; Distro = $distro }
}

$quietLines = '^(GROUP\||ONLINE-BUILD\||LINUX-NET\||FAIL|OK |FAILED|.*STATIC PARITY|.*RUNTIME DATA|.*skipped|# (tests|pass|fail|skipped))'
function Show-Output([string[]]$Lines, [bool]$Failed) {
    foreach ($line in $Lines) {
        if ($Failed -or $line -cmatch $quietLines) { Write-Host $line }
    }
}

$tests = @(
    (Test-Step 'Windows memory tests (sparse backing, commitment, aliases, release)' 'dcp-memory-windows-tests'),
    (Test-Step 'launcher tests (profiles, install, update archives, languages, prompts, settings, and the launcher UI groups: icons, licenses, header art, DPI layout, scrollbar gutter, separators, sidebar, navigation)' 'dcp-launcher-tests' -Parity -Capture),
    (Test-Step 'launcher static gate (source hygiene, TRG patch chain, compiler warnings, Windows and Linux syntax-only compiles)' -Script 'tests\launcher_gate_tests.ps1' -Capture `
        -Arguments (@('-BuildDir', $buildDir) + @(if ($SkipLinux) { '-SkipLinux' }))),
    (Test-Step 'unsupported-build diagnostics tests (direct package analysis, one-shot report, loader validators, edge cases)' 'dcp-build-diagnostics-tests' -Parity),
    (Test-Step 'build feature parity tests (contract, update parity, mappings, unknown builds)' 'dcp-build-parity-tests' -Parity -Capture),
    (Test-Step 'feature locator tests (Cheats, Japanese NPC Voices, True Ultrawide: real images, synthetic images, known builds only)' 'dcp-feature-sites-tests' -Parity),
    (Test-Step 'trophy tests' 'dcp-trophy-tests'),
    (Test-Step 'bug report tests (log choice, privacy filter, preview equals request)' 'dcp-incident-tests' -Parity),
    (Test-Step 'cheat tests' 'dcp-cheats-tests' -Parity),
    (Test-Step 'Japanese NPC voice tests' 'dcp-voice-tests' -Parity),
    (Test-Step 'online System type hook tests' 'dcp-system-type-tests' -Parity),
    (Test-Step 'online party snapshot tests' 'dcp-party-tests' -Parity),
    (Test-Step 'Discord Rich Presence tests (IPC frames, area lookup, activity, no Discord)' 'dcp-discord-tests'),
    (Test-Step 'True Ultrawide hook tests' 'dcp-ultrawide-tests' -Parity),
    (Test-Step 'True Ultrawide side area tests' 'dcp-ultrawide-sides-tests'),
    (Test-Step 'module history tests (crash report)' 'dcp-module-history-tests'),
    (Test-Step 'MOVBE, BMI1 and LZCNT tests' 'dcp-isa-tests' -Parity),
    (Test-Step 'text tests (text dialog, system font set)' 'dcp-text-tests'),
    (Test-Step 'cursor visibility tests' 'dcp-cursor-tests'),
    (Test-Step 'window fit tests' 'dcp-window-tests'),
    (Test-Step 'SDL compat tests (Proton/Wine HIDAPI default)' 'dcp-sdl-compat-tests'),
    (Test-Step 'present tests (swapchain images, present mode, flip pacing)' 'dcp-present-tests'),
    (Test-Step 'frame pacing tests (Frame rate setting, repeat cadence, limiter)' 'dcp-frame-pacing-tests'),
    (Test-Step 'Options menu tests (chord, close gate, live and restart settings, dcp.ini, Online Play lock)' 'dcp-options-menu-tests'),
    (Test-Step 'AMD water workaround tests (vendor defaults, no hotkey)' 'dcp-amd-water-tests'),
    (Test-Step 'water diagnostics tests (AMD only, deduplication, cap)' 'dcp-water-diag-tests'),
    (Test-Step 'narrow water reader tests (16:10 skip, 16:9 and ultrawide untouched, signature)' 'dcp-narrow-water-reader-tests'),
    (Test-Step 'save-data tests (mounts, Japanese startup save hook)' 'dcp-savedata-tests' -Parity),
    (Test-Step 'runtime file overlay tests (borrowed patch data, read observer)' 'dcp-runtime-overlay-tests'),
    (Test-Step 'tiling tests (GPU tile layouts and addresses)' 'dcp-tiling-tests'),
    (Test-Step 'renderer selection tests (Auto, Vulkan, OpenGL, requirements)' 'dcp-renderer-selection-tests'),
    (Test-Step 'OpenGL backend tests (detiler, emitted GLSL through glslang, no context)' 'dcp-opengl-backend-tests'),
    (Test-Step 'OpenGL fast path tests (write watch with real faults, state tracker, stream ring, no context)' 'dcp-opengl-perf-tests'),
    (Test-Step 'ANGLE dependency tests (pin, cache, extraction, DLL staging, package checks, no network)' -Script 'tests\angle_dependency_tests.ps1'),
    (Test-Step 'updater tests' 'dcp-update-tests'),
    (Test-Step 'updater helper tests' 'dcp-update-helper-tests'),
    (Test-Step 'RTC tests (Win32 file time)' 'dcp-rtc-tests'),
    (Test-Step 'Skip Offer tests (synthetic and local boot images)' 'dcp-skip-offers-tests' -Parity),
    (Test-Step 'Skip Intro tests (synthetic and local boot images)' 'dcp-skip-intro-tests' -Parity),
    (Test-Step 'Unlock Gallery tests (view, save safety, synthetic and local boot images)' 'dcp-gallery-unlock-tests' -Parity),
    (Test-Step 'Default message preview tests (records, preview, commit pin, synthetic and local boot images)' 'dcp-default-message-tests' -Parity),
    (Test-Step 'Japanese Menu Buttons tests (synthetic and local boot images)' 'dcp-menu-buttons-tests' -Parity),
    (Test-Step 'Exit Game tests (title text, four-row menu hooks, install checks)' 'dcp-exit-game-tests'),
    (Test-Step 'Title layout tests (four-row art patch, synthetic layouts)' 'dcp-title-layout-tests'),
    (Test-Step 'online tests (JSON, identity, STUN, peer transport, relay fallback, mixed rooms, failure paths, Connection test; binary WebSocket against a loopback lobby)' 'dcp-net-tests' -Capture -Quiet -Lobby),
    (Test-Step 'online build tests (every supported build''s online imports, structural relay parity, one lobby game, cheat lock)' 'dcp-online-builds-tests' -Parity -Capture -Quiet -Lobby),
    (Test-Step 'Worker tests (node --test: lobby protocol, relay routing, limits, cleanup, compatibility)' -Block { Invoke-WorkerTests }),
    (Test-Step 'Linux network tests (WSL: POSIX WebSocket, the same net tests; skipped without WSL)' -Block { Invoke-LinuxNetTests }),
    (Test-Step 'relay source hygiene (no debug logs, no force option, no build or Windows dependency, protocols unchanged)' -Script 'tests\relay_hygiene_tests.ps1'),
    (Test-Step 'online test instance script tests (-Build, with -WhatIf only)' -Script 'tests\online_instance_tests.ps1'),
    (Test-Step 'build.ps1 waits for every loader run (a Windows-subsystem program needs the pipe)' -Script 'tests\build_script_tests.ps1'),
    (Test-Step 'online HLE tests (Matching2, libSceNet and Rudp entry points against the loopback lobby)' 'dcp-online-hle-tests' -Capture -Quiet -Lobby),
    (Test-Step 'online endpoint path, lobby sees global IPv6 (fallback report, late and early STUN, what peers receive)' 'dcp-online-hle-tests' -Capture -Quiet -Lobby `
        -Key 'endpoint path IPv6' -Arguments @('--endpoint-scenario', '2001:db8:1::10') `
        -Requires @('it sees 2001:db8:1::10', 'public endpoint unavailable, relay fallback', 'reporting public STUN endpoint 198.51.100.25:41000',
                    'lobby has observed 2001:db8:1::10, public 198.51.100.25:41000', 'lobby has observed 2001:db8:1::10, public 198.51.100.25:41001') `
        -Forbids @('reporting LAN endpoint', 'LAN lobby')),
    (Test-Step 'online endpoint path, lobby sees public IPv4 (no claim, STUN port, what peers receive)' 'dcp-online-hle-tests' -Capture -Quiet -Lobby `
        -Key 'endpoint path IPv4' -Arguments @('--endpoint-scenario', '203.0.113.9') `
        -Requires @('reporting public STUN endpoint 203.0.113.9:41001', 'lobby has 203.0.113.9:41001') `
        -Forbids @('reporting LAN endpoint', 'LAN lobby', 'lobby has observed')),
    (Test-Step 'Windows log tests (closed standard streams)' 'dcp-log-tests')
)

Sync-GpuDependencies

Write-Step 'Building the focused tests'
# The launcher tests read compiled-in icons and sources: a leftover executable must never be the one that runs.
Remove-Item -LiteralPath (Join-Path $buildDir 'bin\dcp-launcher-tests.exe') -Force -ErrorAction SilentlyContinue
& ninja -C $buildDir dcp-memory-windows-tests dcp-launcher-tests dcp-runtime-overlay-tests dcp-discord-tests dcp-build-diagnostics-tests dcp-build-parity-tests dcp-default-message-tests dcp-feature-sites-tests dcp-trophy-tests dcp-incident-tests dcp-cheats-tests dcp-text-tests dcp-tiling-tests dcp-renderer-selection-tests dcp-opengl-backend-tests dcp-opengl-perf-tests dcp-cursor-tests dcp-window-tests dcp-sdl-compat-tests dcp-present-tests dcp-frame-pacing-tests dcp-options-menu-tests dcp-amd-water-tests dcp-water-diag-tests dcp-narrow-water-reader-tests dcp-savedata-tests dcp-voice-tests dcp-system-type-tests dcp-party-tests dcp-ultrawide-tests dcp-ultrawide-sides-tests dcp-isa-tests dcp-module-history-tests dcp-update-tests dcp-update-helper-tests dcp-log-tests dcp-net-tests dcp-online-hle-tests dcp-online-builds-tests dcp-skip-offers-tests dcp-skip-intro-tests dcp-gallery-unlock-tests dcp-menu-buttons-tests dcp-exit-game-tests dcp-title-layout-tests dcp-rtc-tests
if ($LASTEXITCODE -ne 0) { Stop-Tool 'Test build failed' }
foreach ($test in $tests) {
    if ($test.Exe -and -not (Test-Path -LiteralPath (Join-Path $buildDir "bin\$($test.Exe).exe"))) { Stop-Tool "$($test.Exe) is not built: add it to the ninja line above." }
}

$failed = New-Object System.Collections.Generic.List[string]
$parityFailed = New-Object System.Collections.Generic.List[string]
$captured = @{}
$skipped = @{}
$exitCodes = @{}
$lobby = $null
$lobbyProblem = ''
$lobbySkip = ''
$previousLobbyUrl = $env:DCP_TEST_LOBBY
Remove-Item Env:\DCP_TEST_LOBBY -ErrorAction SilentlyContinue
try {
    if ($tests | Where-Object { $_.Lobby }) {
        Write-Step 'Starting the loopback development lobby'
        $lobby = Start-DevLobby
        if ($lobby.Url) {
            $env:DCP_TEST_LOBBY = $lobby.Url
            Write-Host "Online tests: Node $(& (Get-Node) --version) via $(Get-Node)"
            Write-Host "Development lobby ready at $($lobby.Url) (pid $($lobby.Process.Id)); DCP_TEST_LOBBY is set for every test below."
        } elseif ($lobby.Error) {
            $lobbyProblem = $lobby.Error
            Write-Host "FAILED: $lobbyProblem" -ForegroundColor Red
            $failed.Add('development lobby')
        } else {
            $lobbySkip = $lobby.Skip
            Write-Host "SKIPPED: the lobby-backed tests need a development lobby ($lobbySkip)." -ForegroundColor Yellow
        }
    }
    foreach ($test in $tests) {
        Write-Step "Running the $($test.Title)"
        $name = if ($test.Key) { $test.Key } elseif ($test.Exe) { $test.Exe } elseif ($test.Script) { $test.Script } else { $test.Title }
        $code = 0
        if ($test.Block) {
            $result = & $test.Block
            if ($result.Skip) {
                $skipped[$name] = $result.Skip
                Write-Host "SKIPPED: $($result.Skip)" -ForegroundColor Yellow
                continue
            }
            $captured[$name] = $result.Lines
            $code = $result.Code
            Show-Output $result.Lines ($code -ne 0)
        } elseif ($test.Script) {
            $scriptPath = Join-Path $script:RepoRoot $test.Script
            $scriptArguments = $test.Arguments
            if ($test.Capture) {
                $lines = @(& powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $scriptPath @scriptArguments 2>&1 | ForEach-Object { "$_" })
                $code = $LASTEXITCODE
                $captured[$name] = $lines
                $lines | ForEach-Object { Write-Host $_ }
            } else {
                & powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $scriptPath @scriptArguments
                $code = $LASTEXITCODE
            }
        } else {
            $exe = Join-Path $buildDir "bin\$($test.Exe).exe"
            if ($test.Capture) {
                $arguments = $test.Arguments
                $lines = @(& $exe @arguments 2>&1 | ForEach-Object { "$_" })
                $code = $LASTEXITCODE
                $captured[$name] = $lines
                $problems = @(if ($env:DCP_TEST_LOBBY) { Get-LogProblems $test $lines })
                if ($problems -and -not $code) { $code = 1 }
                if ($test.Quiet) { Show-Output $lines ($code -ne 0) } else { $lines | ForEach-Object { Write-Host $_ } }
                $problems | ForEach-Object { Write-Host $_ -ForegroundColor Red }
            } else {
                & $exe
                $code = $LASTEXITCODE
            }
        }
        if ($code -ne 0) {
            $failed.Add($name)
            if ($test.Parity) { $parityFailed.Add($name) }
            $exitCodes[$name] = $code
            Write-Host "FAILED: $name ($(Format-ExitCode $code))" -ForegroundColor Red
        }
    }
} finally {
    if ($lobby -and $lobby.Process) {
        Stop-Process -Id $lobby.Process.Id -Force -ErrorAction SilentlyContinue
        $lobby.Process.WaitForExit(5000) | Out-Null
        Remove-Item -LiteralPath $lobby.Files -Force -ErrorAction SilentlyContinue
    }
    if ($null -ne $previousLobbyUrl) { $env:DCP_TEST_LOBBY = $previousLobbyUrl } else { Remove-Item Env:\DCP_TEST_LOBBY -ErrorAction SilentlyContinue }
}
$parityOutput = @($captured['dcp-build-parity-tests'])

Write-Host ''
Write-Host 'Build parity:' -ForegroundColor Cyan
$partial = $false
$rows = @($parityOutput | Where-Object { $_ -like 'PARITY-BUILD|*' } | ForEach-Object { ,$_.Split('|') })
if ($rows.Count -eq 0) {
    Write-Host '  no result from dcp-build-parity-tests' -ForegroundColor Red
    if (-not $failed.Contains('dcp-build-parity-tests')) { $failed.Add('dcp-build-parity-tests') }
}
foreach ($row in $rows) {
    $status = $row[2]
    $good = $status -notlike 'FAIL*' -and $parityFailed.Count -eq 0
    if ($status -notlike 'FAIL*' -and -not $good) { $status = 'FAIL (a shared parity test failed)' }
    if ($status -like 'PARTIAL*') { $status += " ($(($row[3] -split '; ' | Where-Object { $_ -like 'DATA UNAVAILABLE*' })))"; $partial = $true }
    Write-Host ('  {0,-34} {1}' -f $row[1], $status) -ForegroundColor $(if (-not $good) { 'Red' } elseif ($status -like 'PARTIAL*') { 'Yellow' } else { 'Green' })
}
Write-Host ''
Write-Host 'Global feature parity:' -ForegroundColor Cyan
$features = @($parityOutput | Where-Object { $_ -like 'PARITY-FEATURE|*' } | ForEach-Object { ,$_.Split('|') })
if ($features.Count -eq 0) {
    Write-Host '  no result from dcp-build-parity-tests' -ForegroundColor Red
    if (-not $failed.Contains('dcp-build-parity-tests')) { $failed.Add('dcp-build-parity-tests') }
}
foreach ($row in $features) {
    $status = $row[2]
    $good = $status -notlike 'FAIL*' -and $parityFailed.Count -eq 0
    if ($status -notlike 'FAIL*' -and -not $good) { $status = 'FAIL (a shared parity test failed)' }
    Write-Host ('  {0,-34} {1}' -f $row[1], $status) -ForegroundColor $(if (-not $good) { 'Red' } elseif ($status -like 'PARTIAL*') { 'Yellow' } else { 'Green' })
}
Write-Host ''
Write-Host 'Eight-build online parity (structural = same code path and tables; runtime = local boot image inspected; no game is started):' -ForegroundColor Cyan
$buildRows = @($captured['dcp-online-builds-tests'] | Where-Object { $_ -like 'ONLINE-BUILD|*' } | ForEach-Object { ,$_.Split('|') })
if ($buildRows.Count -eq 0 -and -not $failed.Contains('dcp-online-builds-tests')) { $failed.Add('dcp-online-builds-tests') }
foreach ($row in $buildRows) {
    $structural = $row[3] -replace '^structural=', ''
    $runtime = $row[4] -replace '^runtime=', ''
    $color = if ($structural -ne 'PASS' -or $runtime -eq 'FAIL') { 'Red' } elseif ($runtime -eq 'PASS') { 'Green' } else { 'Yellow' }
    $text = if ($structural -eq 'PASS') { 'STATIC PARITY PASS' } else { 'STATIC PARITY FAIL' }
    if ($runtime -ne 'PASS') { $text += "; RUNTIME $($runtime.ToUpper())" } else { $text += '; runtime PASS' }
    Write-Host ('  {0,-16} {1,-34} {2}' -f $row[1], $row[2], $text) -ForegroundColor $color
}

function Group-Status([hashtable]$Table, [string[]]$Names) {
    $states = @($Names | ForEach-Object { if ($Table.ContainsKey($_)) { $Table[$_] } else { 'NOT RUN' } })
    if ($states -contains 'FAIL') { return 'FAIL' }
    if ($states -contains 'NOT RUN') { return 'NOT RUN' }
    if ($states -contains 'SKIPPED') { return 'PASS (part skipped: see above)' }
    return 'PASS'
}
function Read-Groups([string[]]$Lines) {
    $table = @{}
    foreach ($line in $Lines) {
        $parts = "$line".Split('|')
        if ($parts.Count -ge 3 -and $parts[0] -eq 'GROUP') { $table[$parts[1]] = $parts[2] }
    }
    return $table
}
$net = Read-Groups $captured['dcp-net-tests']
$netGroups = 'foundations', 'address-classification', 'endpoint-selection', 'connection-test', 'direct', 'relay-codec', 'relay-fallback', 'failure-paths', 'transport-quality', 'protocol', 'linux-parity', 'lobby-binary'
if ($captured.ContainsKey('dcp-net-tests')) {
    foreach ($group in $netGroups | Where-Object { -not $net.ContainsKey($_) }) { $net[$group] = 'FAIL' }
}
if ($lobbyProblem -and $net.ContainsKey('lobby-binary')) { $net['lobby-binary'] = 'FAIL' }
function Block-Name([string]$Prefix) { @($skipped.Keys + $captured.Keys) | Where-Object { $_ -like "$Prefix*" } | Select-Object -First 1 }
function Block-State([string]$Name) {
    if (-not $Name) { return 'NOT RUN' }
    if ($skipped.ContainsKey($Name)) { return 'SKIPPED' }
    if ($failed.Contains($Name)) { return 'FAIL' }
    return 'PASS'
}
# Passing is not enough for these: a required Worker test that was renamed or removed must not go unnoticed.
function Worker-Coverage([string]$Pattern, [int]$Expected) {
    if ($workerState -ne 'PASS' -and $workerState -ne 'FAIL') { return $workerState }
    $results = @($captured[$workerName] | Where-Object { $_ -match "^(not )?ok \d+ - ($Pattern)" })
    if ($results | Where-Object { $_ -like 'not ok*' }) { return 'FAIL' }
    if ($results.Count -lt $Expected) { return "NOT RUN ($($results.Count) of $Expected required tests found)" }
    return "PASS ($($results.Count) tests)"
}
function Endpoint-Path([string]$Key, [string]$Skipped) {
    $paths = Read-Groups $captured[$Key]
    if (-not $captured.ContainsKey($Key)) { return 'NOT RUN' }
    if ($failed.Contains($Key)) { return 'FAIL' }
    if ($lobbySkip) { return "SKIPPED ($lobbySkip)" }
    if ($paths['endpoint-path'] -ne 'PASS') { return "NOT RUN ($Skipped)" }
    return 'PASS'
}
$workerName = Block-Name 'Worker tests'
$linuxName = Block-Name 'Linux network tests'
$workerState = Block-State $workerName
$linuxState = Block-State $linuxName
$runtimeRows = @($buildRows | Where-Object { $_[4] -eq 'runtime=PASS' })
$structuralRows = @($buildRows | Where-Object { $_[3] -eq 'structural=PASS' })
$staticLinux = Group-Status $net @('linux-parity')

$windows = 'NOT RUN'
if ($net.Count -gt 0) {
    $windows = if ($net.Values -contains 'FAIL') { 'FAIL' }
               elseif ($net['lobby-binary'] -eq 'PASS') { 'PASS (net tests built and run; WinHTTP WebSocket round trip against the loopback lobby)' }
               else { "PARTIAL (net tests run; the WinHTTP WebSocket round trip was SKIPPED: $lobbySkip)" }
}
$hleLines = @($captured['dcp-online-hle-tests'])
$hle = if (-not $captured.ContainsKey('dcp-online-hle-tests')) { 'NOT RUN' }
       elseif ($failed.Contains('dcp-online-hle-tests')) { 'FAIL' }
       elseif ($hleLines -match 'skipped') { "SKIPPED ($lobbySkip$lobbyProblem)" }
       else { 'PASS (against the same loopback lobby)' }
$linuxSummary = switch ($linuxState) {
    'PASS' { "PASS (net tests compiled with the POSIX WebSocket and run in WSL; static checks $staticLinux; the WebSocket round trip is not run from WSL)" }
    'SKIPPED' { "static checks $staticLinux; WSL run SKIPPED ($($skipped[$linuxName]))" }
    default { 'FAIL or NOT RUN' }
}
$protocolSummary = "$(Group-Status $net @('protocol'))"
if ($workerState -ne 'PASS') { $protocolSummary += " (Worker side: $workerState)" }

$summary = [ordered]@{
    'Direct networking' = Group-Status $net @('foundations', 'direct')
    'Relay transport' = Group-Status $net @('relay-codec', 'lobby-binary')
    'Automatic fallback' = Group-Status $net @('relay-fallback')
    'Worker relay' = $(if ($workerState -eq 'SKIPPED') { "SKIPPED ($($skipped[$workerName]))" } else { $workerState })
    'Protocol compatibility' = $protocolSummary
    'Failure paths' = Group-Status $net @('failure-paths')
    'Link quality and relay migration' = Group-Status $net @('transport-quality')
    'Connection test' = Group-Status $net @('connection-test')
    'Address classification' = Group-Status $net @('address-classification')
    'Endpoint selection' = Group-Status $net @('endpoint-selection')
    'Endpoint path, global IPv6' = Endpoint-Path 'endpoint path IPv6' 'the lobby did not take the observed address'
    'Endpoint path, public IPv4' = Endpoint-Path 'endpoint path IPv4' 'the lobby did not take the observed address'
    'Worker address security' = Worker-Coverage '(advertised endpoint: |LAN candidates: |old clients: |an advertised IPv4 is never|a claimed observed address)' 11
    'Public endpoint budget' = Worker-Coverage '(advertised endpoints spend|repeating an advertised|flipping the LAN list|clients that advertise nothing|repeating an unchanged room or endpoint)' 5
    'Windows parity' = $windows
    'Online HLE' = $hle
    'Linux parity' = $linuxSummary
    '8-build structural parity' = $(if ($structuralRows.Count -eq 8) { 'PASS' } else { "FAIL ($($structuralRows.Count)/8)" })
    '8-build runtime validation' = $(if ($runtimeRows.Count -eq 8) { 'PASS' } elseif ($runtimeRows.Count -eq 0) { 'DATA UNAVAILABLE (no local boot image: not runtime-tested)' } else { "PARTIAL ($($runtimeRows.Count)/8 inspected)" })
}
Write-Host ''
Write-Host 'Online relay summary:' -ForegroundColor Cyan
foreach ($entry in $summary.GetEnumerator()) {
    $good = $entry.Value -like 'PASS*'
    $color = if ($entry.Value -like 'FAIL*' -or $entry.Value -like 'NOT RUN*') { 'Red' } elseif ($good) { 'Green' } else { 'Yellow' }
    Write-Host ('  {0,-28} {1}' -f ($entry.Key + ':'), $entry.Value) -ForegroundColor $color
    if ($entry.Value -like 'FAIL*' -or $entry.Value -like 'NOT RUN*') { if (-not $failed.Contains("summary: $($entry.Key)")) { $failed.Add("summary: $($entry.Key)") } }
}
$launcherCore = 'dcp-launcher-tests'
$launcherGate = 'tests\launcher_gate_tests.ps1'
$launcherState = @{}
$launcherReason = @{}
foreach ($line in @($captured[$launcherCore]) + @($captured[$launcherGate])) {
    $parts = "$line".Split('|')
    if ($parts.Count -ge 3 -and $parts[0] -eq 'GROUP') {
        $launcherState[$parts[1]] = $parts[2]
        if ($parts.Count -ge 4) { $launcherReason[$parts[1]] = $parts[3] }
    }
}
$launcherGateGroups = 'launcher-patches', 'launcher-warnings', 'launcher-hygiene', 'launcher-parity-windows', 'launcher-parity-linux'
# A group that printed nothing is a failure, never "not run": the executable crashed or lost the group.
function Get-LauncherRow([string[]]$Groups) {
    $states = @()
    $reasons = @()
    foreach ($group in $Groups) {
        $owner = if ($launcherGateGroups -contains $group) { $launcherGate } else { $launcherCore }
        if (-not $launcherState.ContainsKey($group)) {
            $states += 'FAIL'
            $code = if ($exitCodes.ContainsKey($owner)) { ", $(Format-ExitCode $exitCodes[$owner])" } else { '' }
            $reasons += "$group reported nothing from $owner$code"
        } else {
            $states += $launcherState[$group]
            if ($launcherState[$group] -eq 'SKIPPED') { $reasons += "$group skipped: $($launcherReason[$group])" }
        }
    }
    if ($states -contains 'FAIL') { return $(if ($reasons.Count) { "FAIL ($($reasons -join '; '))" } else { 'FAIL' }) }
    if ($states -contains 'SKIPPED') { return "PASS (part skipped: $($reasons -join '; '))" }
    return 'PASS'
}
$coreChecks = @($captured[$launcherCore] | Where-Object { $_ -match '^\d+ checks, \d+ failed$' }) | Select-Object -First 1
$coreRow = if ($failed.Contains($launcherCore)) { "FAIL ($(Format-ExitCode $exitCodes[$launcherCore]))" }
           elseif (-not $coreChecks) { 'FAIL (the launcher test executable printed no result)' }
           else { "PASS ($coreChecks)" }
$launcherSummary = [ordered]@{
    'Core tests' = $coreRow
    'SVG icons' = Get-LauncherRow @('launcher-icons')
    'Icon and NanoSVG licenses' = Get-LauncherRow @('launcher-licenses')
    'Header art' = Get-LauncherRow @('launcher-header-art')
    'DPI/layout' = Get-LauncherRow @('launcher-layout')
    'Scrollbar stability' = Get-LauncherRow @('launcher-scrollbar')
    'Separators/row spacing' = Get-LauncherRow @('launcher-separators')
    'Sidebar EN/PT-BR' = Get-LauncherRow @('launcher-sidebar')
    'Navigation coherence' = Get-LauncherRow @('launcher-navigation')
    'TRG patch chain' = Get-LauncherRow @('launcher-patches')
    'Compiler warnings' = Get-LauncherRow @('launcher-warnings')
    'Windows/Linux parity' = Get-LauncherRow @('launcher-parity', 'launcher-parity-windows', 'launcher-parity-linux')
    'Source hygiene' = Get-LauncherRow @('launcher-hygiene')
}
Write-Host ''
Write-Host 'Launcher UI (structural and geometric invariants; the look itself is checked by hand):' -ForegroundColor Cyan
foreach ($entry in $launcherSummary.GetEnumerator()) {
    $color = if ($entry.Value -like 'FAIL*') { 'Red' } elseif ($entry.Value -eq 'PASS' -or $entry.Value -like 'PASS (*checks*') { 'Green' } else { 'Yellow' }
    Write-Host ('  {0,-28} {1}' -f ($entry.Key + ':'), $entry.Value) -ForegroundColor $color
    if ($entry.Value -like 'FAIL*' -and -not $failed.Contains("launcher: $($entry.Key)")) { $failed.Add("launcher: $($entry.Key)") }
}
Write-Host ''
if ($failed.Count -gt 0) {
    Write-Host ('FAILED: ' + ($failed -join ', ')) -ForegroundColor Red
    exit 1
}
if ($partial) {
    Write-Host 'All tests passed. Feature parity is PARTIAL: DATA UNAVAILABLE above is unproven, not passing.' -ForegroundColor Yellow
} else {
    Write-Host 'All tests passed.' -ForegroundColor Green
}
exit 0
