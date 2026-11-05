# Static launcher gate for tools\test.ps1: source hygiene, the TRG patch chain, compiler warnings and the Windows and
# Linux syntax-only compiles of the launcher sources. No window, no build output is produced and no game or launcher starts.
# Prints GROUP|<name>|PASS, FAIL or SKIPPED|<reason> for each group; exits non-zero when one failed.
[CmdletBinding()]
param([string]$BuildDir = '', [switch]$SkipLinux)

$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
. (Join-Path $root 'tools\_common.ps1')
$ErrorActionPreference = 'Continue'
Set-StrictMode -Off

function Format-ExitCode([long]$Code) { 'exit code {0}, 0x{1:X8}' -f $Code, [uint32]($Code -band 0xFFFFFFFFL) }

$status = [ordered]@{}
$problems = @{}
function Start-Group([string]$Name) { $script:status[$Name] = 'PASS'; $script:problems[$Name] = 0 }
function Fail([string]$Group, [string]$What) {
    $script:problems[$Group]++
    $script:status[$Group] = 'FAIL'
    Write-Host "FAIL: [$Group] $What" -ForegroundColor Red
}
function Skip([string]$Group, [string]$Reason) {
    if ($script:status[$Group] -eq 'PASS') { $script:status[$Group] = "SKIPPED|$Reason" }
    Write-Host "SKIPPED: [$Group] $Reason" -ForegroundColor Yellow
}
function Get-GitLines([string[]]$Arguments) { @(& git @Arguments 2>$null | ForEach-Object { "$_" }) }

foreach ($group in 'launcher-hygiene', 'launcher-patches', 'launcher-warnings', 'launcher-parity-windows', 'launcher-parity-linux') {
    Start-Group $group
}

# ---------------------------------------------------------------- source hygiene

$hygiene = 'launcher-hygiene'
$scopes = @('launcher', 'assets/launcher', 'gpu/patches/trg-launcher', 'packaging/licenses', 'docs/LAUNCHER.md')
$guards = @('tests/launcher_tests.cpp', 'tests/launcher_gate_tests.ps1', 'tools/test.ps1', 'tools/test-linux-launcher.sh')
$textFile = '\.(cpp|h|c|cmake|txt|md|patch|svg|ps1|sh)$'
$tracked = @(Get-GitLines (@('ls-files', '--cached', '--others', '--exclude-standard', '--') + $scopes + $guards) |
    Sort-Object -Unique | Where-Object { Test-Path -LiteralPath (Join-Path $root $_) })
if ($tracked.Count -lt 40) { Fail $hygiene "only $($tracked.Count) launcher files found: is git available and the tree intact?" }

# The applied TRG checkout is what the launcher is built from; it is not tracked here.
$trgDir = Join-Path $root 'third_party\trg-launcher'
$trgFiles = @()
if (Test-Path -LiteralPath $trgDir) {
    $trgFiles = @(Get-ChildItem -LiteralPath (Join-Path $trgDir 'src'), (Join-Path $trgDir 'include') -Recurse -File |
        Where-Object { $_.Name -match $textFile })
} else {
    Fail $hygiene 'third_party\trg-launcher is missing: the dependencies were not fetched'
}

$absolutePath = '(?i)[A-Z]:[\\/]+(Users|VisualStudio|repos)|/mnt/[a-z]/|/home/[a-z][a-z0-9_-]+/'
$obsoleteIcons = 'trg::PageIcon|enum class PageIcon\b|\bPageIcon::|DrawPageIcon'
$diagnostics = 'ShowDemoWindow|ShowMetricsWindow|ShowStyleEditor|ShowDebugLogWindow|UI-DEBUG|DCP_LAUNCHER_DEBUG'
foreach ($file in $tracked) {
    $isGuard = $guards -contains $file
    if ($file -notmatch $textFile) { continue }
    $text = [IO.File]::ReadAllText((Join-Path $root $file))
    if ($text.Contains("`r")) { Fail $hygiene "$file has CR line endings (the repository is LF)" }
    if ($isGuard) { continue }
    if ($text -match 'DEBUG_SAULO') { Fail $hygiene "$file contains DEBUG_SAULO" }
    if ($text -cmatch $absolutePath) { Fail $hygiene "$file contains a local absolute path ($($Matches[0]))" }
    if ($file -notmatch '^(launcher/third_party|launcher/updater/third_party)/' -and $text -match $diagnostics) {
        Fail $hygiene "$file contains a temporary UI diagnostic ($($Matches[0]))"
    }
    $source = $file -match '^(launcher/[^/]+\.(cpp|h)|launcher/updater/[^/]+\.(cpp|h))$'
    if ($source -or $file -like 'gpu/patches/trg-launcher/*') {
        $searched = if ($file -like '*.patch') { ($text -split "`n" | Where-Object { $_.StartsWith('+') }) -join "`n" } else { $text }
        if ($searched -cmatch $obsoleteIcons) { Fail $hygiene "$file refers to the removed generated sidebar icons ($($Matches[0]))" }
    }
}
foreach ($file in $trgFiles) {
    $text = [IO.File]::ReadAllText($file.FullName)
    $relative = $file.FullName.Substring($trgDir.Length + 1)
    if ($text -match 'DEBUG_SAULO') { Fail $hygiene "third_party\trg-launcher\$relative contains DEBUG_SAULO" }
    if ($text -cmatch $obsoleteIcons) { Fail $hygiene "third_party\trg-launcher\$relative has the removed generated sidebar icons ($($Matches[0]))" }
    if ($text -match $diagnostics) { Fail $hygiene "third_party\trg-launcher\$relative has a temporary UI diagnostic ($($Matches[0]))" }
}

