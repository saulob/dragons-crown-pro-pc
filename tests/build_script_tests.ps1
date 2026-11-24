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

$selectorTargets = @($commands | Where-Object { $_.GetCommandName() -eq 'ninja' } | ForEach-Object { $_.CommandElements } |
        Where-Object { $_ -is [Management.Automation.Language.VariableExpressionAst] -and $_.Splatted })
$expectedSelectors = 'bossBattleOnlineSuites', 'earlyStorySessionSuites', 'launcherRendererSuites', 'editorTranslationSuites', 'editorReleaseSuites'
foreach ($name in $expectedSelectors) {
    if (@($selectorTargets | Where-Object { $_.VariablePath.UserPath -eq $name }).Count -eq 0) {
        Write-Host "FAIL: a focused selector no longer hands `$$name to ninja; update this test"
        $failures++
    }
}
foreach ($target in $selectorTargets) {
    $name = $target.VariablePath.UserPath
    $assignments = @($testAst.FindAll({
                param($node)
                $node -is [Management.Automation.Language.AssignmentStatementAst] -and
                $node.Left -is [Management.Automation.Language.VariableExpressionAst] -and
                $node.Left.VariablePath.UserPath -eq $name
            }, $true))
    $isList = $assignments.Count -gt 0
    foreach ($assignment in $assignments) {
        $value = $assignment.Right.Expression
        $isList = $isList -and ($value -is [Management.Automation.Language.ArrayExpressionAst] -or $value -is [Management.Automation.Language.ArrayLiteralAst])
    }
    if (-not $isList) {
        Write-Host "FAIL: `$$name is splatted into ninja but is not a list, so PowerShell passes it one character at a time"
        $failures++
    }
}

$normalNinja = @($commands | Where-Object { $_.GetCommandName() -eq 'ninja' } |
        Where-Object { @($_.CommandElements | Where-Object { $_ -is [Management.Automation.Language.VariableExpressionAst] -and $_.Splatted }).Count -eq 0 })
$normalTargets = @($normalNinja | ForEach-Object { $_.CommandElements | Select-Object -Skip 1 } |
        Where-Object { $_ -is [Management.Automation.Language.StringConstantExpressionAst] -and $_.Value -like 'dcp-*' } | ForEach-Object { $_.Value })
if ($normalNinja.Count -ne 1 -or $normalTargets.Count -eq 0) {
    Write-Host 'FAIL: test.ps1 must have exactly one normal ninja target list'
    $failures++
}
foreach ($target in $normalTargets) {
    if ($target -notmatch '^dcp-[a-z0-9]+(-[a-z0-9]+)*-tests$' -or ($target -split '-tests').Count -gt 2 -or $target -match 'tests-?dcp') {
        Write-Host "FAIL: ninja target '$target' is not one test target; adjacent names were concatenated"
        $failures++
    }
}
if (@($normalTargets | Select-Object -Unique).Count -ne $normalTargets.Count) {
    Write-Host 'FAIL: the normal ninja target list repeats a target'
    $failures++
}
$stepExecutables = @($commands | Where-Object { $_.GetCommandName() -eq 'Test-Step' } | ForEach-Object {
        $positional = @($_.CommandElements | Select-Object -Skip 1 | Where-Object { $_ -is [Management.Automation.Language.StringConstantExpressionAst] })
        if ($positional.Count -ge 2 -and $positional[1].Value -like 'dcp-*') { $positional[1].Value }
    } | Select-Object -Unique)
foreach ($required in @('dcp-test-progress-tests', 'dcp-runtime-overlay-tests') + $stepExecutables) {
    if ($normalTargets -notcontains $required) {
        Write-Host "FAIL: the normal ninja target list lacks $required as its own target"
        $failures++
    }
}

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$cmakeText = @(Get-ChildItem -LiteralPath $repo -Recurse -Filter 'CMakeLists.txt' -File | Where-Object {
        $_.FullName -notmatch '\\third_party\\' -and $_.FullName -notmatch '\\(build|dist|android)\\' } |
        ForEach-Object { Get-Content -Raw -LiteralPath $_.FullName }) -join "`n"
foreach ($variable in 'CMAKE_C_IMPLICIT_INCLUDE_DIRECTORIES', 'CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES',
    'CMAKE_C_STANDARD_INCLUDE_DIRECTORIES', 'CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES') {
    if ($cmakeText.Contains($variable)) {
        Write-Host "FAIL: project CMake passes $variable as an include path; Clang must own its standard-header order"
        $failures++
    }
}
foreach ($module in [regex]::Matches($cmakeText, 'pkg_check_modules\(\s*(\w+)\b[^)]*\bIMPORTED_TARGET\b')) {
    $target = "PkgConfig::$($module.Groups[1].Value)"
    $cleared = 'set_target_properties\(\s*' + [regex]::Escape($target) + '\s+PROPERTIES\s+INTERFACE_INCLUDE_DIRECTORIES\s+""\s*\)'
    if ($cmakeText -notmatch $cleared) {
        Write-Host "FAIL: $target keeps pkg-config's prefix include dir in the compile line; that leading -isystem hides libc++'s C headers"
        $failures++
    }
}

if ($failures -gt 0) {
    exit 1
}
Write-Host "build.ps1 waits for every loader run ($(@($calls).Count) call-operator run(s) checked); test.ps1 refreshes its dependencies first and gives ninja every focused selector's targets as a list ($(@($selectorTargets).Count) checked)"
