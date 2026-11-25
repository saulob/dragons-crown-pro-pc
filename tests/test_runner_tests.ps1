# Checks of tools\_test_runner.ps1 with synthetic steps: small console programs that meet through files in a temporary folder,
# so what may overlap and what may not is proven without relying on how long anything takes.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot '..\tools\_test_history.ps1')
. (Join-Path $PSScriptRoot '..\tools\_progress.ps1')
. (Join-Path $PSScriptRoot '..\tools\_test_runner.ps1')
$failures = 0
$checks = 0
$output = New-Object System.Collections.Generic.List[string]

function Check([bool]$condition, [string]$what) {
    $script:checks++
    if (-not $condition) { $script:failures++; Write-Output "FAIL $what" }
}

# What the runner prints is kept, and part of what is checked.
function Write-Host($Object = '', $ForegroundColor = $null, [switch]$NoNewline) { $script:output.Add("$Object") }
function Write-Step([string]$Text) { $script:output.Add("==> $Text") }

$work = Join-Path ([IO.Path]::GetTempPath()) "dcp-test-runner-$PID"
New-Item -ItemType Directory -Force -Path $work | Out-Null
$buildDir = $work
$shell = (Get-Process -Id $PID).Path

# A step that runs PowerShell code in the temporary folder: Enter/Leave hold a named resource nobody else may hold.
function New-Synthetic([string]$Key, [string]$Code, [string]$Lane = 'main', [switch]$Timing, [switch]$Final, [switch]$Parity, $Nominal = $null) {
    $file = Join-Path $work ("$Key.ps1" -replace ' ', '_')
    $prelude = @'
$ErrorActionPreference = 'Stop'
function Enter([string]$Name) { [IO.File]::Open((Join-Path $PSScriptRoot "$Name.lock"), 'CreateNew', 'Write', 'None') }
function Leave($Stream, [string]$Name) { $Stream.Dispose(); Remove-Item -LiteralPath (Join-Path $PSScriptRoot "$Name.lock") }
function Mark([string]$Name) { [IO.File]::WriteAllText((Join-Path $PSScriptRoot "$Name.mark"), 'x') }
function Seen([string]$Name) { Test-Path -LiteralPath (Join-Path $PSScriptRoot "$Name.mark") }
function Await([string]$Name) { $until = [DateTime]::UtcNow.AddSeconds(20); while (-not (Seen $Name)) { if ([DateTime]::UtcNow -gt $until) { throw "$Name never happened" }; [Threading.Thread]::Sleep(20) } }
'@
    [IO.File]::WriteAllText($file, "$prelude`n$Code`nexit 0`n")
    $command = { [pscustomobject]@{ Skip = ''; File = $shell; Arguments = @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', $file) } }.GetNewClosure()
    Test-Step "synthetic $Key" -Command $command -Key $Key -Lane $Lane -Timing:$Timing -Final:$Final -Parity:$Parity -Nominal $Nominal
}

function Start-Run($Tests, [bool]$Parallel) {
    Get-ChildItem -LiteralPath $work -File | Where-Object { $_.Extension -in '.lock', '.mark' } | Remove-Item -Force
    $script:output.Clear()
    $script:captured = @{}
    $script:skipped = @{}
    $script:exitCodes = @{}
    $script:failed = New-Object System.Collections.Generic.List[string]
    $script:parityFailed = New-Object System.Collections.Generic.List[string]
    $script:Lanes.Clear()
    $script:FinishedRuns.Clear()
    $script:mode = 'synthetic'
    $script:BuildSeconds = 0
    $script:Clock = [Diagnostics.Stopwatch]::StartNew()
    $script:History = New-TestHistory (Join-Path $work 'history\timings.json')
    $groups = @($Tests | ForEach-Object { New-ProgressGroup (Get-TestName $_) $_.Platform $(if ($Parallel) { $_.Lane } else { 'main' }) ([int]($Parallel -and ($_.Timing -or $_.Final))) $_.Nominal })
    $script:Progress = New-ProgressReporter $groups 0 $false { param([string]$Text, [bool]$NoNewline) Write-Host $Text } 3600
    Invoke-TestList $Tests $Parallel
}

function State([string]$Name) { (Get-ProgressGroup $script:Progress $Name).State }

try {
    # The whole schedule: overlap where it is allowed, and never where it is not.
    $tests = @(
        (New-Synthetic 'm1' "Mark 'm1-runs'; Await 'e1-runs'; Await 'l1-runs'; `$s = Enter 'main'; 'm1 line'; Leave `$s 'main'; Mark 'm1-done'"),
        (New-Synthetic 'e1' "Mark 'e1-runs'; `$s = Enter 'save-editor-folder'; Await 'm1-runs'; Await 'l1-runs'; Await 'm2-done'; foreach (`$i in 1..40) { if (Seen 't1-runs') { throw 't1 did not wait' }; [Threading.Thread]::Sleep(20) }; 'e1 first'; 'GROUP|e1|PASS'; Leave `$s 'save-editor-folder'; Mark 'e1-done'" 'editor'),
        (New-Synthetic 't1' "Mark 't1-runs'; if (-not (Seen 'e1-done')) { throw 'the editor lane was still running' }; if (-not (Seen 'm2-done')) { throw 'before the main list' }; `$s = Enter 'main'; 't1 line'; Leave `$s 'main'; Mark 't1-done'" 'main' -Timing),
        (New-Synthetic 'l1' "Mark 'l1-runs'; `$s = Enter 'wsl'; Await 'm1-runs'; Await 'e1-runs'; 'l1 line'; Leave `$s 'wsl'; Mark 'l1-done'" 'linux'),
        (New-Synthetic 'l2' "if (-not (Seen 'l1-done')) { throw 'l1 and l2 share a lane' }; `$s = Enter 'wsl'; Await 't1-done'; '12 checks, 0 failed'; Leave `$s 'wsl'; Mark 'l2-done'" 'linux'),
        (New-Synthetic 'm2' "if (-not (Seen 'm1-done')) { throw 'the main list is sequential' }; `$s = Enter 'main'; 'm2 line'; Leave `$s 'main'; Mark 'm2-done'"),
        (New-Synthetic 'f1' "foreach (`$n in 'e1', 'l1', 'l2', 't1', 'm2') { if (-not (Seen `"`$n-done`")) { throw `"`$n had not finished`" } }; 'f1 line'" 'main' -Final)
    )
    Start-Run $tests $true
    Check ($failed.Count -eq 0) "every group passes (failed: $($failed -join ', '); $($output -join ' / '))"
    Check (@($script:Progress.Groups | Where-Object { $_.State -ne 'PASS' }).Count -eq 0) 'and is accounted for'
    Check ($script:LiveNative.Count -eq 0) 'no process is left'
    $text = $output -join "`n"
    foreach ($test in $tests) { Check (@($output | Where-Object { $_ -match "^==> (Running the|Finished in the background:) synthetic $($test.Key)`$" }).Count -eq 1) "$($test.Key) is announced once" }
    foreach ($name in 'e1', 'l1', 'l2') {
        $at = $output.IndexOf("==> Finished in the background: synthetic $name")
        Check ($at -ge 0 -and $output.GetRange($at, $output.Count - $at).IndexOf('    PASS') -le 2) "$name's result follows its heading"
    }
    Check ($text.Contains("GROUP|e1|PASS`n    PASS") -and -not $text.Contains('e1 first')) 'a passing background group prints its result lines, whole'
    Check ($captured['e1'] -contains 'e1 first') 'and keeps all of its output'
    $running = @($output | Where-Object { $_ -match '^==> Running the synthetic ' } | ForEach-Object { $_.Split(' ')[-1] }) -join ' '
    Check ($running -eq 'm1 m2 t1 f1') "main list order: $running"
    Check ((Get-ProgressGroup $script:Progress 'l2').Checks -eq 12) 'the check count of a background group'
    Check ((Save-TestHistory $script:History) -and (New-TestHistory $script:History.Path).Groups.Count -eq 7) 'every pass has a duration'
    Check ((New-TestHistory $script:History.Path).Groups.ContainsKey('windows|synthetic|l2')) 'stored under platform, mode and group'

    # A failure in a lane: its whole output, its exit code, and the rest of the lane still runs.
    $tests = @(
        (New-Synthetic 'ok' "Await 'bad-done'; 'ok'"),
        (New-Synthetic 'bad' "'hidden detail'; [Console]::Error.WriteLine('on the error stream'); 'FAIL line 7'; Mark 'bad-done'; exit 3" 'linux' -Parity),
        (New-Synthetic 'next' "'next'" 'linux'),
        (Test-Step 'synthetic skipped' -Command { Skipped 'no such platform' } -Key 'Linux skipped' -Lane 'linux'),
        (Test-Step 'synthetic missing' -Command { [pscustomobject]@{ Skip = ''; File = (Join-Path $work 'no-such-program.exe'); Arguments = @() } } -Key 'missing' -Lane 'linux')
    )
    Remove-Item -LiteralPath (Join-Path $work 'history') -Recurse -Force
    Start-Run $tests $true
    Check (($failed -join ' ') -eq 'bad missing' -and ($parityFailed -join ' ') -eq 'bad' -and $exitCodes['bad'] -eq 3) "only the failures fail ($($failed -join ' '))"
    $at = $output.IndexOf('==> Finished in the background: synthetic bad')
    $block = $output.GetRange($at, [Math]::Min(6, $output.Count - $at)) -join "`n"
    foreach ($part in 'hidden detail', 'on the error stream', 'FAIL line 7', '    FAIL: bad (exit code 3, 0x00000003)') { Check ($block.Contains($part)) "the failed group shows '$part'" }
    Check ((State 'next') -eq 'PASS' -and (State 'Linux skipped') -eq 'SKIPPED' -and (State 'missing') -eq 'FAIL' -and $skipped['Linux skipped'] -eq 'no such platform') 'the lane goes on after a failure'
    Check ($output -contains '    SKIPPED: no such platform') 'a skip names its reason'
    $null = Save-TestHistory $script:History
    $stored = @((New-TestHistory $script:History.Path).Groups.Keys | Sort-Object) -join ' '
    Check ($stored -eq 'windows|synthetic|next windows|synthetic|ok') "only passes are timed: $stored"

    # The same steps one after another.
    $tests = @(
        (New-Synthetic 'a' "`$s = Enter 'alone'; 'a'; Leave `$s 'alone'" 'linux'),
        (New-Synthetic 'b' "`$s = Enter 'alone'; 'b'; Leave `$s 'alone'" 'editor'),
        (New-Synthetic 'c' "`$s = Enter 'alone'; '@@dcp-progress checks=5 total=10 estimate=1 detail=half'; 'c'; Leave `$s 'alone'" 'main' -Timing)
    )
    Start-Run $tests $false
    Check ($failed.Count -eq 0 -and $script:Lanes.Count -eq 0 -and (@($script:Progress.Groups | ForEach-Object { $_.Start }) -join ' ') -eq (@($script:Progress.Groups | ForEach-Object { $_.Start } | Sort-Object) -join ' ')) 'sequential mode keeps the list order'
    Check ((Get-ProgressGroup $script:Progress 'c').Checks -eq 5 -and -not ($captured['c'] -match 'dcp-progress')) 'progress lines feed the display and stay out of the output'

    # An interrupted run: the running program is stopped, nothing is timed.
    $job = Start-Native $shell @('-NoProfile', '-NonInteractive', '-Command', '[Threading.Thread]::Sleep(600000)') $true '' '' $true
    $id = $job.Process.Id
    $saved = $script:History.NewGroups.Count
    Stop-LiveNative
    Check ($script:LiveNative.Count -eq 0 -and $job.Process.WaitForExit(20000) -and $script:History.NewGroups.Count -eq $saved) "an interrupted group ($id) is stopped and not stored"

    # The run report.
    $output.Clear()
    $script:History = New-TestHistory (Join-Path $work 'history\timings.json')
    Write-RunReport $true
    Check (@($output | Where-Object { $_ -like 'Total execution time: *' }).Count -eq 1 -and @($output | Where-Object { $_ -like 'Groups: 3 PASS, 0 FAIL, 0 SKIPPED, 0 BLOCKED' }).Count -eq 1) 'the report has the total and the counts'
    $output.Clear()
    $script:History = New-TestHistory (Join-Path $work 'history\timings.json')
    Write-RunReport $true
    Check (@($output | Where-Object { $_ -like 'Previous comparable run: *' }).Count -eq 1) 'and compares with the previous run of its mode'
    $script:mode = 'other'
    $output.Clear()
    Write-RunReport $true
    Check (@($output | Where-Object { $_ -like 'Previous comparable run: *' }).Count -eq 0) 'never with another mode'
    [IO.File]::WriteAllText((Join-Path $work 'history\timings.json'), '{ broken')
    $script:History = New-TestHistory (Join-Path $work 'history\timings.json')
    $output.Clear()
    Write-RunReport $false
    Check (@($output | Where-Object { $_ -like 'Timing history: the file was not readable*' }).Count -eq 1 -and $output -contains 'Run interrupted before the last group finished.') 'a damaged history and an interrupted run are named, not fatal'
} finally {
    Stop-LiveNative
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}

if ($failures) { Write-Output "$failures of $checks test runner checks failed"; exit 1 }
Write-Output "test runner tests: $checks checks, 0 failed"
exit 0