# Untracked files that are not source: scratch output, backups, probes, golden images.
$untracked = Get-GitLines @('ls-files', '--others', '--exclude-standard')
foreach ($file in $untracked) {
    if ($file -match '(?i)\.(tmp|bak|orig|rej|swp|log)$|~$|(^|/)(scratch|scratchpad|probe)[^/]*$|(^|/)probe_[^/]*$') {
        Fail $hygiene "$file looks like a temporary file"
    }
}
foreach ($file in Get-GitLines @('ls-files', '--cached', '--others', '--exclude-standard', '--', 'tests')) {
    if ($file -match '(?i)\.(png|jpg|jpeg|bmp|ppm|tga)$|golden|snapshot|screenshot') { Fail $hygiene "$file is a test image: the launcher gate compares geometry, not pictures" }
}
$expectedAssets = @('assets/launcher/README.md', 'assets/launcher/header-art.png', 'assets/launcher/header-art.svg',
    'assets/launcher/sponsor-heart.png', 'assets/launcher/sponsor-heart.svg', 'assets/launcher/icons/SOURCES.md') +
    @('play', 'display', 'graphics', 'gameplay', 'online', 'cheats', 'controls', 'trophies', 'about' | ForEach-Object { "assets/launcher/icons/$_.svg" })
$assets = @(Get-GitLines @('ls-files', '--cached', '--others', '--exclude-standard', '--', 'assets/launcher') | Sort-Object -Unique |
    Where-Object { Test-Path -LiteralPath (Join-Path $root $_) })
foreach ($file in $assets | Where-Object { $expectedAssets -notcontains $_ }) { Fail $hygiene "unexpected launcher asset: $file" }
foreach ($file in $expectedAssets | Where-Object { $assets -notcontains $_ }) { Fail $hygiene "missing launcher asset: $file" }

$check = @(& git diff --check 2>&1 | ForEach-Object { "$_" })
if ($LASTEXITCODE -ne 0) { Fail $hygiene "git diff --check:`n  $($check -join "`n  ")" }
$staged = @(& git diff --cached --check 2>&1 | ForEach-Object { "$_" })
if ($LASTEXITCODE -ne 0) { Fail $hygiene "git diff --cached --check:`n  $($staged -join "`n  ")" }
$untrackedSource = @($untracked | Where-Object { $_ -match '\.(cpp|h|c|cmake|txt|ps1|sh)$' -and $_ -notmatch '^(launcher/third_party|third_party)/' })
foreach ($file in $untrackedSource) {
    $lines = [IO.File]::ReadAllText((Join-Path $root $file)) -split "`n"
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '[ \t]+$') { Fail $hygiene "${file}:$($i + 1) has trailing whitespace (untracked: git diff --check cannot see it)"; break }
    }
}

# ---------------------------------------------------------------- patch chain

