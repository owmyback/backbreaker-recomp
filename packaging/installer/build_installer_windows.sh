#!/usr/bin/env bash
# Builds the installer for Windows (#174): build-installer-windows/Backbreaker-Recompiled-windows-x86_64.zip,
# and the same folder unpacked beside it. Like the AppImage it holds no game content: the installer,
# the runtime kit, the open-source toolchain and XenonRecomp. The player unzips it and gives the
# installer their own disc image; the game is built on their PC.
#
#   packaging/installer/build_installer_windows.sh [-j N]
#
# Cross-compiled on Linux with llvm-mingw, so it runs on the host, not in the build container:
# it needs what scripts/windows_deps.sh needs (git, cmake, ninja, curl, python3, make) and
# third_party/XenonRecomp (the patched clone, as scripts/setup.sh makes it). No game files.
#
# The folder (installer/build.h has the bundle's layout):
#   Backbreaker-Recompiled.exe  the installer (backbreaker-installer.exe, under the name players and
#                               RecompLauncher start it by: the launcher takes a program with
#                               "install" in its name for a setup program, not the game)
#   README.txt                  how to use it
#   recomplauncher.json         RecompLauncher's manifest for Windows, made from the repository's
#   bundle/toolchain/   llvm-mingw's Windows release, cut down to what compiling and linking the
#                       game needs: clang, lld, the MinGW-w64 headers and x86-64 libraries (UCRT),
#                       libc++ and compiler-rt's builtins
#   bundle/kit/         the runtime kit, built with BB_PORTABLE (SDL3, shaderc, FFmpeg static)
#   bundle/xenonrecomp/ XenonRecomp.exe with patches/, its ppc_context.h, and SOURCE
#   bundle/config/      config/*.toml
#   bundle/MANIFEST.txt every part and version in it
#   bundle/NOTICES.txt  every third-party part's licence and source (notices_windows.sh)
# MANIFEST-windows.txt and NOTICES-windows.txt are copied beside the zip too, for the release.
set -euo pipefail

REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$REPO"
NAME=Backbreaker-Recompiled-windows-x86_64
OUT=build-installer-windows
STAGE=$OUT/$NAME
B=$STAGE/bundle
XENON=third_party/XenonRecomp
LM=third_party/llvm-mingw
# llvm-mingw's Windows release, at the tag scripts/windows_deps.sh pins for the cross-compiler
LLVM_MINGW_WIN_SHA256=552191b5754c49a5a20723276a6fc56541ea1922c4c50b7b669022333fc1b40f

jobs=$(nproc 2>/dev/null || echo 4)
while [ $# -gt 0 ]; do
    case "$1" in
        -j) shift; jobs=$1 ;;
        -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

say() { printf '[installer-build-windows] %s\n' "$*"; }
die() { printf '[installer-build-windows] ERROR: %s\n' "$*" >&2; exit 1; }

[ -d $XENON/.git ] || die "$XENON is missing: scripts/setup.sh clones and patches it (or README, \"XenonRecomp setup\")"
git -C $XENON apply --reverse --check "$REPO/patches/xenonrecomp-backbreaker-instructions.patch" 2>/dev/null ||
    die "$XENON doesn't have patches/xenonrecomp-backbreaker-instructions.patch applied (scripts/setup.sh applies it)"

say "the cross-compiler and the Windows libraries (scripts/windows_deps.sh)"
scripts/windows_deps.sh -j "$jobs"
tag=$(cat $LM/.tag)

rm -rf "$STAGE" "$OUT/$NAME.zip" "$OUT/MANIFEST-windows.txt" "$OUT/NOTICES-windows.txt"
mkdir -p "$B/xenonrecomp" "$B/config"

say "XenonRecomp.exe, built here ($OUT/xenonrecomp) from the patched clone"
# llvm-mingw's libc++ doesn't pull in <cstdlib> where XenonRecomp's {fmt} expects it, and one
# header asks for <Windows.h>, which is windows.h on a file system that tells them apart
mkdir -p $OUT/case-headers
echo '#include <windows.h>' >$OUT/case-headers/Windows.h
cmake -S $XENON -B $OUT/xenonrecomp -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$REPO/cmake/llvm-mingw.cmake" \
    -DCMAKE_CXX_FLAGS="-include cstdlib -I$REPO/$OUT/case-headers" -DCMAKE_EXE_LINKER_FLAGS=-static \
    -Wno-deprecated >/dev/null
