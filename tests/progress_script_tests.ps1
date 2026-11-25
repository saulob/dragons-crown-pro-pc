# Checks of tools\_progress.ps1 and tools\_test_history.ps1 on synthetic time: nothing sleeps and no process is started.
# -Show prints a synthetic run.
param([switch]$Show)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot '..\tools\_test_history.ps1')
. (Join-Path $PSScriptRoot '..\tools\_progress.ps1')
$failures = 0
$checks = 0
$clock = [datetime]'2026-11-25T22:00:00'

function Equal($actual, $expected, [string]$what) {
    $script:checks++
    if ("$actual" -cne "$expected") { $script:failures++; Write-Host "FAIL $what`n  expected: $expected`n  actual:   $actual" }
}

function Check([bool]$condition, [string]$what) {
    $script:checks++
    if (-not $condition) { $script:failures++; Write-Host "FAIL $what" }
}

function New-Reporter($Groups, [bool]$Interactive = $false, [double]$Interval = 0, $Sink = $null) {
    if ($null -eq $Sink) { $Sink = New-Object System.Collections.Generic.List[string] }
    $writer = { param([string]$Text, [bool]$NoNewline) $Sink.Add($Text) }.GetNewClosure()
    New-ProgressReporter $Groups 0 $Interactive $writer $Interval $clock
}

function Known([string]$Name, [double]$Seconds, [string]$Lane = 'main', [int]$Phase = 0, [string]$Platform = 'windows') {
    New-ProgressGroup $Name $Platform $Lane $Phase $Seconds $true $Seconds
}

function Eta($R, [double]$Now) { (Get-ProgressEta $R $Now).Seconds }

# ---- formatting

Equal (Get-ProgressPercent 87 95) 91 'percent of 87/95'
Equal (Get-ProgressPercent 14 14) 100 'complete'
Equal (Get-ProgressPercent 0 0) 0 'no groups: no divide by zero'
Equal (Get-ProgressPercent 30 10) 100 'done above total is capped'
Equal (Format-Elapsed 3) '3s' 'elapsed seconds'
Equal (Format-Elapsed 59.9) '59s' 'elapsed just under a minute'
Equal (Format-Elapsed 60) '1m 00s' 'elapsed one minute'
Equal (Format-Elapsed 1902) '31m 42s' 'elapsed minutes'
Equal (Format-Elapsed 6138) '1h 42m 18s' 'elapsed hours'
Equal (Format-Elapsed -5) '0s' 'negative elapsed'
Equal (Format-ShortDuration 2.1) '2.1s' 'short duration'
Equal (Format-Rounded 270) '4m 30s' 'rounded minutes and seconds'
Equal (Format-Rounded 1080) '18m' 'rounded whole minutes'
Equal (Format-Rounded 6120) '1h 42m' 'rounded hours'
Equal (Format-Rounded -10) '2s' 'a negative estimate is never shown'
Equal (Format-Eta $null) 'calculating' 'no ETA'
Equal (Format-Eta ([pscustomobject]@{ Seconds = [double]::NaN; Confident = $true })) 'calculating' 'NaN ETA'
Equal (Format-Eta ([pscustomobject]@{ Seconds = 1260; Confident = $true })) '21m' 'ETA from history'
Equal (Format-Eta ([pscustomobject]@{ Seconds = 1260; Confident = $false })) '~21m (low confidence)' 'ETA without enough history says so'
Equal (Format-Finish ([datetime]'2026-11-25T23:20:00') ([pscustomobject]@{ Seconds = 1260; Confident = $true })) '23:41' 'finish time'
Equal (Format-Finish ([datetime]'2026-11-25T23:50:00') ([pscustomobject]@{ Seconds = 1260; Confident = $true })) '00:11 (+1d)' 'finish after midnight'
Equal (Format-Finish ([datetime]'2026-11-25T09:05:00') ([pscustomobject]@{ Seconds = 60; Confident = $false })) '~09:06' 'a low confidence finish is marked'
Equal (Format-Finish ([datetime]'2026-11-25T09:05:00') $null) '' 'no ETA, no finish time'
Equal (Format-ChecksText 1284220 2039231 $true) '1,284,220 / ~2,039,231' 'estimated total'
Equal (Format-ChecksText 12845 $null $false) '12,845' 'unknown total'
Equal (Format-ChecksText $null $null $false) '' 'no count'
Equal (Format-Change 6480 7440) '-16m 00s, -12.9%' 'a faster run'
Equal (Format-Change 75 60) '+15s, +25.0%' 'a slower run'