$patches = 'launcher-patches'
$trgLine = @(Get-Content -LiteralPath (Join-Path $root 'gpu\deps.txt') | Where-Object { $_ -match 'trg-launcher' -and $_ -notmatch '^\s*#' }) | Select-Object -First 1
if (-not $trgLine) {
    Fail $patches 'gpu\deps.txt has no trg-launcher line'
} else {
    $depPath, $depUrl, $commit = -split $trgLine
    $patchDir = Join-Path $root 'gpu\patches\trg-launcher'
    $series = @(Get-ChildItem -LiteralPath $patchDir -Filter '*.patch' -File | Sort-Object Name)
    $numbers = @($series | ForEach-Object { if ($_.Name -match '^(\d{4})-[a-z0-9-]+\.patch$') { [int]$Matches[1] } else { -1 } })
    if ($series.Count -eq 0 -or ($numbers -contains -1)) { Fail $patches 'a patch name is not NNNN-name.patch' }
    for ($i = 0; $i -lt $numbers.Count; $i++) {
        if ($numbers[$i] -ne $i + 1) { Fail $patches "the series is not 0001..N without gaps or duplicates (found $($series[$i].Name) at position $($i + 1))"; break }
    }
    foreach ($patch in $series) {
        if ([IO.File]::ReadAllBytes($patch.FullName) -contains 13) { Fail $patches "$($patch.Name) has CR line endings" }
    }

    $stamp = Join-Path $trgDir '.dcp_commit'
    $fingerprint = Get-PatchFingerprint $series
    $expected = if ($fingerprint) { "$commit`n$fingerprint" } else { $commit }
    if (-not (Test-Path -LiteralPath $stamp) -or (Get-Content -LiteralPath $stamp -Raw).Trim() -ne $expected) {
        Fail $patches 'third_party\trg-launcher was not built from this patch series: its stamp differs (tools\test.ps1 refetches it)'
    }

    $work = Join-Path ([IO.Path]::GetTempPath()) "dcp-trg-chain-$PID"
    try {
        if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
        $ready = $false
        & git clone --quiet --no-checkout --local $trgDir $work 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { Fail $patches "cannot clone third_party\trg-launcher" }
        else {
            & git -C $work -c advice.detachedHead=false checkout --quiet $commit 2>&1 | Out-Null
            if ($LASTEXITCODE -ne 0) { Fail $patches "the pinned revision $commit is not in third_party\trg-launcher" } else { $ready = $true }
        }
        if ($ready) {
            $applied = 0
            foreach ($patch in $series) {
                $output = @(& git -C $work apply $patch.FullName 2>&1 | ForEach-Object { "$_" })
                if ($LASTEXITCODE -ne 0) { Fail $patches "$($patch.Name) does not apply on top of $($applied) earlier patches to $($commit.Substring(0, 7)): $($output -join ' ')"; break }
                $applied++
            }
            if ($applied -eq $series.Count -and $series.Count -gt 0) {
                Write-Host "Patch chain: $applied patches ($($series[0].Name) .. $($series[-1].Name)) apply cleanly to $($commit.Substring(0, 7))"
                # What the tests read is exactly what the chain produces.
                $changed = @(& git -C $work status --porcelain --untracked-files=all 2>$null | ForEach-Object { $_.Substring(3).Trim('"') })
                foreach ($file in $changed) {
                    $mine = Join-Path $work $file
                    $theirs = Join-Path $trgDir $file
                    $same = (Test-Path -LiteralPath $theirs) -eq (Test-Path -LiteralPath $mine)
                    if ($same -and (Test-Path -LiteralPath $mine)) {
                        $same = [IO.File]::ReadAllBytes($mine).Length -eq [IO.File]::ReadAllBytes($theirs).Length -and
                                (Get-FileHash -LiteralPath $mine).Hash -eq (Get-FileHash -LiteralPath $theirs).Hash
                    }
                    if (-not $same) { Fail $patches "third_party\trg-launcher\$file differs from the patch chain (edited by hand?)" }
                }
            }
        }
    } finally {
        if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue }
    }
}

# ---------------------------------------------------------------- syntax-only compiles

$warnings = 'launcher-warnings'
$windows = 'launcher-parity-windows'
$units = @('launcher/page_icons.cpp', 'launcher/header_art.cpp', 'launcher/launcher.cpp', 'launcher/online_page.cpp', 'launcher/trophy_page.cpp',
           'launcher/host.cpp', 'launcher/updater/update_ui.cpp', 'tests/launcher_tests.cpp')
