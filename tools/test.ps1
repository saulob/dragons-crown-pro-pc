<#
.SYNOPSIS
  Builds and runs the focused headless tests, and prints the supported-build parity summary. It is the gate to pass before
  committing parity work: no window, no game launch, no production network (a development lobby on loopback at most).
.PARAMETER SkipLinux
  Does not run the Linux network tests through WSL.
.PARAMETER BossBattleOnline
  Builds and runs only the Boss Battle Online focused suites (visitor lifecycle, handoff, Early Story handoff, Boss Battle Online, Boss Battle, Boss
  Battle menu, Early Story, feature locators, build parity) and reports their result alone.
.PARAMETER EarlyStorySession
  Builds and runs only the DCP-7G8RZV Early Story transition and session regression (the early-story-session group of
  dcp-net-tests) and the Early Story Online tests, and reports their result alone.
.PARAMETER LauncherRenderer
  Builds and runs only the DCP-RFWKVY launcher renderer regression (the renderer group of dcp-launcher-tests: Direct3D 11
  with the issue #45 module pins, software fallback, present stall telemetry, frame cap) and reports its result alone. The
  normal run includes the same group.
.PARAMETER HeadlessCli
  Builds and runs only the headless command interface (the headless-cli group of dcp-launcher-tests: --install and --check-game
  parsing, reports and read-only guarantees on synthetic folders, the no-window contract, and the saved Controller prompts and
  Arachnophobia Safe Mode of a skipped launcher) and, through WSL, the syntax-only Linux compile of the portable launcher
  sources. No game, no window and no real game data. The normal run includes the same group.
.PARAMETER EditorTranslation
  Builds and runs only the Character Editor + translation overlay regression (the appearance-translation group of
  dcp-save-editor-tests: custom looks composed on the translated DRACRO.CPK for every supported build and language) and
  reports its result alone. The normal run includes the same group and folds it into the release matrix.
.PARAMETER EditorRelease
  Builds and runs only the Character Editor release gate on Windows (the release groups of dcp-save-editor-tests, the runtime file
  overlay suite and the online appearance suite) and, through WSL, the same groups on Linux (tools/build.sh --editor, which
  builds only those test targets, never the game), then prints the Windows/Linux Editor parity. The normal run includes the
  same groups on both platforms.
.PARAMETER SavePage
  Builds and runs only the Save page tests (dcp-save-page-tests: discovery and overview, DATA slots, backups, export, import,
  restore, the Editor invalidation contract and the page's source routing, on synthetic saves) and the quick groups of
  dcp-save-editor-tests, and reports their result alone. No game, no window, no file chooser. The normal run includes both.
.PARAMETER InventoryEditor
  Builds and runs only the Inventory Editor tests (dcp-inventory-editor-tests: item records, catalog, edits and their safety
  checks, Apply / Discard, navigation, page structure and Portuguese text, on synthetic saves and read-only copies of local
  saves) and the quick groups of dcp-save-editor-tests, and reports their result alone. No game, no window.
  The normal run includes both.
.DESCRIPTION
  Needs the build configured once by tools\build.ps1. The dependencies in gpu\deps.txt are refreshed as in build.ps1 when
  their pin or patches changed. The tests are not part of the default build; only the focused test targets are built,
  incrementally.

  Every test runs even after one fails; the exit code is non-zero when any of them failed. The tests that decide the
  supported-build parity contract (the profile and capability matrix, update archives and languages, render size, True
  Ultrawide, LZCNT, System type, Skip Offers, startup save and cheat mappings, Online Play structure, unknown builds) are
  marked in the summary: if one fails, no build is reported as passing. The release matrix prints one row per supported
  build and platform (Character Editor hooks and save profile, Quick Chat balloon bridge, Online Play, patch fingerprints
  and overlap) from the same executable on Windows and, through the Linux gate, on Linux, and fails when the two differ. Every supported build must offer every GLOBAL
  feature of CUSA10454 1.00; the global feature summary counts the builds that do.

  Local boot images (build\image, then build\research\image for a build the launcher has not prepared) and prepared
  folders (game\prepared) are read only. A build without a real executable is BLOCKED, never supported by synthetic images
  alone: the summary names it and it is no failure. These six builds must PASS with their real executable, or the run
  fails: CUSA10454 1.00 and 1.01, CUSA07001 1.00, 1.02 and 1.03, CUSA10209 1.00. CUSA10209 1.02 and CUSA10487 1.03
  have no executable here and are reported BLOCKED. A feature that needs the game archives counts as proven by the executable and the port's static
  checks where the build has no prepared folder, and the summary says so.

  Online relay: the net tests (frame codec, direct regression, automatic fallback, mixed rooms, failure paths, timeouts,
  compatibility, Connection test) run against loopback sockets and an in-process stand-in for the lobby; with Node.js
  22.13 or later a development lobby is started on loopback for the real WebSocket tests, the Worker relay tests run
  through node --test, and with WSL the same net tests run on Linux (POSIX WebSocket). Whatever is skipped is named, with
  the reason. Structural build parity (same code path, no build-specific relay code) is reported apart from runtime
  validation, which needs the local boot images; no game is ever started.

  Launcher UI: dcp-launcher-tests is relinked on every run, so an executable from an older tree can never be the one that
  runs, and it reports each launcher group (SVG icons, icon and NanoSVG licenses, header art, DPI layout, scrollbar gutter,
  separators and row spacing, sidebar in English and Portuguese, page navigation, DCP-RFWKVY renderer, shared source) as GROUP|launcher-*|PASS or
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
param([switch]$SkipLinux, [switch]$BossBattleOnline, [switch]$EarlyStorySession, [switch]$LauncherRenderer, [switch]$HeadlessCli, [switch]$EditorTranslation, [switch]$EditorRelease, [switch]$SavePage, [switch]$InventoryEditor)

. (Join-Path $PSScriptRoot '_common.ps1')
. (Join-Path $PSScriptRoot '_progress.ps1')
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

function Get-TestName($Test) { if ($Test.Key) { $Test.Key } elseif ($Test.Exe) { $Test.Exe } elseif ($Test.Script) { $Test.Script } else { $Test.Title } }

function Skipped([string]$Reason) { [pscustomobject]@{ Code = 0; Skip = $Reason; Lines = @() } }


$requiredEditorGroups = 'editor-save-parse', 'editor-save-write', 'editor-isolation', 'editor-apply-discard', 'editor-identity', 'editor-looks',
                        'editor-art', 'editor-ui-model', 'appearance-translation', 'editor-real-save', 'editor-real-art', 'editor-real-hooks',
                        'editor-real-overlay', 'editor-real-parts'

# One platform's Character Editor output: the GROUP lines of dcp-save-editor-tests, the APPEARANCE_TRANSLATION rows
# (title|version|language|mode|evidence|result) and the check counts of the two suites that run beside it.
function Read-EditorRun([string[]]$Lines) {
    $groups = [ordered]@{}
    $rows = New-Object System.Collections.Generic.List[object]
    $saves = [ordered]@{}
    $counts = @{}
    foreach ($line in $Lines) {
        $parts = "$line".Trim().Split('|')
        if ($parts.Count -ge 4 -and $parts[0] -eq 'GROUP' -and ($parts[1] -like 'editor-*' -or $parts[1] -eq 'appearance-translation')) {
            $groups[$parts[1]] = [pscustomobject]@{ State = $parts[2]; Checks = ($parts[3] -replace '^checks=', ''); Detail = ($parts[3..($parts.Count - 1)] -join ' ') }
        } elseif ($parts.Count -eq 5 -and $parts[0] -eq 'EDITOR_SAVE') {
            $saves["$($parts[1]) $($parts[2])"] = [pscustomobject]@{ Evidence = $parts[3]; Result = $parts[4] }
        } elseif ($parts.Count -eq 7 -and $parts[0] -eq 'APPEARANCE_TRANSLATION') {
            $rows.Add([pscustomobject]@{ Build = "$($parts[1]) $($parts[2])"; Language = $parts[3]; Mode = $parts[4]; Evidence = $parts[5]; Result = $parts[6] })
        } elseif ("$line" -match '^(runtime overlay tests|appearance online tests): (\d+) checks, (\d+) fail') {
            $counts[$Matches[1]] = "$($Matches[2]) checks, $($Matches[3]) failures"
        }
    }
    [pscustomobject]@{ Groups = $groups; Rows = $rows.ToArray(); Saves = $saves; Counts = $counts }
}

function Write-EditorRun([string]$Platform, $Run) {
    foreach ($name in $requiredEditorGroups) {
        $group = $Run.Groups[$name]
        $state = if ($group) { $group.State } else { 'MISSING' }
        Write-Host ('  EDITOR_GROUP|{0}|{1}|{2}' -f $Platform, $name, $(if ($group) { "$state|$($group.Detail)" } else { $state })) `
            -ForegroundColor $(if ($state -eq 'PASS') { 'Green' } elseif ($state -eq 'NO_DATA') { 'Yellow' } else { 'Red' })
    }
    foreach ($key in $Run.Saves.Keys) { Write-Host ('  EDITOR_SAVE|{0}|{1}|{2}|{3}' -f $Platform, $key, $Run.Saves[$key].Evidence, $Run.Saves[$key].Result) -ForegroundColor $(if ($Run.Saves[$key].Result -eq 'PASS') { 'Green' } else { 'Red' }) }
    foreach ($key in $Run.Counts.Keys) { Write-Host ('  EDITOR_SUITE|{0}|{1}|{2}' -f $Platform, $key, $Run.Counts[$key]) -ForegroundColor Green }
}

function Write-EditorTranslation($Run) {
    foreach ($row in $Run.Rows) {
        Write-Host ('  APPEARANCE_TRANSLATION|{0}|{1}|{2}|{3}|{4}' -f $row.Build, $row.Language, $row.Mode, $row.Evidence, $row.Result) `
            -ForegroundColor $(if ($row.Result -eq 'PASS') { 'Green' } else { 'Red' })
    }
}

# What makes one platform's run unusable on its own: a required group missing or not PASS (NO_DATA is shown, never passed
# as PASS), or no composition rows.
function Get-EditorProblems($Run) {
    foreach ($name in $requiredEditorGroups) {
        $group = $Run.Groups[$name]
        if (-not $group) { "group $name missing" }
        elseif ($group.State -eq 'FAIL') { "group $name FAIL" }
    }
    if ($Run.Rows.Count -eq 0) { 'no APPEARANCE_TRANSLATION rows' }
    if ($Run.Saves.Count -eq 0) { 'no EDITOR_SAVE rows' }
    foreach ($name in $Run.Saves.Keys | Where-Object { $Run.Saves[$_].Result -ne 'PASS' }) { "save profile $name FAIL" }
    foreach ($row in $Run.Rows | Where-Object { $_.Result -ne 'PASS' }) { "$($row.Build) $($row.Language) $($row.Result)" }
}

# Windows against Linux: the same groups with the same states, and the same composition rows with the same evidence.
function Get-EditorParity($Windows, $Linux) {
    $differences = New-Object System.Collections.Generic.List[string]
    foreach ($name in $requiredEditorGroups) {
        $w = $Windows.Groups[$name]; $l = $Linux.Groups[$name]
        if (-not $l) { $differences.Add("$name missing on Linux") }
        elseif (-not $w) { $differences.Add("$name missing on Windows") }
        elseif ($w.State -ne $l.State) { $differences.Add("$name $($w.State) on Windows, $($l.State) on Linux") }
    }
    $windowsRows = @($Windows.Rows | ForEach-Object { "$($_.Build)|$($_.Language)|$($_.Mode)|$($_.Evidence)|$($_.Result)" })
    $linuxRows = @($Linux.Rows | ForEach-Object { "$($_.Build)|$($_.Language)|$($_.Mode)|$($_.Evidence)|$($_.Result)" })
    if ($windowsRows.Count -ne $linuxRows.Count) { $differences.Add("APPEARANCE_TRANSLATION rows: $($windowsRows.Count) on Windows, $($linuxRows.Count) on Linux") }
    foreach ($diff in @(Compare-Object $windowsRows $linuxRows)) { $differences.Add("row $($diff.InputObject) only on $(if ($diff.SideIndicator -eq '<=') { 'Windows' } else { 'Linux' })") }
    $windowsSaves = @($Windows.Saves.Keys | ForEach-Object { "$_|$($Windows.Saves[$_].Evidence)|$($Windows.Saves[$_].Result)" })
    $linuxSaves = @($Linux.Saves.Keys | ForEach-Object { "$_|$($Linux.Saves[$_].Evidence)|$($Linux.Saves[$_].Result)" })
    foreach ($diff in @(Compare-Object $windowsSaves $linuxSaves)) { $differences.Add("save row $($diff.InputObject) only on $(if ($diff.SideIndicator -eq '<=') { 'Windows' } else { 'Linux' })") }
    foreach ($key in $Windows.Counts.Keys) {
        if ($Linux.Counts[$key] -ne $Windows.Counts[$key]) { $differences.Add("${key}: '$($Windows.Counts[$key])' on Windows, '$($Linux.Counts[$key])' on Linux") }
    }
    $differences.ToArray()
}

# Prints both platforms' Character Editor results, the composition rows and the parity line; returns the parity text.
function Show-EditorValidation($Windows, $Linux, [string]$LinuxReason) {
    Write-Host ''
    Write-Host 'Character Editor release groups (the same dcp-save-editor-tests, runtime overlay and online appearance suites on both platforms):' -ForegroundColor Cyan
    Write-EditorRun 'windows' $Windows
    foreach ($problem in @(Get-EditorProblems $Windows)) { Write-Host "  windows: $problem" -ForegroundColor Red; $failed.Add("Character Editor (windows): $problem") }
    $parity = "SKIPPED ($LinuxReason)"
    if ($Linux) {
        Write-EditorRun 'linux' $Linux
        foreach ($problem in @(Get-EditorProblems $Linux)) { Write-Host "  linux: $problem" -ForegroundColor Red; $failed.Add("Character Editor (linux): $problem") }
        $differences = @(Get-EditorParity $Windows $Linux)
        $parity = if ($differences.Count) { "FAIL ($($differences -join '; '))" } else {
            "PASS ($($Windows.Groups.Count) groups, $($Windows.Rows.Count) composition rows, $($Windows.Saves.Count) save rows, same states and evidence on both platforms)"
        }
        if ($differences.Count) { $failed.Add('Character Editor: Windows/Linux parity') }
    }
    Write-Host ''
    Write-Host 'Character Editor + translation overlay (custom looks composed on the translated DRACRO.CPK; every supported build and language, Windows):' -ForegroundColor Cyan
    Write-EditorTranslation $Windows
    Write-Host "  EDITOR_PARITY|windows_vs_linux|$parity" -ForegroundColor $(if ($parity -like 'PASS*') { 'Green' } elseif ($parity -like 'FAIL*') { 'Red' } else { 'Yellow' })
    $parity
}

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
    Invoke-Native $node @('--no-warnings', '--test', '--test-reporter=tap', 'test/*.test.js') $true (Join-Path $script:RepoRoot 'server\lobby')
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
    Invoke-Native 'wsl.exe' @('-d', $distro, '--exec', 'bash', "$root/tools/test-linux-net.sh", $root) $true
}

function Invoke-LinuxGate {
    if ($SkipLinux) { return Skipped 'skipped with -SkipLinux' }
    if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) { return Skipped 'WSL is not installed' }
    $distro = Find-LinuxDistro
    if (-not $distro) { return Skipped 'no WSL distribution has g++ and the OpenSSL headers' }
    $root = (& wsl.exe -d $distro --exec wslpath -u $script:RepoRoot | Select-Object -First 1).Trim()
    Invoke-Native 'wsl.exe' @('-d', $distro, '--exec', 'bash', '-c', "cd '$root' && DCP_TEST_PROGRESS=1 bash tools/build.sh --test 2>&1") $true
}

