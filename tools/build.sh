#!/usr/bin/env bash
# Builds dcp-loader on Linux x86_64 incrementally (Clang, CMake + Ninja): the Linux counterpart of tools/build.ps1.
# The one build configuration, RelWithDebInfo, is what is tested and what an AppImage contains.
#
#   tools/build.sh            build build/cmake/linux/bin/dcp-loader
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
while read -r path url commit; do
    [[ -z ${path:-} || $path == \#* ]] && continue
    dir=$root/gpu/$path
    [[ -f $dir/.dcp_commit && $(<"$dir/.dcp_commit") == "$commit" ]] && continue
    echo "Fetching $path (${commit:0:7})"
    rm -rf "$dir"
    mkdir -p "$dir"
    git -C "$dir" init --quiet
    git -C "$dir" fetch --quiet --depth 1 "$url" "$commit"
    git -C "$dir" -c advice.detachedHead=false checkout --quiet FETCH_HEAD
    [[ -f $dir/.gitmodules ]] && git -C "$dir" submodule update --quiet --init --recursive --depth 1
    for patch in "$root/gpu/patches/$(basename "$path")"/*.patch; do
        [[ -f $patch ]] && git -C "$dir" apply "$patch"
    done
    echo "$commit" > "$dir/.dcp_commit"
done < "$root/gpu/deps.txt"

((clean)) && { step 'Cleaning build/cmake/linux'; rm -rf "$build_dir"; }
if ! grep -qs "CMAKE_BUILD_TYPE:STRING=$config" "$build_dir/CMakeCache.txt" || [[ ! -f $build_dir/build.ninja ]]; then
    step 'Configuring'
    cmake -S "$root" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=$config \
        -DCMAKE_C_COMPILER="$cc" -DCMAKE_CXX_COMPILER="$cxx"
fi
step 'Building'
targets=(dcp-loader)
((test)) && targets+=(dcp-launcher-tests dcp-trophy-tests dcp-cheats-tests dcp-runtime-tests dcp-text-tests dcp-savedata-tests dcp-tiling-tests dcp-cursor-tests dcp-window-tests)
ninja -C "$build_dir" "${targets[@]}"
loader=$build_dir/bin/dcp-loader
[[ -x $loader ]] || die "Missing $loader"

if ((test)); then
    step 'Running the launcher tests'
    "$build_dir/bin/dcp-launcher-tests"
    step 'Running the trophy tests'
    "$build_dir/bin/dcp-trophy-tests"
    step 'Running the cheat tests'
    "$build_dir/bin/dcp-cheats-tests"
    step 'Running the runtime tests (memory, fibers)'
    "$build_dir/bin/dcp-runtime-tests"
    step 'Running the text tests (text dialog, system font set)'
    "$build_dir/bin/dcp-text-tests"
    step 'Running the save-data tests (mounts, Japanese startup save hook)'
    "$build_dir/bin/dcp-savedata-tests"
    step 'Running the cursor auto-hide tests'
    "$build_dir/bin/dcp-cursor-tests"
    step 'Running the window fit tests'
    "$build_dir/bin/dcp-window-tests"
    step 'Running the tiling tests (GPU tile layouts and addresses)'
    "$build_dir/bin/dcp-tiling-tests"
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
