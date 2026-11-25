# Compiles the production sources of Touchscreen Mode (or of the launcher language, -Group launcher-language) into throwaway
# objects with the build's own commands: zero warnings, nothing linked, the build directory untouched. Prints
# GROUP|<group>-compile|PASS or FAIL.
[CmdletBinding()]
param([Parameter(Mandatory)][string]$BuildDir, [ValidateSet('touchscreen', 'launcher-language')][string]$Group = 'touchscreen')

$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $root 'tools\_common.ps1')
$ErrorActionPreference = 'Continue'
Set-StrictMode -Off

$languageUnits = @(
    @{ Source = 'launcher/launcher.cpp'; Target = 'dcp_launcher' }
    @{ Source = 'launcher/settings.cpp'; Target = 'dcp_launcher_core' }
    @{ Source = 'launcher/game_profile.cpp'; Target = 'dcp_launcher_core' }
    @{ Source = 'launcher/game_translation.cpp'; Target = 'dcp_launcher_core' }
    @{ Source = 'launcher/character_art.cpp'; Target = 'dcp_launcher_core' }
    @{ Source = 'launcher/item_catalog.cpp'; Target = 'dcp_launcher_core' }
)
$units = @(
    @{ Source = 'src/loader.c'; Target = 'dragons_crown_pro' }
    @{ Source = 'src/runtime_pad.c'; Target = 'dragons_crown_pro' }
    @{ Source = 'src/touch_absolute.c'; Target = 'dragons_crown_pro' }
    @{ Source = 'src/dcp_touchscreen.c'; Target = 'dcp_keys' }
    @{ Source = 'launcher/settings.cpp'; Target = 'dcp_launcher_core' }
    @{ Source = 'launcher/launcher.cpp'; Target = 'dcp_launcher' }
)
if ($Group -eq 'launcher-language') { $units = $languageUnits }

$failures = 0
$json = (& ninja -C $BuildDir -t compdb 2>$null | Out-String)
$entries = if ($LASTEXITCODE -eq 0 -and $json) { @($json | ConvertFrom-Json) } else { @() }
$work = Join-Path ([IO.Path]::GetTempPath()) "dcp-touchscreen-compile-$PID"
New-Item -ItemType Directory -Force -Path $work | Out-Null
try {
    $jobs = @(for ($i = 0; $i -lt $units.Count; $i++) {
            $unit = $units[$i]
            $entry = $entries | Where-Object { ($_.file -replace '\\', '/').EndsWith("/$($unit.Source)") -and
                ($_.output -replace '\\', '/') -match "(^|/)$([regex]::Escape($unit.Target))\.dir/" } | Select-Object -First 1
            if (-not $entry) {
                Write-Host "FAIL: $($unit.Source) is not a source of $($unit.Target) in the configured build"
                $failures++
                continue
            }
            $object = Join-Path $work "unit$i.obj"
            $line = $entry.command -replace ' -MD ', ' ' -replace ' -MT \S+', ' ' -replace ' -MF \S+', ' ' -replace ' -o \S+', " -o `"$object`""
            $script = Join-Path $work "unit$i.cmd"
            [IO.File]::WriteAllText($script, "@echo off`r`ncd /d `"$($entry.directory)`"`r`n$line`r`n")
            $process = Start-Process -FilePath cmd.exe -ArgumentList @('/d', '/c', "`"$script`"") -WindowStyle Hidden -PassThru `
                -RedirectStandardOutput (Join-Path $work "unit$i.out") -RedirectStandardError (Join-Path $work "unit$i.err")
            $null = $process.Handle # without it ExitCode reads null once the process is gone
            [pscustomobject]@{ Unit = $unit; Process = $process; Object = $object; Index = $i; Command = $line }
        })
    foreach ($job in $jobs) {
        $job.Process.WaitForExit()
        $lines = @(Get-Content -LiteralPath (Join-Path $work "unit$($job.Index).out"), (Join-Path $work "unit$($job.Index).err") -ErrorAction SilentlyContinue)
        $diagnostics = @($lines | Where-Object { $_ -match '(warning|error):' })
        $warningPolicy = $job.Command -match ' -Wall ' -and $job.Command -match ' -Wextra '
        $compiled = $job.Process.ExitCode -eq 0 -and (Test-Path -LiteralPath $job.Object) -and (Get-Item -LiteralPath $job.Object).Length -gt 0
        if ($compiled -and $diagnostics.Count -eq 0 -and $warningPolicy) {
            Write-Host "COMPILE|$($job.Unit.Source)|$($job.Unit.Target)|PASS"
            continue
        }
        $failures++
        Write-Host "COMPILE|$($job.Unit.Source)|$($job.Unit.Target)|FAIL"
        if (-not $warningPolicy) { Write-Host '  the build command carries no -Wall -Wextra' }
        $lines | Select-Object -First 20 | ForEach-Object { Write-Host "  $_" }
    }
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
if ($entries.Count -eq 0) { $failures++; Write-Host 'FAIL: ninja -t compdb gave no compile commands' }
Write-Host "GROUP|$Group-compile|$(if ($failures) { 'FAIL' } else { 'PASS' })"
exit $(if ($failures) { 1 } else { 0 })
