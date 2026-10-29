#!/usr/bin/env bash
# Builds dragons_crown_pro on Linux x86_64 incrementally (Clang, CMake + Ninja): the Linux counterpart of tools/build.ps1.
# The one build configuration, RelWithDebInfo, is what is tested and what an AppImage contains.
#
#   tools/build.sh            build build/cmake/linux/bin/dragons_crown_pro
#   tools/build.sh --clean    delete build/cmake/linux first
#   tools/build.sh --test     also build and run the focused tests
#   tools/build.sh --run [args...]   start the loader that was just built (arguments after --run go to it)
#
# Missing tools or libraries are listed; packaging/linux/ci-deps.sh installs them on Ubuntu 20.04 (the release baseline).
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$root/build/cmake/linux
config=RelWithDebInfo
clean=0 test=0 run=0
loader_args=()
while (($#)); do
    case $1 in
        --clean) clean=1 ;;
        --test) test=1 ;;
        --run) run=1; shift; loader_args=("$@"); break ;;
        -h|--help) sed -n '2,11p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

step() { printf '\n==> %s\n' "$*"; }
die() { echo "ERROR: $*" >&2; exit 1; }

[[ $(uname -s) == Linux && $(uname -m) == x86_64 ]] || die 'This script builds for Linux x86_64 only (Windows: tools\build.ps1).'

cc=${CC:-clang} cxx=${CXX:-clang++}
missing=()
for tool in "$cc" "$cxx" cmake ninja pkg-config git; do
    command -v "$tool" >/dev/null || missing+=("$tool")
done
command -v glslang >/dev/null || command -v glslangValidator >/dev/null || missing+=(glslang)
if command -v pkg-config >/dev/null; then
    for module in sdl3 freetype2 libpng zlib openssl libxxhash vulkan; do
        pkg-config --exists "$module" || missing+=("pkg-config:$module")
    done
