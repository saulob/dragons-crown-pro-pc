#!/usr/bin/env bash
# Builds the AppImage runtime that tools/package-linux.sh puts at the front of the AppImage: the upstream
# AppImage/type2-runtime source at the commit pinned in patches/appimage-runtime/baseline.env, with this project's
# patches, built the way upstream's own chroot build does it (an Alpine mini root file system, a static musl binary).
# See docs/LINUX.md.
#
#   tools/build-appimage-runtime.sh OUT [WORK_DIR]   build when OUT is missing or the pins or patches changed, write OUT
#   tools/build-appimage-runtime.sh --check FILE     verify a runtime
#
# Needs root (it builds in a chroot inside a private mount namespace; passwordless sudo is used when not root), git, tar, unshare,
# curl or wget, and network access for the source, the Alpine root file system and its packages, and the libfuse and
# squashfuse sources upstream's scripts download (those carry their own SHA-256 checks). WORK_DIR holds the fetched source
# and the build (default: out/appimage-runtime in this repository).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PATCH_DIR="$ROOT/patches/appimage-runtime"

# shellcheck source=../patches/appimage-runtime/baseline.env
. "$PATCH_DIR/baseline.env"
VERSION_MARK="https://github.com/AppImage/type2-runtime/commit/${RUNTIME_COMMIT:0:7}+extract-fallback"

fail() { echo "ERROR: $*" >&2; exit 1; }

