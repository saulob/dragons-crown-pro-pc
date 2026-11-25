#!/usr/bin/env bash
# Builds dragons_crown_pro on Linux x86_64 incrementally (Clang, CMake + Ninja): the Linux counterpart of tools/build.ps1.
# The one build configuration, RelWithDebInfo, is what is tested and what an AppImage contains.
#
#   tools/build.sh            build build/cmake/linux/bin/dragons_crown_pro
#   tools/build.sh --clean    delete build/cmake/linux first
#   tools/build.sh --test     also build and run the focused tests (--keep-image: without updating the boot image)
#   tools/build.sh --image    only update the boot image with the loader that was just built
#   tools/build.sh --editor   build and run only the Character Editor release tests (no game build)
#   tools/build.sh --launcher build and run only the launcher tests (no game build)
#   tools/build.sh --steam-deck-input build and run only the Steam Deck input tests (no game build)
#   tools/build.sh --vulkan-pipeline  build and run only the Vulkan pipeline and presenter tests (no game build)
#   tools/build.sh --localization     build and run only the localization and text tests (no game build)
#   tools/build.sh --korean-localization build and run only the Korean Menu & Subtitles tests (no game build)
#   tools/build.sh --manual-visual    build and run only the PT-BR manual visual tests (no game build)
#   tools/build.sh --run [args...]   start the loader that was just built (arguments after --run go to it)
#
# Missing tools or libraries are listed; packaging/linux/ci-deps.sh installs them on Ubuntu 20.04 (the release baseline).
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$root/build/cmake/linux
config=RelWithDebInfo
clean=0 test=0 image=0 keep_image=0 editor=0 launcher=0 steam_deck_input=0 vulkan_pipeline=0 localization=0 korean=0 manual_visual=0 run=0
loader_args=()
while (($#)); do
    case $1 in
        --clean) clean=1 ;;
        --test) test=1 ;;
        --image) image=1 ;;
        --keep-image) keep_image=1 ;;
        --editor) editor=1 ;;
        --launcher) launcher=1 ;;
        --steam-deck-input) steam_deck_input=1 ;;
        --vulkan-pipeline) vulkan_pipeline=1 ;;
        --localization) localization=1 ;;
        --korean-localization) korean=1 ;;
        --manual-visual) manual_visual=1 ;;
        --run) run=1; shift; loader_args=("$@"); break ;;
        -h|--help) sed -n '2,18p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

step() { printf '\n==> %s\n' "$*"; }
die() { echo "ERROR: $*" >&2; exit 1; }

update_image() {
    local view=$root/game/prepared/v1.03 image=$root/build/image/v1.03 rebuild=0 atlas
    if [[ -f $view/.dcp_view.json ]]; then
        if [[ ! -f $image/boot.bin || $view/.dcp_view.json -nt $image/boot.bin ||
              $root/launcher/boot_image.cpp -nt $image/boot.bin || $root/launcher/game_profile.h -nt $image/boot.bin ||
              $root/launcher/atlas_signatures.cpp -nt $image/boot.bin || $root/src/character_highlight_core.h -nt $image/boot.bin ]]; then
            rebuild=1
        fi
        atlas=$image/atlas-signatures.txt
        if [[ -f $view/DRACRO.CPK && ( ! -f $atlas || $atlas -ot $image/boot.bin ) ]]; then
            rebuild=1
        fi
        if ((rebuild)); then
            step 'Updating the boot image'
            "$build_dir/bin/dragons_crown_pro" --prepare-image "$view" "$image"
        fi
    else
        echo 'The game is not installed yet: start the loader and use Install game... on the launcher'\''s Play page.'
    fi
}

now_ms() { local now=${EPOCHREALTIME/[.,]/}; echo $((now / 1000)); }

