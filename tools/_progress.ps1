# Progress reporting for long step lists. Formatting and accounting only: callers pass the current time in seconds, so the
# same code runs against a stopwatch or synthetic time. Works in Windows PowerShell 5.1 and PowerShell 7.

$script:ProgressMaxSeconds = 99 * 3600
$script:ProgressChildPrefix = '@@dcp-progress '
$script:ProgressOverrun = 0.15      # a group past its estimate is expected to need this share of its time again
$script:ProgressConfidence = 0.75   # share of the remaining time that must come from history for a plain ETA
$script:ProgressUnknown = 60         # seconds for a group nothing is known about
$script:RegressionFactor = 1.5
$script:RegressionSeconds = 10

function Format-Count([double]$Value) { ([long][Math]::Max(0, [Math]::Floor($Value))).ToString('N0', [cultureinfo]::InvariantCulture) }

function ConvertTo-SafeSeconds($Seconds) {
    if ($null -eq $Seconds) { return 0.0 }
    $value = [double]$Seconds
    if ([double]::IsNaN($value) -or $value -lt 0) { return 0.0 }
    return [Math]::Min($value, [double]$script:ProgressMaxSeconds)
}

function Format-Elapsed($Seconds) {
    $whole = [long][Math]::Floor((ConvertTo-SafeSeconds $Seconds))
    $hours = [Math]::Floor($whole / 3600)
    $minutes = [Math]::Floor(($whole % 3600) / 60)
    $secs = $whole % 60
    if ($hours -gt 0) { return '{0}h {1:00}m {2:00}s' -f $hours, $minutes, $secs }
    if ($minutes -gt 0) { return '{0}m {1:00}s' -f $minutes, $secs }
    return '{0}s' -f $secs
}

function Format-ShortDuration($Seconds) {
    $value = ConvertTo-SafeSeconds $Seconds
    if ($value -lt 60) { return $value.ToString('0.0', [cultureinfo]::InvariantCulture) + 's' }
    return Format-Elapsed $value
}

# Rounded so the estimate does not flicker: 2 s steps under a minute, 30 s under ten minutes, whole minutes above.
function Format-Rounded($Seconds) {
    $value = ConvertTo-SafeSeconds $Seconds
    if ($value -lt 60) { return '{0}s' -f [Math]::Max(2, 2 * [Math]::Round($value / 2, [MidpointRounding]::AwayFromZero)) }
    $step = if ($value -lt 600) { 30 } else { 60 }
    $rounded = $step * [Math]::Round($value / $step, [MidpointRounding]::AwayFromZero)
    $hours = [Math]::Floor($rounded / 3600)
    $minutes = [Math]::Floor(($rounded % 3600) / 60)
    $secs = $rounded % 60
    if ($hours -gt 0) { return '{0}h {1:00}m' -f $hours, $minutes }
    if ($secs -gt 0) { return '{0}m {1:00}s' -f $minutes, $secs }
    return '{0}m' -f $minutes
}

function Format-Eta($Eta) {
    if ($null -eq $Eta -or $null -eq $Eta.Seconds) { return 'calculating' }
    $value = [double]$Eta.Seconds
    if ([double]::IsNaN($value) -or [double]::IsInfinity($value)) { return 'calculating' }
    if ($Eta.Confident) { return Format-Rounded $value }
    return '~{0} (low confidence)' -f (Format-Rounded $value)
}

function Format-Finish([datetime]$Now, $Eta) {
    if ($null -eq $Eta -or $null -eq $Eta.Seconds) { return '' }
    $end = $Now.AddSeconds((ConvertTo-SafeSeconds $Eta.Seconds))
    $days = ($end.Date - $Now.Date).Days
    $text = $end.ToString('HH:mm', [cultureinfo]::InvariantCulture)
    if ($days -gt 0) { $text += ' (+{0}d)' -f $days }
    if (-not $Eta.Confident) { $text = '~' + $text }
    return $text
}

function Format-ChecksText($Current, $Expected, [bool]$Estimate) {
    if ($null -eq $Current) { return '' }
    $text = Format-Count $Current
    if ($null -ne $Expected -and $Expected -gt 0 -and $Current -le $Expected) {
        $mark = if ($Estimate) { '~' } else { '' }
        return '{0} / {1}{2}' -f $text, $mark, (Format-Count $Expected)
    }
    return $text
}