# The pins and the patches decide what is built; the stamp next to the runtime records which it was built from.
fingerprint() {
  { cat "$PATCH_DIR/baseline.env" "$PATCH_DIR"/*.patch; echo "$VERSION_MARK"; } | sha256sum | cut -c1-12
}

check_runtime() {
  local file="$1"
  [ -f "$file" ] || fail "missing $file"
  [ "$(dd if="$file" bs=1 skip=8 count=3 2>/dev/null | od -An -tx1 | tr -d ' \n')" = 414902 ] ||
    fail "$file does not carry the AppImage type 2 magic"
  readelf -h "$file" | grep -q 'Class:.*ELF64' || fail "$file is not a 64-bit ELF file"
  readelf -h "$file" | grep -q 'Machine:.*X86-64' || fail "$file is not an x86-64 program"
  if readelf -d "$file" 2>/dev/null | grep -q NEEDED; then fail "$file is not statically linked"; fi
  "$file" --appimage-version 2>&1 | grep -qF "$VERSION_MARK" || fail "$file is not the runtime built from the pinned source"
}

download() {  # url file sha256
  if [ ! -f "$2" ] || ! echo "$3  $2" | sha256sum -c --status -; then
    rm -f "$2"
    if command -v curl >/dev/null; then curl -fsSL -o "$2" "$1"; else wget -q -O "$2" "$1"; fi
  fi
  echo "$3  $2" | sha256sum -c --status - || fail "$2 does not match its pinned SHA-256"
}

as_root() { if [ "$(id -u)" = 0 ]; then "$@"; else sudo -n "$@"; fi; }

fetch_source() {  # dir
  local src="$1"
  if [ "$(git -C "$src" rev-parse HEAD 2>/dev/null || true)" != "$RUNTIME_COMMIT" ]; then
    rm -rf "$src"
    git init -q "$src"
    git -C "$src" fetch -q --depth 1 "$RUNTIME_REPO" "$RUNTIME_COMMIT"
    git -C "$src" checkout -q --detach FETCH_HEAD
  fi
  [ "$(git -C "$src" rev-parse HEAD)" = "$RUNTIME_COMMIT" ] || fail "$src is not at $RUNTIME_COMMIT"
  git -C "$src" reset -q --hard "$RUNTIME_COMMIT"
  git -C "$src" clean -qfdx
  local patch
  for patch in "$PATCH_DIR"/*.patch; do
    git -C "$src" apply --check "$patch" || fail "$(basename "$patch") does not apply to $RUNTIME_COMMIT"
    git -C "$src" apply "$patch"
  done
  printf '%s' "$VERSION_MARK" > "$src/src/runtime/version"
}

build_runtime() {  # out work
  local out="$1" work="$2"
  local src="$work/source" root="$work/chroot"
  local tarball="$work/alpine-minirootfs-$ALPINE_VERSION-x86_64.tar.gz"
  for tool in git tar unshare readelf od; do command -v "$tool" >/dev/null || fail "$tool not found"; done
  command -v curl >/dev/null || command -v wget >/dev/null || fail "curl or wget not found"
  [ "$(id -u)" = 0 ] || sudo -n true 2>/dev/null || fail "run as root, or allow passwordless sudo (sudo -n true must succeed)"
  mkdir -p "$work"

  echo "== AppImage runtime source ($RUNTIME_TAG, ${RUNTIME_COMMIT:0:7}) with $(find "$PATCH_DIR" -name '*.patch' | wc -l) patch(es)"
  fetch_source "$src"

  echo "== Alpine $ALPINE_VERSION root file system"
  download "https://dl-cdn.alpinelinux.org/alpine/v${ALPINE_VERSION%.*}/releases/x86_64/alpine-minirootfs-$ALPINE_VERSION-x86_64.tar.gz" \
    "$tarball" "$ALPINE_SHA256"
  if grep -qs " $root/" /proc/mounts; then fail "$root still has mounts; unmount them first"; fi
  as_root rm -rf "$root"
  mkdir -p "$root"
  as_root tar -xzf "$tarball" -C "$root"
  as_root cp -R "$src/src" "$src/patches" "$src/scripts" "$root/"
  as_root cp -p /etc/resolv.conf "$root/etc/resolv.conf"

  echo "== Build"
  # The mounts live in a private mount namespace and are gone when it ends, so removing the tree below is safe.
  as_root unshare --mount -- bash -euc '
    mount --make-rprivate /
    mount --rbind /dev "$1/dev"
    mount -t proc proc "$1/proc"
    chroot "$1" /bin/sh -ex /scripts/chroot/build.sh
    chroot "$1" /bin/sh -c "apk info -v | grep -E \"^(musl|musl-dev|clang[0-9]*|llvm[0-9]*|lld|binutils|zstd|zstd-dev|zstd-static|zlib|zlib-dev|zlib-static|mimalloc[0-9]*|mimalloc[0-9]*-dev|mimalloc[0-9]*-static|gcc|libgcc|compiler-rt)-[0-9]\" | sort; clang --version | head -1; ld --version | head -1; grep -E \"wget|sha256sum -c\" /scripts/common/install-dependencies.sh" > "$1/out/toolchain.txt" 2>&1 || true
  ' _ "$root" > "$work/build.log" 2>&1 || { tail -40 "$work/build.log" >&2; fail "the runtime build failed (log: $work/build.log)"; }

  [ -f "$root/out/runtime-x86_64" ] || fail "the build produced no runtime"
  mkdir -p "$(dirname "$out")"
  as_root cp "$root/out/runtime-x86_64" "$out.new"
  as_root cp "$root/out/toolchain.txt" "$work/toolchain.txt" 2>/dev/null || : > "$work/toolchain.txt"
  if [ -n "${SUDO_UID:-}" ]; then as_root chown "$SUDO_UID:${SUDO_GID:-$SUDO_UID}" "$out.new"; fi
  if grep -qs " $root/" /proc/mounts; then fail "$root still has mounts; not removing it"; fi
  as_root rm -rf "$root"

  # The build id and the debug link name the random build folder; without them the same inputs give the same file.
  # objcopy clears the type 2 magic bytes, so they are written again.
  objcopy --remove-section=.note.gnu.build-id --remove-section=.gnu_debuglink "$out.new"
  printf 'AI\002' | dd of="$out.new" bs=1 seek=8 count=3 conv=notrunc 2>/dev/null
  chmod 755 "$out.new"
  mv -f "$out.new" "$out"
  write_manifest "$out" "$work"
}

write_manifest() {  # runtime work
  local patch
  {
    echo "AppImage runtime build manifest"
    echo "upstream: $RUNTIME_REPO tag $RUNTIME_TAG commit $RUNTIME_COMMIT"
    for patch in "$PATCH_DIR"/*.patch; do echo "patch: $(basename "$patch") sha256 $(sha256sum "$patch" | cut -d' ' -f1)"; done
    echo "alpine: $ALPINE_VERSION minirootfs sha256 $ALPINE_SHA256"
    echo "runtime: $(basename "$1") sha256 $(sha256sum "$1" | cut -d' ' -f1)"
    echo "installed in the build root (versions as resolved by apk at build time):"
    sed 's/^/  /' "$2/toolchain.txt"
  } > "$1.manifest"
  cat "$1.manifest"
}

case "${1:-}" in
  --check)
    [ $# -eq 2 ] || fail "usage: $0 --check FILE"
    check_runtime "$2"
    echo "AppImage runtime: ${VERSION_MARK##*/}, patched build verified"
    ;;
  "" | -*)
    fail "usage: $0 OUT [WORK_DIR] | --check FILE"
    ;;
  *)
    OUT="$(realpath -m "$1")"
    WORK="$(realpath -m "${2:-$ROOT/out/appimage-runtime}")"
    STAMP="$OUT.stamp"
    if [ -f "$OUT" ] && [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$(fingerprint)" ] && check_runtime "$OUT" >/dev/null 2>&1 &&
      [ "$(sha256sum "$OUT" | cut -d' ' -f1)" = "$RUNTIME_SHA256" ]; then
      echo "AppImage runtime: current ($(fingerprint))"
    else
      rm -f "$STAMP"
      build_runtime "$OUT" "$WORK"
      check_runtime "$OUT"
      BUILT="$(sha256sum "$OUT" | cut -d' ' -f1)"
      [ "$BUILT" = "$RUNTIME_SHA256" ] ||
        fail "the runtime built here has SHA-256 $BUILT, not the reviewed $RUNTIME_SHA256 (patches/appimage-runtime/baseline.env). If the Alpine packages changed, review the new build and update RUNTIME_SHA256."
      fingerprint > "$STAMP"
      echo "AppImage runtime: built ($(fingerprint)), sha256 $BUILT"
    fi
    ;;
esac
