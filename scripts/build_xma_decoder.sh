#!/usr/bin/env bash
# Build the XMA audio decoder the runtime links (issue #93): FFmpeg's WMA Pro code with the
# "xmaframes" decoder, which decodes ONE XMA frame per packet - the unit the Xbox 360's XMA
# hardware works in. Upstream FFmpeg has no such decoder; it comes from the fork Xenia uses
# (xenia-canary's third_party/FFmpeg submodule, branch "xmaframes").
#
#   scripts/build_xma_decoder.sh [--force] [-j N] [--prefix DIR] [--build-dir DIR]
#
# --prefix installs somewhere else (the portable build's container uses its own, #134);
# --build-dir builds from a clean copy of the source there instead of in third_party/FFmpeg.
#
# Fetches the fork at a pinned commit into third_party/FFmpeg (shallow, about 20 MB) and
# installs a static, decoder-only libavcodec + libavutil into third_party/ffmpeg-xma/, which
# CMake picks up (see CMakeLists.txt). Both are gitignored. FFmpeg is LGPL-2.1-or-later.
# Without it the runtime still builds and runs; XMA sounds are then silent.
set -euo pipefail

REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$REPO"

FFMPEG_URL=https://github.com/has207/FFmpeg.git
FFMPEG_REV=dfc5a3993b405f3a8b382756828cdfae06380c8b # branch xmaframes, 2026-11-07
SRC=third_party/FFmpeg
PREFIX=$REPO/third_party/ffmpeg-xma
STAMP=$PREFIX/.rev
BUILD_DIR=

force=0
jobs=$(nproc 2>/dev/null || echo 4)
while [ $# -gt 0 ]; do
    case "$1" in
        --force) force=1 ;;
        -j) shift; jobs=$1 ;;
        -j*) jobs=${1#-j} ;;
        --prefix) shift; PREFIX=$(mkdir -p "$1" && cd "$1" && pwd); STAMP=$PREFIX/.rev ;;
        --build-dir) shift; BUILD_DIR=$1 ;;
        -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

say() { printf '[xma-decoder] %s\n' "$*"; }

if [ $force -eq 0 ] && [ -f "$PREFIX/lib/libavcodec.a" ] && [ "$(cat "$STAMP" 2>/dev/null)" = "$FFMPEG_REV" ]; then
    say "already built at ${FFMPEG_REV:0:10} ($PREFIX)"
    exit 0
fi

for tool in git make clang pkg-config; do
    command -v "$tool" >/dev/null 2>&1 || { echo "[xma-decoder] ERROR: $tool is not installed" >&2; exit 1; }
done

if [ ! -d "$SRC/.git" ]; then
    say "fetching the FFmpeg xmaframes fork at ${FFMPEG_REV:0:10}"
    git init -q "$SRC"
    git -C "$SRC" remote add origin "$FFMPEG_URL"
fi
if [ "$(git -C "$SRC" rev-parse -q --verify HEAD 2>/dev/null)" != "$FFMPEG_REV" ]; then
    git -C "$SRC" fetch -q --depth 1 origin "$FFMPEG_REV"
    git -C "$SRC" checkout -q --detach FETCH_HEAD
fi

# Assembly needs nasm; the C fallbacks are fine for a few dozen voices.
asm=--disable-x86asm
command -v nasm >/dev/null 2>&1 && asm=""

say "configuring (xmaframes decoder only, static)"
if [ -n "$BUILD_DIR" ]; then
    rm -rf "$BUILD_DIR" && mkdir -p "$BUILD_DIR"
    git -C "$SRC" archive HEAD | tar -x -C "$BUILD_DIR"
    cd "$BUILD_DIR"
else
    cd "$SRC"
fi
# shellcheck disable=SC2086 # $asm is empty or one switch
./configure --prefix="$PREFIX" --cc=clang \
    --disable-everything --disable-autodetect --disable-programs --disable-doc \
    --disable-avdevice --disable-avformat --disable-swscale --disable-swresample --disable-avfilter \
    --disable-network --enable-decoder=xmaframes \
    --enable-static --disable-shared --enable-pic --disable-debug $asm >/dev/null
grep -q "^CONFIG_XMAFRAMES_DECODER=yes" ffbuild/config.mak ||
    { echo "[xma-decoder] ERROR: configure did not enable the xmaframes decoder" >&2; exit 1; }

say "building with $jobs jobs"
make -s -j"$jobs" >/dev/null
make -s install >/dev/null
echo "$FFMPEG_REV" >"$STAMP"
say "installed to $PREFIX ($(du -sh "$PREFIX/lib" | cut -f1) of static libraries)"
