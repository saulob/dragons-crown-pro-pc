<#
.SYNOPSIS
  Headless tests of tools\online-instance.ps1 -Build: every run uses -WhatIf, so nothing is started and no folder is
  created. The game folders are a synthetic build tree under the temporary folder (markers and boot images only).
#>
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$script = Join-Path $repo 'tools\online-instance.ps1'
$checks = 0
$failures = 0
function Check([bool]$Condition, [string]$What) {
    $script:checks++
    if (-not $Condition) {
        $script:failures++
        Write-Host "FAIL: $What"
    }
}

# Every build key of the script, and the ones prepared in the synthetic tree (a core-scope folder is not complete).
$folders = [ordered]@{
    'CUSA10454-1.00' = 'v1.03'; 'CUSA07001-1.00' = 'CUSA07001-1.00'; 'CUSA07001-1.02' = 'CUSA07001-1.02'
    'CUSA07001-1.03' = 'CUSA07001-1.03'; 'CUSA10209-1.00' = 'CUSA10209-1.00'
}
$labels = @{
    'CUSA10454-1.00' = 'CUSA10454 1.00 USA'; 'CUSA07001-1.00' = 'CUSA07001 1.00 Japan'
    'CUSA07001-1.02' = 'CUSA07001 1.02 Japan'; 'CUSA07001-1.03' = 'CUSA07001 1.03 Japan'
    'CUSA10209-1.00' = 'CUSA10209 1.00 Asia/Hong Kong'
}
$tree = Join-Path ([IO.Path]::GetTempPath()) "dcp-online-instance-tests-$PID"
if (Test-Path -LiteralPath $tree) { Remove-Item -LiteralPath $tree -Recurse -Force }
$bin = Join-Path $tree 'build\cmake\clang64\bin'
New-Item -ItemType Directory -Force $bin | Out-Null
$exe = Join-Path $bin 'dragons_crown_pro.exe'
[IO.File]::WriteAllBytes($exe, [byte[]]@())
function Prepare([string]$Folder, [string]$Scope) {
    $app0 = Join-Path $tree "game\prepared\$Folder"
    New-Item -ItemType Directory -Force $app0, (Join-Path $tree "build\image\$Folder") | Out-Null
    [IO.File]::WriteAllText((Join-Path $app0 '.dcp_view.json'), "{`n  `"version`": `"x`",`n  `"scope`": `"$Scope`"`n}`n")
    [IO.File]::WriteAllBytes((Join-Path $tree "build\image\$Folder\boot.bin"), [byte[]]@(1))
}
foreach ($folder in $folders.Values) { Prepare $folder 'full' }
Prepare 'CUSA10209-1.02' 'core'

function Snapshot {
    Get-ChildItem -LiteralPath $tree -Recurse -File | ForEach-Object { "$($_.FullName)|$($_.Length)|$($_.LastWriteTimeUtc.Ticks)" }
}
$watched = 'PATH', 'DCP_ONLINE', 'DCP_ONLINE_NAME', 'DCP_JOIN_CODE', 'DCP_STORY_SKIP', 'DCP_TUTORIAL_SKIP', 'DCP_GAME_BUILD', 'DCP_LOBBY'
function Environment { ($watched | ForEach-Object { "$_=$([Environment]::GetEnvironmentVariable($_, 'Process'))" }) -join "`n" }

# One run of the script in this session: its output and exit code.
function Run([hashtable]$Arguments) {
    $global:LASTEXITCODE = 0
    $text = & $script @Arguments -Executable $exe -WhatIf 6>&1 2>&1 | Out-String
    return @{ Text = $text; Code = $global:LASTEXITCODE }
}
function Line([string]$Text, [string]$Label) {
    $match = [regex]::Match($Text, "(?m)^\s*$([regex]::Escape($Label)):\s*(.+?)\s*$")
    if ($match.Success) { return $match.Groups[1].Value }
    return ''
}

$before = Snapshot
$environment = Environment
$userEnvironment = [Environment]::GetEnvironmentVariable('DCP_GAME_BUILD', 'User')

# 1, 15: every prepared build key gives its own label and folder.
foreach ($key in $folders.Keys) {
    $run = Run @{ Name = 'a'; Build = $key }
    Check ($run.Code -eq 0) "$key starts (exit $($run.Code)): $($run.Text)"
    Check ((Line $run.Text 'Game build') -eq $labels[$key]) "$key label"
    Check ((Line $run.Text 'Game root') -eq (Join-Path $tree "game\prepared\$($folders[$key])")) "$key root"
}

