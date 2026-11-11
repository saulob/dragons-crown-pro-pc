<#
.SYNOPSIS
  Static test of tools\build.ps1: dragons_crown_pro.exe is a Windows-subsystem program, so every call-operator run of
  the loader must be piped, or the script moves on while that process is still alive and its own duplicate check
  reports it as already running. Static test of tools\test.ps1: it refreshes the dependencies before building, or a changed
  gpu\patches set is compiled against the stale checkout.
#>
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script = Join-Path (Resolve-Path (Join-Path $PSScriptRoot '..')).Path 'tools\build.ps1'
$tokens = $null
$errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($script, [ref]$tokens, [ref]$errors)
$failures = 0
if ($errors.Count -gt 0) {
    Write-Host "FAIL: build.ps1 does not parse ($($errors.Count) errors)"
    $failures++
}

$calls = $ast.FindAll({
        param($node)
        $node -is [Management.Automation.Language.PipelineAst] -and
        $node.PipelineElements[0] -is [Management.Automation.Language.CommandAst] -and
        $node.PipelineElements[0].InvocationOperator -eq [Management.Automation.Language.TokenKind]::Ampersand -and
        $node.PipelineElements[0].CommandElements[0].Extent.Text -eq '$loader'
    }, $true)

if (@($calls).Count -eq 0) {
    Write-Host 'FAIL: build.ps1 no longer runs the loader with the call operator; update this test'
    $failures++
}
foreach ($call in $calls) {
    if ($call.PipelineElements.Count -lt 2) {
        Write-Host "FAIL: the loader is run without a pipe, so PowerShell does not wait for it: $($call.Extent.Text)"
        $failures++
    }
}
$prepare = @($calls | Where-Object { $_.Extent.Text -match '--prepare-image' })
if ($prepare.Count -ne 1) {
    Write-Host 'FAIL: expected exactly one --prepare-image call'
    $failures++
}

$buildCommands = @($ast.FindAll({ param($node) $node -is [Management.Automation.Language.CommandAst] }, $true))
$logStart = @($buildCommands | Where-Object { $_.GetCommandName() -eq 'Start-BuildLog' })
$logStop = @($buildCommands | Where-Object { $_.GetCommandName() -eq 'Stop-BuildLog' })
if ($logStart.Count -ne 1 -or $logStop.Count -lt 1) {
    Write-Host 'FAIL: build.ps1 must start exactly one numbered build log and stop it when the build ends'
    $failures++
}
$logged = @($buildCommands | Where-Object { $_.GetCommandName() -eq 'Invoke-Logged' } | ForEach-Object { $_.CommandElements[1].Extent.Text })
foreach ($tool in "'cmake'", "'ninja'") {
    if ($logged -notcontains $tool) {
        Write-Host "FAIL: build.ps1 must run $tool through Invoke-Logged so its output reaches the build log"
        $failures++
    }
}
$commonText = Get-Content -Raw -LiteralPath (Join-Path (Split-Path $script -Parent) '_common.ps1')
if ($commonText -notmatch 'CreateNew' -or $commonText -notmatch 'build\\logs\\build') {
    Write-Host 'FAIL: _common.ps1 must reserve each build log with CreateNew under build\logs\build'
    $failures++
}

if ($commonText -notmatch "'build_\{0:D3\}\.log'" -or $commonText -match '(?m)^\s*(?!#).*dragons_crown_pro_') {
    Write-Host 'FAIL: the build log must be build_NNN.log; dragons_crown_pro_NNN.log is the loader run log, so one build -Run must not allocate two'
    $failures++
}
$buildText = Get-Content -Raw -LiteralPath $script
if ($buildText -match '(?m)^\s*(?!#)[^#\r\n]*--logs?\b') {
    Write-Host 'FAIL: build.ps1 must leave the run log to the loader (no --log or --logs argument)'
    $failures++
}

$testScript = Join-Path (Resolve-Path (Join-Path $PSScriptRoot '..')).Path 'tools\test.ps1'
$testAst = [Management.Automation.Language.Parser]::ParseFile($testScript, [ref]$tokens, [ref]$errors)
if ($errors.Count -gt 0) {
    Write-Host "FAIL: test.ps1 does not parse ($($errors.Count) errors)"
    $failures++
}
$commands = @($testAst.FindAll({ param($node) $node -is [Management.Automation.Language.CommandAst] }, $true))
$sync = @($commands | Where-Object { $_.GetCommandName() -eq 'Sync-GpuDependencies' })
$ninja = @($commands | Where-Object { $_.GetCommandName() -eq 'ninja' })
if ($sync.Count -ne 1 -or $ninja.Count -eq 0 -or $sync[0].Extent.StartOffset -gt $ninja[0].Extent.StartOffset) {
    Write-Host 'FAIL: test.ps1 must call Sync-GpuDependencies once, before ninja'
    $failures++
}

if ($failures -gt 0) {
    exit 1
}
Write-Host "build.ps1 waits for every loader run ($(@($calls).Count) call-operator run(s) checked); test.ps1 refreshes its dependencies first"
