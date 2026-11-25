# Runs the steps of tools\test.ps1: processes, the lanes that run beside the main list, and their results. The caller owns
# $captured, $skipped, $failed, $parityFailed, $exitCodes, $buildDir, $mode, $script:Clock, $script:Progress and
# $script:History. Works in Windows PowerShell 5.1 and PowerShell 7.

# Lane: groups of one lane run one after another; under -Full the lanes run side by side. Timing: measures real time (loopback
# timeouts, pacing), so it waits for the Character Editor lane. Final: writes what
# other groups read, so it waits for every lane. Nominal: seconds to expect before there is any history.
function Test-Step([string]$Title, [string]$Exe = '', [string]$Script = '', [switch]$Parity, [switch]$Capture,
                   [switch]$Quiet, [switch]$Lobby, [scriptblock]$Block = $null, [scriptblock]$Command = $null, [string]$Key = '', [string[]]$Arguments = @(),
                   [string[]]$Requires = @(), [string[]]$Forbids = @(), [string]$Lane = 'main', [switch]$Timing, [switch]$Final, $Nominal = $null) {
    [pscustomobject]@{ Title = $Title; Exe = $Exe; Script = $Script; Parity = [bool]$Parity; Capture = [bool]$Capture
                       Quiet = [bool]$Quiet; Lobby = [bool]$Lobby; Block = $Block; Command = $Command; Key = $Key; Arguments = $Arguments
                       Requires = $Requires; Forbids = $Forbids; Lane = $Lane; Timing = [bool]$Timing; Final = [bool]$Final; Nominal = $Nominal
                       Platform = $(if ($Key -like 'Linux *') { 'linux' } else { 'windows' }) }
}

function Format-ExitCode([long]$Code) {
    'exit code {0}, 0x{1:X8}' -f $Code, [uint32]($Code -band 0xFFFFFFFFL)
}

function Get-TestName($Test) { if ($Test.Key) { $Test.Key } elseif ($Test.Exe) { $Test.Exe } elseif ($Test.Script) { $Test.Script } else { $Test.Title } }

function Skipped([string]$Reason) { [pscustomobject]@{ Code = 0; Skip = $Reason; Lines = @() } }

# A log line the player's report depends on, missing or present where it must not be.
function Get-LogProblems($Test, [string[]]$Lines) {
    foreach ($text in $Test.Requires) {
        if (-not ($Lines | Where-Object { $_.Contains($text) })) { "missing log line: $text" }
    }
    foreach ($text in $Test.Forbids) {
        if ($Lines | Where-Object { $_.Contains($text) }) { "forbidden log line: $text" }
    }
}