$child = ConvertFrom-ChildProgress '@@dcp-progress checks=1284220 total=2039231 estimate=1 detail=12/67 units: fighter, dwarf'
Check ($child.Checks -eq 1284220 -and $child.Expected -eq 2039231 -and $child.Estimate -and $child.Detail -eq '12/67 units: fighter, dwarf') 'child progress line'
Check ($null -eq (ConvertFrom-ChildProgress 'GROUP|editor-real-parts|PASS|checks=5|ms=9')) 'a GROUP line is not progress'
Check ($null -eq (ConvertFrom-ChildProgress '@@dcp-progress checks=abc')) 'a malformed progress line is ignored'
Equal (Get-FinalCheckCount @('x', '13666 checks, 0 failed')) 13666 'final check count'

$savedAnimate = $env:DCP_TEST_ANIMATE
$env:DCP_TEST_ANIMATE = '1'
Check (Test-InteractiveConsole) 'DCP_TEST_ANIMATE=1 forces the animated line'
$env:DCP_TEST_ANIMATE = '0'
Check (-not (Test-InteractiveConsole)) 'DCP_TEST_ANIMATE=0 forces plain lines'
if ($null -eq $savedAnimate) { Remove-Item Env:\DCP_TEST_ANIMATE } else { $env:DCP_TEST_ANIMATE = $savedAnimate }

# ---- ETA

$r = New-Reporter @((Known 'a' 10), (Known 'b' 20), (Known 'huge' 3600), (Known 'tiny' 1))
Equal (Eta $r 0) 3631 'the ETA is the sum of what each group takes'
Start-ProgressGroup $r 'a' 0
Complete-ProgressGroup $r 'PASS' 10
Start-ProgressGroup $r 'b' 10
Complete-ProgressGroup $r 'PASS' 30
Equal (Eta $r 30) 3601 'two thirds of the groups done say nothing about the expensive one left'
Start-ProgressGroup $r 'huge' 30
Equal (Eta $r 1830) 1801 'a running group counts what it still needs'
$at = (Eta $r 3630)
Check ($at -gt 500) 'a group at its estimate is not about to end'
$later = (Eta $r 7230)
Check ($later -gt $at) 'a group past its estimate raises the ETA instead of reaching zero'
Equal $later (0.15 * 7200 + 1) 'the overrun is a share of the time already taken'
Check ((Get-ProgressEta $r 7230).Confident) 'history makes the ETA confident'
Complete-ProgressGroup $r 'PASS' 7230
Start-ProgressGroup $r 'tiny' 7230
Complete-ProgressGroup $r 'PASS' 7231
Equal (Eta $r 7231) 0 'nothing left'

$previous = 0.0
$growing = New-Reporter @((Known 'x' 100))
Start-ProgressGroup $growing 'x' 0
foreach ($second in 120, 150, 300, 900) {
    $value = Eta $growing $second
    Check ($value -gt $previous -and $value -gt 0) "the ETA keeps growing at $second s of an expected 100 s"
    $previous = $value
}