ninja -C $OUT/xenonrecomp -j "$jobs" XenonRecomp >/dev/null
install -m 755 $OUT/xenonrecomp/XenonRecomp/XenonRecomp.exe "$B/xenonrecomp/"
install -m 644 $XENON/XenonUtils/ppc_context.h "$B/xenonrecomp/"
{
    echo "XenonRecomp $(git -C $XENON rev-parse HEAD)"
    echo "patches/xenonrecomp-backbreaker-instructions.patch $(sha256sum patches/xenonrecomp-backbreaker-instructions.patch | cut -d' ' -f1)"
} >"$B/xenonrecomp/SOURCE"
install -m 644 config/backbreaker.toml config/backbreaker_switch_tables.toml "$B/config/"

say "the runtime kit and the installer (BB_PORTABLE, no game files)"
cmake -S . -B $OUT/build -G Ninja -DCMAKE_TOOLCHAIN_FILE="$REPO/cmake/llvm-mingw.cmake" -DBB_PORTABLE=ON \
    -DBB_RUNTIME_ONLY=ON -DFFMPEG_XMA="$REPO/third_party/win64/ffmpeg-xma" >/dev/null
ninja -C $OUT/build -j "$jobs" backbreaker_runtime backbreaker-installer >/dev/null
cmake --install $OUT/build --component runtime --prefix "$B/kit" >/dev/null
install -m 755 $OUT/build/backbreaker-installer.exe "$STAGE/Backbreaker-Recompiled.exe"
$LM/bin/x86_64-w64-mingw32-strip "$STAGE/Backbreaker-Recompiled.exe"

# RecompLauncher reads recomplauncher.json from the top of a release's zip (its docs/DEVELOPERS.md,
# "Where the file goes"). The repository's file is the AppImage's; Windows differs in three things:
# the program, the game's name (bin/backbreaker.exe), and where the game keeps settings.ini
# (%LOCALAPPDATA%, the launcher's {local}; its {config} is AppData\Roaming there).
python3 -I - recomplauncher.json "$STAGE/recomplauncher.json" <<'PY'
import json, sys
m = json.load(open(sys.argv[1]))
appimage, exe = "Backbreaker-Recompiled-x86_64.AppImage", "Backbreaker-Recompiled.exe"
for step in ("prepare", "start"):
    if m[step]["program"] != [appimage]:
        sys.exit(f"recomplauncher.json: {step}.program isn't [{appimage}] any more: look at this script's changes for Windows")
    m[step]["program"] = [exe]
game, settings = "{install}/build/bin/backbreaker", "{config}/backbreaker-recomp/settings.ini"
if game not in m["prepare"]["done"] or settings not in m["saves"] or m["config"][0]["file"] != [{"path": settings}]:
    sys.exit("recomplauncher.json: the game's program or its settings file moved: look at this script's changes for Windows")
m["prepare"]["done"] = [d + ".exe" if d == game else d for d in m["prepare"]["done"]]
m["prepare"]["label"] = "Building the game from your disc: under a minute on a fast PC"
local = settings.replace("{config}", "{local}")
m["config"][0]["file"] = [{"path": local}]
m["saves"] = [local if s == settings else s for s in m["saves"]]
with open(sys.argv[2], "w", newline="\n") as f:
    json.dump(m, f, indent=2)
    f.write("\n")
PY

say "toolchain: llvm-mingw $tag for Windows, cut down"
zip=llvm-mingw-$tag-ucrt-x86_64.zip
dl=third_party/downloads/llvm-mingw-$tag
mkdir -p "$dl"
[ -f "$dl/$zip" ] || curl -fsSL -o "$dl/$zip" "https://github.com/mstorsjo/llvm-mingw/releases/download/$tag/$zip"
echo "$LLVM_MINGW_WIN_SHA256  $dl/$zip" | sha256sum -c --quiet - ||
    die "$dl/$zip doesn't match its checksum (a new llvm-mingw tag needs LLVM_MINGW_WIN_SHA256 set here)"
W=$OUT/llvm-mingw-win
if [ "$(cat "$W/.tag" 2>/dev/null)" != "$tag" ]; then
    rm -rf "$W" && mkdir -p "$W"
    python3 -I -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' "$dl/$zip" "$W"
    echo "$tag" >"$W/.tag"
fi
L=$W/llvm-mingw-$tag-ucrt-x86_64
T=$B/toolchain
clang_major=$(ls "$L/lib/clang")
mkdir -p "$T/bin" "$T/lib/clang/$clang_major/lib/windows" "$T/x86_64-w64-mingw32"
# clang.exe and clang++.exe are llvm-mingw's small starters for clang-NN.exe; the .cfg files give
# clang its defaults (compiler-rt, libunwind, libc++, lld); the DLLs are what the programs load
for f in clang.exe clang++.exe "clang-$clang_major.exe" ld.lld.exe "libLLVM-$clang_major.dll" libclang-cpp.dll \
    libc++.dll libunwind.dll libwinpthread-1.dll x86_64-w64-windows-gnu.cfg x86_64-pc-windows-gnu.cfg mingw32-common.cfg; do
    [ -f "$L/bin/$f" ] || die "llvm-mingw $tag has no bin/$f: the list here is for the layout of 20261104"
    cp "$L/bin/$f" "$T/bin/"
