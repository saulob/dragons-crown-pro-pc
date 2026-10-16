<#
.SYNOPSIS
  Packages the tested build as dist\DragonsCrownProPC-v<version>-windows-x64 (folder, ZIP and .zip.sha256). Local only:
  nothing is published, tagged or uploaded.
.DESCRIPTION
  Builds with tools\build.ps1 (the one RelWithDebInfo configuration) unless -SkipBuild, then stages dcp-loader.exe
  without its debug information, the MSYS2 DLLs it loads (found by walking its imports), README.txt, LICENSE.txt and
  the licence texts. The debug information goes to dist\<name>-symbols\dcp-loader.debug, which is not packaged.
  The staging folder is checked for game data, user files, local paths and anything unexpected before the ZIP is
  written. The version is DCP_PORT_VERSION in CMakeLists.txt.
.PARAMETER SkipBuild
  Packages the existing build\cmake\clang64\bin\dcp-loader.exe.
.PARAMETER Clean
  Passes -Clean to tools\build.ps1.
.PARAMETER VerifyOnly
  Only checks an existing staged folder, then exits.
.EXAMPLE
  .\tools\package.ps1
  .\tools\package.ps1 -VerifyOnly dist\DragonsCrownProPC-v0.5.0-windows-x64
#>
[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [switch]$Clean,
    [string]$VerifyOnly
)

. (Join-Path $PSScriptRoot '_common.ps1')

$root = $script:RepoRoot
$loader = Join-Path $root 'build\cmake\clang64\bin\dcp-loader.exe'
$tc = Resolve-Toolchain 'auto'
$bin = $tc.Bin
$prefix = Split-Path $bin -Parent
$msys = Split-Path $prefix -Parent
$pacman = Join-Path $msys 'usr\bin\pacman.exe'
$pacmanDb = Join-Path $msys 'var\lib\pacman\local'
$objcopy = Join-Path $bin 'llvm-objcopy.exe'
$objdump = Join-Path $bin 'llvm-objdump.exe'
$packagePrefix = 'mingw-w64-clang-x86_64-'

# Licence texts of the code compiled into dcp-loader.exe from this repository and its fetched dependencies.
$BuiltInLicenses = [ordered]@{
    'shadPS4'       = 'gpu\shadps4\LICENSE'
    'sirit'         = 'gpu\third_party\sirit\LICENSE.txt'
    'SPIRV-Headers' = 'gpu\third_party\sirit\externals\SPIRV-Headers\LICENSE'
    'half'          = 'gpu\third_party\half\LICENSE.txt'
    'SMAA'          = 'gpu\shadps4\video_core\host_shaders\smaa\LICENSE.txt'
    'DejaVu'        = 'gpu\third_party\fonts\LICENSE-DejaVu.txt'
    'NotoSans'      = 'third_party\fonts\LICENSE-NotoSans.txt'
    'imgui'         = 'gpu\third_party\imgui\LICENSE.txt'
    'magic_enum'    = 'gpu\third_party\magic_enum\LICENSE'
    'miniz'         = 'gpu\third_party\miniz\LICENSE'
    'xbyak'         = 'gpu\third_party\xbyak\COPYRIGHT'
}
# MSYS2 packages compiled in as headers or static libraries.
$StaticPackages = @('fmt', 'boost', 'robin-map', 'vulkan-memory-allocator', 'vulkan-headers', 'compiler-rt')
# The packages whose DLLs may ship, each listed in packaging\licenses\THIRD-PARTY-LICENSES.txt. A DLL from any other
# package stops the packaging until its licence has been reviewed and added there.
$DllPackages = @('sdl3', 'freetype', 'libpng', 'zydis', 'xxhash', 'libc++', 'libwinpthread', 'zlib', 'bzip2',
    'harfbuzz', 'brotli', 'graphite2', 'pcre2', 'glib2', 'gettext-runtime', 'libiconv')
