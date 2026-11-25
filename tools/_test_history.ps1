# Local test durations for tools\test.ps1, kept in build\test-history\timings.json. Only the ETA and the timing report read
# them: every function here degrades to "no history" instead of failing. Works in Windows PowerShell 5.1 and PowerShell 7.

$script:HistorySchema = 1
$script:HistorySampleLimit = 8

function Get-TimingKey([string]$Platform, [string]$Mode, [string]$Group) { '{0}|{1}|{2}' -f $Platform, $Mode, $Group }

function Get-Field($Object, [string]$Name) {
    if ($null -eq $Object) { return $null }
    $property = $Object.PSObject.Properties[$Name]
    if ($property) { return , $property.Value }
    return $null
}

function ConvertTo-Seconds($Value) {
    if ($null -eq $Value -or $Value -is [bool]) { return $null }
    $seconds = 0.0
    $style = [Globalization.NumberStyles]::Float
    if (-not [double]::TryParse([string]::Format([cultureinfo]::InvariantCulture, '{0}', $Value), $style, [cultureinfo]::InvariantCulture, [ref]$seconds)) { return $null }
    if ([double]::IsNaN($seconds) -or [double]::IsInfinity($seconds) -or $seconds -le 0) { return $null }
    return $seconds
}

function ConvertTo-HistorySamples($Values) {
    $samples = New-Object System.Collections.Generic.List[object]
    foreach ($value in @($Values)) {
        $seconds = ConvertTo-Seconds (Get-Field $value 'seconds')
        if ($null -eq $seconds) { continue }
        $at = Get-Field $value 'at'
        if ($at -is [datetime]) { $at = $at.ToString('s', [cultureinfo]::InvariantCulture) }
        $sample = [ordered]@{ seconds = [Math]::Round($seconds, 2); at = "$at" }
        foreach ($name in 'checks', 'groups') {
            $number = ConvertTo-Seconds (Get-Field $value $name)
            if ($null -ne $number) { $sample[$name] = [long]$number }
        }
        $samples.Add([pscustomobject]$sample)
    }
    while ($samples.Count -gt $script:HistorySampleLimit) { $samples.RemoveAt(0) }
    return , $samples
}

function ConvertTo-HistoryTable($Object) {
    $table = @{}
    if ($Object -is [System.Management.Automation.PSCustomObject]) {
        foreach ($property in $Object.PSObject.Properties) {
            $samples = ConvertTo-HistorySamples $property.Value
            if ($samples.Count) { $table[$property.Name] = $samples }
        }
    }
    return $table
}

function Read-HistoryText([string]$Text) {
    $result = [pscustomobject]@{ Groups = @{}; Runs = @{}; Writable = $true; Problem = '' }
    if ([string]::IsNullOrWhiteSpace($Text)) { $result.Problem = 'empty'; return $result }
    try { $json = $Text | ConvertFrom-Json } catch { $result.Problem = 'not readable as JSON'; return $result }
    if ($json -isnot [System.Management.Automation.PSCustomObject]) { $result.Problem = 'not a JSON object'; return $result }
    $schema = ConvertTo-Seconds (Get-Field $json 'schema')
    if ($null -eq $schema -or $schema -lt $script:HistorySchema) { $result.Problem = 'an older schema'; return $result }
    if ($schema -gt $script:HistorySchema) { $result.Problem = 'a newer schema'; $result.Writable = $false; return $result }
    $result.Groups = ConvertTo-HistoryTable (Get-Field $json 'groups')
    $result.Runs = ConvertTo-HistoryTable (Get-Field $json 'runs')
    return $result
}

function Read-HistoryFile([string]$Path) {
    try {
        if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return [pscustomobject]@{ Groups = @{}; Runs = @{}; Writable = $true; Problem = '' } }
        return Read-HistoryText ([IO.File]::ReadAllText($Path))
    } catch {
        return [pscustomobject]@{ Groups = @{}; Runs = @{}; Writable = $false; Problem = "not readable: $($_.Exception.Message)" }
    }
}

function New-TestHistory([string]$Path) {
    $file = Read-HistoryFile $Path
    [pscustomobject]@{ Path = $Path; Groups = $file.Groups; Runs = $file.Runs; Writable = $file.Writable; Problem = $file.Problem; SaveProblem = ''
                       NewGroups = (New-Object System.Collections.Generic.List[object]); NewRuns = (New-Object System.Collections.Generic.List[object]) }
}

# Only a group that ran to its end and passed is a duration worth remembering.
function Add-TimingSample($History, [string]$Key, [string]$State, $Seconds, $Checks = $null, [datetime]$At = [datetime]::Now) {
    $value = ConvertTo-Seconds $Seconds
    if ($State -ne 'PASS' -or $null -eq $value) { return $false }
    $sample = [ordered]@{ seconds = [Math]::Round($value, 2); at = $At.ToString('s', [cultureinfo]::InvariantCulture) }
    if ($null -ne $Checks) { $sample['checks'] = [long]$Checks }
    $History.NewGroups.Add([pscustomobject]@{ Key = $Key; Sample = [pscustomobject]$sample })
    return $true
}