# run_test <title> <executable> [arguments]: announces, runs and times one test.
run_test() {
    local title=$1 begin
    shift
    [[ -n $title ]] && step "Running the $title"
    begin=$(now_ms)
    "$@"
    printf 'LINUX-STEP|%s|ms=%s\n' "$(basename "$1")" "$(($(now_ms) - begin))"
}

# Processes the Character Editor release groups may use: DCP_TEST_JOBS, else the processors but one, at most 8. The suite
# itself keeps them inside the available memory.
test_jobs() {
    local jobs=${DCP_TEST_JOBS:-}
    if [[ -z $jobs ]]; then
        jobs=$(($(nproc) - 1))
        ((jobs > 8)) && jobs=8
        ((jobs < 1)) && jobs=1
    fi
    echo "$jobs"
}

# The Character Editor release groups beside the tests that follow; wait_editor prints their output whole and check_editor,
# after the last test, fails the run when they failed. With one job they run here and now instead.
editor_pid= editor_log= editor_status=0
start_editor() {
    export DCP_TEST_JOBS
    DCP_TEST_JOBS=$(test_jobs)
    if ((DCP_TEST_JOBS < 2)); then
        run_test 'Character Editor release groups (saves, identity, looks, art, composition with the translation overlay)' "$build_dir/bin/dcp-save-editor-tests" release
        return
    fi
    step "Starting the Character Editor release groups ($DCP_TEST_JOBS processes) beside the next tests"
    editor_log=$(mktemp)
    trap 'stop_editor' EXIT
    set -m # a process group of its own, so stop_editor reaches its units
    run_test '' "$build_dir/bin/dcp-save-editor-tests" release > "$editor_log" 2>&1 &
    editor_pid=$!
    set +m
}
stop_editor() {
    [[ -n $editor_pid ]] && kill -- "-$editor_pid" 2>/dev/null
    [[ -n $editor_log ]] && rm -f "$editor_log"
    return 0
}
wait_editor() {
    [[ -n $editor_pid ]] || return 0
    wait "$editor_pid" || editor_status=$?
    editor_pid=
    step 'Character Editor release groups (saves, identity, looks, art, composition with the translation overlay)'
    grep -v '^@@dcp-progress ' "$editor_log" || true
}
check_editor() {
    ((editor_status == 0)) || die "The Character Editor release groups failed (exit code $editor_status, output above)."
}

[[ $(uname -s) == Linux && $(uname -m) == x86_64 ]] || die 'This script builds for Linux x86_64 only (Windows: tools\build.ps1).'

if ((image)); then
    update_image
    exit 0
fi

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
if ((editor)); then
    step 'Building the Character Editor release tests'
    ninja -C "$build_dir" dcp-save-editor-tests dcp-inventory-editor-tests dcp-character-override-tests dcp-runtime-overlay-tests dcp-appearance-online-tests
    step 'Running the runtime file overlay tests (borrowed patch data, read observer, swapped data, layered overlays)'
    "$build_dir/bin/dcp-runtime-overlay-tests"
    step 'Running the online appearance tests (recipe, room session, remote textures, seat-aware hooks)'
    "$build_dir/bin/dcp-appearance-online-tests"
    export DCP_TEST_JOBS
    DCP_TEST_JOBS=$(test_jobs)
    run_test 'Character Editor release groups' "$build_dir/bin/dcp-save-editor-tests" release
    step 'Running the Inventory Editor tests'
    "$build_dir/bin/dcp-inventory-editor-tests"
    step 'Running the F3K8S character stat override tests'
    "$build_dir/bin/dcp-character-override-tests"
    exit 0
fi
if ((launcher)); then
    step 'Building the launcher tests'
    ninja -C "$build_dir" dcp-launcher-tests
    step 'Running the launcher tests'
    "$build_dir/bin/dcp-launcher-tests"
    exit 0