function Invoke-LinuxLauncherSyntax {
    if ($SkipLinux) { return Skipped 'skipped with -SkipLinux' }
    if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) { return Skipped 'WSL is not installed' }
    $distro = Find-LinuxDistro
    if (-not $distro) { return Skipped 'no WSL distribution has g++ and the OpenSSL headers' }
    $root = (& wsl.exe -d $distro --exec wslpath -u $script:RepoRoot | Select-Object -First 1).Trim()
    Invoke-Native 'wsl.exe' @('-d', $distro, '--exec', 'bash', "$root/tools/test-linux-launcher.sh", $root) $true
}

function Invoke-LinuxEditorGate {
    if ($SkipLinux) { return Skipped 'skipped with -SkipLinux' }
    if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) { return Skipped 'WSL is not installed' }
    $distro = Find-LinuxDistro
    if (-not $distro) { return Skipped 'no WSL distribution has g++ and the OpenSSL headers' }
    $root = (& wsl.exe -d $distro --exec wslpath -u $script:RepoRoot | Select-Object -First 1).Trim()
    Invoke-Native 'wsl.exe' @('-d', $distro, '--exec', 'bash', '-c', "cd '$root' && DCP_TEST_PROGRESS=1 bash tools/build.sh --editor 2>&1") $true
}

