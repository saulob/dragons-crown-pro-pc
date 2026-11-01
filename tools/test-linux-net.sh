#!/usr/bin/env bash
# Compiles the shared online layer with the POSIX WebSocket and runs tests/net_tests.cpp on Linux: no CMake, no game
# code, no window. tools/test.ps1 runs it through WSL; it works on any Linux with g++ and the OpenSSL headers.
# Usage: tools/test-linux-net.sh [repository root]
set -u
root="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$root" || exit 2

version="$(sed -n 's/^project(dragons_crown_pro_pc VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)"
sources="$(sed -n '/^add_library(dcp_net STATIC/,/)/p' CMakeLists.txt | grep -o 'src/net/[a-z_]*\.cpp' | grep -v websocket_win)"
sources="$sources src/net/websocket_posix.cpp"
[ -n "$version" ] && [ -n "$sources" ] || { echo "cannot read the dcp_net sources from CMakeLists.txt"; exit 2; }

pids=""
for file in $sources; do
    g++ -std=c++20 -O1 -Wall -Wextra -DDCP_PORT_VERSION="\"$version\"" -Isrc -c "$file" -o "$work/$(basename "$file" .cpp).o" \
        2> "$work/$(basename "$file" .cpp).log" &
    pids="$pids $!"
done
failed=0
for pid in $pids; do wait "$pid" || failed=1; done
cat "$work"/*.log
[ "$failed" = 0 ] || { echo "LINUX-NET|compile|FAIL"; exit 1; }

g++ -std=c++20 -O1 -Wall -Wextra -DDCP_SOURCE_DIR="\"$root\"" -Isrc -c tests/net_tests.cpp -o "$work/net_tests.o" 2> "$work/net_tests.log" \
    || { cat "$work/net_tests.log"; echo "LINUX-NET|compile|FAIL"; exit 1; }
cat "$work/net_tests.log"
g++ "$work"/*.o -o "$work/net_tests" -lssl -lcrypto -lpthread || { echo "LINUX-NET|link|FAIL"; exit 1; }
echo "LINUX-NET|compile|PASS"

cd "$work" && ./net_tests
status=$?
echo "LINUX-NET|run|$([ "$status" = 0 ] && echo PASS || echo FAIL)"
exit "$status"
