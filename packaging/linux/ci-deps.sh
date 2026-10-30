#!/usr/bin/env bash
# Release build environment of the Linux AppImage: Ubuntu 20.04, whose glibc (2.31) is the oldest the AppImage runs on
# (tools/package-linux.sh rejects any shipped ELF that needs a newer one). Run once as root on a clean Ubuntu 20.04
# machine, container or WSL distribution (docs/LINUX.md); it needs network access and changes system packages.
# Clang 20 and the GCC 14 C++ library come from apt.llvm.org and the ubuntu-toolchain-r PPA, whose signing keys are
# checked by fingerprint. What 20.04 lacks or ships too old is built into /usr/local from pinned, verified sources.
set -euo pipefail

CMAKE_VERSION=3.31.8
CMAKE_SHA256=630615d8e98ac33eba7fbe472626dff5c899c85af3c024585ae109166a6909d0
MESON_VERSION=1.3.2
MESON_SHA256=492eb450c8b073024276f916f5adbb3c4bb7e90e9e6ec124efda064f3d9b5baa
OPENSSL_VERSION=3.5.9
OPENSSL_SHA256=603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a
SDL_VERSION=3.4.16
SDL_SHA256=7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68
VMA_VERSION=v3.4.0
VMA_SHA256=8487b7995ad3b263eb73bc5b9a77d71aa69b6bef5d58a715c02d2663afd81f1a
BOOST_VERSION=1.83.0
BOOST_SHA256=6478edfe2f3305127cffe8caf73ea0176c53769f4bf1585be237eb30798c3b8e
UFFD_LINUX_TAG=v6.8
UFFD_SHA256=ea46598f511c1f9156e16ffae7e76b3ded628caf9f8c52ca140315e0534cef86
UFFD_INSTALLED_SHA256=b7c2f6ae0080a09372d221d286dcf86de3053f208233b835cfa3adf37128a551
LLVM_KEY=6084F3CF814B57C1CF12EFD515CF4D18AF4F7421
TOOLCHAIN_KEY=60C317803A41BA51845E371A1E9377A2BA9EF27F

die() { echo "ERROR: $*" >&2; exit 1; }
step() { printf '\n==> %s\n' "$*"; }

[[ $(id -u) == 0 ]] || die 'run as root'
. /etc/os-release
[[ $ID == ubuntu && $VERSION_ID == 20.04 ]] || die "this is $PRETTY_NAME, not Ubuntu 20.04"
export DEBIAN_FRONTEND=noninteractive LC_ALL=C.UTF-8
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

fetch() { # url sha256 file
    curl -fsSL --retry 3 -o "$3" "$1"
    echo "$2  $3" | sha256sum -c --status - || die "$1 does not match its pinned SHA-256"
}
clone() { # dir url tag commit
    git clone -q --depth 1 --branch "$3" -c advice.detachedHead=false "$2" "$1"
    [[ $(git -C "$1" rev-parse HEAD) == "$4" ]] || die "$2 $3 is not commit $4"
}
add_key() { # name fingerprint url
    local ring=/usr/share/keyrings/$1.gpg
    curl -fsSL --retry 3 "$3" | gpg --dearmor --yes -o "$ring"
    [[ $(gpg --no-default-keyring --keyring "$ring" --with-colons --list-keys |
         awk -F: '/^pub/ {primary = 1; next} primary && /^fpr/ {print $10; primary = 0}') == "$2" ]] ||
        { rm -f "$ring"; die "the $1 signing key is not (only) $2"; }
}
cmake_install() { # source build-dir cmake-options...
    local src=$1 build=$2
    shift 2
    cmake -S "$src" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local "$@" >/dev/null
    ninja -C "$build" install >/dev/null
}
licence() { # component files...
    local component=$1
    shift
    install -Dm644 -t "/usr/local/share/licenses/$component" "$@"
}

step 'Base packages'
apt-get update
apt-get install -y --no-install-recommends ca-certificates curl gnupg git

step 'Clang 20 (apt.llvm.org) and the GCC 14 C++ library (ubuntu-toolchain-r PPA)'
add_key llvm "$LLVM_KEY" https://apt.llvm.org/llvm-snapshot.gpg.key
add_key ubuntu-toolchain-r "$TOOLCHAIN_KEY" "https://keyserver.ubuntu.com/pks/lookup?op=get&search=0x$TOOLCHAIN_KEY"
echo 'deb [signed-by=/usr/share/keyrings/llvm.gpg] https://apt.llvm.org/focal/ llvm-toolchain-focal-20 main' \
    > /etc/apt/sources.list.d/llvm-20.list
echo 'deb [signed-by=/usr/share/keyrings/ubuntu-toolchain-r.gpg] https://ppa.launchpadcontent.net/ubuntu-toolchain-r/test/ubuntu focal main' \
    > /etc/apt/sources.list.d/ubuntu-toolchain-r.list
