# Checks of tools\_progress.ps1 on synthetic time: nothing sleeps and no process is started. -Show prints a synthetic long step.
param([switch]$Show)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot '..\tools\_progress.ps1')
$failures = 0
$checks = 0

function Equal($actual, $expected, [string]$what) {
    $script:checks++
    if ("$actual" -ne "$expected") { $script:failures++; Write-Host "FAIL $what`n  expected: $expected`n  actual:   $actual" }
}

function Check([bool]$condition, [string]$what) {
    $script:checks++
    if (-not $condition) { $script:failures++; Write-Host "FAIL $what" }
}

function New-Reporter([string[]]$Names, [bool]$Interactive = $false, [double]$Interval = 0, $Sink = $null) {
    if ($null -eq $Sink) { $Sink = New-Object System.Collections.Generic.List[string] }
    $writer = { param([string]$Text, [bool]$NoNewline) $Sink.Add($Text) }.GetNewClosure()
    New-ProgressReporter $Names 0 $Interactive $writer $Interval
}

Equal (Get-ProgressPercent 9 14) 64 'percent of 9/14'
Equal (Get-ProgressPercent 3 14 0.5 $true) 25 'a running group counts partially'
Equal (Get-ProgressPercent 13 14 0.99 $true) 99 'the last running group never reaches 100'
Equal (Get-ProgressPercent 14 14) 100 'complete'
Equal (Get-ProgressPercent 0 0) 0 'no groups: no divide by zero'
Equal (Get-ProgressPercent 30 10) 100 'done above total is capped'
Equal (Format-ProgressBar 0) '[------------------------]' 'bar 0'
Equal (Format-ProgressBar 63) '[###############---------]' 'bar 63'
Equal (Format-ProgressBar 100) '[########################]' 'bar 100'
Equal (Format-ProgressBar 400) '[########################]' 'bar above 100'
Equal (Format-Elapsed 3) '0m 03s' 'elapsed seconds'
Equal (Format-Elapsed 1902) '31m 42s' 'elapsed minutes'
Equal (Format-Elapsed 7384) '2h 03m 04s' 'elapsed hours'
Equal (Format-Elapsed -5) '0m 00s' 'negative elapsed'
Equal (Format-ShortDuration 2.1) '2.1s' 'short duration'
Equal (Format-Eta $null) 'calculating' 'no ETA'
Equal (Format-Eta ([double]::NaN)) 'calculating' 'NaN ETA'
Equal (Format-Eta 270) '~4m 30s' 'ETA minutes and seconds'
Equal (Format-Eta 1080) '~18m' 'ETA whole minutes'
Equal (Format-Eta -10) '~2s' 'a negative ETA is never shown'
Equal (Format-ChecksText 1284220 2039231 $true) '1,284,220 / ~2,039,231' 'estimated total'
Equal (Format-ChecksText 1284220 2039231 $false) '1,284,220 / 2,039,231' 'exact total'
Equal (Format-ChecksText 12845 $null $false) '12,845' 'unknown total'
Equal (Format-ChecksText $null $null $false) 'unavailable' 'no count'

$child = ConvertFrom-ChildProgress '@@dcp-progress checks=1284220 total=2039231 estimate=1 detail=editor-real-parts 13/14 | step 4/19 Appearance preparation'
Check ($child.Checks -eq 1284220 -and $child.Expected -eq 2039231 -and $child.Estimate -and $child.Detail -eq 'editor-real-parts 13/14 | step 4/19 Appearance preparation') 'child progress line'
Check ($null -eq (ConvertFrom-ChildProgress 'GROUP|editor-real-parts|PASS|checks=5|ms=9')) 'a GROUP line is not progress'
Check ($null -eq (ConvertFrom-ChildProgress '@@dcp-progress checks=abc')) 'a malformed progress line is ignored'
Equal (Get-FinalCheckCount @('x', '13666 checks, 0 failed')) 13666 'final check count'

