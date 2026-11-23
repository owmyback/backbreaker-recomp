#!/usr/bin/env bash
# The Windows build's toolchain and libraries (#174), for cross-compiling on Linux:
#
#   scripts/windows_deps.sh [-j N]
#
# - llvm-mingw (clang, lld and the MinGW-w64 headers and libraries, UCRT) into third_party/llvm-mingw,
#   checked against its SHA-256;
# - for Windows, into third_party/win64: the Vulkan headers and an import library for vulkan-1.dll
#   (the GPU driver installs the DLL), SDL3 and shaderc (static), at the versions the release's
#   build container pins (packaging/portable/Containerfile), and the XMA decoder (FFmpeg's
#   xmaframes fork at scripts/build_xma_decoder.sh's commit).
#
# Then build with cmake/llvm-mingw.cmake (its header says how). Nothing here needs game files.
# Each step is skipped when its result is already there; delete third_party/win64 to start over.
set -euo pipefail
REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$REPO"

LLVM_MINGW_TAG=20261104
LLVM_MINGW_SHA256=919380403de87dca3c581c2053230cd298af5e891286628ba0613b7fed370926
jobs=$(nproc 2>/dev/null || echo 4)
while [ $# -gt 0 ]; do
    case "$1" in
        -j) shift; jobs=$1 ;;
        -h|--help) sed -n '2,14p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

say() { printf '[windows-deps] %s\n' "$*"; }
pin() { sed -n "s/^ARG $1=//p" packaging/portable/Containerfile; }
SDL3_TAG=$(pin SDL3_TAG)
VULKAN_HEADERS_TAG=$(pin VULKAN_HEADERS_TAG)
SHADERC_TAG=$(pin SHADERC_TAG)
[ -n "$SDL3_TAG" ] && [ -n "$VULKAN_HEADERS_TAG" ] && [ -n "$SHADERC_TAG" ] ||
    { echo "[windows-deps] ERROR: can't read the pins in packaging/portable/Containerfile" >&2; exit 1; }
for tool in git cmake ninja curl python3 make; do
    command -v "$tool" >/dev/null 2>&1 || { echo "[windows-deps] ERROR: $tool is not installed" >&2; exit 1; }
done

TP=$REPO/third_party
LM=$TP/llvm-mingw
W=$TP/win64
SRC=$TP/downloads/src
TOOLCHAIN=$REPO/cmake/llvm-mingw.cmake
mkdir -p "$W" "$SRC"

# 1. llvm-mingw
if [ "$(cat "$LM/.tag" 2>/dev/null)" != "$LLVM_MINGW_TAG" ]; then
    name=llvm-mingw-$LLVM_MINGW_TAG-ucrt-ubuntu-22.04-x86_64.tar.xz
    dl=$TP/downloads/llvm-mingw-$LLVM_MINGW_TAG
    mkdir -p "$dl"
    say "fetching llvm-mingw $LLVM_MINGW_TAG"
    curl -fsSL -o "$dl/$name" "https://github.com/mstorsjo/llvm-mingw/releases/download/$LLVM_MINGW_TAG/$name"
    echo "$LLVM_MINGW_SHA256  $dl/$name" | sha256sum -c --quiet -
    rm -rf "$LM" && mkdir -p "$LM"
    tar -xJf "$dl/$name" -C "$LM" --strip-components=1
    echo "$LLVM_MINGW_TAG" >"$LM/.tag"
fi
cmake_win() { # cmake_win <source> <build dir> [options]: always a fresh build dir
    local src=$1 build=$2
    shift 2
    rm -rf "$build"
    cmake -S "$src" -B "$build" "$@" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DCMAKE_INSTALL_PREFIX="$W" \
        -DCMAKE_BUILD_TYPE=Release >/dev/null
}
fetch() { # fetch <dir> <url> <tag>
    [ -d "$SRC/$1" ] || git -c advice.detachedHead=false clone -q --depth 1 -b "$3" "$2" "$SRC/$1"
}

# 2. Vulkan headers, and vulkan-1.dll's import library from their function list
if [ ! -f "$W/lib/libvulkan-1.a" ]; then
    say "Vulkan headers $VULKAN_HEADERS_TAG and the vulkan-1 import library"
    fetch vh https://github.com/KhronosGroup/Vulkan-Headers "$VULKAN_HEADERS_TAG"
    cmake_win "$SRC/vh" "$SRC/vh/b-win"
    cmake --install "$SRC/vh/b-win" >/dev/null
    python3 -I - "$W/include/vulkan/vulkan_core.h" "$W/include/vulkan/vulkan_win32.h" >"$W/vulkan-1.def" <<'EOF'