$unknown = New-Reporter @((New-ProgressGroup 'new-1'), (New-ProgressGroup 'new-2'), (New-ProgressGroup 'new-3'))
$first = Get-ProgressEta $unknown 0
Check ($first.Seconds -eq 180 -and -not $first.Confident) 'nothing known: a conservative guess, marked as one'
Start-ProgressGroup $unknown 'new-1' 0
Complete-ProgressGroup $unknown 'PASS' 40
$second = Get-ProgressEta $unknown 40
Check ($second.Seconds -eq 80 -and -not $second.Confident) 'then comparable groups of this run'
$mixed = New-Reporter @((Known 'old' 100), (New-ProgressGroup 'new' 'windows' 'main' 0 20 $false))
Check ((Get-ProgressEta $mixed 0).Confident) 'mostly history stays confident'
$nominal = New-Reporter @((Known 'old' 20), (New-ProgressGroup 'new' 'windows' 'main' 0 100 $false))
Check (-not (Get-ProgressEta $nominal 0).Confident) 'mostly guesses do not'
$platforms = New-Reporter @((Known 'w' 10), (Known 'w2' 10), (New-ProgressGroup 'Linux gate' 'linux'))
Start-ProgressGroup $platforms 'w' 0
Complete-ProgressGroup $platforms 'PASS' 10
Equal (Eta $platforms 10) 70 'a Linux group without history is not taken for a Windows one'

$lanes = New-Reporter @((Known 'm1' 60), (Known 'm2' 60), (Known 'late' 90 'main' 1), (Known 'editor' 200 'editor'), (Known 'l1' 100 'linux' 0 'linux'), (Known 'l2' 150 'linux' 0 'linux'))
Equal (Eta $lanes 0) 290 'lanes overlap: the editor lane, then the timing groups'
Start-ProgressGroup $lanes 'editor' 0 -Background
Start-ProgressGroup $lanes 'l1' 0 -Background
Start-ProgressGroup $lanes 'm1' 0
Equal (Eta $lanes 30) 260 'every lane advances at once'
Complete-ProgressGroup $lanes 'PASS' 200 $null 'editor'
Complete-ProgressGroup $lanes 'PASS' 60
Start-ProgressGroup $lanes 'm2' 60
Complete-ProgressGroup $lanes 'PASS' 120
Complete-ProgressGroup $lanes 'PASS' 200 $null 'l1'
Start-ProgressGroup $lanes 'l2' 200 -Background
Equal (Eta $lanes 200) 150 'the Linux lane is the longest one left'
Check ((Format-ProgressLine $lanes 210).StartsWith('[4/6] 66% | l2 | Group 10s of 2m 30s | Total 3m 30s | ETA')) 'with nothing in the foreground the line is about the background group'

# ---- progress lines

$line = New-Reporter @((Known 'done' 5), (Known 'editor-real-parts' 1080), (Known 'rest' 1260))
Start-ProgressGroup $line 'done' 0
Complete-ProgressGroup $line 'PASS' 5
Start-ProgressGroup $line 'editor-real-parts' 5
$null = Set-ProgressChecks $line 5 2038817 $null $false
Equal (Format-ProgressLine $line 1071) '[1/3] 33% | editor-real-parts | Group 17m 46s of 18m | Total 17m 51s | ETA 24m | Finish 22:41 | Checks 2,038,817' 'progress line'
foreach ($width in 60, 75, 90, 110, 140) {
    $narrow = Format-ProgressLine $line 1071 $width
    Check ($narrow.Length -le $width -and $narrow.Contains('Group 17m 46s') -and $narrow.Contains('Total 17m 51s') -and $narrow.Contains('ETA 24m')) "Group, Total and ETA stay at $width columns"
}

$redirected = New-Object System.Collections.Generic.List[string]
$r = New-Reporter @((Known 'a' 100), (Known 'b' 10)) $false 0 $redirected
Start-ProgressGroup $r 'a' 0
Equal $redirected.Count 1 'a group starts with one line'
Equal $redirected[0] '[0/2] 0% | a | Group 0s of 1m 30s | Total 0s | ETA 2m | Finish 22:01' 'first line'
foreach ($second in 1..29) { $null = Update-Progress $r $second }
Equal $redirected.Count 1 'redirected output is quiet inside the interval'
foreach ($second in 30..90) { $null = Set-ProgressChecks $r $second ($second * 100) 1000000 $true 'step 4' }
Equal $redirected.Count 4 'one line per interval, not one per update'
Check ($redirected[3].StartsWith('RUNNING [0/2] 0% | a | Group 1m 30s') -and $redirected[3].EndsWith('| step 4')) 'redirected line'
Check (($redirected -join '').IndexOf("`r") -lt 0 -and ($redirected -join '').IndexOf([char]27) -lt 0) 'redirected output has no carriage return or escape'