# Provided by the graphics driver.
$SystemProvided = @('vulkan-1.dll')

$RootFiles = @('dcp-loader.exe', 'README.txt', 'LICENSE.txt')
$ForbiddenNames = @('game', 'user', 'logs', 'cache', 'build', 'prepared', 'extracted', 'image', 'sce_sys', 'sce_module',
    'eboot.bin', 'param.sfo', 'dcp.ini', 'boot.bin', 'content.bin', 'image.json', 'imports.json', 'symbols')
$ForbiddenPatterns = @('*.pkg', '*.pdb', '*.debug', '*.log', '*.dmp', '*.ini', '*.bin', '*.self', '*.sprx', '*.prx',
    '*.elf', '*.cpk', '*.sfo', '*.json', '*.spv', '*.pipeline_cache', '*.obj', '*.o', '*.a', '*.lib', '*.tmp', '*.bak',
    '*.sav', '*.py', '*.ps1', '*.c', '*.cpp', '*.h')
# File signatures of PS4 packages, SELF/ELF executables and CRI archives.
$ForbiddenMagic = @{ '7F434E54' = 'PS4 package'; '4F153D1D' = 'PS4 SELF'; '7F454C46' = 'ELF executable'; '43504B20' = 'CPK archive' }

$Latin1 = [Text.Encoding]::GetEncoding(28591)
$LocalText = New-Object System.Collections.Generic.List[string]
foreach ($path in @($root, $env:USERPROFILE)) {
    if ($path) { $LocalText.Add($path); $LocalText.Add($path.Replace('\', '/')) }
}
foreach ($word in @('VisualStudio', 'DEBUG_SAULO', 'gow3', 'God of War', 'referencias-ps4', 'build\cmake', 'build/cmake')) {
    $LocalText.Add($word)
}
$LocalNeedles = foreach ($text in $LocalText) { $text; $Latin1.GetString([Text.Encoding]::Unicode.GetBytes($text)) }

function Get-Imports([string]$Path) {
    $out = & $objdump -p $Path
    if ($LASTEXITCODE -ne 0) { Stop-Tool "llvm-objdump failed on $Path" }
    foreach ($line in $out) { if ($line -match 'DLL Name:\s*(\S+)') { $Matches[1] } }
}

# The DLLs that $Exe loads from $Dir, directly or through each other; Windows' own DLLs are not in $Dir.
function Get-DllClosure([string]$Exe, [string]$Dir) {
    $found = [ordered]@{}
    $queue = New-Object System.Collections.Generic.Queue[string]
    $queue.Enqueue($Exe)
    while ($queue.Count -gt 0) {
        foreach ($name in Get-Imports $queue.Dequeue()) {
            $key = $name.ToLowerInvariant()
            if ($SystemProvided -contains $key -or $found.Contains($key)) { continue }
            $file = Join-Path $Dir $name
            if (Test-Path -LiteralPath $file -PathType Leaf) {
                $found[$key] = (Get-Item -LiteralPath $file).FullName
                $queue.Enqueue($found[$key])
            }
        }
    }
    return $found
}

function Get-PackageInfo([string]$Short) {
    $name = $packagePrefix + $Short
    $query = & $pacman -Q $name 2>$null
    if ($LASTEXITCODE -ne 0) { Stop-Tool "MSYS2 package $name is not installed" }
    $version = ($query -split ' ')[1]
    $fields = @{}
    $key = $null
    foreach ($line in Get-Content -LiteralPath (Join-Path $pacmanDb "$name-$version\desc")) {
        if ($line -match '^%(\w+)%$') { $key = $Matches[1]; $fields[$key] = @() }
        elseif ($line -and $key) { $fields[$key] += $line }
    }
    $base = if ($fields.ContainsKey('BASE')) { $fields['BASE'][0] } else { $name }
    $licenses = @(& $pacman -Ql $name | ForEach-Object { ($_ -split ' ', 2)[1] } |
        Where-Object { $_ -match '/share/licenses/.+[^/]$' })
    return @{
        Short = $Short; Version = $version; License = ($fields['LICENSE'] -join ' ') -replace 'spdx:', ''
        Source = "https://mirror.msys2.org/mingw/sources/$base-$version.src.tar.zst"; Licenses = $licenses
    }
}

function Test-ReleaseFolder([string]$Dir) {
    $problems = New-Object System.Collections.Generic.List[string]
    if (-not (Test-Path -LiteralPath $Dir -PathType Container)) { Stop-Tool "Not a folder: $Dir" }
    $base = (Resolve-Path -LiteralPath $Dir).Path.TrimEnd('\')
    $exe = Join-Path $base 'dcp-loader.exe'
    $dlls = if (Test-Path -LiteralPath $exe) { Get-DllClosure $exe $base } else { [ordered]@{} }
    if (Test-Path -LiteralPath $exe) {
        foreach ($name in (Get-DllClosure $exe $bin).Keys) {
            if (-not $dlls.Contains($name)) { $problems.Add("missing DLL: $name") }
        }
    }
    foreach ($item in Get-ChildItem -LiteralPath $base -Recurse -Force) {
        $relative = $item.FullName.Substring($base.Length + 1)
        $parts = $relative -split '\\'
        if ($ForbiddenNames -contains $item.Name) { $problems.Add("forbidden name: $relative") }
        # Licence texts have names such as COPYING.LIB; the folder is still checked for programs and content below.
        foreach ($pattern in $ForbiddenPatterns) {
            if ($parts[0] -ne 'licenses' -and $item.Name -like $pattern) { $problems.Add("forbidden file type: $relative") }
        }
        if ($parts.Count -eq 1) {
            if ($item.PSIsContainer) { if ($item.Name -ne 'licenses') { $problems.Add("unexpected folder: $relative") } }
            elseif ($RootFiles -notcontains $item.Name -and -not $dlls.Contains($item.Name.ToLowerInvariant())) {
                $problems.Add("unexpected file: $relative")
            }
        } elseif ($parts[0] -ne 'licenses' -or $item.Name -like '*.exe' -or $item.Name -like '*.dll') {
            $problems.Add("unexpected item: $relative")
        }
        if ($item.PSIsContainer) { continue }
        $bytes = [IO.File]::ReadAllBytes($item.FullName)
        if ($bytes.Length -ge 4) {
            $magic = -join ($bytes[0..3] | ForEach-Object { $_.ToString('X2') })
            if ($ForbiddenMagic.ContainsKey($magic)) { $problems.Add("$($ForbiddenMagic[$magic]): $relative") }
        }
        $text = $Latin1.GetString($bytes)
        foreach ($needle in $LocalNeedles) {
            if ($text.IndexOf($needle, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
                $problems.Add("local path or stale name in $relative"); break
            }
        }
    }
    foreach ($leaf in $RootFiles + @('licenses\THIRD-PARTY-LICENSES.txt', 'licenses\MSYS2-PACKAGES.txt')) {
        if (-not (Test-Path -LiteralPath (Join-Path $base $leaf) -PathType Leaf)) { $problems.Add("missing: $leaf") }
    }
    return $problems
}

function Write-Problems($Problems) {
    Write-Host ''
    Write-Host 'RELEASE CHECK FAILED' -ForegroundColor Red
    foreach ($problem in $Problems) { Write-Host "  $problem" -ForegroundColor Red }
}

function Write-Text([string]$Path, [string]$Text) {
    [IO.File]::WriteAllText($Path, ($Text -replace "\r?\n", "`r`n"), (New-Object Text.UTF8Encoding $false))
}

if ($VerifyOnly) {
    $problems = @(Test-ReleaseFolder $VerifyOnly)
    if ($problems.Count) { Write-Problems $problems; exit 1 }
    Write-Ok "Release folder is clean: $VerifyOnly"
    return
}

$cmake = Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Raw
if ($cmake -notmatch '(?m)^set\(DCP_PORT_VERSION "([^"]+)"\)') { Stop-Tool 'DCP_PORT_VERSION not found in CMakeLists.txt' }
$version = $Matches[1]
$name = "DragonsCrownProPC-v$version-windows-x64"
$dist = Join-Path $root 'dist'
$stage = Join-Path $dist $name
$symbols = Join-Path $dist "$name-symbols"
$zip = Join-Path $dist "$name.zip"
$shaFile = Join-Path $dist "$name.zip.sha256"

Write-Step "Dragon's Crown Pro PC $version - Windows release package (local only)"
if (-not $SkipBuild) {
    $build = @{}
    if ($Clean) { $build.Clean = $true }
    & (Join-Path $PSScriptRoot 'build.ps1') @build
    if ($LASTEXITCODE -ne 0) { Stop-Tool 'Build failed' }
}
if (-not (Test-Path -LiteralPath $loader)) { Stop-Tool "Missing $loader (run without -SkipBuild)" }
$built = (Get-Item -LiteralPath $loader).VersionInfo.ProductVersion
if ($built -ne $version) { Stop-Tool "dcp-loader.exe is version $built, CMakeLists.txt says $version (run without -SkipBuild)" }

New-Item -ItemType Directory -Force $dist | Out-Null
foreach ($old in @($stage, $symbols, $zip, $shaFile, (Join-Path $dist "$name.sha256"))) {
    if (Test-Path -LiteralPath $old) { Remove-Item -LiteralPath $old -Recurse -Force }
}
$licenses = Join-Path $stage 'licenses'
New-Item -ItemType Directory $stage, $licenses, $symbols | Out-Null

Write-Step 'Staging dcp-loader.exe without debug information'
$debugFile = Join-Path $symbols 'dcp-loader.debug'
& $objcopy --only-keep-debug $loader $debugFile
if ($LASTEXITCODE -ne 0) { Stop-Tool 'llvm-objcopy --only-keep-debug failed' }
& $objcopy --strip-debug "--add-gnu-debuglink=$debugFile" $loader (Join-Path $stage 'dcp-loader.exe')
if ($LASTEXITCODE -ne 0) { Stop-Tool 'llvm-objcopy --strip-debug failed' }

Write-Step 'Staging the DLLs it loads'
$dlls = Get-DllClosure $loader $bin
$owners = [ordered]@{}
foreach ($dll in $dlls.Values) {
    $owner = & $pacman -Qqo $dll.Substring($msys.Length).Replace('\', '/') 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $owner) { Stop-Tool "No MSYS2 package owns $dll" }
    $short = $owner.Substring($packagePrefix.Length)
    if ($DllPackages -notcontains $short) {
        Stop-Tool "$(Split-Path $dll -Leaf) comes from $owner, which is not in packaging\licenses\THIRD-PARTY-LICENSES.txt"
    }
    if (-not $owners.Contains($short)) { $owners[$short] = @() }
    $owners[$short] += Split-Path $dll -Leaf
    Copy-Item -LiteralPath $dll $stage
}

Write-Step 'Staging the licences'
Copy-Item -LiteralPath (Join-Path $root 'LICENSE') (Join-Path $stage 'LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $root 'packaging\licenses\THIRD-PARTY-LICENSES.txt') $licenses
foreach ($component in $BuiltInLicenses.Keys) {
    $source = Join-Path $root $BuiltInLicenses[$component]
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { Stop-Tool "Missing licence file: $source" }
    New-Item -ItemType Directory -Force (Join-Path $licenses $component) | Out-Null
    Copy-Item -LiteralPath $source (Join-Path $licenses $component)
}
$manifest = New-Object System.Collections.Generic.List[string]
$manifest.Add("MSYS2 CLANG64 packages in dcp-loader.exe $version and next to it. Source packages: the URL on each line.")
foreach ($short in @($owners.Keys) + $StaticPackages) {
    $info = Get-PackageInfo $short
    if ($info.Licenses.Count -eq 0) { Stop-Tool "MSYS2 package $short installs no licence file" }
    foreach ($file in $info.Licenses) {
        $relative = $file.Substring($file.IndexOf('/share/licenses/') + '/share/licenses/'.Length).Replace('/', '\')
        $target = Join-Path $licenses $relative
        New-Item -ItemType Directory -Force (Split-Path $target -Parent) | Out-Null
        Copy-Item -LiteralPath (Join-Path $prefix ($file.Substring($file.IndexOf('/share/licenses/') + 1))) $target
    }
    $use = if ($owners.Contains($short)) { $owners[$short] -join ', ' } else { 'compiled in' }
    $manifest.Add('')
    $manifest.Add("$short $($info.Version) ($use)")
    $manifest.Add("  Licence: $($info.License)")
    $manifest.Add("  Source:  $($info.Source)")
}
Write-Text (Join-Path $licenses 'MSYS2-PACKAGES.txt') (($manifest -join "`n") + "`n")
$readme = (Get-Content -LiteralPath (Join-Path $root 'packaging\README.txt') -Raw).Replace('@VERSION@', $version)
Write-Text (Join-Path $stage 'README.txt') $readme

Write-Step 'Checking the release folder'
$problems = @(Test-ReleaseFolder $stage)
if ($problems.Count) { Write-Problems $problems; Stop-Tool 'The staging folder holds files that must not be released' }

Write-Step 'Writing the ZIP'
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$files = @(Get-ChildItem -LiteralPath $stage -Recurse -File |
    Sort-Object { $_.FullName.Substring($stage.Length + 1).Replace('\', '/') })
$stream = [IO.File]::Open($zip, [IO.FileMode]::CreateNew)
try {
    $archive = New-Object IO.Compression.ZipArchive($stream, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($file in $files) {
            $entry = "$name/" + $file.FullName.Substring($stage.Length + 1).Replace('\', '/')
            [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $file.FullName, $entry,
                [IO.Compression.CompressionLevel]::Optimal)
        }
    } finally { $archive.Dispose() }
} finally { $stream.Dispose() }
$archive = [IO.Compression.ZipFile]::OpenRead($zip)
try { $entries = @($archive.Entries | ForEach-Object { $_.FullName }) } finally { $archive.Dispose() }
$expected = @($files | ForEach-Object { "$name/" + $_.FullName.Substring($stage.Length + 1).Replace('\', '/') })
if (Compare-Object $expected $entries) { Stop-Tool 'The ZIP does not hold exactly the staged files' }
if ($entries | Where-Object { $_ -match '^[\\/]|\.\.|:|\\' }) { Stop-Tool 'The ZIP holds an unsafe entry name' }
$hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Text $shaFile "$hash  $name.zip`n"

function Format-Size([long]$Bytes) { '{0:N1} MB' -f ($Bytes / 1MB) }
$dllBytes = ($dlls.Values | ForEach-Object { (Get-Item -LiteralPath $_).Length } | Measure-Object -Sum).Sum
$stageBytes = ($files | Measure-Object -Property Length -Sum).Sum
Write-Host ''
Write-Host 'RELEASE PACKAGE READY (nothing was published)' -ForegroundColor Green
Write-Host "  Folder:   $stage ($(Format-Size $stageBytes), $($files.Count) files)"
Write-Host "  ZIP:      $zip ($(Format-Size (Get-Item -LiteralPath $zip).Length))"
Write-Host "  SHA-256:  $hash"
Write-Host "  Symbols:  $debugFile (not in the package)"
Write-Host "  Loader:   $(Format-Size (Get-Item -LiteralPath $loader).Length) built, $(Format-Size (Get-Item -LiteralPath (Join-Path $stage 'dcp-loader.exe')).Length) packaged"
Write-Host "  DLLs:     $($dlls.Count), $(Format-Size $dllBytes)"
