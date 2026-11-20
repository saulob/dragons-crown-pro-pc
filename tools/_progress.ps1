# Progress reporting for long step lists. Formatting and accounting only: callers pass the current time in seconds, so the
# same code runs against a stopwatch or synthetic time. Works in Windows PowerShell 5.1 and PowerShell 7.

$script:ProgressBarWidth = 24
$script:ProgressMaxSeconds = 99 * 3600
$script:ProgressChildPrefix = '@@dcp-progress '

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
    return '{0}m {1:00}s' -f $minutes, $secs
}

function Format-ShortDuration($Seconds) {
    $value = ConvertTo-SafeSeconds $Seconds
    if ($value -lt 60) { return $value.ToString('0.0', [cultureinfo]::InvariantCulture) + 's' }
    return Format-Elapsed $value
}

function Format-Eta($Seconds) {
    if ($null -eq $Seconds) { return 'calculating' }
    $value = [double]$Seconds
    if ([double]::IsNaN($value) -or [double]::IsInfinity($value)) { return 'calculating' }
    $value = ConvertTo-SafeSeconds $value
    if ($value -lt 60) { return '~{0}s' -f [Math]::Max(2, 2 * [Math]::Round($value / 2, [MidpointRounding]::AwayFromZero)) }
    $step = if ($value -lt 600) { 30 } else { 60 }
    $rounded = $step * [Math]::Round($value / $step, [MidpointRounding]::AwayFromZero)
    $hours = [Math]::Floor($rounded / 3600)
    $minutes = [Math]::Floor(($rounded % 3600) / 60)
    $secs = $rounded % 60
    if ($hours -gt 0) { return '~{0}h {1:00}m' -f $hours, $minutes }
    if ($secs -gt 0) { return '~{0}m {1:00}s' -f $minutes, $secs }
    return '~{0}m' -f $minutes
}

function Format-ChecksText($Current, $Expected, [bool]$Estimate) {
    if ($null -eq $Current) { return 'unavailable' }
    $text = Format-Count $Current
    if ($null -ne $Expected -and $Expected -gt 0 -and $Current -le $Expected) {
        $mark = if ($Estimate) { '~' } else { '' }
        return '{0} / {1}{2}' -f $text, $mark, (Format-Count $Expected)
    }
    return $text
}

function Get-ProgressPercent([int]$Done, [int]$Total, [double]$GroupFraction = 0, [bool]$Running = $false) {
    if ($Total -le 0) { return 0 }
    $finished = [Math]::Max(0, [Math]::Min($Done, $Total))
    $partial = 0.0
    if ($Running -and $finished -lt $Total -and -not [double]::IsNaN($GroupFraction)) { $partial = [Math]::Max(0.0, [Math]::Min($GroupFraction, 0.99)) }
    $percent = [int][Math]::Floor(($finished + $partial) * 100.0 / $Total)
    if ($finished -lt $Total) { $percent = [Math]::Min($percent, 99) }
    return [Math]::Max(0, [Math]::Min($percent, 100))
}

function Format-ProgressBar([int]$Percent, [int]$Width = $script:ProgressBarWidth) {
    $clamped = [Math]::Max(0, [Math]::Min($Percent, 100))
    $filled = [int][Math]::Floor($clamped * $Width / 100)
    return '[' + ('#' * $filled) + ('-' * ($Width - $filled)) + ']'
}

# A terminal that redraws a line: output not redirected and a console window size available. DCP_TEST_ANIMATE=1 or 0 forces it.
function Test-InteractiveConsole {
    switch ($env:DCP_TEST_ANIMATE) { '1' { return $true } '0' { return $false } }
    if ($Host.Name -in 'Windows PowerShell ISE Host', 'ServerRemoteHost') { return $false }
    try { if ([Console]::IsOutputRedirected) { return $false } } catch { return $false }
    try { return $Host.UI.RawUI.WindowSize.Width -gt 0 } catch { return $false }
}

function New-ProgressReporter([string[]]$Names, [double]$Now, [bool]$Interactive, [scriptblock]$Write, [double]$Interval = 0) {
    if ($Interval -le 0) { $Interval = if ($Interactive) { 0.5 } else { 15 } }
    [pscustomobject]@{
        Names = @($Names)
        Total = @($Names).Count
        Done = 0
        Running = $false
        Name = ''
        RunStart = $Now
        GroupStart = $Now
        Checks = $null
        Expected = $null
        Estimate = $false
        Detail = ''
        Results = New-Object System.Collections.Generic.List[object]
        Interactive = $Interactive
        Interval = $Interval
        LastEmit = $Now
        LineDirty = $false
        Tick = 0
        Width = 0
        EtaShown = $null
        EtaShownAt = $Now
        Write = $Write
    }
}