import re, sys
names = []
for path in sys.argv[1:]:
    for m in re.finditer(r'VKAPI_ATTR\s+[\w\s\*]+?\s+VKAPI_CALL\s+(vk\w+)\s*\(', open(path).read()):
        if m.group(1) not in names: names.append(m.group(1))
print("LIBRARY vulkan-1.dll\nEXPORTS")
for n in names: print("    " + n)
EOF
    mkdir -p "$W/lib"
    "$LM/bin/llvm-dlltool" -m i386:x86-64 -d "$W/vulkan-1.def" -l "$W/lib/libvulkan-1.a"
fi

# 3. SDL3, static
if [ ! -f "$W/lib/libSDL3.a" ]; then
    say "SDL3 $SDL3_TAG"
    fetch sdl https://github.com/libsdl-org/SDL "$SDL3_TAG"
    cmake_win "$SRC/sdl" "$SRC/sdl/b-win" -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF -DSDL_EXAMPLES=OFF
    cmake --build "$SRC/sdl/b-win" -j "$jobs" >/dev/null
    cmake --install "$SRC/sdl/b-win" >/dev/null
fi

# 4. shaderc (with glslang, SPIRV-Tools and SPIRV-Headers at the commits it pins), static
if [ ! -f "$W/lib/libshaderc_combined.a" ]; then
    say "shaderc $SHADERC_TAG (the longest step)"
    fetch shaderc https://github.com/google/shaderc "$SHADERC_TAG"
    (cd "$SRC/shaderc" && python3 utils/git-sync-deps >/dev/null)
    cmake_win "$SRC/shaderc" "$SRC/shaderc/b-win" -DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON \
        -DSHADERC_SKIP_COPYRIGHT_CHECK=ON -DSPIRV_SKIP_TESTS=ON -DSPIRV_SKIP_EXECUTABLES=ON \
        -DENABLE_GLSLANG_BINARIES=OFF -DSPIRV_TOOLS_BUILD_STATIC=ON -DSHADERC_ENABLE_SHARED_CRT=OFF
    cmake --build "$SRC/shaderc/b-win" --target shaderc_combined -j "$jobs" >/dev/null
    install -D -m 644 "$SRC/shaderc/b-win/libshaderc/libshaderc_combined.a" "$W/lib/libshaderc_combined.a"
    cp -r "$SRC/shaderc/libshaderc/include/shaderc" "$W/include/"
fi

# 5. The XMA decoder: the FFmpeg fork build_xma_decoder.sh pins, cross-compiled, decoder only
if [ ! -f "$W/ffmpeg-xma/lib/libavcodec.a" ]; then
    say "XMA decoder (FFmpeg xmaframes)"
    scripts/build_xma_decoder.sh --fetch
    rm -rf "$W/ffmpeg-src" && mkdir -p "$W/ffmpeg-src"
    git -C "$TP/FFmpeg" archive HEAD | tar -x -C "$W/ffmpeg-src"
    (
        cd "$W/ffmpeg-src"
        ./configure --prefix="$W/ffmpeg-xma" --enable-cross-compile --target-os=mingw32 --arch=x86_64 \
            --cc="$LM/bin/x86_64-w64-mingw32-clang" --cxx="$LM/bin/x86_64-w64-mingw32-clang++" \
            --ar="$LM/bin/llvm-ar" --ranlib="$LM/bin/llvm-ranlib" --nm="$LM/bin/llvm-nm" --strip="$LM/bin/llvm-strip" \
            --disable-everything --disable-autodetect --disable-programs --disable-doc \
            --disable-avdevice --disable-avformat --disable-swscale --disable-swresample --disable-avfilter \
            --disable-network --enable-decoder=xmaframes --enable-static --disable-shared --disable-debug \
            --disable-x86asm >/dev/null
        grep -q '^CONFIG_XMAFRAMES_DECODER=yes' ffbuild/config.mak ||
            { echo "[windows-deps] ERROR: configure did not enable the xmaframes decoder" >&2; exit 1; }
        make -s -j"$jobs" >/dev/null
        make -s install >/dev/null
    )
fi
say "ready: cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/llvm-mingw.cmake -DBB_PORTABLE=ON -DFFMPEG_XMA=$W/ffmpeg-xma"