done
cp -r "$L/include" "$T/"
cp -r "$L/lib/clang/$clang_major/include" "$T/lib/clang/$clang_major/"
cp "$L/lib/clang/$clang_major/lib/windows/libclang_rt.builtins-x86_64.a" "$T/lib/clang/$clang_major/lib/windows/"
cp -r "$L/x86_64-w64-mingw32/lib" "$T/x86_64-w64-mingw32/"
cp "$L/LICENSE.TXT" "$T/"
mkdir -p "$T/share/mingw32"
cp "$L"/x86_64-w64-mingw32/share/mingw32/COPYING* "$T/share/mingw32/"

pin() { sed -n "s/^ARG $1=//p" packaging/portable/Containerfile; }
{
    echo "Backbreaker installer for Windows: the parts in this folder and their versions"
    echo "llvm-mingw $tag (https://github.com/mstorsjo/llvm-mingw/releases/tag/$tag, $zip, sha256 $LLVM_MINGW_WIN_SHA256): clang and lld $("$LM/bin/clang" --version | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1), MinGW-w64 (UCRT), libc++, compiler-rt"
    echo "SDL $(pin SDL3_TAG)"
    echo "shaderc $(pin SHADERC_TAG) (with glslang, SPIRV-Tools and SPIRV-Headers as it pins them)"
    echo "Vulkan-Headers $(pin VULKAN_HEADERS_TAG)"
    echo "XenonRecomp $(git -C $XENON rev-parse --short HEAD) + patches/xenonrecomp-backbreaker-instructions.patch"
    echo "FFmpeg (XMA decoder) $(sed -n 's/^FFMPEG_REV=\([0-9a-f]*\).*/\1/p' scripts/build_xma_decoder.sh)"
    echo "runtime kit $(cat "$B/kit/share/backbreaker-runtime/version.txt")"
} >"$B/MANIFEST.txt"

say "notices: every third-party part's licence and source"
packaging/installer/notices_windows.sh "$B/NOTICES.txt" "$tag"

# For Notepad: CRLF line ends
sed -e "s|@COMMIT@|$(git rev-parse --short HEAD 2>/dev/null || echo unknown)|" -e 's/$/\r/' \
    packaging/installer/README-windows.txt >"$STAGE/README.txt"

# Nothing from a game: no disc image, executable, translated code or title image
found=$(find "$STAGE" \( -iname '*.iso' -o -iname '*.xex' -o -iname '*.pe' -o -name 'ppc_recomp*' \
    -o -name 'ppc_func_mapping*' -o -name icon.png -o -name icon.ico -o -path '*/game/*' \) -print)
[ -z "$found" ] || die "game files in the folder: $found"

say "zip"
# The same inputs make the same archive: names sorted, one date (the commit's)
python3 -I - "$OUT" "$NAME" "$(git log -1 --format=%ct 2>/dev/null || echo 0)" <<'EOF'
import os, sys, time, zipfile
out, name, when = sys.argv[1], sys.argv[2], int(sys.argv[3])
date = time.gmtime(max(when, 315532800))[:6]  # a zip's dates start in 1980
paths = []
for root, dirs, files in os.walk(os.path.join(out, name)):
    dirs.sort()
    paths += [os.path.join(root, f) for f in sorted(files)]
with zipfile.ZipFile(os.path.join(out, name + ".zip"), "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for p in paths:
        info = zipfile.ZipInfo(os.path.relpath(p, out).replace(os.sep, "/"), date)
        info.compress_type = zipfile.ZIP_DEFLATED
        info.external_attr = 0o644 << 16
        with open(p, "rb") as f:
            z.writestr(info, f.read(), compresslevel=9)
EOF
cp "$B/MANIFEST.txt" "$OUT/MANIFEST-windows.txt"
cp "$B/NOTICES.txt" "$OUT/NOTICES-windows.txt"

say "done: $OUT/$NAME.zip ($(du -h "$OUT/$NAME.zip" | cut -f1)), unpacked $(du -sh "$STAGE" | cut -f1)"
say "  toolchain $(du -sh "$T" | cut -f1), kit $(du -sh "$B/kit" | cut -f1)"