apt-get update
apt-get install -y --no-install-recommends \
    clang-20 lld-20 libstdc++-14-dev make ninja-build pkg-config python3 perl file patchelf binutils rsync xz-utils bzip2 \
    desktop-file-utils libvulkan-dev libfreetype-dev libpng-dev zlib1g-dev \
    libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxtst-dev libxfixes-dev \
    libxinerama-dev libxkbcommon-dev libwayland-dev libwayland-bin wayland-protocols libdrm-dev libgbm-dev \
    libegl-dev libasound2-dev libpulse-dev libudev-dev libdbus-1-dev libibus-1.0-dev libpango1.0-dev
update-alternatives --install /usr/bin/clang clang /usr/bin/clang-20 200 --slave /usr/bin/clang++ clang++ /usr/bin/clang++-20
update-alternatives --install /usr/bin/ld.lld ld.lld /usr/bin/ld.lld-20 200
export CC=clang CXX=clang++

step "CMake $CMAKE_VERSION and Meson $MESON_VERSION"
fetch "https://github.com/Kitware/CMake/releases/download/v$CMAKE_VERSION/cmake-$CMAKE_VERSION-linux-x86_64.tar.gz" \
    "$CMAKE_SHA256" "$work/cmake.tar.gz"
rm -rf /opt/cmake
mkdir -p /opt/cmake
tar -xzf "$work/cmake.tar.gz" -C /opt/cmake --strip-components=1
ln -sf /opt/cmake/bin/cmake /opt/cmake/bin/ctest /opt/cmake/bin/cpack /usr/local/bin/
hash -r
fetch "https://github.com/mesonbuild/meson/releases/download/$MESON_VERSION/meson-$MESON_VERSION.tar.gz" \
    "$MESON_SHA256" "$work/meson.tar.gz"
tar -xzf "$work/meson.tar.gz" -C "$work"
meson() { python3 "$work/meson-$MESON_VERSION/meson.py" "$@"; }

step 'Vulkan headers, VMA, glslang'
clone "$work/vulkan-headers" https://github.com/KhronosGroup/Vulkan-Headers.git vulkan-sdk-1.4.350.1 \
    8864cdc896bbc2a9b6eb36b3218fc9ef57908d77
cmake_install "$work/vulkan-headers" "$work/vulkan-headers/build"
fetch "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/$VMA_VERSION/include/vk_mem_alloc.h" \
    "$VMA_SHA256" "$work/vk_mem_alloc.h"
install -Dm644 "$work/vk_mem_alloc.h" /usr/local/include/vk_mem_alloc.h
clone "$work/glslang" https://github.com/KhronosGroup/glslang.git 15.1.0 1062752a891c95b2bfeed9e356562d88f9df84ac
cmake_install "$work/glslang" "$work/glslang/build" -DENABLE_OPT=OFF -DGLSLANG_TESTS=OFF -DBUILD_SHARED_LIBS=OFF

step "Boost $BOOST_VERSION headers and Linux $UFFD_LINUX_TAG userfaultfd.h (20.04's are too old for the video core)"
fetch "https://archives.boost.io/release/$BOOST_VERSION/source/boost_${BOOST_VERSION//./_}.tar.bz2" \
    "$BOOST_SHA256" "$work/boost.tar.bz2"
tar -xjf "$work/boost.tar.bz2" -C "$work"
# Boost::headers comes with an install; Boost.System is the smallest compiled library that gives one.
(cd "$work/boost_${BOOST_VERSION//./_}" &&
    ./bootstrap.sh --with-toolset=clang --with-libraries=system --prefix=/usr/local >/dev/null &&
    ./b2 -d0 -j"$(nproc)" toolset=clang variant=release link=shared install >/dev/null)
# The kernel's headers_install rewrites __packed; the result must equal Ubuntu 24.04's linux-libc-dev copy. The
# video core probes the running kernel for these userfaultfd features and falls back to signals without them.
fetch "https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/include/uapi/linux/userfaultfd.h?h=$UFFD_LINUX_TAG" \
    "$UFFD_SHA256" "$work/userfaultfd.h"
sed -i 's/\b__packed\b/__attribute__((packed))/g' "$work/userfaultfd.h"
echo "$UFFD_INSTALLED_SHA256  $work/userfaultfd.h" | sha256sum -c --status - || die 'userfaultfd.h did not convert as expected'
install -Dm644 "$work/userfaultfd.h" /usr/local/include/linux/userfaultfd.h

