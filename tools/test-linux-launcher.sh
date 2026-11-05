#!/usr/bin/env bash
# Syntax-only compile with g++ of the launcher sources that both platforms share, with the build's warning flags: no CMake, no
# window, nothing linked or run. tools/test.ps1 runs it through WSL; it works on any Linux with g++.
# Usage: tools/test-linux-launcher.sh [repository root]
set -u
root="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
cd "$root" || exit 2

flags=(-std=c++20 -fsyntax-only -Wall -Wextra -Ilauncher -Isrc -Igpu/shim -Ithird_party/trg-launcher/include)
failed=0
for unit in launcher/header_art.cpp launcher/page_icons.cpp launcher/incident_report.cpp launcher/log_scrubber.cpp launcher/report_history.cpp; do
    output="$(g++ "${flags[@]}" "$unit" 2>&1)" || failed=1
    [[ $output == *"warning:"* ]] && failed=1
    [[ -n $output ]] && printf '%s\n' "$output"
done
for header in trg/layout.h trg/page_navigation.h; do
    output="$(printf '#include <%s>\n' "$header" | g++ "${flags[@]}" -x c++ - 2>&1)" || failed=1
    [[ $output == *"warning:"* ]] && failed=1
    [[ -n $output ]] && printf '%s\n' "$output"
done
echo "LINUX-LAUNCHER|syntax|$([ "$failed" = 0 ] && echo PASS || echo FAIL)"
exit "$failed"