function Get-GroupFraction($R) {
    if (-not $R.Running -or $null -eq $R.Checks -or $null -eq $R.Expected -or $R.Expected -le 0) { return 0.0 }
    return [Math]::Max(0.0, [Math]::Min([double]$R.Checks / [double]$R.Expected, 0.99))
}

function Get-ProgressEta($R, [double]$Now) {
    $timed = @($R.Results | Where-Object { $_.State -ne 'SKIPPED' })
    if ($timed.Count -eq 0 -or $R.Total -le 0) { return $null }
    $average = ($timed | Measure-Object -Property Seconds -Average).Average
    $remaining = $R.Total - $R.Done
    $current = 0.0
    if ($R.Running) {
        $remaining--
        $elapsed = ConvertTo-SafeSeconds ($Now - $R.GroupStart)
        $fraction = Get-GroupFraction $R
        $current = if ($fraction -ge 0.02) { $elapsed * (1 - $fraction) / $fraction } else { [Math]::Max(0.0, $average - $elapsed) }
    }
    $sinceShown = $Now - $R.EtaShownAt
    if ($null -ne $R.EtaShown -and $sinceShown -ge 0 -and $sinceShown -lt 1) { return $R.EtaShown }
    $raw = ConvertTo-SafeSeconds ($current + $average * [Math]::Max(0, $remaining))
    if ($null -eq $R.EtaShown) { $smoothed = $raw }
    else { $smoothed = 0.7 * $raw + 0.3 * [Math]::Max(0.0, $R.EtaShown - ($Now - $R.EtaShownAt)) }
    $R.EtaShown = ConvertTo-SafeSeconds $smoothed
    $R.EtaShownAt = $Now
    return $R.EtaShown
}

function Get-PreviousText($R) {
    if ($R.Results.Count -eq 0) { return 'none' }
    $last = $R.Results[$R.Results.Count - 1]
    return '{0} {1}  {2}' -f $last.Name, $last.State, (Format-Elapsed $last.Seconds)
}

function Format-ProgressBlock($R, [double]$Now) {
    $percent = Get-ProgressPercent $R.Done $R.Total (Get-GroupFraction $R) $R.Running
    $head = '{0} {1}% | {2}/{3} groups' -f (Format-ProgressBar $percent), $percent, $R.Done, $R.Total
    $name = if ($R.Running) { $R.Name } elseif ($R.Done -ge $R.Total) { 'complete' } else { 'waiting' }
    $times = 'Elapsed: {0}' -f (Format-Elapsed ($Now - $R.RunStart))
    if ($R.Running) { $times += ' | Group: {0}' -f (Format-Elapsed ($Now - $R.GroupStart)) }
    $times += ' | ETA: {0}' -f (Format-Eta (Get-ProgressEta $R $Now))
    $lines = @($head, $name, $times)
    if ($R.Running) { $lines += 'Checks: {0}' -f (Format-ChecksText $R.Checks $R.Expected $R.Estimate) }
    $lines += 'Previous: {0}' -f (Get-PreviousText $R)
    return $lines
}