function Get-ProgressPercent([int]$Done, [int]$Total) {
    if ($Total -le 0) { return 0 }
    return [int][Math]::Floor([Math]::Max(0, [Math]::Min($Done, $Total)) * 100.0 / $Total)
}

# A terminal that redraws a line: output not redirected and a console window size available. DCP_TEST_ANIMATE=1 or 0 forces it.
function Test-InteractiveConsole {
    switch ($env:DCP_TEST_ANIMATE) { '1' { return $true } '0' { return $false } }
    if ($Host.Name -in 'Windows PowerShell ISE Host', 'ServerRemoteHost') { return $false }
    try { if ([Console]::IsOutputRedirected) { return $false } } catch { return $false }
    try { return $Host.UI.RawUI.WindowSize.Width -gt 0 } catch { return $false }
}

# One group of a run. Lane: groups of one lane run one after another, lanes run side by side. Phase 1 of the main lane starts
# when its phase 0 and the 'editor' lane are done.
function New-ProgressGroup([string]$Name, [string]$Platform = 'windows', [string]$Lane = 'main', [int]$Phase = 0, $Estimate = $null, [bool]$Known = $false, $Previous = $null) {
    [pscustomobject]@{ Name = $Name; Platform = $Platform; Lane = $Lane; Phase = $Phase; Estimate = $Estimate; Known = $Known; Previous = $Previous
                       State = 'WAITING'; Start = 0.0; Seconds = 0.0; Checks = $null; Expected = $null; Guess = $false; Detail = '' }
}

function New-ProgressReporter($Groups, [double]$Now, [bool]$Interactive, [scriptblock]$Write, [double]$Interval = 0, [datetime]$StartClock = [datetime]::Now) {
    if ($Interval -le 0) { $Interval = if ($Interactive) { 0.5 } else { 30 } }
    $list = @($Groups | ForEach-Object { if ($_ -is [string]) { New-ProgressGroup $_ } else { $_ } })
    [pscustomobject]@{
        Groups = $list
        Total = $list.Count
        Foreground = ''
        RunStart = $Now
        StartClock = $StartClock
        Interactive = $Interactive
        Interval = $Interval
        LastEmit = $Now
        LineDirty = $false
        Tick = 0
        Width = 0
        Write = $Write
    }
}

function Get-ProgressGroup($R, [string]$Name) { foreach ($group in $R.Groups) { if ($group.Name -eq $Name) { return $group } } }
function Get-RunningGroups($R) { return , @($R.Groups | Where-Object { $_.State -eq 'RUNNING' }) }
function Get-FinishedGroups($R) { return , @($R.Groups | Where-Object { $_.State -ne 'RUNNING' -and $_.State -ne 'WAITING' }) }

# The running group the progress line is about: the foreground one, else the one that started first.
function Get-ShownGroup($R) {
    $running = Get-RunningGroups $R
    $shown = @($running | Where-Object { $_.Name -eq $R.Foreground })
    if ($shown.Count) { return $shown[0] }
    return ($running | Sort-Object -Property Start | Select-Object -First 1)
}

# Without an estimate of its own a group is taken for a slow one of those that finished on its platform.
function Get-GroupEstimate($R, $Group) {
    if ($null -ne $Group.Estimate) { return [double]$Group.Estimate }
    $finished = Get-FinishedGroups $R
    $alike = @($finished | Where-Object { $_.State -eq 'PASS' -and $_.Platform -eq $Group.Platform } | ForEach-Object { [double]$_.Seconds } | Sort-Object)
    if ($alike.Count -eq 0) { $alike = @($R.Groups | Where-Object { $_.Known -and $_.Platform -eq $Group.Platform } | ForEach-Object { [double]$_.Estimate } | Sort-Object) }
    if ($alike.Count -eq 0) { return [double]$script:ProgressUnknown }
    return [double]$alike[[int][Math]::Min($alike.Count - 1, [Math]::Floor($alike.Count * 0.75))]
}

