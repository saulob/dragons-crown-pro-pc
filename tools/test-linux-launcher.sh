#!/usr/bin/env bash
# Syntax-only compile with g++ of the launcher sources that both platforms share, with the build's warning flags: no CMake, no
# window, nothing linked or run. tools/test.ps1 runs it through WSL; it works on any Linux with g++.
# Usage: tools/test-linux-launcher.sh [repository root]
set -u
root="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
cd "$root" || exit 2

flags=(-std=c++20 -fsyntax-only -Wall -Wextra -Ilauncher -Isrc -Igpu/shim -Ithird_party/trg-launcher/include)
failed=0
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
jobs="$(nproc)"
count=0
# check <compiler arguments>: one compile in the background, at most one per processor; the output is kept in call order.
check() {
    while (($(jobs -rp | wc -l) >= jobs)); do wait -n || true; done
    count=$((count + 1))
    local log
    log="$work/$(printf '%04d' "$count")"
    { g++ "${flags[@]}" "$@" > "$log.log" 2>&1 || touch "$log.failed"; } &
}
for unit in launcher/header_art.cpp launcher/page_icons.cpp launcher/home_stats.cpp launcher/home_art.cpp launcher/home_community.cpp launcher/home_portraits.cpp launcher/incident_report.cpp launcher/log_scrubber.cpp launcher/report_history.cpp launcher/cpk_patch.cpp launcher/appearance_runtime.cpp launcher/headless.cpp launcher/game_patches.cpp launcher/save_page_model.cpp launcher/save_import.cpp launcher/save_sfo.cpp launcher/save_zip.cpp launcher/save_ps3.cpp launcher/save_ps3_layout.cpp launcher/crypto_hmac.cpp launcher/inventory_data.cpp launcher/item_catalog.cpp launcher/inventory_model.cpp launcher/character_art.cpp launcher/game_translation.cpp; do
    check "$unit"
done
for unit in tests/save_editor_tests.cpp tests/save_page_tests.cpp tests/inventory_editor_tests.cpp; do
    check -DDCP_SOURCE_DIR="\"$root\"" "$unit"
done
for header in trg/layout.h trg/page_navigation.h frame_pacer.h launcher_renderer.h graphics_cards.h dcp_vulkan_probe.h; do
    printf '#include <%s>\n' "$header" > "$work/header_$count.cpp"
    check "$work/header_$count.cpp"
done
wait
# The startup logic is pure: it is compiled for real and run, so the Linux renderer order and fallback are executed here.
startup_ok=1
g++ -std=c++20 -Wall -Wextra -Ilauncher -Isrc -Igpu/shim tests/launcher_startup_tests.cpp -o "$work/startup_tests" > "$work/startup_build.txt" 2>&1 || startup_ok=0
if [ "$startup_ok" = 1 ]; then "$work/startup_tests" > "$work/startup_run.txt" 2>&1 || startup_ok=0; fi
cat "$work"/startup_build.txt "$work"/startup_run.txt 2> /dev/null
grep -q 'warning:' "$work/startup_build.txt" && startup_ok=0
echo "LINUX-LAUNCHER|startup-logic|$([ "$startup_ok" = 1 ] && echo PASS || echo FAIL)"
[ "$startup_ok" = 1 ] || failed=1
cat "$work"/*.log
compgen -G "$work/*.failed" > /dev/null && failed=1
grep -q 'warning:' "$work"/*.log && failed=1
[ "$(find "$work" -name '*.log' | wc -l)" = "$count" ] || failed=1
echo "LINUX-LAUNCHER|syntax|$([ "$failed" = 0 ] && echo PASS || echo FAIL)"
exit "$failed"