$live = New-Object System.Collections.Generic.List[string]
$i = New-Reporter @('a', 'b') $true 0 $live
$i.Width = 100
Start-ProgressGroup $i 'a' 0
$null = Update-Progress $i 0.5
$null = Update-Progress $i 1.0
$frames = @($live | Where-Object { $_.StartsWith("`r") })
Equal $frames.Count 2 'interactive mode redraws one line'
Check ($frames[0].StartsWith("`r| [0/2]") -and $frames[1].StartsWith("`r/ [0/2]") -and $frames[0].Length -eq 101) 'the spinner advances and the line fits'
Clear-ProgressLine $i
Check ($live[$live.Count - 1].StartsWith("`r") -and $live[$live.Count - 1].Trim() -eq '') 'the line is cleared when the step ends'

# ---- final report

$timing = New-Reporter @((Known 'fast' 2), (Known 'slow' 1000), (Known 'mid' 100), (Known 'broken' 5), (Known 'absent' 5), (Known 'never' 5))
foreach ($row in @(@('fast', 'PASS', 0, 2.5), @('slow', 'PASS', 2.5, 1702.5), @('mid', 'PASS', 1702.5, 1832.5), @('broken', 'FAIL', 1832.5, 2032.5), @('absent', 'SKIPPED', 2032.5, 2032.5))) {
    Start-ProgressGroup $timing $row[0] $row[2]
    Complete-ProgressGroup $timing $row[1] $row[3] 10
}
$report = @(Format-TimingReport $timing 2032.5 2 ([pscustomobject]@{ seconds = 2400.0; at = '2026-11-24T22:00:00' }))
$text = $report -join "`n"
Check ($text.Contains('Total execution time: 33m 52s')) 'total execution time'
Check ($text.Contains('Groups: 3 PASS, 1 FAIL, 1 SKIPPED, 2 BLOCKED') -and $text.Contains('Incomplete: 5/6 groups finished')) 'counts'
Check ($text -match '(?m)^ 1\. slow\s+28m 20s\s+83\.6%\s+previous 16m 40s$') 'the slowest group: duration, share and previous'
Check ($text.IndexOf(' 2. broken') -gt 0 -and $text -notmatch '(?m)^ \d\. absent') 'a skipped group is not a slow one'
Check ($text.Contains('WARNING: slow took 28m 20s, usually 16m 40s (+11m 40s, +70.0%); not a failure')) 'a clear regression is reported'
Equal (@($report | Where-Object { $_ -like 'WARNING:*' }).Count) 1 'small or failed groups are no regression'
Check ($text.Contains('Current run: 33m 52s') -and $text.Contains('Previous comparable run: 40m 00s (2026-11-24T22:00:00)') -and $text.Contains('Change: -6m 07s, -15.3%')) 'comparison with the previous run'
Check (-not ((Format-TimingReport $timing 2032.5) -join "`n").Contains('Previous comparable run')) 'no previous run, no comparison'
$many = New-Reporter @(1..14 | ForEach-Object { "g$_" })
foreach ($index in 1..14) { Start-ProgressGroup $many "g$index" ($index * 100); Complete-ProgressGroup $many 'PASS' ($index * 100 + $index) }
Equal (Get-SlowestGroups $many 10).Count 10 'ten slowest groups'
Equal ((Get-SlowestGroups $many 10)[0].Name) 'g14' 'slowest first'

# ---- history

