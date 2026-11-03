<#
.SYNOPSIS
  Static test of tools\build.ps1: dragons_crown_pro.exe is a Windows-subsystem program, so every call-operator run of
  the loader must be piped, or the script moves on while that process is still alive and its own duplicate check
  reports it as already running.
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

if ($failures -gt 0) {
    exit 1
}
Write-Host "build.ps1 waits for every loader run ($(@($calls).Count) call-operator run(s) checked)"