$compile = @()
if (-not $BuildDir -or -not (Test-Path -LiteralPath (Join-Path $BuildDir 'build.ninja'))) {
    Fail $warnings 'no configured build directory: pass -BuildDir (tools\test.ps1 does)'
    Fail $windows 'no configured build directory'
} else {
    $json = (& ninja -C $BuildDir -t compdb 2>$null | Out-String)
    $entries = if ($LASTEXITCODE -eq 0 -and $json) { @($json | ConvertFrom-Json) } else { @() }
    if ($entries.Count -eq 0) { Fail $warnings 'ninja -t compdb gave no compile commands'; Fail $windows 'ninja -t compdb gave no compile commands' }
    $tempDir = Join-Path ([IO.Path]::GetTempPath()) "dcp-launcher-syntax-$PID"
    New-Item -ItemType Directory -Force -Path $tempDir | Out-Null
    try {
        $index = 0
        foreach ($unit in $units) {
            $entry = $entries | Where-Object { ($_.file -replace '\\', '/').EndsWith("/$unit") } | Select-Object -First 1
            if (-not $entry) { Fail $warnings "$unit is not in the build"; Fail $windows "$unit is not in the build"; continue }
            # The build's own flags, with warnings counted rather than relaxed; only the output side is replaced.
            $line = $entry.command -replace ' -MD ', ' ' -replace ' -MT \S+', ' ' -replace ' -MF \S+', ' ' -replace ' -o \S+', ' ' -replace ' -c (\S+)$', ' -fsyntax-only $1'
            $script = Join-Path $tempDir "unit$index.cmd"
            [IO.File]::WriteAllText($script, "@echo off`r`ncd /d `"$($entry.directory)`"`r`n$line`r`n")
            $compile += [pscustomobject]@{ Unit = $unit; Script = $script; Out = Join-Path $tempDir "unit$index.out"; Err = Join-Path $tempDir "unit$index.err"; Process = $null }
            $index++
        }
        $running = New-Object System.Collections.Generic.List[object]
        foreach ($item in $compile) {
            while (@($running | Where-Object { -not $_.Process.HasExited }).Count -ge 4) { Start-Sleep -Milliseconds 100 }
            $item.Process = Start-Process -FilePath cmd.exe -ArgumentList @('/d', '/c', "`"$($item.Script)`"") -WindowStyle Hidden -PassThru `
                -RedirectStandardOutput $item.Out -RedirectStandardError $item.Err
            $null = $item.Process.Handle  # without it ExitCode reads null once the process is gone
            $running.Add($item)
        }
        foreach ($item in $compile) {
            $item.Process.WaitForExit()
            $item.Process.Refresh()
            $lines = @(Get-Content -LiteralPath $item.Out, $item.Err -ErrorAction SilentlyContinue | ForEach-Object { "$_" })
            $warned = @($lines | Where-Object { $_ -match 'warning:' })
            $errored = @($lines | Where-Object { $_ -match 'error:' })
            if ($item.Process.ExitCode -ne 0 -or $errored.Count -gt 0) { Fail $windows "$($item.Unit) does not compile ($(Format-ExitCode $item.Process.ExitCode)): $($errored | Select-Object -First 3 | Out-String)" }
            if ($warned.Count -gt 0) { Fail $warnings "$($item.Unit) produces $($warned.Count) warning(s): $($warned | Select-Object -First 3 | Out-String)" }
        }
        if ($compile.Count -eq $units.Count -and $problems[$warnings] -eq 0) {
            Write-Host "Warnings: $($units.Count) launcher translation units, zero warnings with the build's own -Wall -Wextra (NanoSVG included, via page_icons.cpp)"
        }
    } finally {
        Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# ---------------------------------------------------------------- Linux

$linux = 'launcher-parity-linux'
if ($SkipLinux) {
    Skip $linux 'skipped with -SkipLinux'
} elseif (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) {
    Skip $linux 'WSL is not installed'
} else {
    $distro = $null
    foreach ($name in @(& wsl.exe -l -q 2>$null | ForEach-Object { ($_ -replace "`0", '').Trim() } | Where-Object { $_ })) {
        & wsl.exe -d $name --exec bash -c 'command -v g++ >/dev/null' 2>$null
        if ($LASTEXITCODE -eq 0) { $distro = $name; break }
    }
    if (-not $distro) {
        Skip $linux 'no WSL distribution has g++'
    } else {
        $wslRoot = (& wsl.exe -d $distro --exec wslpath -u $root | Select-Object -First 1).Trim()
        $lines = @(& wsl.exe -d $distro --exec bash "$wslRoot/tools/test-linux-launcher.sh" $wslRoot 2>&1 | ForEach-Object { "$_" })
        $code = $LASTEXITCODE
        $lines | ForEach-Object { Write-Host $_ }
        if ($code -ne 0) { Fail $linux "the Linux syntax check failed ($(Format-ExitCode $code))" }
    }
}

Write-Host ''
$failed = 0
foreach ($entry in $status.GetEnumerator()) {
    Write-Host "GROUP|$($entry.Key)|$($entry.Value)"
    if ($entry.Value -eq 'FAIL') { $failed++ }
}
exit $(if ($failed) { 1 } else { 0 })