function Add-RunSample($History, [string]$Key, [bool]$Complete, $Seconds, [int]$Groups, [datetime]$At = [datetime]::Now) {
    $value = ConvertTo-Seconds $Seconds
    if (-not $Complete -or $null -eq $value) { return $false }
    $sample = [pscustomobject][ordered]@{ seconds = [Math]::Round($value, 2); at = $At.ToString('s', [cultureinfo]::InvariantCulture); groups = $Groups }
    $History.NewRuns.Add([pscustomobject]@{ Key = $Key; Sample = $sample })
    return $true
}

function Merge-HistorySamples([hashtable]$Table, $New) {
    foreach ($entry in $New) {
        $values = New-Object System.Collections.Generic.List[object]
        if ($Table.ContainsKey($entry.Key)) { foreach ($sample in $Table[$entry.Key]) { $values.Add($sample) } }
        $values.Add($entry.Sample)
        $Table[$entry.Key] = ConvertTo-HistorySamples $values.ToArray()
    }
}

function ConvertTo-HistoryJson([hashtable]$Groups, [hashtable]$Runs) {
    $body = [ordered]@{ schema = $script:HistorySchema; groups = [ordered]@{}; runs = [ordered]@{} }
    foreach ($key in @($Groups.Keys | Sort-Object)) { $body.groups[$key] = $Groups[$key].ToArray() }
    foreach ($key in @($Runs.Keys | Sort-Object)) { $body.runs[$key] = $Runs[$key].ToArray() }
    return ($body | ConvertTo-Json -Depth 6) + "`n"
}

function Write-AtomicText([string]$Path, [string]$Text) {
    $folder = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $folder)) { New-Item -ItemType Directory -Force -Path $folder | Out-Null }
    $partial = "$Path.$PID.tmp"
    try {
        [IO.File]::WriteAllText($partial, $Text, (New-Object System.Text.UTF8Encoding($false)))
        if (Test-Path -LiteralPath $Path) { [IO.File]::Replace($partial, $Path, [NullString]::Value) } else { [IO.File]::Move($partial, $Path) }
    } finally {
        if (Test-Path -LiteralPath $partial) { Remove-Item -LiteralPath $partial -Force -ErrorAction SilentlyContinue }
    }
}

# Adds this run's samples to what the file holds now, so two runs never erase each other's. False when nothing was written.
function Save-TestHistory($History) {
    if ($History.NewGroups.Count -eq 0 -and $History.NewRuns.Count -eq 0) { return $false }
    try {
        $file = Read-HistoryFile $History.Path
        if (-not $file.Writable) { return $false }
        Merge-HistorySamples $file.Groups $History.NewGroups
        Merge-HistorySamples $file.Runs $History.NewRuns
        Write-AtomicText $History.Path (ConvertTo-HistoryJson $file.Groups $file.Runs)
        $History.NewGroups.Clear()
        $History.NewRuns.Clear()
        return $true
    } catch {
        $History.SaveProblem = $_.Exception.Message
        return $false
    }
}

function Get-Median([double[]]$Values) {
    $sorted = @($Values | Sort-Object)
    if ($sorted.Count -eq 0) { return $null }
    $middle = [int][Math]::Floor($sorted.Count / 2)
    if ($sorted.Count % 2) { return [double]$sorted[$middle] }
    return ([double]$sorted[$middle - 1] + [double]$sorted[$middle]) / 2
}

function Get-HistoryDurations($History, [string]$Key) {
    if (-not $History.Groups.ContainsKey($Key)) { return @() }
    return @($History.Groups[$Key] | ForEach-Object { [double]$_.seconds })
}

# What a group should take: the median of its own recent runs in this mode, else in another mode on the same platform, else
# the first-run guess its step declares. Known is true only for the first.
function Get-DurationEstimate($History, [string]$Platform, [string]$Mode, [string]$Group, $Nominal = $null) {
    $own = @(Get-HistoryDurations $History (Get-TimingKey $Platform $Mode $Group))
    if ($own.Count) { return [pscustomobject]@{ Seconds = (Get-Median $own); Known = $true; Previous = $own[$own.Count - 1] } }
    $other = New-Object System.Collections.Generic.List[double]
    foreach ($key in $History.Groups.Keys) {
        $parts = $key.Split('|', 3)
        if ($parts.Count -eq 3 -and $parts[0] -eq $Platform -and $parts[2] -eq $Group) { foreach ($value in (Get-HistoryDurations $History $key)) { $other.Add($value) } }
    }
    if ($other.Count) { return [pscustomobject]@{ Seconds = (Get-Median $other.ToArray()); Known = $false; Previous = $null } }
    $guess = ConvertTo-Seconds $Nominal
    return [pscustomobject]@{ Seconds = $guess; Known = $false; Previous = $null }
}

function Get-PreviousRun($History, [string]$Key) {
    if (-not $History.Runs.ContainsKey($Key)) { return $null }
    $samples = $History.Runs[$Key]
    return $samples[$samples.Count - 1]
}