$quietLines = '^(GROUP\||MATRIX\||APPEARANCE_TRANSLATION\||runtime overlay tests:|appearance online tests:|ONLINE-BUILD\||LINUX-NET\||FAIL|OK |FAILED|.*STATIC PARITY|.*RUNTIME DATA|.*skipped|# (tests|pass|fail|skipped))'
function ConvertTo-ArgumentString([string[]]$Arguments) {
    ($Arguments | ForEach-Object { if ($_ -match '[\s"]' -or $_ -eq '') { '"' + ($_ -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"' } else { $_ } }) -join ' '
}

$script:LiveNative = New-Object System.Collections.Generic.List[object]

# Starts a program with stdout and stderr merged. Its visible lines (with Quiet only result lines and a timing table; the rest
# is kept in Hidden for a failure report) are written as they arrive, or kept in Shown for a Buffered program.
# "@@dcp-progress" lines from instrumented suites feed the check count of Group and are not part of Lines.
function Start-Native([string]$File, [string[]]$Arguments = @(), [bool]$Quiet = $false, [string]$WorkingDirectory = '', [string]$Group = '', [bool]$Buffered = $false) {
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $File
    if ($WorkingDirectory) { $info.WorkingDirectory = $WorkingDirectory }
    $info.Arguments = ConvertTo-ArgumentString $Arguments
    $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.CreateNoWindow = $true
    $info.EnvironmentVariables['DCP_TEST_PROGRESS'] = '1'
    $job = [pscustomobject]@{ Process = $null; Readers = @(); Pending = @($null, $null); InTable = $false; Quiet = $Quiet; Buffered = $Buffered; Group = $Group
                              Lines = (New-Object System.Collections.Generic.List[string]); Hidden = (New-Object System.Collections.Generic.List[string])
                              Shown = (New-Object System.Collections.Generic.List[string]) }
    try { $job.Process = [System.Diagnostics.Process]::Start($info) }
    catch {
        $job.Lines.Add("could not start ${File}: $($_.Exception.Message)")
        $job.Hidden.Add($job.Lines[0])
        return $job
    }
    $job.Readers = @($job.Process.StandardOutput, $job.Process.StandardError)
    $job.Pending = @($job.Readers[0].ReadLineAsync(), $job.Readers[1].ReadLineAsync())
    $script:LiveNative.Add($job)
    return $job
}

function Add-NativeLine($Job, [string]$Line) {
    $child = ConvertFrom-ChildProgress $Line
    if ($child) {
        $null = Set-ProgressChecks $script:Progress $script:Clock.Elapsed.TotalSeconds $child.Checks $child.Expected $child.Estimate $child.Detail $Job.Group
        return
    }
    $Job.Lines.Add($Line)
    if ($Line -cmatch '^.+ timing$') { $Job.InTable = $true } elseif ($Job.InTable -and -not $Line.StartsWith('  ')) { $Job.InTable = $false }
    if ($Job.Quiet -and -not $Job.InTable -and $Line -cnotmatch $quietLines) { $Job.Hidden.Add($Line); return }
    if ($Job.Buffered) { $Job.Shown.Add($Line); return }
    Clear-ProgressLine $script:Progress
    Write-Host $Line
}

# Takes the lines that have arrived; false once both streams have ended.
function Read-Native($Job) {
    foreach ($slot in 0, 1) {
        for ($count = 0; $count -lt 500 -and $Job.Pending[$slot] -and $Job.Pending[$slot].IsCompleted; $count++) {
            $line = $Job.Pending[$slot].Result
            if ($null -eq $line) { $Job.Pending[$slot] = $null; break }
            $Job.Pending[$slot] = $Job.Readers[$slot].ReadLineAsync()
            Add-NativeLine $Job $line
        }
    }
    return [bool]($Job.Pending[0] -or $Job.Pending[1])
}

function Complete-Native($Job) {
    $code = -1
    if ($Job.Process) {
        $Job.Process.WaitForExit()
        $code = $Job.Process.ExitCode
        $Job.Process.Dispose()
        $null = $script:LiveNative.Remove($Job)
    }
    [pscustomobject]@{ Code = $code; Skip = ''; Lines = $Job.Lines.ToArray(); Hidden = $Job.Hidden.ToArray(); Shown = $Job.Shown.ToArray(); Streamed = $true }
}

# An interrupted run must not leave its test processes behind.
function Stop-LiveNative {
    foreach ($job in $script:LiveNative.ToArray()) {
        try { if (-not $job.Process.HasExited) { & taskkill.exe /PID $job.Process.Id /T /F *> $null } } catch { $null = $_ }
    }
    $script:LiveNative.Clear()
}

function Invoke-Native([string]$File, [string[]]$Arguments = @(), [bool]$Quiet = $false, [string]$WorkingDirectory = '') {
    $job = Start-Native $File $Arguments $Quiet $WorkingDirectory $script:Progress.Foreground
    while (Read-Native $job) {
        $active = @($job.Pending | Where-Object { $_ })
        $null = [System.Threading.Tasks.Task]::WaitAny([System.Threading.Tasks.Task[]]$active, 500)
        Step-Lanes
        $null = Update-Progress $script:Progress $script:Clock.Elapsed.TotalSeconds
    }
    return Complete-Native $job
}

# ---- lanes that run beside the main list

$script:Lanes = [ordered]@{}
$script:FinishedRuns = New-Object System.Collections.Generic.Queue[object]
$script:LaneStep = 0.0

function Get-NativeCommand($Test) {
    if ($Test.Command) { return (& $Test.Command) }
    if ($Test.Script) {
        return [pscustomobject]@{ Skip = ''; File = 'powershell'
                                  Arguments = @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $script:RepoRoot $Test.Script)) + $Test.Arguments }
    }
    return [pscustomobject]@{ Skip = ''; File = (Join-Path $buildDir "bin\$($Test.Exe).exe"); Arguments = $Test.Arguments }
}

function Get-TestQuiet($Test) { [bool]($Test.Command -or ($Test.Capture -and $Test.Quiet)) }

function Start-LaneNext($Lane) {
    $Lane.Job = $null
    while (-not $Lane.Job -and $Lane.Queue.Count) {
        $Lane.Test = $Lane.Queue.Dequeue()
        $name = Get-TestName $Lane.Test
        $command = Get-NativeCommand $Lane.Test
        if ($command.Skip) {
            $skipped[$name] = $command.Skip
            Start-ProgressGroup $script:Progress $name $script:Clock.Elapsed.TotalSeconds -Background
            Complete-ProgressGroup $script:Progress 'SKIPPED' $script:Clock.Elapsed.TotalSeconds $null $name
            $script:FinishedRuns.Enqueue([pscustomobject]@{ Test = $Lane.Test; Run = $command })
            continue
        }
        Start-ProgressGroup $script:Progress $name $script:Clock.Elapsed.TotalSeconds -Background
        $Lane.Job = Start-Native $command.File $command.Arguments (Get-TestQuiet $Lane.Test) '' $name $true
    }
}

function Step-Lanes {
    $now = $script:Clock.Elapsed.TotalSeconds
    if ($now - $script:LaneStep -lt 0.2) { return }
    $script:LaneStep = $now
    foreach ($lane in $script:Lanes.Values) {
        if (-not $lane.Job -or (Read-Native $lane.Job)) { continue }
        $run = Complete-Native $lane.Job
        Complete-Test $lane.Test $run
        $script:FinishedRuns.Enqueue([pscustomobject]@{ Test = $lane.Test; Run = $run })
        Start-LaneNext $lane
    }
}

# A background group's output, whole, between two foreground groups: all of it when the group failed.
function Write-FinishedRuns {
    while ($script:FinishedRuns.Count) {
        $finished = $script:FinishedRuns.Dequeue()
        Clear-ProgressLine $script:Progress
        Write-Step "Finished in the background: $($finished.Test.Title)"
        Write-TestResult $finished.Test $finished.Run $true
    }
}

function Wait-Lanes([string[]]$Names) {
    while (@($Names | Where-Object { $script:Lanes.Contains($_) -and $script:Lanes[$_].Job }).Count) {
        Start-Sleep -Milliseconds 200
        Step-Lanes
        Write-FinishedRuns
        $null = Update-Progress $script:Progress $script:Clock.Elapsed.TotalSeconds
    }
    Write-FinishedRuns
}

# ---- results

# Records a group that ran: capture, failure lists, progress and, for a pass, its duration.
function Complete-Test($Test, $Run) {
    $name = Get-TestName $Test
    if ($Test.Block -or $Test.Command -or $Test.Capture) { $captured[$name] = $Run.Lines }
    $problems = @(if ($Test.Exe -and $Test.Capture -and $env:DCP_TEST_LOBBY) { Get-LogProblems $Test $Run.Lines })
    if ($problems.Count -and -not $Run.Code) { $Run.Code = 1 }
    $Run | Add-Member -NotePropertyName Problems -NotePropertyValue $problems -Force
    $state = 'PASS'
    if ($Run.Code -ne 0) {
        $state = 'FAIL'
        $failed.Add($name)
        if ($Test.Parity) { $parityFailed.Add($name) }
        $exitCodes[$name] = $Run.Code
    }
    $checks = Get-FinalCheckCount @($Run.Lines)
    Complete-ProgressGroup $script:Progress $state $script:Clock.Elapsed.TotalSeconds $checks $name
    $seconds = (Get-ProgressGroup $script:Progress $name).Seconds
    $null = Add-TimingSample $script:History (Get-TimingKey $Test.Platform $mode $name) $state $seconds $checks
    if ($seconds -ge 30) { $null = Save-TestHistory $script:History }
}

function Write-TestResult($Test, $Run, [bool]$Background) {
    Clear-ProgressLine $script:Progress
    if ($Run.Skip) { Write-Host "    SKIPPED: $($Run.Skip)" -ForegroundColor Yellow; return }
    $name = Get-TestName $Test
    if ($Background) { foreach ($line in $(if ($Run.Code -ne 0) { $Run.Lines } else { $Run.Shown })) { Write-Host $line } }
    elseif ($Run.Code -ne 0) { foreach ($line in $Run.Hidden) { Write-Host $line } }
    foreach ($problem in $Run.Problems) { Write-Host $problem -ForegroundColor Red }
    if ($Run.Code -ne 0) { Write-Host "    FAIL: $name ($(Format-ExitCode $Run.Code))" -ForegroundColor Red }
    else { Write-Host '    PASS' -ForegroundColor Green }
}

function Invoke-Test($Test) {
    $name = Get-TestName $Test
    Start-ProgressGroup $script:Progress $name $script:Clock.Elapsed.TotalSeconds
    Write-Step "Running the $($Test.Title)"
    if ($Test.Block) { $run = & $Test.Block }
    else {
        $command = Get-NativeCommand $Test
        $run = if ($command.Skip) { $command } else { Invoke-Native $command.File $command.Arguments (Get-TestQuiet $Test) }
    }
    if ($run -is [hashtable]) { $run = [pscustomobject]$run }
    if ($run.Skip) {
        $skipped[$name] = $run.Skip
        Complete-ProgressGroup $script:Progress 'SKIPPED' $script:Clock.Elapsed.TotalSeconds $null $name
    } else {
        Complete-Test $Test $run
    }
    Write-TestResult $Test $run $false
}

# Runs every step. Parallel: the other lanes start first and the main list goes on beside them; steps that measure real time
# wait for the Character Editor lane, and Final ones for every lane.
function Invoke-TestList($Tests, [bool]$Parallel) {
    if (-not $Parallel) {
        foreach ($test in $Tests) { Invoke-Test $test }
        return
    }
    $background = @($Tests | Where-Object { $_.Lane -ne 'main' })
    foreach ($test in $background) {
        if (-not $script:Lanes.Contains($test.Lane)) { $script:Lanes[$test.Lane] = [pscustomobject]@{ Job = $null; Test = $null; Queue = (New-Object System.Collections.Generic.Queue[object]) } }
        $script:Lanes[$test.Lane].Queue.Enqueue($test)
    }
    if ($background.Count) { Write-Step "In the background: $(@($background | ForEach-Object { Get-TestName $_ }) -join ', ')" }
    foreach ($lane in $script:Lanes.Values) { Start-LaneNext $lane }
    $main = @($Tests | Where-Object { $_.Lane -eq 'main' })
    foreach ($test in $main | Where-Object { -not $_.Timing -and -not $_.Final }) { Invoke-Test $test; Write-FinishedRuns }
    Wait-Lanes @('editor')
    foreach ($test in $main | Where-Object { $_.Timing -and -not $_.Final }) { Invoke-Test $test; Write-FinishedRuns }
    Wait-Lanes @($script:Lanes.Keys)
    foreach ($test in $main | Where-Object { $_.Final }) { Invoke-Test $test }
}

# The shared run report: every group's time, the slowest ones, what changed since the last comparable run.
function Write-RunReport([bool]$Finished) {
    $now = $script:Clock.Elapsed.TotalSeconds
    $runKey = $mode + $(if ($skipped.Count) { '|without ' + (@($skipped.Keys | Sort-Object) -join ', ') } else { '' })
    $blocked = @($captured['dcp-build-parity-tests'] | Where-Object { "$_" -like 'PARITY-BUILD|*|BLOCKED*' }).Count
    $previous = if ($Finished) { Get-PreviousRun $script:History $runKey } else { $null }
    foreach ($line in (Format-TimingReport $script:Progress $now $blocked $previous)) {
        if ($line -like 'WARNING:*') { Write-Host $line -ForegroundColor Yellow } else { Write-Host $line }
    }
    Write-Host "Before the first group (dependencies and Windows build): $(Format-Elapsed $script:BuildSeconds)"
    $linuxSteps = @($captured['Linux gate (WSL)'] | Where-Object { "$_" -match '^LINUX-STEP\|(.+)\|ms=(\d+)$' } | ForEach-Object { [pscustomobject]@{ Name = $Matches[1]; Ms = [long]$Matches[2] } })
    if ($linuxSteps.Count) {
        Write-Host 'Slowest Linux gate steps (the Character Editor groups run beside the others)'
        foreach ($step in @($linuxSteps | Sort-Object -Property Ms -Descending | Select-Object -First 5)) { Write-Host ('    {0,-36} {1,10}' -f $step.Name, (Format-Elapsed ($step.Ms / 1000))) }
    }
    if (-not $Finished) { Write-Host 'Run interrupted before the last group finished.' -ForegroundColor Yellow }
    $complete = $Finished -and $failed.Count -eq 0
    $null = Add-RunSample $script:History $runKey $complete $now ($script:Progress.Total - $skipped.Count)
    $null = Save-TestHistory $script:History
    if ($script:History.SaveProblem) { Write-Host "Timing history: not saved ($($script:History.SaveProblem))" -ForegroundColor Yellow }
    if ($script:History.Problem) { Write-Host "Timing history: the file was $($script:History.Problem), so this run started without it ($($script:History.Path))" -ForegroundColor Yellow }
}