$savedAnimate = $env:DCP_TEST_ANIMATE
$env:DCP_TEST_ANIMATE = '1'
Check (Test-InteractiveConsole) 'DCP_TEST_ANIMATE=1 forces the animated line'
$env:DCP_TEST_ANIMATE = '0'
Check (-not (Test-InteractiveConsole)) 'DCP_TEST_ANIMATE=0 forces plain lines'
if ($null -eq $savedAnimate) { Remove-Item Env:\DCP_TEST_ANIMATE } else { $env:DCP_TEST_ANIMATE = $savedAnimate }

$redirected = New-Object System.Collections.Generic.List[string]
$r = New-Reporter @('a', 'b', 'c') $false 15 $redirected
Start-ProgressGroup $r 'a' 0
Equal $redirected.Count 5 'the block is five lines'
Check ($redirected[0] -eq '[------------------------] 0% | 0/3 groups' -and $redirected[1] -eq 'a' -and $redirected[4] -eq 'Previous: none') 'block layout'
Check (-not (Update-Progress $r 10)) 'redirected output is quiet inside the interval'
Check (Set-ProgressChecks $r 16 500 1000 $true 'editor-real-parts 13/14') 'redirected output reports after the interval'
Check ($redirected[$redirected.Count - 1] -eq 'RUNNING a | group 0m 16s | ETA calculating | checks 500 / ~1,000 | editor-real-parts 13/14') 'redirected line'
Check (($redirected -join '').IndexOf("`r") -lt 0 -and ($redirected -join '').IndexOf([char]27) -lt 0) 'redirected output has no carriage return or escape'
Complete-ProgressGroup $r 'PASS' 20 500
Start-ProgressGroup $r 'b' 20
Check ($redirected[$redirected.Count - 1] -eq 'Previous: a PASS  0m 20s') 'the previous result is shown'

$live = New-Object System.Collections.Generic.List[string]
$i = New-Reporter @('a', 'b') $true 0 $live
Start-ProgressGroup $i 'a' 0
$null = Update-Progress $i 0.5
$null = Update-Progress $i 1.0
$frames = @($live | Where-Object { $_.StartsWith("`r") })
Equal $frames.Count 2 'interactive mode redraws one line'
Check ($frames[0].Contains('[|]') -and $frames[1].Contains('[/]')) 'the spinner advances'
Clear-ProgressLine $i
Check ($live[$live.Count - 1].StartsWith("`r") -and $live[$live.Count - 1].Trim() -eq '') 'the line is cleared when the step ends'

$timing = New-Reporter @('fast', 'slow', 'mid', 'none')
foreach ($row in @(@('fast', 'PASS', 0, 2), @('slow', 'FAIL', 2, 1700), @('mid', 'PASS', 1700, 1830))) {
    Start-ProgressGroup $timing $row[0] $row[2]
    Complete-ProgressGroup $timing $row[1] $row[3] 10
}
$summary = @(Format-TimingSummary $timing 1830)
Check ($summary[0] -eq 'Test timing' -and ($summary -join "`n").Contains('Incomplete: 3/4 groups finished') -and ($summary -join "`n").Contains('1. slow')) 'timing summary'
Check ($null -ne (Get-ProgressEta $timing 1830)) 'ETA from finished groups'

if ($Show) {
    $demo = New-Reporter @('editor-save-parse', 'editor-real-overlay', 'dcp-save-editor-tests release') $false 300
    Start-ProgressGroup $demo 'dcp-save-editor-tests release' 0
    foreach ($second in 300, 600, 900) {
        $null = Set-ProgressChecks $demo $second ($second * 1200) 1000000 $true 'editor-real-parts 13/14 | step 4/19 Appearance preparation'
    }
}

if ($failures) { Write-Host "$failures of $checks progress script checks failed"; exit 1 }
Write-Host "progress script tests: $checks checks, 0 failed"
exit 0