fi
if ((${#missing[@]})); then
    echo "Missing: ${missing[*]}" >&2
    echo 'packaging/linux/ci-deps.sh prepares the Ubuntu 20.04 release build environment (docs/LINUX.md).' >&2
    exit 1
fi

step "Toolchain $($cc --version | head -1), $config"

step 'Video core dependencies (gpu/deps.txt)'
patches_of() {
    local LC_ALL=C dir=$1 file
    PATCHES=()
    for file in "$dir"/*.patch; do
        [[ -f $file ]] || continue
        PATCHES+=("$file")
    done
}
patches_fingerprint() {
    local file hash
    (($#)) || return 0
    for file in "$@"; do
        hash=$(sha256sum < "$file" | cut -d' ' -f1)
        printf '%s:%s\n' "$(basename "$file")" "${hash^^}"
    done | sha256sum | cut -d' ' -f1 | tr 'a-f' 'A-F'
}
while read -r path url commit; do
    [[ -z ${path:-} || $path == \#* ]] && continue
    dir=$root/gpu/$path
    patches_of "$root/gpu/patches/$(basename "$path")"
    fingerprint=$(patches_fingerprint "${PATCHES[@]}")
    expected=$commit
    [[ -n $fingerprint ]] && expected=$commit$'\n'$fingerprint
    [[ -f $dir/.dcp_commit && $(<"$dir/.dcp_commit") == "$expected" ]] && continue
    echo "Fetching $path (${commit:0:7})"
    rm -rf "$dir"
    mkdir -p "$dir"
    git -C "$dir" init --quiet
    git -C "$dir" fetch --quiet --depth 1 "$url" "$commit"
    git -C "$dir" -c advice.detachedHead=false checkout --quiet FETCH_HEAD
    [[ -f $dir/.gitmodules ]] && git -C "$dir" submodule update --quiet --init --recursive --depth 1
    for patch in "${PATCHES[@]}"; do
        git -C "$dir" apply "$patch"
    done
    printf '%s' "$expected" > "$dir/.dcp_commit"
done < "$root/gpu/deps.txt"

((clean)) && { step 'Cleaning build/cmake/linux'; rm -rf "$build_dir"; }
if ! grep -qs "CMAKE_BUILD_TYPE:STRING=$config" "$build_dir/CMakeCache.txt" || [[ ! -f $build_dir/build.ninja ]]; then
    step 'Configuring'
    cmake -S "$root" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=$config \
        -DCMAKE_C_COMPILER="$cc" -DCMAKE_CXX_COMPILER="$cxx"
fi
step 'Building'
targets=(dragons_crown_pro)
((test)) && targets+=(dcp-launcher-tests dcp-trophy-tests dcp-cheats-tests dcp-voice-tests dcp-runtime-tests dcp-text-tests dcp-savedata-tests dcp-tiling-tests dcp-cursor-tests dcp-window-tests dcp-present-tests dcp-frame-pacing-tests dcp-amd-water-tests dcp-ultrawide-tests dcp-ultrawide-sides-tests dcp-isa-tests dcp-update-tests dcp-net-tests dcp-skip-offers-tests dcp-rtc-tests)
ninja -C "$build_dir" "${targets[@]}"
loader=$build_dir/bin/dragons_crown_pro
[[ -x $loader ]] || die "Missing $loader"

if ((test)); then
    step 'Running the launcher tests'
    "$build_dir/bin/dcp-launcher-tests"
    step 'Running the trophy tests'
    "$build_dir/bin/dcp-trophy-tests"
    step 'Running the cheat tests'
    "$build_dir/bin/dcp-cheats-tests"
    step 'Running the Japanese NPC voice tests'
    "$build_dir/bin/dcp-voice-tests"
    step 'Running the runtime tests (memory, fibers)'
    "$build_dir/bin/dcp-runtime-tests"
    step 'Running the text tests (text dialog, system font set)'
    "$build_dir/bin/dcp-text-tests"
    step 'Running the save-data tests (mounts, Japanese startup save hook)'
    "$build_dir/bin/dcp-savedata-tests"
    step 'Running the cursor visibility tests'
    "$build_dir/bin/dcp-cursor-tests"
    step 'Running the window fit tests'
    "$build_dir/bin/dcp-window-tests"
    step 'Running the present tests (swapchain images, present mode, flip pacing)'
    "$build_dir/bin/dcp-present-tests"
    step 'Running the frame pacing tests (Frame rate setting, repeat cadence, limiter)'
    "$build_dir/bin/dcp-frame-pacing-tests"
    step 'Running the AMD water fix tests (vendor defaults, F3 presses)'
    "$build_dir/bin/dcp-amd-water-tests"
    step 'Running the True Ultrawide hook tests'
    "$build_dir/bin/dcp-ultrawide-tests"
    step 'Running the True Ultrawide side area tests'
    "$build_dir/bin/dcp-ultrawide-sides-tests"
    step 'Running the MOVBE and BMI1 emulation tests'
    "$build_dir/bin/dcp-isa-tests"
    step 'Running the tiling tests (GPU tile layouts and addresses)'
    "$build_dir/bin/dcp-tiling-tests"
    step 'Running the updater tests (versions, release JSON, SHA-256, ZIP safety, staging and rollback)'
    "$build_dir/bin/dcp-update-tests"
    step 'Running the RTC tests (Win32 file time)'
    "$build_dir/bin/dcp-rtc-tests"
    step 'Running the Skip Offer tests (synthetic and local boot images)'
    "$build_dir/bin/dcp-skip-offers-tests"
    step 'Running the online tests (JSON, identity, STUN, peer transport over loopback)'
    "$build_dir/bin/dcp-net-tests"
fi

view=$root/game/prepared/v1.03 image=$root/build/image/v1.03
if [[ -f $view/.dcp_view.json ]]; then
    if [[ ! -f $image/boot.bin || $view/.dcp_view.json -nt $image/boot.bin ||
          $root/launcher/boot_image.cpp -nt $image/boot.bin || $root/launcher/game_profile.h -nt $image/boot.bin ]]; then
        step 'Updating the boot image'
        "$loader" --prepare-image "$view" "$image"
    fi
else
    echo 'The game is not installed yet: start the loader and use Install game... on the launcher'\''s Play page.'
fi
printf '\nBuilt %s\n' "$loader"
((run)) || exit 0
cd -- "$(dirname -- "$loader")"
exec "$loader" "${loader_args[@]}"
