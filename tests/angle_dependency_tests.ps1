# The ANGLE runtime dependency: the pin, the cache, extraction, DLL staging and the package checks, against a small local
# archive. No network, no build output is produced and no game or launcher starts.
[CmdletBinding()]
param()

$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $root 'tools\_common.ps1')
$ErrorActionPreference = 'Stop'

$script:checks = 0
$script:failures = 0
function Check([bool]$Passed, [string]$Name) {
    $script:checks++
    if (-not $Passed) {
        $script:failures++
        Write-Host "FAIL: $Name" -ForegroundColor Red
    }
}
function Read-Source([string]$Relative) { Get-Content -LiteralPath (Join-Path $root $Relative) -Raw }
function Get-Sha256([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
function Get-Failure([scriptblock]$Block) {
    try { & $Block | Out-Null; return '' } catch { return $_.Exception.Message }
}

$tc = Resolve-Toolchain 'auto'
$cmake = Join-Path $tc.Bin 'cmake.exe'
$work = Join-Path ([IO.Path]::GetTempPath()) "dcp-angle-tests-$PID"
New-Item -ItemType Directory -Force -Path $work | Out-Null

function Test-Pin {
    $pin = Read-AnglePin
    Check ($pin.revision -eq '890b5d8fa2988e3719e0d80421bf3e927db9cd5c') 'the revision is the pinned one'
    Check ($pin.version -eq '2.1.r25748.890b5d8f-3') 'the package version is the pinned one'
    Check ($pin.sha256 -eq '71c0daec0503735a15b441bd45cb55296715fd03b89e1ad3a7dc460eabbad56a') 'the package hash is the pinned one'
    Check ($pin.url -ceq "https://repo.msys2.org/mingw/clang64/$($pin.package)") 'the URL is MSYS2''s own server, over https, for that exact file'
    Check ($pin.package -match "angleproject-$([regex]::Escape($pin.version))-any\.pkg\.tar\.zst$") 'the file name carries the version'
    Check ($pin.revision.StartsWith($pin.version.Split('.')[-1].Split('-')[0])) 'the version names the revision'
    $names = @($pin.Files | ForEach-Object { $_.Name } | Sort-Object)
    Check (($names -join ' ') -eq 'libEGL.dll libGLESv2.dll LICENSE') 'exactly the two DLLs and the licence are taken from the package'
    Check (@($pin.Files | Where-Object { $_.Sha256 -notmatch '^[0-9a-f]{64}$' }).Count -eq 0) 'every file has a full SHA-256'
    Check ((($pin.Needs | Sort-Object) -join ' ') -eq 'libc++.dll zlib1.dll') 'the DLLs ANGLE imports from outside Windows are declared'
    Check ($null -ne (Read-Source 'gpu/angle-runtime.txt') -and (Read-Source 'gpu/angle-runtime.txt') -notmatch 'latest') 'nothing floats'
}

# A package shaped like the real one, served from disk.
function New-Fixture([string]$Name, [hashtable]$Tamper = @{}) {
    $dir = Join-Path $work $Name
    $tree = Join-Path $dir 'tree'
    $members = 'clang64/bin/libEGL.dll', 'clang64/bin/libGLESv2.dll', 'clang64/share/licenses/angleproject/LICENSE', 'clang64/bin/unrelated.dll'
    foreach ($member in $members) {
        $path = Join-Path $tree $member.Replace('/', '\')
        New-Item -ItemType Directory -Force -Path (Split-Path $path -Parent) | Out-Null
        [IO.File]::WriteAllText($path, "content of $member")
    }
    $lines = New-Object System.Collections.Generic.List[string]
    $package = "$Name.pkg.tar.zst"
    Push-Location $tree
    try { & $cmake -E tar cf (Join-Path $dir $package) --zstd clang64 } finally { Pop-Location }
    $archive = Join-Path $dir $package
    $lines.Add('revision 890b5d8fa2988e3719e0d80421bf3e927db9cd5c')
    $lines.Add("version $Name")
    $lines.Add("package $package")
    $lines.Add("url $(([Uri]$archive).AbsoluteUri)")
    $lines.Add("sha256 $(if ($Tamper['Package']) { '0' * 64 } else { Get-Sha256 $archive })")
    foreach ($member in $members[0..2]) {
        $hash = Get-Sha256 (Join-Path $tree $member.Replace('/', '\'))
        if ($Tamper['Member'] -eq $member) { $hash = '0' * 64 }
        $lines.Add("file $member $hash")
    }
    $lines.Add('needs libc++.dll')
    $descriptor = Join-Path $dir 'angle-runtime.txt'
    [IO.File]::WriteAllLines($descriptor, $lines)
    return @{ Descriptor = $descriptor; Archive = $archive; Cache = (Join-Path $dir 'cache'); Name = $Name }
}

function Test-Cache {
    $fixture = New-Fixture 'first'
    $update = { Update-AngleRuntime -Descriptor $fixture.Descriptor -CacheRoot $fixture.Cache -CMake $cmake }
    $runtime = & $update
    $cached = Join-Path $fixture.Cache "angle\first\first.pkg.tar.zst"
    Check (Test-Path -LiteralPath $cached) 'the verified archive stays in the cache'
    Check ((@(Get-ChildItem -LiteralPath $runtime | ForEach-Object { $_.Name } | Sort-Object) -join ' ') -eq 'libEGL.dll libGLESv2.dll LICENSE') 'only the pinned files are extracted'
    Check (-not (Test-Path -LiteralPath (Join-Path (Split-Path $runtime -Parent) 'extract'))) 'nothing is left of the extraction'
    $stamp = (Get-Item -LiteralPath (Join-Path $runtime 'libEGL.dll')).LastWriteTimeUtc

    Remove-Item -LiteralPath $fixture.Archive -Force
    Check ((& $update) -eq $runtime) 'current files are reused with the source gone: an offline rebuild works'
    Check ((Get-Item -LiteralPath (Join-Path $runtime 'libEGL.dll')).LastWriteTimeUtc -eq $stamp) 'and they are left alone'

    [IO.File]::WriteAllText((Join-Path $runtime 'libEGL.dll'), 'changed')
    & $update | Out-Null
    Check ((Get-Content -LiteralPath (Join-Path $runtime 'libEGL.dll') -Raw) -eq 'content of clang64/bin/libEGL.dll') 'a changed file comes back from the cached archive, offline'

    Remove-Item -LiteralPath (Join-Path $runtime 'LICENSE') -Force
    & $update | Out-Null
    Check (Test-Path -LiteralPath (Join-Path $runtime 'LICENSE')) 'a missing file comes back too'
}

function Test-Integrity {
    $wrong = New-Fixture 'wrongpackage' @{ Package = $true }
    $failure = Get-Failure { Update-AngleRuntime -Descriptor $wrong.Descriptor -CacheRoot $wrong.Cache -CMake $cmake }
    Check ($failure -match 'does not match its pinned SHA-256') 'a download with the wrong hash stops the build'
    $folder = Join-Path $wrong.Cache 'angle\wrongpackage'
    Check (@(Get-ChildItem -LiteralPath $folder -File).Count -eq 0) 'and leaves nothing behind'
    Check (-not (Test-Path -LiteralPath (Join-Path $folder 'runtime'))) 'and nothing was extracted'

    $member = New-Fixture 'wrongmember' @{ Member = 'clang64/bin/libGLESv2.dll' }
    $failure = Get-Failure { Update-AngleRuntime -Descriptor $member.Descriptor -CacheRoot $member.Cache -CMake $cmake }
    Check ($failure -match 'libGLESv2.dll .*does not match its pinned SHA-256') 'an extracted file with the wrong hash stops the build'
    Check (-not (Test-Path -LiteralPath (Join-Path $member.Cache 'angle\wrongmember\runtime'))) 'and is not left to be used'

    $corrupt = New-Fixture 'corrupt'
    $cached = Join-Path $corrupt.Cache "angle\corrupt\corrupt.pkg.tar.zst"
    New-Item -ItemType Directory -Force -Path (Split-Path $cached -Parent) | Out-Null
    [IO.File]::WriteAllText($cached, 'truncated')
    $runtime = Update-AngleRuntime -Descriptor $corrupt.Descriptor -CacheRoot $corrupt.Cache -CMake $cmake
    Check (Test-AngleRuntimeCurrent (Read-AnglePin $corrupt.Descriptor) $runtime) 'a damaged cached archive is fetched again'

    $offline = New-Fixture 'offline'
    Remove-Item -LiteralPath $offline.Archive -Force
    $failure = Get-Failure { Update-AngleRuntime -Descriptor $offline.Descriptor -CacheRoot $offline.Cache -CMake $cmake }
    Check ($failure -match 'could not be downloaded') 'with nothing cached and no source, the failure says so'
}

function Test-Staging {
    $dir = Join-Path $work 'staged'
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $egl = Join-Path $dir 'libEGL.dll'
    $gles = Join-Path $dir 'libGLESv2.dll'
    [IO.File]::WriteAllText($egl, 'egl')
    [IO.File]::WriteAllText($gles, 'gles')
    $needs = Join-Path $dir 'libc++.dll'
    [IO.File]::WriteAllText($needs, 'c++')
    $pin = @{ Needs = @('libc++.dll'); Files = @(
        @{ Name = 'libEGL.dll'; Member = 'clang64/bin/libEGL.dll'; Sha256 = (Get-Sha256 $egl) }
        @{ Name = 'libGLESv2.dll'; Member = 'clang64/bin/libGLESv2.dll'; Sha256 = (Get-Sha256 $gles) }
        @{ Name = 'LICENSE'; Member = 'clang64/share/licenses/angleproject/LICENSE'; Sha256 = ('0' * 64) }) }
    $graph = @{ 'libEGL.dll' = @('KERNEL32.dll', 'libc++.dll', 'libEGL.dll', 'api-ms-win-crt-heap-l1-1-0.dll')
                'libGLESv2.dll' = @('dxgi.dll', 'd3d9.dll', 'libc++.dll'); 'libc++.dll' = @('KERNEL32.dll', 'transitive.dll') }
    $imports = { param($path) $graph[(Split-Path $path -Leaf)] }

    $problems = @(Get-AngleRuntimeProblems $dir $pin $imports)
    Check (($problems -join ';') -match 'missing DLL: transitive.dll, needed by libc\+\+.dll') 'a DLL missing behind a staged one is found'
    $graph['libc++.dll'] = @('KERNEL32.dll')
    Check (@(Get-AngleRuntimeProblems $dir $pin $imports).Count -eq 0) 'imports beside it and Windows DLLs are enough'

    $graph['libGLESv2.dll'] += 'zlib1.dll'
    Check ((@(Get-AngleRuntimeProblems $dir $pin $imports) -join ';') -match 'missing DLL: zlib1.dll') 'a DLL only PATH could provide is a problem'
    $graph['libGLESv2.dll'] = @('dxgi.dll')

    Remove-Item -LiteralPath $needs -Force
    Check ((@(Get-AngleRuntimeProblems $dir $pin $imports) -join ';') -match 'missing DLL: libc\+\+.dll') 'a declared dependency has to be there'
    [IO.File]::WriteAllText($needs, 'c++')

    [IO.File]::WriteAllText($gles, 'an older build')
    Check ((@(Get-AngleRuntimeProblems $dir $pin $imports) -join ';') -match 'stale ANGLE file: libGLESv2.dll') 'a stale ANGLE DLL is rejected'
    Remove-Item -LiteralPath $egl -Force
    Check ((@(Get-AngleRuntimeProblems $dir $pin $imports) -join ';') -match 'missing DLL: libEGL.dll') 'a missing ANGLE DLL is rejected'

    $source = Join-Path $work 'source.dll'
    [IO.File]::WriteAllText($source, 'one')
    $target = Join-Path $dir 'source.dll'
    Copy-IfChanged $source $dir
    $time = [DateTime]::UtcNow.AddHours(-1)
    (Get-Item -LiteralPath $target).LastWriteTimeUtc = $time
    Copy-IfChanged $source $dir
    Check ((Get-Item -LiteralPath $target).LastWriteTimeUtc -eq $time) 'an unchanged file is not written again'
    [IO.File]::WriteAllText($source, 'two')
    Copy-IfChanged $source $dir
    Check ((Get-Content -LiteralPath $target -Raw) -eq 'two') 'a changed one is'
}

# The real package.ps1 gate over a folder with no ANGLE in it, then one with a stale copy.
function Test-PackageGate {
    $shell = (Get-Process -Id $PID).Path
    $folder = Join-Path $work 'release'
    New-Item -ItemType Directory -Force -Path $folder | Out-Null
    $script = Join-Path $root 'tools\package.ps1'
    $ErrorActionPreference = 'Continue'
    $output = (& $shell -NoProfile -ExecutionPolicy Bypass -File $script -VerifyOnly $folder 2>&1 | Out-String)
    Check ($LASTEXITCODE -ne 0) 'a release folder without ANGLE fails'
    foreach ($text in 'missing DLL: libEGL.dll', 'missing DLL: libGLESv2.dll', 'missing DLL: libc\+\+.dll', 'missing DLL: zlib1.dll',
                      'missing or stale: licenses\\angleproject\\LICENSE', 'missing licence: licenses\\spirv-tools',
                      'missing licence: licenses\\vulkan-loader', 'missing licence: licenses\\VulkanMemoryAllocator', 'missing licence: licenses\\xxhash') {
        Check ($output -match $text) "the package gate reports $text"
    }
    # A real DLL under each name: only its hash is wrong.
    foreach ($name in 'libEGL.dll', 'libGLESv2.dll') { Copy-Item -LiteralPath (Join-Path $tc.Bin 'zlib1.dll') (Join-Path $folder $name) }
    $output = (& $shell -NoProfile -ExecutionPolicy Bypass -File $script -VerifyOnly $folder 2>&1 | Out-String)
    $ErrorActionPreference = 'Stop'
    Check ($output -match 'stale ANGLE file: libGLESv2.dll') 'the package gate rejects a stale ANGLE'
}

function Test-Boundaries {
    $package = Read-Source 'tools/package.ps1'
    $common = Read-Source 'tools/_common.ps1'
    $build = Read-Source 'tools/build.ps1'
    Check ($build -match 'Sync-AngleRuntime \$tc\.Bin') 'the normal build prepares ANGLE'
    Check ($build.IndexOf('Sync-AngleRuntime') -lt $build.IndexOf("Write-Step 'Building'")) 'before it builds'
    Check ($build -match 'Install-GameRuntime \$loader \$angleRuntime \$tc\.Bin') 'and stages it beside the game'
    Check ($package -match 'Sync-AngleRuntime \$bin') 'the package takes the same verified files'
    Check ($common -notmatch 'angleproject\}') 'ANGLE is not a toolchain package'
    Check ($package -notmatch '\$DllPackages = [^)]*angleproject') 'and pacman does not have to own it'
    foreach ($source in 'tools/build.ps1', 'tools/setup.ps1', 'tools/package.ps1') {
        Check ((Read-Source $source) -notmatch 'pacman[^\r\n]*angle') "$source does not ask pacman for ANGLE"
    }
    Check ($common -notmatch 'pacman[^\r\n]*Angle') 'neither does the fetch'
    Check ($common -notmatch 'mirror\.msys2\.org') 'no redirecting mirror'

    foreach ($source in 'CMakeLists.txt', 'gpu/CMakeLists.txt', 'launcher/CMakeLists.txt') {
        $text = Read-Source $source
        Check ($text -notmatch '(?i)find_(library|package|file|path)\([^)]*(egl|gles|angle)') "$source never searches for ANGLE"
        Check ($text -notmatch '(?i)(-l|target_link_libraries[^)]*)(libegl|libglesv2|angle)') "$source does not link it"
    }
    foreach ($source in 'tools/build.sh', 'tools/package-linux.sh', 'tools/package-release-linux.sh', 'tools/build-appimage-runtime.sh', 'tools/test-linux-launcher.sh') {
        Check ((Read-Source $source) -notmatch '(?i)angle') "$source has nothing to do with ANGLE"
    }
    Check ((Read-Source 'gpu/shadps4/video_core/renderer_opengl/gl_angle.cpp').TrimStart().StartsWith('// SPDX') -and
           (Read-Source 'gpu/shadps4/video_core/renderer_opengl/gl_angle.cpp') -match '#ifdef _WIN32') 'the ANGLE code exists only on Windows'

    $window = Read-Source 'gpu/shim/window.cpp'
    Check ($window -match '(?s)if \(api == GraphicsApi::Angle\) \{\s+OpenGL::RequestAngleDirect3D11\(\);') 'only the Direct3D 11 window loads ANGLE'
    Check ($window -match '(?s)if \(api == GraphicsApi::OpenGL\) \{\s+SDL_GL_SetAttribute\(SDL_GL_CONTEXT_MAJOR_VERSION, 3\)') 'native OpenGL stays a plain desktop context'

    foreach ($source in 'README.md', 'docs/d3d11-renderer.md', 'docs/GRAPHICS.md', 'docs/LAUNCHER.md', 'packaging/README.txt') {
        Check ((Read-Source $source) -notmatch '(?i)(pacman|install)[^\r\n]*angle') "$source does not tell anyone to install ANGLE"
    }
}

function Test-NothingStartsTheGame {
    $build = Read-Source 'tools/build.ps1'
    $install = $build.IndexOf('Install-GameRuntime $loader')
    Check ($install -ge 0 -and $install -lt $build.IndexOf('if (-not $Run) { return }')) 'a plain build stages everything before the run option is even read'
    foreach ($source in 'README.md', 'docs/d3d11-renderer.md', 'docs/GRAPHICS.md', 'docs/LAUNCHER.md', 'docs/FIRST_BOOT.md', 'packaging/README.txt', 'gpu/angle-runtime.txt') {
        Check ((Read-Source $source) -notmatch 'build\.ps1 -Run(?! -CheckImports)|tools\\run\.ps1') "$source does not tell anyone to build and launch in one step"
    }
    Check ((Read-Source 'docs/d3d11-renderer.md') -notmatch '-Run\b') 'the Direct3D 11 notes have no run option'
    $common = Read-Source 'tools/_common.ps1'
    $staging = $common.Substring($common.IndexOf('function Test-Sha256'), $common.IndexOf('# Content fingerprint') - $common.IndexOf('function Test-Sha256'))
    Check ($staging -notmatch 'Start-Process|\.exe''? -|\$loader') 'the ANGLE sync and staging start nothing'
    foreach ($source in 'CMakeLists.txt', 'gpu/CMakeLists.txt', 'launcher/CMakeLists.txt') {
        Check ((Read-Source $source) -notmatch 'COMMAND\s+("?\$<TARGET_FILE:dragons_crown_pro>|dragons_crown_pro)') "$source never runs the game"
    }
}

try {
    Test-Pin
    Test-Cache
    Test-Integrity
    Test-Staging
    Test-PackageGate
    Test-Boundaries
    Test-NothingStartsTheGame
}
finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
Write-Host "ANGLE dependency tests: $script:checks checks, $script:failures failures"
exit $(if ($script:failures -eq 0) { 0 } else { 1 })