function Get-GroupRemaining($R, $Group, [double]$Now) {
    if ($Group.State -ne 'RUNNING' -and $Group.State -ne 'WAITING') { return 0.0 }
    $estimate = Get-GroupEstimate $R $Group
    if ($Group.State -eq 'WAITING') { return $estimate }
    $elapsed = ConvertTo-SafeSeconds ($Now - $Group.Start)
    return [Math]::Max($estimate - $elapsed, $script:ProgressOverrun * $elapsed)
}

function Get-ProgressEta($R, [double]$Now) {
    $lanes = @{}
    $late = 0.0
    $total = 0.0
    $known = 0.0
    foreach ($group in $R.Groups) {
        $left = Get-GroupRemaining $R $group $Now
        $total += $left
        if ($group.Known) { $known += $left }
        if ($group.Lane -eq 'main' -and $group.Phase -gt 0) { $late += $left; continue }
        if (-not $lanes.ContainsKey($group.Lane)) { $lanes[$group.Lane] = 0.0 }
        $lanes[$group.Lane] += $left
    }
    $first = 0.0
    foreach ($lane in 'main', 'editor') { if ($lanes.ContainsKey($lane)) { $first = [Math]::Max($first, $lanes[$lane]) } }
    $seconds = $first + $late
    foreach ($lane in $lanes.Keys) { $seconds = [Math]::Max($seconds, $lanes[$lane]) }
    return [pscustomobject]@{ Seconds = (ConvertTo-SafeSeconds $seconds); Confident = ($total -le 0 -or $known / $total -ge $script:ProgressConfidence) }
}