$quietLines = '^(GROUP\||MATRIX\||APPEARANCE_TRANSLATION\||runtime overlay tests:|appearance online tests:|ONLINE-BUILD\||LINUX-NET\||FAIL|OK |FAILED|.*STATIC PARITY|.*RUNTIME DATA|.*skipped|# (tests|pass|fail|skipped))'
function ConvertTo-ArgumentString([string[]]$Arguments) {
    ($Arguments | ForEach-Object { if ($_ -match '[\s"]' -or $_ -eq '') { '"' + ($_ -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"' } else { $_ } }) -join ' '
}

# Runs a program with stdout and stderr merged. Its lines are written as they arrive (with Quiet only result lines and a
# timing table; the rest is returned in Hidden for a failure report), and while it runs the step's progress line is refreshed
# on a timer. "@@dcp-progress" lines from instrumented suites feed the check count and are not part of Lines.
function Invoke-Native([string]$File, [string[]]$Arguments = @(), [bool]$Quiet = $false, [string]$WorkingDirectory = '') {
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $File
    if ($WorkingDirectory) { $info.WorkingDirectory = $WorkingDirectory }
    $info.Arguments = ConvertTo-ArgumentString $Arguments
    $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.CreateNoWindow = $true
    $info.EnvironmentVariables['DCP_TEST_PROGRESS'] = '1'
    $lines = New-Object System.Collections.Generic.List[string]
    $hidden = New-Object System.Collections.Generic.List[string]
    try { $process = [System.Diagnostics.Process]::Start($info) }
    catch { return [pscustomobject]@{ Code = -1; Skip = ''; Lines = @("could not start ${File}: $($_.Exception.Message)"); Hidden = @(); Streamed = $false } }
    try {
        $readers = @($process.StandardOutput, $process.StandardError)
        $pending = @($readers[0].ReadLineAsync(), $readers[1].ReadLineAsync())
        $inTable = $false
        while ($pending[0] -or $pending[1]) {
            $active = @($pending | Where-Object { $_ })
            $index = [System.Threading.Tasks.Task]::WaitAny([System.Threading.Tasks.Task[]]$active, 500)
            if ($index -ge 0) {
                $slot = [Array]::IndexOf($pending, $active[$index])
                $line = $pending[$slot].Result
                if ($null -eq $line) { $pending[$slot] = $null }
                else {
                    $pending[$slot] = $readers[$slot].ReadLineAsync()
                    $child = ConvertFrom-ChildProgress $line
                    if ($child) { $null = Set-ProgressChecks $script:Progress $script:Clock.Elapsed.TotalSeconds $child.Checks $child.Expected $child.Estimate $child.Detail }
                    else {
                        $lines.Add($line)
                        if ($line -cmatch '^.+ timing$') { $inTable = $true } elseif ($inTable -and -not $line.StartsWith('  ')) { $inTable = $false }
                        if (-not $Quiet -or $inTable -or $line -cmatch $quietLines) { Clear-ProgressLine $script:Progress; Write-Host $line } else { $hidden.Add($line) }
                    }
                }
            }
            $null = Update-Progress $script:Progress $script:Clock.Elapsed.TotalSeconds
        }
        $process.WaitForExit()
        return [pscustomobject]@{ Code = $process.ExitCode; Skip = ''; Lines = $lines.ToArray(); Hidden = $hidden.ToArray(); Streamed = $true }
    } finally {
        $process.Dispose()
    }
}

$requiredSuites = 'dcp-build-parity-tests', 'dcp-boss-battle-tests', 'dcp-boss-battle-text-tests', 'dcp-boss-battle-menu-tests',
                  'dcp-boss-battle-record-tests', 'dcp-boss-battle-online-tests', 'dcp-boss-battle-visitor-lifecycle-tests', 'dcp-boss-battle-handoff-tests', 'dcp-boss-battle-early-story-handoff-tests', 'dcp-skip-intro-tests', 'dcp-character-highlight-tests', 'dcp-gallery-unlock-tests', 'dcp-kraken-fix-tests', 'dcp-endgame-unlocks-tests', 'dcp-thread-lifecycle-tests',
                  'dcp-menu-buttons-tests', 'dcp-skip-offers-tests', 'dcp-exit-game-tests', 'dcp-feature-sites-tests',
                  'dcp-launcher-tests', 'dcp-save-editor-tests', 'dcp-save-page-tests', 'dcp-inventory-editor-tests'
$requiredBuilds = 'CUSA10454 1.00 USA', 'CUSA10454 1.01 USA', 'CUSA07001 1.00 Japan', 'CUSA07001 1.02 Japan', 'CUSA07001 1.03 Japan',
                  'CUSA10209 1.00 Asia/Hong Kong'
$blockedBuilds = 'CUSA10209 1.02 Asia/Hong Kong', 'CUSA10487 1.03 Europe'

$tests = @(
    (Test-Step 'Windows memory tests (sparse backing, commitment, aliases, release)' 'dcp-memory-windows-tests'),
    (Test-Step 'launcher tests (profiles, install, update archives, languages, prompts, settings, and the launcher UI groups: icons, licenses, header art, DPI layout, scrollbar gutter, separators, sidebar, navigation, renderer)' 'dcp-launcher-tests' -Parity -Capture),
    (Test-Step 'launcher static gate (source hygiene, TRG patch chain, compiler warnings, Windows and Linux syntax-only compiles)' -Script 'tests\launcher_gate_tests.ps1' -Capture `
        -Arguments (@('-BuildDir', $buildDir) + @(if ($SkipLinux) { '-SkipLinux' }))),
    (Test-Step 'unsupported-build diagnostics tests (direct package analysis, one-shot report, loader validators, edge cases)' 'dcp-build-diagnostics-tests' -Parity),
    (Test-Step 'build feature parity tests (contract, update parity, mappings, unknown builds)' 'dcp-build-parity-tests' -Parity -Capture),
    (Test-Step 'feature locator tests (Cheats, Voice Language, True Ultrawide: real and relocated images, synthetic images, known builds only)' 'dcp-feature-sites-tests' -Parity),
    (Test-Step 'trophy tests' 'dcp-trophy-tests'),
    (Test-Step 'bug report tests (log choice, privacy filter, preview equals request, crash dump pairing and Attach crash report)' 'dcp-incident-tests' -Parity),
    (Test-Step 'bug report crash dump upload tests (one Report ID for a report and its dump, log-only report, unsendable dump, resumed retry, receipt guard, on the loopback lobby)' 'dcp-incident-upload-tests' -Capture -Quiet -Lobby),
    (Test-Step 'Windows crash dump tests (dump path, session log pairing, collision, headless minidump write; no crash, no dialog)' 'dcp-crash-dump-tests'),
    (Test-Step 'cheat tests' 'dcp-cheats-tests' -Parity),
    (Test-Step 'Kraken crash fix tests (tentacle null mast: faulting sequence in every supported build, patch, null path, refusals)' 'dcp-kraken-fix-tests' -Parity),
    (Test-Step 'Voice language tests (issue #69: player, resurrected characters, narrator, NPCs, enemies, allies, all builds; narrator off alone)' 'dcp-voice-tests' -Parity),
    (Test-Step 'online System type hook tests' 'dcp-system-type-tests' -Parity),
    (Test-Step 'online party snapshot tests' 'dcp-party-tests' -Parity),
    (Test-Step 'DCP-YYM3TL online Inn report-adventurers lifecycle (detached ally loader: cancel waits for the worker before its owner is freed)' 'dcp-thread-lifecycle-tests'),
    (Test-Step 'Comms Wheel tests (message ids, radial layout, hold input, wire body, limits, every language, usage counts, balloon bridge on synthetic and local boot images)' 'dcp-comms-tests' -Parity),
    (Test-Step 'Comms Wheel session tests (rules, telemetry batching, the real send entry point)' 'dcp-comms-session-tests'),
    (Test-Step 'Comms Wheel production send tests (one accepted send, one count)' 'dcp-comms-session-runtime-tests'),
    (Test-Step 'Discord Rich Presence tests (IPC frames, area lookup, activity, no Discord)' 'dcp-discord-tests'),
    (Test-Step 'True Ultrawide hook tests' 'dcp-ultrawide-tests' -Parity),
    (Test-Step 'True Ultrawide side area tests' 'dcp-ultrawide-sides-tests'),
    (Test-Step 'module history tests (crash report)' 'dcp-module-history-tests'),
    (Test-Step 'MOVBE, BMI1 and LZCNT tests' 'dcp-isa-tests' -Parity),
    (Test-Step 'text tests (text dialog, system font set)' 'dcp-text-tests'),
    (Test-Step 'cursor visibility tests' 'dcp-cursor-tests'),
    (Test-Step 'mouse input tests (full mouse rebinding, Pointer Control Hold / Always On, Steam Deck trackpad behavior, touchscreen behavior, controller ownership, capture rules, settings, telemetry)' 'dcp-mouse-tests'),
    (Test-Step 'SteamOS touchscreen tests (Gamescope routing, touchpad mapping, synthetic mouse suppression, mouse/trackpad regression)' 'dcp-touchscreen-steamos-tests' -Parity),
    (Test-Step 'Steam Deck trackpad tests (native touchpad, automatic pointer, click routing, mouse/controller/touchscreen regression)' 'dcp-steam-deck-trackpad-tests' -Parity),
    (Test-Step 'window fit tests' 'dcp-window-tests'),
    (Test-Step 'SDL compat tests (Proton/Wine HIDAPI default, Steam Wayland window backend)' 'dcp-sdl-compat-tests'),
    (Test-Step 'present tests (swapchain images, present mode, flip pacing)' 'dcp-present-tests'),
    (Test-Step 'frame pacing tests (Frame rate setting, repeat cadence, limiter)' 'dcp-frame-pacing-tests'),
    (Test-Step 'copy pool tests (epoch accounting, stale epoch report, callback order)' 'dcp-copy-pool-tests'),
    (Test-Step 'Options menu tests (chord, close gate, live and restart settings, dcp.ini, Online Play lock)' 'dcp-options-menu-tests'),
    (Test-Step 'Comms Wheel UI tests (input glue, layout, key binding)' 'dcp-comms-wheel-ui-tests'),
    (Test-Step 'AMD water workaround tests (vendor defaults, no hotkey)' 'dcp-amd-water-tests'),
    (Test-Step 'pipeline cache tests (seal, identity, warm-up mode, recovery, build gate)' 'dcp-pipeline-cache-tests'),
    (Test-Step 'GPU compatibility tests (interpolation route, integer fix, dynamic state, feedback loop, mixed samples)' 'dcp-gpu-compat-tests'),
    (Test-Step 'water diagnostics tests (AMD only, deduplication, cap)' 'dcp-water-diag-tests'),
    (Test-Step 'narrow water reader tests (16:10 skip, 16:9 and ultrawide untouched, signature)' 'dcp-narrow-water-reader-tests'),
    (Test-Step 'save-data tests (mounts, Japanese startup save hook)' 'dcp-savedata-tests' -Parity),
    (Test-Step 'runtime file overlay tests (borrowed patch data, read observer, swapped data, layered overlays)' 'dcp-runtime-overlay-tests' -Capture -Quiet),
    (Test-Step 'online appearance tests (recipe, room session, remote textures, seat-aware hooks)' 'dcp-appearance-online-tests' -Capture -Quiet),
    (Test-Step 'Character Editor release groups (save parse and write, isolation, apply and discard, identity, looks, art, UI model, composition with the translation overlay, real archives)' 'dcp-save-editor-tests' -Parity -Capture -Quiet `
        -Key 'dcp-save-editor-tests release' -Arguments @('release')),
    (Test-Step 'Save page tests (overview, DATA slots, backups, export, import, restore, PS3/Vita detection, Editor invalidation, page routing)' 'dcp-save-page-tests'),
    (Test-Step 'Inventory Editor tests (item records, catalog, edits, safety, Apply and Discard, navigation, page structure, Portuguese text)' 'dcp-inventory-editor-tests'),
    (Test-Step 'test progress reporter tests (percentage, bar, ETA, check counts, timing table, redirected output, injected clock)' 'dcp-test-progress-tests'),
    (Test-Step 'test progress runner tests (tools\_progress.ps1 on synthetic time: bar, percentage, ETA, spinner line, redirected output, timing)' -Script 'tests\progress_script_tests.ps1' -Capture),
    (Test-Step 'tiling tests (GPU tile layouts and addresses)' 'dcp-tiling-tests'),
    (Test-Step 'renderer selection tests (Auto, Vulkan, OpenGL, requirements)' 'dcp-renderer-selection-tests'),
    (Test-Step 'OpenGL backend tests (detiler, emitted GLSL through glslang, no context)' 'dcp-opengl-backend-tests'),
    (Test-Step 'OpenGL fast path tests (write watch with real faults, state tracker, stream ring, no context)' 'dcp-opengl-perf-tests'),
    (Test-Step 'ANGLE dependency tests (pin, cache, extraction, DLL staging, package checks, no network)' -Script 'tests\angle_dependency_tests.ps1'),
    (Test-Step 'updater tests' 'dcp-update-tests'),
    (Test-Step 'updater network tests (504, dropped connections, stalls, redirects, retries on a loopback server)' 'dcp-update-network-tests'),
    (Test-Step 'updater helper tests' 'dcp-update-helper-tests'),
    (Test-Step 'RTC tests (Win32 file time)' 'dcp-rtc-tests'),
    (Test-Step 'Skip Offer tests (synthetic and local boot images)' 'dcp-skip-offers-tests' -Parity),
    (Test-Step 'Item Attack Cap tests (weapon clamp patch: synthetic and local boot images)' 'dcp-item-attack-cap-tests' -Parity),
    (Test-Step 'Skip Intro tests (startup logos, opening movie, first boot, title transition, all builds)' 'dcp-skip-intro-tests' -Parity),
    (Test-Step 'Character Highlight tests (setting, F7 hotkey, atlas signature table, runtime image match, mask outline)' 'dcp-character-highlight-tests' -Parity),
    (Test-Step 'Unlock Gallery tests (view, save safety, synthetic and local boot images)' 'dcp-gallery-unlock-tests' -Parity),
    (Test-Step 'Endgame unlock tests (Stables rule, independent settings, hooks, synthetic and local boot images)' 'dcp-endgame-unlocks-tests' -Parity),
    (Test-Step 'Boss Battle tests (Boss Battle screen on the native Options page, session milestones, native Win signals, Attempt/Win/Death/Quit, result bypass, Inn return and campaign restore, statistics, telemetry caps, local boot images)' 'dcp-boss-battle-tests' -Parity),
    (Test-Step 'Boss Battle text tests (every Menu & Subtitles language: official boss names, labels, Fight All, quit texts, kana Russian)' 'dcp-boss-battle-text-tests'),
    (Test-Step 'Boss Battle record tests (own data file: format, outcomes, attempt invariant, atomic replace)' 'dcp-boss-battle-record-tests'),
    (Test-Step 'Boss Battle menu tests (bosses 01 to 20 on the screen, Return to tavern, F4 quit path, no ImGui selector or Inn-list menu, title menu restored)' 'dcp-boss-battle-menu-tests'),
    (Test-Step 'Boss Battle Online tests (prompt once per visit, Network Status, room isolation, host authority, guest follow, Fight All checkpoint, shared Early Story hooks, follower sessions, text in every language)' 'dcp-boss-battle-online-tests'),
    (Test-Step 'Boss Battle visitor lifecycle tests (own page, Network Menu join, Inn rebuilt into stale Options mode with no menu frame and into its menu act, page back waiting for host, one guest party, host 1 then 2 of 2, first Harpy start followed, second start, Fight All)' 'dcp-boss-battle-visitor-lifecycle-tests'),
    (Test-Step 'Boss Battle handoff tests (Return to tavern keeps the shared room and resets the Boss seats natively, a lone host leaves its room, guest page closes when the host ends, host reenters as host in the same room and starts Harpy, guest becomes visitor again, Hydeland seats one local and one remote player, handoff sites on six builds)' 'dcp-boss-battle-handoff-tests'),
    (Test-Step 'Boss Battle to Early Story handoff tests (real order: host Return to tavern sends its end before the room goes neutral, guest cleanup and Early Story guest role in the Inn before host travel, nothing torn down during travel, scene-departure fallback, Boss reclaim, live characters)' 'dcp-boss-battle-early-story-handoff-tests'),
    (Test-Step 'Early Story Online tests (session rules, shadow record, writer blocks, atomic install, synthetic and local boot images)' 'dcp-early-story-tests' -Parity),
    (Test-Step 'Default message preview tests (records, preview, commit pin, synthetic and local boot images)' 'dcp-default-message-tests' -Parity),
    (Test-Step 'Japanese Menu Buttons tests (synthetic and local boot images)' 'dcp-menu-buttons-tests' -Parity),
    (Test-Step 'Exit Game tests (title text, four-row menu hooks, install checks)' 'dcp-exit-game-tests'),
    (Test-Step 'Title layout tests (four-row art patch, synthetic layouts)' 'dcp-title-layout-tests'),
    (Test-Step 'Simulated crash trigger tests (DCP_SIMULATE_CRASH parsing and wiring; nothing crashes)' 'dcp-simulate-crash-tests'),
    (Test-Step 'online tests (JSON, identity, STUN, peer transport, relay fallback, mixed rooms, failure paths, Connection test; binary WebSocket against a loopback lobby)' 'dcp-net-tests' -Capture -Quiet -Lobby),
    (Test-Step 'online build tests (every supported build''s online imports, structural relay parity, one lobby game, cheat lock)' 'dcp-online-builds-tests' -Parity -Capture -Quiet -Lobby),
    (Test-Step 'Worker tests (node --test: lobby protocol, relay routing, limits, cleanup, compatibility)' -Block { Invoke-WorkerTests }),
    (Test-Step 'Linux network tests (WSL: POSIX WebSocket, the same net tests; skipped without WSL)' -Block { Invoke-LinuxNetTests }),
    (Test-Step 'Linux gate (WSL: tools/build.sh --test, the authoritative Linux build and test run; skipped without WSL)' -Block { Invoke-LinuxGate }),
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

$bossBattleOnlineSuites = 'dcp-boss-battle-visitor-lifecycle-tests', 'dcp-boss-battle-handoff-tests', 'dcp-boss-battle-early-story-handoff-tests', 'dcp-boss-battle-online-tests', 'dcp-boss-battle-tests',
                          'dcp-boss-battle-menu-tests', 'dcp-early-story-tests', 'dcp-feature-sites-tests', 'dcp-build-parity-tests'
$earlyStorySessionSuites = 'dcp-net-tests', 'dcp-early-story-tests'
$launcherRendererSuites = @('dcp-launcher-tests')
$headlessCliSuites = @('dcp-launcher-tests')
$editorTranslationSuites = @('dcp-save-editor-tests')
$editorReleaseSuites = @('dcp-save-editor-tests', 'dcp-runtime-overlay-tests', 'dcp-appearance-online-tests')
$savePageSuites = @('dcp-save-page-tests', 'dcp-save-editor-tests')
$inventoryEditorSuites = @('dcp-inventory-editor-tests', 'dcp-save-editor-tests')
if ($BossBattleOnline) {
    $tests = @($tests | Where-Object { $bossBattleOnlineSuites -contains $_.Exe })
    $requiredSuites = $bossBattleOnlineSuites
} elseif ($EarlyStorySession) {
    $tests = @(
        (Test-Step 'DCP-7G8RZV Early Story transition/session regression (dcp-net-tests early-story-session)' 'dcp-net-tests' -Capture `
            -Key 'dcp-net-tests early-story-session' -Arguments @('early-story-session')),
        (Test-Step 'Early Story Online tests (session rules, shadow record, writer blocks, atomic install, synthetic and local boot images)' 'dcp-early-story-tests')
    )
    $requiredSuites = $earlyStorySessionSuites
} elseif ($LauncherRenderer) {
    $tests = @(
        (Test-Step 'DCP-RFWKVY launcher renderer and presentation (dcp-launcher-tests renderer)' 'dcp-launcher-tests' -Capture `
            -Key 'dcp-launcher-tests renderer' -Arguments @('renderer'))
    )
    $requiredSuites = $launcherRendererSuites
} elseif ($HeadlessCli) {
    $tests = @(
        (Test-Step 'headless command interface (dcp-launcher-tests headless: --install, --check-game, skipped-launcher patches)' 'dcp-launcher-tests' -Capture `
            -Key 'dcp-launcher-tests headless' -Arguments @('headless')),
        (Test-Step 'Linux launcher syntax (WSL: g++ -fsyntax-only on the portable launcher sources; skipped without WSL)' -Block { Invoke-LinuxLauncherSyntax })
    )
    $requiredSuites = $headlessCliSuites
} elseif ($EditorTranslation) {
    $tests = @(
        (Test-Step 'Character Editor + translation overlay tests (custom looks composed on the translated DRACRO.CPK, every supported build and language)' 'dcp-save-editor-tests' -Parity -Capture -Quiet `
            -Key 'dcp-save-editor-tests appearance-translation' -Arguments @('appearance-translation'))
    )
    $requiredSuites = $editorTranslationSuites
} elseif ($SavePage) {
    $tests = @(
        (Test-Step 'Save page tests (overview, DATA slots, backups, export, import, restore, PS3/Vita detection, Editor invalidation, page routing)' 'dcp-save-page-tests'),
        (Test-Step 'Character Editor quick groups (save parse and write, isolation, apply and discard, identity, looks, art, UI model)' 'dcp-save-editor-tests' `
            -Key 'dcp-save-editor-tests quick' -Arguments @('quick'))
    )
    $requiredSuites = $savePageSuites
} elseif ($InventoryEditor) {
    $tests = @(
        (Test-Step 'Inventory Editor tests (item records, catalog, edits, safety, Apply and Discard, navigation, page structure, Portuguese text)' 'dcp-inventory-editor-tests'),
        (Test-Step 'Character Editor quick groups (save parse and write, isolation, apply and discard, identity, looks, art, UI model)' 'dcp-save-editor-tests' `
            -Key 'dcp-save-editor-tests quick' -Arguments @('quick'))
    )
    $requiredSuites = $inventoryEditorSuites
} elseif ($EditorRelease) {
    $tests = @($tests | Where-Object { $editorReleaseSuites -contains $_.Exe })
    $tests += Test-Step 'Linux Character Editor release groups (WSL: tools/build.sh --editor, builds only the Editor test targets; skipped without WSL)' -Block { Invoke-LinuxEditorGate }
    $requiredSuites = $editorReleaseSuites
} else {
    Sync-GpuDependencies
}

Write-Step 'Building the focused tests'
# The launcher tests read compiled-in icons and sources: a leftover executable must never be the one that runs.
Remove-Item -LiteralPath (Join-Path $buildDir 'bin\dcp-launcher-tests.exe') -Force -ErrorAction SilentlyContinue
if ($BossBattleOnline) { & ninja -C $buildDir @bossBattleOnlineSuites } elseif ($EarlyStorySession) { & ninja -C $buildDir @earlyStorySessionSuites } elseif ($LauncherRenderer) { & ninja -C $buildDir @launcherRendererSuites } elseif ($HeadlessCli) { & ninja -C $buildDir @headlessCliSuites } elseif ($EditorTranslation) { & ninja -C $buildDir @editorTranslationSuites } elseif ($EditorRelease) { & ninja -C $buildDir @editorReleaseSuites } elseif ($SavePage) { & ninja -C $buildDir @savePageSuites } elseif ($InventoryEditor) { & ninja -C $buildDir @inventoryEditorSuites } else {
& ninja -C $buildDir dcp-memory-windows-tests dcp-launcher-tests dcp-save-editor-tests dcp-save-page-tests dcp-inventory-editor-tests dcp-test-progress-tests dcp-runtime-overlay-tests dcp-appearance-online-tests dcp-discord-tests dcp-build-diagnostics-tests dcp-build-parity-tests dcp-default-message-tests dcp-feature-sites-tests dcp-trophy-tests dcp-incident-tests dcp-incident-upload-tests dcp-crash-dump-tests dcp-cheats-tests dcp-kraken-fix-tests dcp-text-tests dcp-tiling-tests dcp-renderer-selection-tests dcp-opengl-backend-tests dcp-opengl-perf-tests dcp-cursor-tests dcp-mouse-tests dcp-touchscreen-steamos-tests dcp-steam-deck-trackpad-tests dcp-window-tests dcp-sdl-compat-tests dcp-present-tests dcp-frame-pacing-tests dcp-copy-pool-tests dcp-options-menu-tests dcp-comms-wheel-ui-tests dcp-amd-water-tests dcp-pipeline-cache-tests dcp-gpu-compat-tests dcp-water-diag-tests dcp-narrow-water-reader-tests dcp-savedata-tests dcp-voice-tests dcp-system-type-tests dcp-party-tests dcp-thread-lifecycle-tests dcp-comms-tests dcp-comms-session-tests dcp-comms-session-runtime-tests dcp-ultrawide-tests dcp-ultrawide-sides-tests dcp-isa-tests dcp-module-history-tests dcp-update-tests dcp-update-network-tests dcp-update-helper-tests dcp-log-tests dcp-net-tests dcp-online-hle-tests dcp-online-builds-tests dcp-skip-offers-tests dcp-item-attack-cap-tests dcp-skip-intro-tests dcp-character-highlight-tests dcp-gallery-unlock-tests dcp-endgame-unlocks-tests dcp-boss-battle-tests dcp-boss-battle-text-tests dcp-boss-battle-record-tests dcp-boss-battle-menu-tests dcp-boss-battle-online-tests dcp-boss-battle-visitor-lifecycle-tests dcp-boss-battle-handoff-tests dcp-boss-battle-early-story-handoff-tests dcp-early-story-tests dcp-menu-buttons-tests dcp-exit-game-tests dcp-title-layout-tests dcp-rtc-tests dcp-simulate-crash-tests
}
if ($LASTEXITCODE -ne 0) { Stop-Tool 'Test build failed' }
foreach ($test in $tests) {
    if ($test.Exe -and -not (Test-Path -LiteralPath (Join-Path $buildDir "bin\$($test.Exe).exe"))) { Stop-Tool "$($test.Exe) is not built: add it to the ninja line above." }
}
foreach ($suite in $requiredSuites) {
    if (-not ($tests | Where-Object { $_.Exe -eq $suite })) { Stop-Tool "$suite is a required suite but is not in the test list." }
}

$failed = New-Object System.Collections.Generic.List[string]
$parityFailed = New-Object System.Collections.Generic.List[string]
$captured = @{}
$skipped = @{}
$exitCodes = @{}
$lobby = $null
$lobbyProblem = ''
$lobbySkip = ''
$finishedRun = $false
$script:Clock = [Diagnostics.Stopwatch]::StartNew()
$interactive = Test-InteractiveConsole
$script:Progress = New-ProgressReporter @($tests | ForEach-Object { Get-TestName $_ }) 0 $interactive {
    param([string]$Text, [bool]$NoNewline)
    if ($NoNewline) { Write-Host $Text -NoNewline } else { Write-Host $Text }
}
Write-Host $(if ($interactive) { 'Progress: animated line (set DCP_TEST_ANIMATE=0 for plain lines)' } else { 'Progress: plain RUNNING lines every 15 s (set DCP_TEST_ANIMATE=1 to force the animated line)' }) -ForegroundColor DarkGray
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
    $stepIndex = 0
    foreach ($test in $tests) {
        $stepIndex++
        $name = Get-TestName $test
        Start-ProgressGroup $script:Progress $name $script:Clock.Elapsed.TotalSeconds
        Write-Step "[$stepIndex/$($tests.Count)] Running the $($test.Title)"
        $code = 0
        $run = $null
        if ($test.Block) {
            $run = & $test.Block
            if ($run.Skip) {
                $skipped[$name] = $run.Skip
                Clear-ProgressLine $script:Progress
                Write-Host "    SKIPPED: $($run.Skip)" -ForegroundColor Yellow
                Complete-ProgressGroup $script:Progress 'SKIPPED' $script:Clock.Elapsed.TotalSeconds
                continue
            }
            $captured[$name] = $run.Lines
            $code = $run.Code
        } elseif ($test.Script) {
            $run = Invoke-Native 'powershell' (@('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $script:RepoRoot $test.Script)) + $test.Arguments) $false
            $code = $run.Code
            if ($test.Capture) { $captured[$name] = $run.Lines }
        } else {
            $run = Invoke-Native (Join-Path $buildDir "bin\$($test.Exe).exe") $test.Arguments ($test.Capture -and $test.Quiet)
            $code = $run.Code
            if ($test.Capture) {
                $captured[$name] = $run.Lines
                $problems = @(if ($env:DCP_TEST_LOBBY) { Get-LogProblems $test $run.Lines })
                if ($problems -and -not $code) { $code = 1 }
                $problems | ForEach-Object { Write-Host $_ -ForegroundColor Red }
            }
        }
        Clear-ProgressLine $script:Progress
        if ($code -ne 0) {
            $run.Hidden | ForEach-Object { Write-Host $_ }
            $failed.Add($name)
            if ($test.Parity) { $parityFailed.Add($name) }
            $exitCodes[$name] = $code
            Write-Host "    FAIL: $name ($(Format-ExitCode $code))" -ForegroundColor Red
        } else {
            Write-Host '    PASS' -ForegroundColor Green
        }
        Complete-ProgressGroup $script:Progress $(if ($code -ne 0) { 'FAIL' } else { 'PASS' }) $script:Clock.Elapsed.TotalSeconds (Get-FinalCheckCount @($run.Lines))
    }
    $finishedRun = $true
} finally {
    Clear-ProgressLine $script:Progress
    Write-Host ''
    Format-TimingSummary $script:Progress $script:Clock.Elapsed.TotalSeconds | ForEach-Object { Write-Host $_ }
    if (-not $finishedRun) { Write-Host 'Run interrupted before the last group finished.' -ForegroundColor Yellow }
    if ($lobby -and $lobby.Process) {
        Stop-Process -Id $lobby.Process.Id -Force -ErrorAction SilentlyContinue
        $lobby.Process.WaitForExit(5000) | Out-Null
        Remove-Item -LiteralPath $lobby.Files -Force -ErrorAction SilentlyContinue
    }
    if ($null -ne $previousLobbyUrl) { $env:DCP_TEST_LOBBY = $previousLobbyUrl } else { Remove-Item Env:\DCP_TEST_LOBBY -ErrorAction SilentlyContinue }
}
if ($BossBattleOnline) {
    Write-Host ''
    Write-Host ('Boss Battle Online: {0} PASS, {1} FAIL' -f ($tests.Count - $failed.Count), $failed.Count) -ForegroundColor Cyan
    if ($failed.Count -gt 0) {
        Write-Host ('FAILED: ' + ($failed -join ', ')) -ForegroundColor Red
        exit 1
    }
    Write-Host 'Boss Battle Online tests passed.' -ForegroundColor Green
    exit 0
}
if ($EarlyStorySession) {
    $sessionLine = @($captured['dcp-net-tests early-story-session']) | Where-Object { "$_" -like 'GROUP|early-story-session|*' } | Select-Object -First 1
    $sessionState = if ($sessionLine) { "$sessionLine".Split('|')[2] } else { 'FAIL (no result)' }
    if ($sessionState -ne 'PASS' -and -not $failed.Contains('dcp-net-tests early-story-session')) { $failed.Add('dcp-net-tests early-story-session') }
    Write-Host ''
    Write-Host "DCP-7G8RZV Early Story transition/session regression: $sessionState" -ForegroundColor Cyan
    if ($failed.Count -gt 0) {
        Write-Host ('FAILED: ' + ($failed -join ', ')) -ForegroundColor Red
        exit 1
    }
    Write-Host 'Early Story session tests passed.' -ForegroundColor Green
    exit 0
}
if ($LauncherRenderer) {
    $rendererLine = @($captured['dcp-launcher-tests renderer']) | Where-Object { "$_" -like 'GROUP|launcher-renderer|*' } | Select-Object -First 1
    $rendererState = if ($rendererLine) { "$rendererLine".Split('|')[2] } else { 'FAIL (no result)' }
    if ($rendererState -ne 'PASS' -and -not $failed.Contains('dcp-launcher-tests renderer')) { $failed.Add('dcp-launcher-tests renderer') }
    Write-Host ''
    Write-Host "DCP-RFWKVY launcher renderer and presentation: $rendererState" -ForegroundColor Cyan
    if ($failed.Count -gt 0) {
        Write-Host ('FAILED: ' + ($failed -join ', ')) -ForegroundColor Red
        exit 1
    }
    Write-Host 'Launcher renderer tests passed.' -ForegroundColor Green
    exit 0
}
if ($HeadlessCli) {
    $headlessLine = @($captured['dcp-launcher-tests headless']) | Where-Object { "$_" -like 'GROUP|launcher-headless-cli|*' } | Select-Object -First 1
    $headlessState = if ($headlessLine) { "$headlessLine".Split('|')[2] } else { 'FAIL (no result)' }
    if ($headlessState -ne 'PASS' -and -not $failed.Contains('dcp-launcher-tests headless')) { $failed.Add('dcp-launcher-tests headless') }
    Write-Host ''
    Write-Host "Headless command interface: $headlessState" -ForegroundColor Cyan
    $linuxSyntax = $skipped.Keys | Where-Object { $_ -like 'Linux launcher syntax*' } | Select-Object -First 1
    if ($linuxSyntax) { Write-Host "Linux launcher syntax: SKIPPED ($($skipped[$linuxSyntax]))" -ForegroundColor Yellow }
    if ($failed.Count -gt 0) {
        Write-Host ('FAILED: ' + ($failed -join ', ')) -ForegroundColor Red
        exit 1
    }
    Write-Host 'Headless command interface tests passed.' -ForegroundColor Green
    exit 0
}
if ($SavePage) {
    Write-Host ''
    if ($failed.Count -gt 0) {
        Write-Host ('FAILED: ' + ($failed -join ', ')) -ForegroundColor Red
        exit 1
    }
    Write-Host 'Save page tests passed.' -ForegroundColor Green
    exit 0
}
if ($InventoryEditor) {
    Write-Host ''
    if ($failed.Count -gt 0) {
        Write-Host ('FAILED: ' + ($failed -join ', ')) -ForegroundColor Red
        exit 1
    }
    Write-Host 'Inventory Editor tests passed.' -ForegroundColor Green
    exit 0
}
if ($EditorTranslation -or $EditorRelease) {
    $windowsLines = if ($EditorRelease) { @($captured['dcp-save-editor-tests release'] + $captured['dcp-runtime-overlay-tests'] + $captured['dcp-appearance-online-tests']) } else { @($captured['dcp-save-editor-tests appearance-translation']) }
    $windowsEditor = Read-EditorRun $windowsLines
    if ($EditorTranslation) {
        Write-Host ''
        Write-EditorTranslation $windowsEditor
        $group = $windowsEditor.Groups['appearance-translation']
        if (-not $group -or $group.State -ne 'PASS' -or $windowsEditor.Rows.Count -eq 0 -or ($windowsEditor.Rows | Where-Object { $_.Result -ne 'PASS' })) { $failed.Add('Character Editor + translation overlay') }
        Write-Host "Character Editor + translation overlay: $(if ($group) { $group.State } else { 'FAIL (no result)' }), $($windowsEditor.Rows.Count) build/language rows" -ForegroundColor Cyan
    } else {
        $linuxName = @($skipped.Keys + $captured.Keys) | Where-Object { $_ -like 'Linux Character Editor*' } | Select-Object -First 1
        $linuxEditor = if ($linuxName -and $captured.ContainsKey($linuxName)) { Read-EditorRun @($captured[$linuxName]) } else { $null }
        $linuxReason = if ($linuxName -and $skipped.ContainsKey($linuxName)) { $skipped[$linuxName] } else { 'the Linux editor gate did not run' }
        $parity = Show-EditorValidation $windowsEditor $linuxEditor $linuxReason
        Write-Host "Character Editor release gate: Windows $($windowsEditor.Groups.Count) groups, $($windowsEditor.Rows.Count) composition rows; parity $parity" -ForegroundColor Cyan
    }
    if ($failed.Count -gt 0) {
        Write-Host ('FAILED: ' + ($failed -join ', ')) -ForegroundColor Red
        exit 1
    }
    Write-Host 'Character Editor tests passed.' -ForegroundColor Green
    exit 0
}
$parityOutput = @($captured['dcp-build-parity-tests'])

Write-Host ''
Write-Host 'Build parity:' -ForegroundColor Cyan
$partial = $false
$blockedNames = New-Object System.Collections.Generic.List[string]
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
    if ($status -like 'BLOCKED*') { $status = 'BLOCKED (no real executable here; not supported on synthetic evidence)'; $blockedNames.Add($row[1]); $partial = $true }
    Write-Host ('  {0,-34} {1}' -f $row[1], $status) -ForegroundColor $(if (-not $good) { 'Red' } elseif ($status -like 'PARTIAL*' -or $status -like 'BLOCKED*') { 'Yellow' } else { 'Green' })
}
foreach ($name in $requiredBuilds) {
    $row = $rows | Where-Object { $_[1] -eq $name } | Select-Object -First 1
    if (-not $row -or $row[2] -ne 'PASS' -or $row[3] -notlike 'executable*') {
        Write-Host "  required build $name did not PASS with its real executable" -ForegroundColor Red
        $failed.Add("required build: $name")
    }
}
foreach ($name in $blockedNames) {
    if ($blockedBuilds -notcontains $name) { Write-Host "  $name lost its executable evidence" -ForegroundColor Red; $failed.Add("required build: $name") }
}

# One MATRIX line per supported build and platform from dcp-build-parity-tests; Linux comes from the WSL gate's run of
# the same executable.
function Read-Matrix([string[]]$Lines) {
    $table = [ordered]@{}
    foreach ($line in $Lines) {
        $parts = "$line".Trim().Split('|')
        if ($parts.Count -eq 8 -and $parts[0] -eq 'MATRIX') { $table[$parts[1]] = @($parts[1..7] | ForEach-Object { $_ -replace '^[A-Z_]+=', '' }) }
    }
    return $table
}
$matrixBuilds = @($requiredBuilds) + @($blockedBuilds)
$linuxMatrixName = @($skipped.Keys + $captured.Keys) | Where-Object { $_ -like 'Linux gate*' } | Select-Object -First 1
$matrices = [ordered]@{ windows = Read-Matrix $parityOutput }
if ($linuxMatrixName -and $captured.ContainsKey($linuxMatrixName)) { $matrices['linux'] = Read-Matrix $captured[$linuxMatrixName] }
$windowsEditor = Read-EditorRun @($captured['dcp-save-editor-tests release'] + $captured['dcp-runtime-overlay-tests'] + $captured['dcp-appearance-online-tests'])
$linuxEditor = if ($matrices.Contains('linux')) { Read-EditorRun @($captured[$linuxMatrixName]) } else { $null }
$linuxReason = if ($linuxMatrixName -and $skipped.ContainsKey($linuxMatrixName)) { $skipped[$linuxMatrixName] } else { 'the Linux gate did not run' }
$editorParity = Show-EditorValidation $windowsEditor $linuxEditor $linuxReason

# Every supported build, with how deep its validation went: PASS_REAL needs the build's executable and archives, PASS_SYNTHETIC
# is the shared code path on fixtures, BLOCKED_EXECUTABLE is a build whose executable is not here.
$onlineStatic = @{}
foreach ($line in @($captured['dcp-online-builds-tests'])) {
    $parts = "$line".Trim().Split('|')
    if ($parts.Count -ge 4 -and $parts[0] -eq 'ONLINE-BUILD') { $onlineStatic[$parts[2]] = ($parts[3] -replace '^structural=', '') -eq 'PASS' }
}
$commsSynthetic = -not @('dcp-comms-tests', 'dcp-comms-session-tests', 'dcp-comms-session-runtime-tests', 'dcp-comms-wheel-ui-tests' | Where-Object { $failed.Contains($_) })
function Get-EditorState($Run, [string]$Name) {
    if (-not $Run) { return 'SKIPPED' }
    $own = @($Run.Rows | Where-Object { $Name.StartsWith("$($_.Build) ") })
    if (@(Get-EditorProblems $Run).Count -or $own.Count -eq 0) { return 'FAIL' }
    'PASS'
}
Write-Host ''
Write-Host 'Validation matrix (every supported build; REAL = its executable was checked here, SYNTHETIC = fixtures and the shared code path only):' -ForegroundColor Cyan
Write-Host '  BUILD|WINDOWS_EDITOR|LINUX_EDITOR|TRANSLATION_COMPOSITION|SAVE|QUICK_CHAT|ONLINE|HOOKS|VALIDATION_LEVEL|RESULT'
foreach ($name in $matrixBuilds) {
    $hookRow = $matrices['windows'][$name]
    $hookStates = if ($hookRow) { @($hookRow[2..5]) } else { @('MISSING') }
    $hooks = if (-not ($hookStates | Where-Object { $_ -ne 'PASS' })) { 'PASS_REAL' } elseif (-not ($hookStates | Where-Object { $_ -ne 'BLOCKED' })) { 'BLOCKED_EXECUTABLE' } else { 'FAIL' }
    $quick = if ($hookRow -and $hookRow[3] -eq 'PASS') { 'PASS_REAL' } elseif ($hookRow -and $hookRow[3] -eq 'BLOCKED' -and $commsSynthetic -and $onlineStatic[$name]) { 'PASS_SYNTHETIC' } else { 'FAIL' }
    $online = if ($hookRow -and $hookRow[4] -eq 'PASS') { 'PASS_REAL' } elseif ($hookRow -and $hookRow[4] -eq 'BLOCKED' -and $onlineStatic[$name]) { 'PASS_SYNTHETIC' } else { 'FAIL' }
    $own = @($windowsEditor.Rows | Where-Object { $name.StartsWith("$($_.Build) ") })
    $composition = if (-not $own.Count -or ($own | Where-Object { $_.Result -ne 'PASS' })) { 'FAIL' } elseif ($own | Where-Object { $_.Evidence -like 'real*' }) { 'PASS_REAL' } else { 'PASS_SYNTHETIC' }
    $saveKey = @($windowsEditor.Saves.Keys | Where-Object { $name.StartsWith("$_ ") }) | Select-Object -First 1
    $save = if (-not $saveKey -or $windowsEditor.Saves[$saveKey].Result -ne 'PASS') { 'FAIL' } elseif ($windowsEditor.Saves[$saveKey].Evidence -eq 'real') { 'PASS_REAL' } else { 'PASS_SYNTHETIC' }
    $windowsState = Get-EditorState $windowsEditor $name
    $linuxState = Get-EditorState $linuxEditor $name
    $cells = @($windowsState, $linuxState, $composition, $save, $quick, $online, $hooks)
    $level = if ($hooks -eq 'PASS_REAL') { 'REAL' } else { 'SYNTHETIC' }
    $result = if ($cells -contains 'FAIL') { 'FAIL' } elseif ($hooks -eq 'BLOCKED_EXECUTABLE') { 'BLOCKED' } else { 'PASS' }
    if ($result -eq 'FAIL' -or ($requiredBuilds -contains $name -and $result -ne 'PASS')) { $failed.Add("validation matrix: $name") }
    Write-Host ('  ' + (@($name) + $cells + @($level, $result) -join '|')) -ForegroundColor $(if ($result -eq 'PASS') { 'Green' } elseif ($result -eq 'BLOCKED') { 'Yellow' } else { 'Red' })
}
foreach ($name in @($matrices['windows'].Keys | Where-Object { $matrixBuilds -notcontains $_ })) {
    Write-Host "  $name|not in the required or blocked build lists" -ForegroundColor Red
    $failed.Add("validation matrix: $name unexpected")
}
$matrixParity = if (-not $matrices.Contains('linux')) {
    "SKIPPED ($linuxReason)"
} else {
    $differences = @($matrixBuilds | Where-Object {
        $w = $matrices['windows'][$_]; $l = $matrices['linux'][$_]
        -not $w -or -not $l -or (($w[2..6] -join '|') -ne ($l[2..6] -join '|'))
    })
    if ($differences.Count) { "FAIL (Windows and Linux differ for: $($differences -join ', '))" } else { "PASS ($($matrixBuilds.Count) builds, same hook states on Windows and Linux)" }
}
Write-Host "  Windows/Linux hook matrix parity: $matrixParity" -ForegroundColor $(if ($matrixParity -like 'PASS*') { 'Green' } elseif ($matrixParity -like 'FAIL*') { 'Red' } else { 'Yellow' })
if ($matrixParity -like 'FAIL*') { $failed.Add('release matrix: Windows/Linux parity') }
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
    if ($states -contains 'SKIPPED') { return 'SKIPPED (a part did not run, see above; the rest PASS)' }
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
$netGroups = 'foundations', 'address-classification', 'endpoint-selection', 'connection-test', 'direct', 'relay-codec', 'relay-fallback', 'failure-paths', 'transport-quality', 'session-watch', 'early-story-session', 'protocol', 'linux-parity', 'lobby-binary'
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
$linuxGateName = Block-Name 'Linux gate'
$linuxGateState = Block-State $linuxGateName
$linuxNet = Read-Groups $captured[$linuxName]
$linuxWebSocket = if ($linuxState -eq 'SKIPPED') { "SKIPPED ($($skipped[$linuxName]))" }
                  elseif ($linuxNet['lobby-binary'] -eq 'PASS') { 'PASS' }
                  elseif ($linuxNet['lobby-binary'] -eq 'SKIPPED') { 'SKIPPED (WSL runs the net tests without a lobby)' }
                  else { 'NOT RUN' }
$linuxGateSummary = switch ($linuxGateState) {
    'PASS' { 'PASS (tools/build.sh --test exited 0)' }
    'SKIPPED' { "SKIPPED ($($skipped[$linuxGateName]))" }
    'FAIL' { "FAIL ($(Format-ExitCode $exitCodes[$linuxGateName]))" }
    default { 'NOT RUN' }
}
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
    'PASS' { "PASS (net tests compiled with the POSIX WebSocket and run in WSL; static checks $staticLinux)" }
    'SKIPPED' { "SKIPPED ($($skipped[$linuxName])); static checks $staticLinux" }
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
    'Early Story session telemetry' = Group-Status $net @('session-watch')
    'Early Story transition/session regression (DCP-7G8RZV)' = Group-Status $net @('early-story-session')
    'Connection test' = Group-Status $net @('connection-test')
    'Address classification' = Group-Status $net @('address-classification')
    'Endpoint selection' = Group-Status $net @('endpoint-selection')
    'Endpoint path, global IPv6' = Endpoint-Path 'endpoint path IPv6' 'the lobby did not take the observed address'
    'Endpoint path, public IPv4' = Endpoint-Path 'endpoint path IPv4' 'the lobby did not take the observed address'
    'Worker address security' = Worker-Coverage '(advertised endpoint: |LAN candidates: |old clients: |an advertised IPv4 is never|a claimed observed address)' 11
    'Public endpoint budget' = Worker-Coverage '(advertised endpoints spend|repeating an advertised|flipping the LAN list|clients that advertise nothing|repeating an unchanged room or endpoint)' 5
    'Windows parity' = $windows
    'Online HLE' = $hle
    'Linux net tests (WSL)' = $linuxSummary
    'Linux WebSocket round trip' = $linuxWebSocket
    'Linux gate (build.sh --test)' = $linuxGateSummary
    '8-build structural parity' = $(if ($structuralRows.Count -eq 8) { 'PASS' } else { "FAIL ($($structuralRows.Count)/8)" })
    '8-build runtime validation' = $(if ($runtimeRows.Count -eq 8) { 'PASS' } elseif ($runtimeRows.Count -eq 0) { 'DATA UNAVAILABLE (no local boot image: not runtime-tested)' } else { "PARTIAL ($($runtimeRows.Count)/8 inspected)" })
}
Write-Host ''
Write-Host 'Online relay summary:' -ForegroundColor Cyan
foreach ($entry in $summary.GetEnumerator()) {
    $good = $entry.Value -like 'PASS*'
    $color = if ($entry.Value -like 'FAIL*' -or $entry.Value -like 'NOT RUN*') { 'Red' } elseif ($good) { 'Green' } else { 'Yellow' }
    Write-Host ('  {0,-30} {1}' -f ($entry.Key + ':'), $entry.Value) -ForegroundColor $color
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
    'DCP-RFWKVY renderer' = Get-LauncherRow @('launcher-renderer')
    'Headless CLI' = Get-LauncherRow @('launcher-headless-cli')
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
$ranCount = $tests.Count - $skipped.Count
Write-Host ''
Write-Host ('Result: {0} PASS, {1} FAIL, {2} SKIPPED, {3} BLOCKED' -f ($ranCount - $failed.Count), $failed.Count, $skipped.Count, $blockedNames.Count) -ForegroundColor Cyan
foreach ($name in $skipped.Keys) { Write-Host "  SKIPPED: $name ($($skipped[$name]))" -ForegroundColor Yellow }
foreach ($name in $blockedNames) { Write-Host "  BLOCKED: $name (no real executable here)" -ForegroundColor Yellow }
if ($failed.Count -gt 0) {
    Write-Host ('FAILED: ' + ($failed -join ', ')) -ForegroundColor Red
    exit 1
}
if ($partial) {
    Write-Host ('All tests passed. The six required builds PASS; BLOCKED (no real executable): ' + ($blockedNames -join ', ') + '.') -ForegroundColor Yellow
} else {
    Write-Host 'All tests passed.' -ForegroundColor Green
}
exit 0