$folder = Join-Path ([IO.Path]::GetTempPath()) "dcp-test-history-$PID"
$path = Join-Path $folder 'test-history\timings.json'
try {
    $history = New-TestHistory $path
    Check ($history.Groups.Count -eq 0 -and $history.Problem -eq '' -and $history.Writable) 'a missing file is an empty history'
    $key = Get-TimingKey 'windows' 'full' 'dcp-net-tests'
    Check (Add-TimingSample $history $key 'PASS' 91.234 100 $clock) 'a pass is stored'
    foreach ($state in 'FAIL', 'SKIPPED', 'BLOCKED', 'RUNNING', 'WAITING', '') { Check (-not (Add-TimingSample $history $key $state 5)) "$state is not a duration" }
    foreach ($bad in 0, -1, $null, 'soon', [double]::NaN, [double]::PositiveInfinity) { Check (-not (Add-TimingSample $history $key 'PASS' $bad)) "'$bad' is not a duration" }
    Check (-not (Add-RunSample $history 'full' $false 100 5)) 'an interrupted or failed run is not stored'
    Check (Add-RunSample $history 'full' $true 100 5 $clock) 'a complete run is stored'
    Check (Save-TestHistory $history) 'the file is written'
    Check (-not (Save-TestHistory $history)) 'nothing new, nothing written'
    Equal (@(Get-ChildItem -LiteralPath (Split-Path $path)).Count) 1 'no partial file is left'
    $bytes = [IO.File]::ReadAllBytes($path)
    Check ($bytes[0] -eq [byte][char]'{' -and [IO.File]::ReadAllText($path).EndsWith("}`n")) 'plain JSON text'
    $again = New-TestHistory $path
    Equal ((Get-HistoryDurations $again $key) -join ',') '91.23' 'a duration survives the file'
    Equal $again.Groups[$key][0].at '2026-11-25T22:00:00' 'so does its time'
    Equal $again.Groups[$key][0].checks 100 'and its check count'
    Check ((Get-PreviousRun $again 'full').seconds -eq 100 -and $null -eq (Get-PreviousRun $again 'SavePage') -and $null -eq (Get-PreviousRun $again 'full-sequential')) 'runs are compared only inside their mode'

    foreach ($seconds in 1..20) { $null = Add-TimingSample $again $key 'PASS' $seconds; $null = Save-TestHistory $again }
    $bounded = New-TestHistory $path
    Equal ((Get-HistoryDurations $bounded $key) -join ',') '13,14,15,16,17,18,19,20' 'the newest samples are kept, and no more'
    Equal (Get-DurationEstimate $bounded 'windows' 'full' 'dcp-net-tests').Seconds 16.5 'the estimate is their median'
    Equal (Get-DurationEstimate $bounded 'windows' 'full' 'dcp-net-tests').Previous 20 'the previous duration is the last one'
    $null = Add-TimingSample $bounded $key 'PASS' 5000
    $null = Save-TestHistory $bounded
    Equal (Get-DurationEstimate (New-TestHistory $path) 'windows' 'full' 'dcp-net-tests').Seconds 17.5 'one outlier does not move the median'

    $two = New-TestHistory $path
    $one = New-TestHistory $path
    $null = Add-TimingSample $one 'windows|full|from-one' 'PASS' 3
    $null = Add-TimingSample $two 'windows|full|from-two' 'PASS' 4
    Check ((Save-TestHistory $one) -and (Save-TestHistory $two)) 'two runs write'
    $both = New-TestHistory $path
    Check ($both.Groups.ContainsKey('windows|full|from-one') -and $both.Groups.ContainsKey('windows|full|from-two')) 'neither erases the other'

    $null = Add-TimingSample $both (Get-TimingKey 'linux' 'full' 'shared-name') 'PASS' 500
    $null = Add-TimingSample $both (Get-TimingKey 'windows' 'default' 'shared-name') 'PASS' 7
    $null = Add-TimingSample $both (Get-TimingKey 'windows' 'full' 'shared-name') 'PASS' 70
    $null = Save-TestHistory $both
    $separate = New-TestHistory $path
    Equal (Get-DurationEstimate $separate 'linux' 'full' 'shared-name').Seconds 500 'Linux has its own durations'
    Equal (Get-DurationEstimate $separate 'windows' 'full' 'shared-name').Seconds 70 'Full has its own durations'
    Equal (Get-DurationEstimate $separate 'windows' 'default' 'shared-name').Seconds 7 'and so has a focused run'
    $borrowed = Get-DurationEstimate $separate 'windows' 'SavePage' 'shared-name' 999
    Check ($borrowed.Seconds -eq 38.5 -and -not $borrowed.Known -and $null -eq $borrowed.Previous) 'another mode is a guess, never history, and never another platform'
    $guess = Get-DurationEstimate $separate 'windows' 'full' 'brand-new' 240
    Check ($guess.Seconds -eq 240 -and -not $guess.Known) 'without history the declared guess'
    Check ($null -eq (Get-DurationEstimate $separate 'windows' 'full' 'brand-new').Seconds) 'or nothing'

    $good = [IO.File]::ReadAllText($path)
    foreach ($broken in @('', '   ', '{', $good.Substring(0, [int]($good.Length / 2)), 'not json', '[1, 2]', '"text"', 'null', '{"schema":1}', '{"schema":1,"groups":[],"runs":7}',
            '{"groups":{"windows|full|a":[{"seconds":5}]}}', '{"schema":0,"groups":{"a":5}}', '{"schema":"x"}',
            '{"schema":1,"groups":{"k":[{"seconds":"fast"},{"seconds":-4},{},null,3,"x",{"seconds":true}]}}')) {
        [IO.File]::WriteAllText($path, $broken)
        $recovered = New-TestHistory $path
        Check ($recovered.Groups.Count -eq 0 -and $recovered.Writable) "damaged history is no history: '$($broken.Substring(0, [Math]::Min(40, $broken.Length)))'"
        $null = Add-TimingSample $recovered $key 'PASS' 12
        Check ((Save-TestHistory $recovered) -and (New-TestHistory $path).Groups.ContainsKey($key)) 'and the next save repairs it'
    }
    [IO.File]::WriteAllText($path, '{"schema":1,"future":{"a":1},"groups":{"windows|full|a":[{"seconds":5,"at":"x","cpu":9,"nested":{"b":2}},{"seconds":9}],"bad":3},"runs":{}}')
    Equal ((Get-HistoryDurations (New-TestHistory $path) 'windows|full|a') -join ',') '5,9' 'unknown fields are ignored'
    $newer = '{"schema":99,"groups":{"windows|full|a":[{"seconds":5}]}}'
    [IO.File]::WriteAllText($path, $newer)
    $future = New-TestHistory $path
    $null = Add-TimingSample $future $key 'PASS' 12
    Check ($future.Groups.Count -eq 0 -and $future.Problem -eq 'a newer schema' -and -not (Save-TestHistory $future) -and [IO.File]::ReadAllText($path) -eq $newer) 'a newer file is neither read nor overwritten'

    [IO.File]::WriteAllText($path, $good)
    $locked = New-TestHistory $path
    $null = Add-TimingSample $locked $key 'PASS' 12
    $hold = [IO.File]::Open($path, 'Open', 'Read', 'None')
    try { Check (-not (Save-TestHistory $locked)) 'a file in use only costs the save' } finally { $hold.Dispose() }
    Equal ([IO.File]::ReadAllText($path)) $good 'an unfinished write leaves the old file whole'
    Equal (@(Get-ChildItem -LiteralPath (Split-Path $path)).Count) 1 'and no partial file'
    Check (Save-TestHistory $locked) 'the samples are written on the next try'
} finally {
    Remove-Item -LiteralPath $folder -Recurse -Force -ErrorAction SilentlyContinue
}

if ($Show) {
    $demo = New-Reporter @((Known 'editor-save-parse' 6), (Known 'editor-real-overlay' 260), (Known 'editor-real-parts' 720))
    Start-ProgressGroup $demo 'editor-save-parse' 0
    Complete-ProgressGroup $demo 'PASS' 6
    Start-ProgressGroup $demo 'editor-real-overlay' 6
    Complete-ProgressGroup $demo 'PASS' 270
    Start-ProgressGroup $demo 'editor-real-parts' 270
    foreach ($second in 300, 600, 900, 1200) { Write-Host (Format-ProgressLine $demo $second) }
    Complete-ProgressGroup $demo 'PASS' 1210 2022772
    Format-TimingReport $demo 1210 | ForEach-Object { Write-Host $_ }
}

if ($failures) { Write-Host "$failures of $checks progress script checks failed"; exit 1 }
Write-Host "progress script tests: $checks checks, 0 failed"
exit 0