step 'fmt, Zydis, xxHash, robin-map (the versions of the Ubuntu 24.04 packages)'
clone "$work/fmt" https://github.com/fmtlib/fmt.git 9.1.0 a33701196adfad74917046096bf5a2aa0ab0bb50
cmake_install "$work/fmt" "$work/fmt/build" -DBUILD_SHARED_LIBS=ON -DFMT_DOC=OFF -DFMT_TEST=OFF
licence fmt "$work/fmt/LICENSE.rst"
clone "$work/zycore" https://github.com/zyantific/zycore-c.git v1.4.1 1401fb85ac313f6605ec795c52bf99ea3f292a69
cmake_install "$work/zycore" "$work/zycore/build" -DZYCORE_BUILD_SHARED_LIB=ON
clone "$work/zydis" https://github.com/zyantific/zydis.git v4.0.0 1ba75aeefae37094c7be8eba07ff81d4fe0f1f20
cmake_install "$work/zydis" "$work/zydis/build" -DZYAN_SYSTEM_ZYCORE=ON -DZYDIS_BUILD_SHARED_LIB=ON \
    -DZYDIS_BUILD_TOOLS=OFF -DZYDIS_BUILD_EXAMPLES=OFF -DZYDIS_BUILD_DOXYGEN=OFF
licence zydis "$work/zydis/LICENSE"
install -Dm644 "$work/zycore/LICENSE" /usr/local/share/licenses/zydis/LICENSE-Zycore
clone "$work/xxhash" https://github.com/Cyan4973/xxHash.git v0.8.2 bbb27a5efb85b92a0486cf361a8635715a53f6ba
make -s -C "$work/xxhash" PREFIX=/usr/local install >/dev/null
licence xxhash "$work/xxhash/LICENSE"
clone "$work/robin-map" https://github.com/Tessil/robin-map.git v1.2.1 d37a41003bfbc7e12e34601f93c18ca2ff6d7c07
cmake_install "$work/robin-map" "$work/robin-map/build"

step "OpenSSL $OPENSSL_VERSION (libcrypto, libssl)"
fetch "https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VERSION/openssl-$OPENSSL_VERSION.tar.gz" \
    "$OPENSSL_SHA256" "$work/openssl.tar.gz"
tar -xzf "$work/openssl.tar.gz" -C "$work"
(cd "$work/openssl-$OPENSSL_VERSION" &&
    ./Configure --prefix=/usr/local --libdir=lib shared no-tests no-docs >/dev/null &&
    make -s -j"$(nproc)" build_libs >/dev/null &&
    make -s install_dev >/dev/null)
licence openssl "$work/openssl-$OPENSSL_VERSION/LICENSE.txt"

step 'libdecor and PipeWire headers for SDL (SDL loads both at run time; they are not bundled)'
clone "$work/libdecor" https://gitlab.freedesktop.org/libdecor/libdecor.git 0.2.2 7807ae3480f5c6a37c5e8505d94af1e764aaf704
meson setup "$work/libdecor/build" "$work/libdecor" --prefix=/usr/local --libdir=lib --buildtype=release \
    -Ddemo=false -Ddbus=disabled -Dgtk=disabled >/dev/null
ninja -C "$work/libdecor/build" install >/dev/null
clone "$work/pipewire" https://gitlab.freedesktop.org/pipewire/pipewire.git 0.3.48 6c4d3a51583f823b789b0de2df1e36d6c2f8dff8
meson setup "$work/pipewire/build" "$work/pipewire" --prefix=/usr/local --libdir=lib --buildtype=release \
    -Dtests=disabled -Dexamples=disabled -Dman=disabled -Ddocs=disabled -Dgstreamer=disabled -Dsystemd=disabled \
    -Dsession-managers=[] -Dbluez5=disabled -Dvulkan=disabled -Dlibcamera=disabled -Dffmpeg=disabled \
    -Djack=disabled -Dpipewire-jack=disabled -Dpipewire-alsa=disabled -Dspa-plugins=disabled -Dlibusb=disabled >/dev/null
ninja -C "$work/pipewire/build" install >/dev/null
ldconfig

step "SDL $SDL_VERSION"
fetch "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL3-$SDL_VERSION.tar.gz" \
    "$SDL_SHA256" "$work/sdl.tar.gz"
tar -xzf "$work/sdl.tar.gz" -C "$work"
cmake_install "$work/SDL3-$SDL_VERSION" "$work/sdl" -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
for feature in SDL_VIDEO_DRIVER_X11_DYNAMIC SDL_VIDEO_DRIVER_WAYLAND_DYNAMIC SDL_VIDEO_DRIVER_WAYLAND_DYNAMIC_LIBDECOR \
               SDL_AUDIO_DRIVER_PIPEWIRE_DYNAMIC SDL_AUDIO_DRIVER_PULSEAUDIO_DYNAMIC SDL_AUDIO_DRIVER_ALSA_DYNAMIC; do
    grep -rqs "#define $feature " "$work/sdl/include-config-release" ||
        die "SDL was configured without $feature (missing development package?)"
done
ldconfig

printf '\nReady: Ubuntu 20.04 (%s), %s. Build with tools/package-linux.sh.\n' \
    "$(getconf GNU_LIBC_VERSION)" "$(clang --version | head -1)"