# 2-4: A and B keep different builds whatever order they start in.
$a1 = Run @{ Name = 'a'; Build = 'CUSA10454-1.00' }
$b1 = Run @{ Name = 'b'; Build = 'CUSA10209-1.00' }
$a2 = Run @{ Name = 'a'; Build = 'CUSA10454-1.00' }
Check ((Line $a1.Text 'Game root') -ne (Line $b1.Text 'Game root')) 'A and B use different roots'
Check ((Line $a1.Text 'Game root') -eq (Line $a2.Text 'Game root')) 'B did not move A'
Check ((Line $b1.Text 'Game root') -like '*\CUSA10209-1.00') 'B keeps Asia/Hong Kong 1.00'

# 5, 6: an explicit build is never replaced by another one.
$missing = Run @{ Name = 'b'; Build = 'CUSA10209-1.02' }
Check ($missing.Code -eq 1 -and $missing.Text -match 'Build CUSA10209-1\.02 is not prepared\.') "core folder refused: $($missing.Text)"
Check (-not (Line $missing.Text 'Game root')) 'no root for a refused build'
$absent = Run @{ Name = 'b'; Build = 'CUSA10454-1.01' }
Check ($absent.Code -eq 1 -and $absent.Text -match 'Build CUSA10454-1\.01 is not prepared\.') 'missing folder refused'
$unknown = Run @{ Name = 'b'; Build = 'CUSA99999-1.00' }
Check ($unknown.Code -eq 1 -and $unknown.Text -match 'Unknown build CUSA99999-1\.00') 'unknown key refused'
$unpinned = Run @{ Name = 'a' }
Check ($unpinned.Code -eq 0 -and (Line $unpinned.Text 'Game build') -like 'the active build*') 'no -Build keeps the active build'

# 7-9: separate user folders (settings, identity) and logs.
Check ((Line $a1.Text 'user folder') -eq (Join-Path $repo 'user\test-client-a')) 'A user folder'
Check ((Line $b1.Text 'user folder') -eq (Join-Path $repo 'user\test-client-b')) 'B user folder'
Check ((Line $a1.Text 'logs') -ne (Line $b1.Text 'logs')) 'separate logs'

# 12-14: production unless the caller set DCP_LOBBY; the other options still apply.
Check ((Line $a1.Text 'Online service') -eq 'production') 'production by default'
$env:DCP_LOBBY = 'ws://127.0.0.1:9/'
$override = Run @{ Name = 'a'; Build = 'CUSA10454-1.00'; ForceOnline = $true; OnlineName = 'Tester'; JoinCode = 'ABC123' }
Check ((Line $override.Text 'Online service') -eq 'DCP_LOBBY override ws://127.0.0.1:9/') 'DCP_LOBBY override shown'
Check ($override.Text -match 'Online test instance A as Tester') 'OnlineName shown'
Check ($env:DCP_LOBBY -eq 'ws://127.0.0.1:9/') 'DCP_LOBBY left to the caller'
Remove-Item Env:DCP_LOBBY

# 10, 3, 4, 16: no game file written or copied, no marker touched, the shell's environment as it was.
Check ((Compare-Object @($before) @(Snapshot) | Measure-Object).Count -eq 0) 'game folders unchanged'
Check ((Environment) -eq $environment) 'process environment restored'
Check ([Environment]::GetEnvironmentVariable('DCP_GAME_BUILD', 'User') -eq $userEnvironment) 'user environment untouched'
Check (-not (Test-Path -LiteralPath (Join-Path $repo 'user\test-client-zz-whatif'))) 'no instance folder created'
$probe = Run @{ Name = 'zz-whatif'; Build = 'CUSA10454-1.00' }
Check ($probe.Code -eq 0 -and -not (Test-Path -LiteralPath (Join-Path $repo 'user\test-client-zz-whatif'))) '-WhatIf creates no folder'

# 17: both scripts parse.
foreach ($file in $script, $PSCommandPath) {
    $errors = $null
    [System.Management.Automation.Language.Parser]::ParseFile($file, [ref]$null, [ref]$errors) | Out-Null
    Check ($errors.Count -eq 0) "$file parses"
}

Remove-Item -LiteralPath $tree -Recurse -Force
Write-Host "online instance tests: $checks checks, $failures failures"
exit ([int]($failures -gt 0))