fi
if ((steam_deck_input)); then
    step 'Building the Steam Deck input tests'
    ninja -C "$build_dir" dcp-steam-deck-trackpad-tests dcp-mouse-tests dcp-cursor-tests dcp-touchscreen-steamos-tests dcp-sdl-compat-tests
    step 'Running the Steam Deck trackpad tests'
    "$build_dir/bin/dcp-steam-deck-trackpad-tests"
    step 'Running the mouse input tests'
    "$build_dir/bin/dcp-mouse-tests"
    step 'Running the cursor visibility tests'
    "$build_dir/bin/dcp-cursor-tests"
    step 'Running the SteamOS touchscreen tests'
    "$build_dir/bin/dcp-touchscreen-steamos-tests"
    step 'Running the SDL compat tests'
    "$build_dir/bin/dcp-sdl-compat-tests"
    exit 0
fi
if ((vulkan_pipeline)); then
    step 'Building the Vulkan pipeline and presenter tests'
    ninja -C "$build_dir" dcp-gpu-compat-tests dcp-pipeline-cache-tests dcp-present-tests dcp-frame-pacing-tests
    step 'Running the GPU compatibility tests'
    "$build_dir/bin/dcp-gpu-compat-tests"
    step 'Running the pipeline cache tests'
    "$build_dir/bin/dcp-pipeline-cache-tests"
    step 'Running the present tests'
    "$build_dir/bin/dcp-present-tests"
    step 'Running the frame pacing tests'
    "$build_dir/bin/dcp-frame-pacing-tests"
    exit 0
fi
if ((manual_visual)); then
    step 'Building the launcher manual visual tests'
    ninja -C "$build_dir" dcp-launcher-tests
    step 'Running the launcher manual visual tests'
    "$build_dir/bin/dcp-launcher-tests" manual-visual
    exit 0
fi
if ((localization)); then
    step 'Building the localization and text tests'
    ninja -C "$build_dir" dcp-launcher-tests dcp-text-tests
    step 'Running the launcher localization tests'
    "$build_dir/bin/dcp-launcher-tests" localization
    step 'Running the text tests'
    "$build_dir/bin/dcp-text-tests"
    exit 0
fi
if ((korean)); then
    step 'Building the Korean Menu & Subtitles tests'
    ninja -C "$build_dir" dcp-korean-localization-tests dcp-launcher-tests dcp-build-parity-tests dcp-text-tests
    step 'Running the Korean localization tests (profiles, corpus, Hangul atlas and glyph table)'
    "$build_dir/bin/dcp-korean-localization-tests"
    step 'Running the Korean launcher tests (language lists, saved Korean, Europe profile, voice choices)'
    "$build_dir/bin/dcp-launcher-tests" korean
    step 'Running the Korean build parity tests (contract, Europe profile, real USA corpus and glyph table)'
    "$build_dir/bin/dcp-build-parity-tests" korean
    step 'Running the text tests (Noto Sans KR only for the port Korean)'
    "$build_dir/bin/dcp-text-tests"
    exit 0