function Get-ProgressSpinnerFrame([long]$Tick) { '|/-\'[[int](((($Tick % 4) + 4) % 4))] }

function Get-ConsoleWidth {
    try { $width = $Host.UI.RawUI.WindowSize.Width; if ($width -gt 20) { return $width - 1 } } catch { $null = $_ }
    return 160
}

function Get-LineWidth($R) { if ($R.Width -gt 0) { $R.Width } else { Get-ConsoleWidth } }

# [87/95] 91% | editor-real-parts +1 | Group 17m 46s of 18m | Total 1h 42m 18s | ETA 21m | Finish 23:41 | Checks 2,038,817
# Group, Total and ETA always stay; the rest is left out from the end when the line is too narrow.
function Format-ProgressLine($R, [double]$Now, [int]$Width = 0) {
    $done = (Get-FinishedGroups $R).Count
    $group = Get-ShownGroup $R
    $others = (Get-RunningGroups $R).Count - 1
    $eta = Get-ProgressEta $R $Now
    $head = '[{0}/{1}] {2}%' -f $done, $R.Total, (Get-ProgressPercent $done $R.Total)
    $name = if ($group) { $group.Name } elseif ($done -ge $R.Total) { 'complete' } else { 'waiting' }
    $suffix = if ($others -gt 0) { ' +{0}' -f $others } else { '' }
    $elapsed = if ($group) { ConvertTo-SafeSeconds ($Now - $group.Start) } else { 0.0 }
    $groupText = 'Group {0}' -f (Format-Elapsed $elapsed)
    $expected = if ($group -and $group.Known) { ' of {0}' -f (Format-Rounded $group.Estimate) } else { '' }
    $total = 'Total {0} | ETA {1}' -f (Format-Elapsed ($Now - $R.RunStart)), (Format-Eta $eta)
    $finish = Format-Finish $R.StartClock.AddSeconds((ConvertTo-SafeSeconds ($Now - $R.RunStart))) $eta
    $tail = New-Object System.Collections.Generic.List[string]
    if ($finish) { $tail.Add('Finish {0}' -f $finish) }
    $checks = if ($group) { Format-ChecksText $group.Checks $group.Expected $group.Guess } else { '' }
    if ($checks) { $tail.Add('Checks {0}' -f $checks) }
    if ($group -and $group.Detail) { $tail.Add($group.Detail) }

    $fixed = '{0} |  | {1} | {2}' -f $head, $groupText, $total
    if ($Width -le 0) { $Width = [int]::MaxValue }
    $room = $Width - $fixed.Length
    $label = $name + $suffix
    if ($label.Length -gt $room) { $label = if ($room -ge 8) { $label.Substring(0, $room - 3) + '...' } else { '' } }
    $line = if ($label) { '{0} | {1} | {2}' -f $head, $label, $groupText } else { '{0} | {1}' -f $head, $groupText }
    if ($expected -and $line.Length + $expected.Length + 3 + $total.Length -le $Width) { $line += $expected }
    $line += ' | ' + $total
    foreach ($part in $tail) {
        if ($line.Length + 3 + $part.Length -gt $Width) { break }
        $line += ' | ' + $part
    }
    return $line
}

function Clear-ProgressLine($R) {
    if (-not $R.LineDirty) { return }
    & $R.Write ("`r" + (' ' * (Get-LineWidth $R)) + "`r") $true
    $R.LineDirty = $false
}

function Write-ProgressUpdate($R, [double]$Now) {
    $R.LastEmit = $Now
    if ($R.Interactive) {
        $width = Get-LineWidth $R
        $text = '{0} {1}' -f (Get-ProgressSpinnerFrame $R.Tick), (Format-ProgressLine $R $Now ($width - 2))
        & $R.Write ("`r" + $text.PadRight($width)) $true
        $R.LineDirty = $true
        $R.Tick++
        return
    }
    & $R.Write ('RUNNING ' + (Format-ProgressLine $R $Now)) $false
}

function Start-ProgressGroup($R, [string]$Name, [double]$Now, [switch]$Background) {
    $group = Get-ProgressGroup $R $Name
    $group.State = 'RUNNING'
    $group.Start = $Now
    $group.Checks = $null
    $group.Expected = $null
    $group.Detail = ''
    if ($Background) { return }
    $R.Foreground = $Name
    $R.Tick = 0
    & $R.Write (Format-ProgressLine $R $Now) $false
    $R.LastEmit = $Now
}

function Set-ProgressChecks($R, [double]$Now, $Checks, $Expected = $null, [bool]$Estimate = $false, [string]$Detail = '', [string]$Name = '') {
    $group = Get-ProgressGroup $R $(if ($Name) { $Name } else { $R.Foreground })
    if ($group) {
        $group.Checks = $Checks
        if ($null -ne $Expected) { $group.Expected = $Expected; $group.Guess = $Estimate }
        if ($Detail) { $group.Detail = $Detail }
    }
    return Update-Progress $R $Now
}

function Update-Progress($R, [double]$Now, [switch]$Force) {
    if ((Get-RunningGroups $R).Count -eq 0) { return $false }
    if (-not $Force -and ($Now - $R.LastEmit) -lt ($R.Interval - 0.05)) { return $false }
    Write-ProgressUpdate $R $Now
    return $true
}

function Complete-ProgressGroup($R, [string]$State, [double]$Now, $Checks = $null, [string]$Name = '') {
    $group = Get-ProgressGroup $R $(if ($Name) { $Name } else { $R.Foreground })
    if (-not $group -or $group.State -ne 'RUNNING') { return }
    if ($null -ne $Checks) { $group.Checks = $Checks }
    $group.Seconds = ConvertTo-SafeSeconds ($Now - $group.Start)
    $group.State = $State
    $group.Detail = ''
    if ($R.Foreground -eq $group.Name) { $R.Foreground = '' }
}

function Get-SlowestGroups($R, [int]$Count = 10) {
    $finished = Get-FinishedGroups $R
    $ranked = @($finished | Where-Object { $_.State -ne 'SKIPPED' } | Sort-Object -Property @{ Expression = 'Seconds'; Descending = $true }, @{ Expression = 'Name'; Descending = $false })
    return , @($ranked | Select-Object -First $Count)
}

# Groups that passed clearly slower than their own history: diagnostic only.
function Get-Regressions($R) {
    return , @($R.Groups | Where-Object {
            $_.State -eq 'PASS' -and $_.Known -and $_.Seconds -gt $_.Estimate * $script:RegressionFactor -and $_.Seconds - $_.Estimate -ge $script:RegressionSeconds
        })
}

function Format-Change([double]$Seconds, [double]$Previous) {
    $delta = $Seconds - $Previous
    $sign = if ($delta -lt 0) { '-' } else { '+' }
    $percent = if ($Previous -gt 0) { (100.0 * [Math]::Abs($delta) / $Previous).ToString('0.0', [cultureinfo]::InvariantCulture) } else { '0.0' }
    return '{0}{1}, {0}{2}%' -f $sign, (Format-Elapsed ([Math]::Abs($delta))), $percent
}

# Every group in list order, the counts, the slowest groups and the change since the last comparable run.
function Format-TimingReport($R, [double]$Now, [int]$Blocked = 0, $PreviousRun = $null) {
    $total = ConvertTo-SafeSeconds ($Now - $R.RunStart)
    $finished = Get-FinishedGroups $R
    $width = [Math]::Max(20, [int]($R.Groups | ForEach-Object { $_.Name.Length } | Measure-Object -Maximum).Maximum)
    $lines = @('Test timing', '')
    foreach ($group in $finished) {
        $line = ('{0,-' + $width + '} {1,-8} {2,10}') -f $group.Name, $group.State, (Format-ShortDuration $group.Seconds)
        if ($null -ne $group.Checks) { $line += '  {0} checks' -f (Format-Count $group.Checks) }
        $lines += $line
    }
    $lines += ''
    $lines += 'Total execution time: {0}' -f (Format-Elapsed $total)
    if ($finished.Count -lt $R.Total) { $lines += 'Incomplete: {0}/{1} groups finished' -f $finished.Count, $R.Total }
    $count = @{}
    foreach ($state in 'PASS', 'FAIL', 'SKIPPED', 'BLOCKED') { $count[$state] = @($finished | Where-Object { $_.State -eq $state }).Count }
    $lines += 'Groups: {0} PASS, {1} FAIL, {2} SKIPPED, {3} BLOCKED' -f $count['PASS'], $count['FAIL'], $count['SKIPPED'], ($count['BLOCKED'] + $Blocked)
    $slowest = Get-SlowestGroups $R 10
    if ($slowest.Count) {
        $lines += ''
        $overlap = if (@($R.Groups | Where-Object { $_.Lane -ne 'main' }).Count) { ' (groups overlap, so the shares of the total run time add up to more than 100%)' } else { '' }
        $lines += 'Slowest groups' + $overlap
        $rank = 1
        foreach ($group in $slowest) {
            $share = if ($total -gt 0) { (100.0 * $group.Seconds / $total).ToString('0.0', [cultureinfo]::InvariantCulture) + '%' } else { '' }
            $line = ('{0,2}. {1,-' + $width + '} {2,10} {3,7}') -f $rank, $group.Name, (Format-Elapsed $group.Seconds), $share
            if ($null -ne $group.Previous) { $line += '   previous {0}' -f (Format-Elapsed $group.Previous) }
            $lines += $line
            $rank++
        }
    }
    $regressions = Get-Regressions $R
    if ($regressions.Count) {
        $lines += ''
        foreach ($group in $regressions) {
            $lines += 'WARNING: {0} took {1}, usually {2} ({3}); not a failure' -f $group.Name, (Format-Elapsed $group.Seconds), (Format-Elapsed $group.Estimate), (Format-Change $group.Seconds $group.Estimate)
        }
    }
    if ($null -ne $PreviousRun) {
        $lines += ''
        $lines += 'Current run: {0}' -f (Format-Elapsed $total)
        $lines += 'Previous comparable run: {0} ({1})' -f (Format-Elapsed $PreviousRun.seconds), $PreviousRun.at
        $lines += 'Change: {0}' -f (Format-Change $total $PreviousRun.seconds)
    }
    return $lines
}

function ConvertFrom-ChildProgress([string]$Line) {
    if (-not $Line.StartsWith($script:ProgressChildPrefix)) { return $null }
    if ($Line -notmatch '^@@dcp-progress checks=(\d+)(?: total=(\d+))?(?: estimate=([01]))?(?: detail=(.*))?$') { return $null }
    [pscustomobject]@{
        Checks = [long]$Matches[1]
        Expected = if ($Matches[2]) { [long]$Matches[2] } else { $null }
        Estimate = ($Matches[3] -eq '1')
        Detail = "$($Matches[4])".Trim()
    }
}

function Get-FinalCheckCount([string[]]$Lines) {
    $count = $null
    foreach ($line in $Lines) { if ($line -match '\b(\d+) checks\b') { $count = [long]$Matches[1] } }
    return $count
}