function Get-ProgressSpinnerFrame([long]$Tick) { '|/-\'[[int](((($Tick % 4) + 4) % 4))] }

function Format-HeartbeatDetail($R, [double]$Now) {
    $text = 'group {0} | ETA {1} | checks {2}' -f (Format-Elapsed ($Now - $R.GroupStart)), (Format-Eta (Get-ProgressEta $R $Now)), (Format-ChecksText $R.Checks $R.Expected $R.Estimate)
    if ($R.Detail) { $text += ' | ' + $R.Detail }
    return $text
}

function Format-ProgressLine($R, [double]$Now, [int]$Width) {
    $percent = Get-ProgressPercent $R.Done $R.Total (Get-GroupFraction $R) $R.Running
    $spinner = Get-ProgressSpinnerFrame $R.Tick
    $detail = Format-HeartbeatDetail $R $Now
    $line = '[{0}] {1} {2}% | {3}/{4} groups | {5} | {6}' -f $spinner, (Format-ProgressBar $percent), $percent, $R.Done, $R.Total, $R.Name, $detail
    if ($Width -le 8 -or $line.Length -le $Width) { return $line }
    $compact = '[{0}] {1}% {2}/{3} {4} | {5}' -f $spinner, $percent, $R.Done, $R.Total, '{0}', $detail
    $room = $Width - ($compact -f '').Length
    $name = if ($R.Name.Length -le $room) { $R.Name } else { $R.Name.Substring(0, [Math]::Max(1, $room - 3)) + '...' }
    $line = $compact -f $name
    if ($line.Length -gt $Width) { $line = $line.Substring(0, $Width - 3) + '...' }
    return $line
}

function Get-ConsoleWidth {
    try { $width = $Host.UI.RawUI.WindowSize.Width; if ($width -gt 20) { return $width - 1 } } catch { $null = $_ }
    return 160
}

function Get-LineWidth($R) { if ($R.Width -gt 0) { $R.Width } else { Get-ConsoleWidth } }

function Clear-ProgressLine($R) {
    if (-not $R.LineDirty) { return }
    & $R.Write ("`r" + (' ' * (Get-LineWidth $R)) + "`r") $true
    $R.LineDirty = $false
}

function Write-ProgressUpdate($R, [double]$Now) {
    $R.LastEmit = $Now
    if ($R.Interactive) {
        & $R.Write ("`r" + (Format-ProgressLine $R $Now (Get-LineWidth $R)).PadRight((Get-LineWidth $R))) $true
        $R.LineDirty = $true
        $R.Tick++
        return
    }
    & $R.Write ('RUNNING {0} | {1}' -f $R.Name, (Format-HeartbeatDetail $R $Now)) $false
}

function Start-ProgressGroup($R, [string]$Name, [double]$Now) {
    $R.Running = $true
    $R.Name = $Name
    $R.GroupStart = $Now
    $R.Tick = 0
    $R.EtaShown = $null
    $R.Checks = $null
    $R.Expected = $null
    $R.Estimate = $false
    $R.Detail = ''
    foreach ($line in (Format-ProgressBlock $R $Now)) { & $R.Write $line $false }
    $R.LastEmit = $Now
}

function Set-ProgressChecks($R, [double]$Now, $Checks, $Expected = $null, [bool]$Estimate = $false, [string]$Detail = '') {
    $R.Checks = $Checks
    if ($null -ne $Expected) { $R.Expected = $Expected; $R.Estimate = $Estimate }
    if ($Detail) { $R.Detail = $Detail }
    return Update-Progress $R $Now
}

function Update-Progress($R, [double]$Now, [switch]$Force) {
    if (-not $R.Running) { return $false }
    if (-not $Force -and ($Now - $R.LastEmit) -lt ($R.Interval - 0.05)) { return $false }
    Write-ProgressUpdate $R $Now
    return $true
}

function Complete-ProgressGroup($R, [string]$State, [double]$Now, $Checks = $null) {
    if (-not $R.Running) { return }
    if ($null -ne $Checks) { $R.Checks = $Checks }
    $R.Results.Add([pscustomobject]@{ Name = $R.Name; State = $State; Seconds = (ConvertTo-SafeSeconds ($Now - $R.GroupStart)); Checks = $R.Checks })
    $R.Done = [Math]::Min($R.Done + 1, $R.Total)
    $R.Running = $false
    $R.Checks = $null
    $R.Expected = $null
    $R.Detail = ''
}

function Get-SlowestGroups($R, [int]$Count = 3) {
    $ranked = @($R.Results | Where-Object { $_.State -ne 'SKIPPED' } | Sort-Object -Property @{ Expression = 'Seconds'; Descending = $true }, @{ Expression = 'Name'; Descending = $false })
    return @($ranked | Select-Object -First $Count)
}

function Format-TimingSummary($R, [double]$Now) {
    $lines = @('Test timing', '')
    foreach ($result in $R.Results) {
        $line = '{0,-36} {1,-8} {2,8}' -f $result.Name, $result.State, (Format-ShortDuration $result.Seconds)
        if ($null -ne $result.Checks) { $line += '  {0} checks' -f (Format-Count $result.Checks) }
        $lines += $line
    }
    $lines += ''
    $lines += '{0,-36} {1,-8} {2,8}' -f 'Total', '', (Format-Elapsed ($Now - $R.RunStart))
    if ($R.Done -lt $R.Total) { $lines += 'Incomplete: {0}/{1} groups finished' -f $R.Done, $R.Total }
    $slowest = @(Get-SlowestGroups $R 3)
    if ($slowest.Count -ge 3) {
        $lines += ''
        $lines += 'Slowest:'
        $rank = 1
        foreach ($result in $slowest) {
            $lines += '{0}. {1,-32} {2}' -f $rank, $result.Name, (Format-Elapsed $result.Seconds)
            $rank++
        }
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