fi
step 'Building'
targets=(dragons_crown_pro)
((test)) && targets+=(dcp-launcher-tests dcp-korean-localization-tests dcp-runtime-overlay-tests dcp-appearance-online-tests dcp-save-editor-tests dcp-save-page-tests dcp-inventory-editor-tests dcp-character-override-tests dcp-test-progress-tests dcp-discord-tests dcp-build-diagnostics-tests dcp-build-parity-tests dcp-default-message-tests dcp-feature-sites-tests dcp-trophy-tests dcp-incident-tests dcp-incident-upload-tests dcp-cheats-tests dcp-kraken-fix-tests dcp-voice-tests dcp-system-type-tests dcp-party-tests dcp-thread-lifecycle-tests dcp-comms-tests dcp-comms-session-tests dcp-comms-session-runtime-tests dcp-comms-wheel-ui-tests dcp-runtime-tests dcp-sigpipe-tests dcp-text-tests dcp-savedata-tests dcp-tiling-tests dcp-renderer-selection-tests dcp-opengl-backend-tests dcp-opengl-perf-tests dcp-cursor-tests dcp-mouse-tests dcp-touchscreen-steamos-tests dcp-steam-deck-trackpad-tests dcp-window-tests dcp-sdl-compat-tests dcp-present-tests dcp-frame-pacing-tests dcp-copy-pool-tests dcp-options-menu-tests dcp-amd-water-tests dcp-pipeline-cache-tests dcp-gpu-compat-tests dcp-issue84-tests dcp-water-diag-tests dcp-narrow-water-reader-tests dcp-ultrawide-tests dcp-ultrawide-sides-tests dcp-isa-tests dcp-module-history-tests dcp-update-tests dcp-update-network-tests dcp-net-tests dcp-online-hle-tests dcp-online-builds-tests dcp-skip-offers-tests dcp-item-attack-cap-tests dcp-skip-intro-tests dcp-character-highlight-tests dcp-gallery-unlock-tests dcp-endgame-unlocks-tests dcp-boss-battle-tests dcp-boss-battle-text-tests dcp-boss-battle-record-tests dcp-boss-battle-menu-tests dcp-boss-battle-online-tests dcp-boss-battle-visitor-lifecycle-tests dcp-boss-battle-handoff-tests dcp-boss-battle-early-story-handoff-tests dcp-early-story-tests dcp-menu-buttons-tests dcp-exit-game-tests dcp-title-layout-tests dcp-rtc-tests dcp-simulate-crash-tests)
ninja -C "$build_dir" "${targets[@]}"
loader=$build_dir/bin/dragons_crown_pro
[[ -x $loader ]] || die "Missing $loader"

if ((test)); then
    start_editor
    run_test 'launcher tests' "$build_dir/bin/dcp-launcher-tests"
    run_test 'Korean localization tests (USA and Europe profiles, corpus, Hangul atlas and glyph table)' "$build_dir/bin/dcp-korean-localization-tests"
    run_test 'unsupported-build diagnostics tests' "$build_dir/bin/dcp-build-diagnostics-tests"
    run_test 'build feature parity tests (contract, update parity, mappings, unknown builds)' "$build_dir/bin/dcp-build-parity-tests"
    run_test 'feature locator tests (Cheats, Voice Language, True Ultrawide)' "$build_dir/bin/dcp-feature-sites-tests"
    run_test 'trophy tests' "$build_dir/bin/dcp-trophy-tests"
    run_test 'bug report tests' "$build_dir/bin/dcp-incident-tests"
    run_test 'cheat tests' "$build_dir/bin/dcp-cheats-tests"
    run_test 'Kraken crash fix tests' "$build_dir/bin/dcp-kraken-fix-tests"
    run_test 'Voice Language tests (player, resurrected characters, narrator, NPCs, enemies, allies)' "$build_dir/bin/dcp-voice-tests"
    run_test 'online System type hook tests' "$build_dir/bin/dcp-system-type-tests"
    run_test 'online party snapshot tests' "$build_dir/bin/dcp-party-tests"
    run_test 'Comms Wheel tests' "$build_dir/bin/dcp-comms-tests"
    run_test 'Comms Wheel session tests (rules, telemetry batching, the real send entry point)' "$build_dir/bin/dcp-comms-session-tests"
    run_test 'Comms Wheel production send tests (one accepted send, one count)' "$build_dir/bin/dcp-comms-session-runtime-tests"
    run_test 'Comms Wheel UI tests (input glue, layout, key binding)' "$build_dir/bin/dcp-comms-wheel-ui-tests"
    run_test 'Discord Rich Presence tests' "$build_dir/bin/dcp-discord-tests"
    run_test 'runtime tests (memory, fibers)' "$build_dir/bin/dcp-runtime-tests"
    run_test 'text tests (text dialog, system font set)' "$build_dir/bin/dcp-text-tests"
    run_test 'save-data tests (mounts, Japanese startup save hook)' "$build_dir/bin/dcp-savedata-tests"
    run_test 'runtime file overlay tests (borrowed patch data, read observer, swapped data)' "$build_dir/bin/dcp-runtime-overlay-tests"
    run_test 'online appearance tests (recipe, room session, remote textures, seat-aware hooks)' "$build_dir/bin/dcp-appearance-online-tests"
    run_test 'test progress reporter tests' "$build_dir/bin/dcp-test-progress-tests"
    run_test 'F3K8S character stat override tests' "$build_dir/bin/dcp-character-override-tests"
    run_test 'Save page tests' "$build_dir/bin/dcp-save-page-tests"
    run_test 'Inventory Editor tests' "$build_dir/bin/dcp-inventory-editor-tests"
    run_test 'cursor visibility tests' "$build_dir/bin/dcp-cursor-tests"
    run_test 'mouse input tests (full mouse rebinding, Pointer Control Hold / Always On, Steam Deck trackpad behavior, touchscreen behavior, controller ownership)' "$build_dir/bin/dcp-mouse-tests"
    run_test 'SteamOS touchscreen tests (Gamescope routing, touchpad mapping, synthetic mouse suppression, mouse/trackpad regression)' "$build_dir/bin/dcp-touchscreen-steamos-tests"
    run_test 'Steam Deck trackpad tests (native touchpad, automatic pointer, click routing, mouse/controller/touchscreen regression)' "$build_dir/bin/dcp-steam-deck-trackpad-tests"
    run_test 'window fit tests' "$build_dir/bin/dcp-window-tests"
    run_test 'SDL compat tests (Proton/Wine HIDAPI default)' "$build_dir/bin/dcp-sdl-compat-tests"
    run_test 'Options menu tests (chord, close gate, live and restart settings, dcp.ini, Online Play lock)' "$build_dir/bin/dcp-options-menu-tests"
    run_test 'AMD water workaround tests (vendor defaults, no hotkey)' "$build_dir/bin/dcp-amd-water-tests"
    run_test 'pipeline cache tests (seal, identity, warm-up mode, recovery, build gate)' "$build_dir/bin/dcp-pipeline-cache-tests"
    run_test 'GPU compatibility tests (interpolation route, integer fix, dynamic state, feedback loop, mixed samples)' "$build_dir/bin/dcp-gpu-compat-tests"
    run_test 'Issue #84 Adreno regression tests (feedback snapshot fallback, provoking pass break, static pipeline key audit, cache invalidation, diagnostics)' "$build_dir/bin/dcp-issue84-tests"
    run_test 'water diagnostics tests (AMD only, deduplication, cap)' "$build_dir/bin/dcp-water-diag-tests"
    run_test 'narrow water reader tests' "$build_dir/bin/dcp-narrow-water-reader-tests"
    run_test 'True Ultrawide hook tests' "$build_dir/bin/dcp-ultrawide-tests"
    run_test 'True Ultrawide side area tests' "$build_dir/bin/dcp-ultrawide-sides-tests"
    run_test 'module history tests (crash report)' "$build_dir/bin/dcp-module-history-tests"
    run_test 'MOVBE, BMI1 and LZCNT tests' "$build_dir/bin/dcp-isa-tests"
    run_test 'tiling tests (GPU tile layouts and addresses)' "$build_dir/bin/dcp-tiling-tests"
    run_test 'renderer selection tests' "$build_dir/bin/dcp-renderer-selection-tests"
    run_test 'OpenGL backend tests (detiler, emitted GLSL through glslang, no context)' "$build_dir/bin/dcp-opengl-backend-tests"
    run_test 'updater tests (versions, release JSON, SHA-256, ZIP safety, staging and rollback)' "$build_dir/bin/dcp-update-tests"
    run_test 'RTC tests (Win32 file time)' "$build_dir/bin/dcp-rtc-tests"
    run_test 'Skip Offer tests (synthetic and local boot images)' "$build_dir/bin/dcp-skip-offers-tests"
    run_test 'item attack cap tests' "$build_dir/bin/dcp-item-attack-cap-tests"
    run_test 'Skip Intro tests (startup logos, opening movie, first boot, title transition, all builds)' "$build_dir/bin/dcp-skip-intro-tests"
    run_test 'Character Highlight tests (setting, F7 hotkey, atlas signature table, runtime image match)' "$build_dir/bin/dcp-character-highlight-tests"
    run_test 'gallery unlock tests' "$build_dir/bin/dcp-gallery-unlock-tests"
    run_test 'endgame unlocks tests' "$build_dir/bin/dcp-endgame-unlocks-tests"
    run_test 'boss battle tests' "$build_dir/bin/dcp-boss-battle-tests"
    run_test 'boss battle text tests' "$build_dir/bin/dcp-boss-battle-text-tests"
    run_test 'boss battle record tests' "$build_dir/bin/dcp-boss-battle-record-tests"
    run_test 'boss battle menu tests' "$build_dir/bin/dcp-boss-battle-menu-tests"
    run_test 'boss battle online tests' "$build_dir/bin/dcp-boss-battle-online-tests"
    run_test 'boss battle visitor lifecycle tests' "$build_dir/bin/dcp-boss-battle-visitor-lifecycle-tests"
    run_test 'boss battle handoff tests' "$build_dir/bin/dcp-boss-battle-handoff-tests"
    run_test 'boss battle early story handoff tests' "$build_dir/bin/dcp-boss-battle-early-story-handoff-tests"
    run_test 'early story tests' "$build_dir/bin/dcp-early-story-tests"
    run_test 'default message tests' "$build_dir/bin/dcp-default-message-tests"
    run_test 'menu buttons tests' "$build_dir/bin/dcp-menu-buttons-tests"
    run_test 'Exit Game tests' "$build_dir/bin/dcp-exit-game-tests"
    run_test 'title layout tests' "$build_dir/bin/dcp-title-layout-tests"
    run_test 'simulated crash trigger tests' "$build_dir/bin/dcp-simulate-crash-tests"
    run_test "online build tests (every supported build's online imports, one lobby game, cheat lock)" "$build_dir/bin/dcp-online-builds-tests"
    wait_editor
    # These measure real time (timeouts, pacing, signals), so nothing runs beside them.
    run_test 'DCP-YYM3TL online Inn report-adventurers thread lifecycle tests' "$build_dir/bin/dcp-thread-lifecycle-tests"
    run_test 'SIGPIPE tests (a closed peer fails the write, not the process)' "$build_dir/bin/dcp-sigpipe-tests"
    run_test 'present tests (swapchain images, present mode, flip pacing)' "$build_dir/bin/dcp-present-tests"
    run_test 'frame pacing tests (Frame rate setting, repeat cadence, limiter)' "$build_dir/bin/dcp-frame-pacing-tests"
    run_test 'copy pool tests' "$build_dir/bin/dcp-copy-pool-tests"
    run_test 'OpenGL fast path tests (write watch with real faults, state tracker, stream ring, no context)' "$build_dir/bin/dcp-opengl-perf-tests"
    run_test 'updater network tests (504, dropped connections, stalls, redirects, retries on a loopback server)' "$build_dir/bin/dcp-update-network-tests"
    run_test 'online tests (JSON, identity, STUN, peer transport over loopback)' "$build_dir/bin/dcp-net-tests"
    run_test 'online HLE tests (needs DCP_TEST_LOBBY; skipped without it)' "$build_dir/bin/dcp-online-hle-tests"
    check_editor
fi

((keep_image)) || update_image
printf '\nBuilt %s\n' "$loader"
((run)) || exit 0
cd -- "$(dirname -- "$loader")"
exec "$loader" "${loader_args[@]}"
