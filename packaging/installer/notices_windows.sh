#!/usr/bin/env bash
# The Windows installer's third-party notices (#174): NOTICES.txt in the zip's bundle, attached to
# each release as NOTICES-windows.txt. For every part of the zip that isn't this project's own
# code: where it is, its licence as its project ships it, and where its source is.
# packaging/installer/notices.sh does the same for the AppImage.
#
#   packaging/installer/notices_windows.sh <out> <llvm-mingw tag>
#
# Runs on the host after scripts/windows_deps.sh and build_installer_windows.sh's toolchain step:
# the licences come from the sources those fetched (third_party/downloads/src, third_party/win64)
# and from llvm-mingw's Windows release (build-installer-windows/llvm-mingw-win).
set -euo pipefail

REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$REPO"
out=$1
tag=$2
XENON=third_party/XenonRecomp
SRC=third_party/downloads/src
FFMPEG=third_party/win64/ffmpeg-src
L=build-installer-windows/llvm-mingw-win/llvm-mingw-$tag-ucrt-x86_64
MINGW=$L/x86_64-w64-mingw32/share/mingw32

die() { printf '[notices-windows] ERROR: %s\n' "$*" >&2; exit 1; }
for f in LICENSE "$XENON/LICENSE.md" "$FFMPEG/COPYING.LGPLv2.1" "$FFMPEG/LICENSE.md" "$SRC/sdl/LICENSE.txt" "$SRC/shaderc/LICENSE" \
    "$SRC/shaderc/third_party/glslang/LICENSE.txt" "$SRC/shaderc/third_party/spirv-tools/LICENSE" \
    "$SRC/shaderc/third_party/spirv-headers/LICENSE" "$SRC/vh/LICENSE.md" "$L/LICENSE.TXT" "$MINGW/COPYING.MinGW-w64-runtime.txt"; do
    [ -f "$f" ] || die "$f is missing (run scripts/windows_deps.sh and packaging/installer/build_installer_windows.sh)"
done

repo=$(python3 -I -c 'import json, sys; print(json.load(open(sys.argv[1]))["repo"])' recomplauncher.json)
rev=$(git rev-parse HEAD 2>/dev/null || echo unknown)
pin() { sed -n "s/^ARG $1=//p" packaging/portable/Containerfile; }
ffmpeg_rev=$(sed -n 's/^FFMPEG_REV=\([0-9a-f]*\).*/\1/p' scripts/build_xma_decoder.sh)
ffmpeg_url=$(sed -n 's/^FFMPEG_URL=//p' scripts/build_xma_decoder.sh | tr -d '"')

part() { # $1 = name, $2 = where in the bundle, $3 = licence, $4 = source
    printf '\n\n==================================================================================\n'
    printf '%s\n  in:      %s\n  licence: %s\n  source:  %s\n' "$1" "$2" "$3" "$4"
}
text() { # $1 = title, $2... = files
    local title=$1
    shift
    printf '\n---- %s ----\n\n' "$title"
    cat "$@"
}
comment() { sed -n '1,/\*\//p' "$1"; } # a source file's licence comment

{
    cat <<EOF
Backbreaker installer for Windows: third-party notices

Backbreaker-Recompiled-windows-x86_64.zip builds the game from the player's own disc image. It
holds no game content: its programs, the runtime kit the game links against, and the open-source
toolchain and libraries below. Paths are inside the zip's folder, under bundle/ unless they say
otherwise; MANIFEST.txt beside this file lists every part and version.

This project's own code (the installer, Backbreaker-Recompiled.exe, and the runtime kit,
kit/lib/libbackbreaker_runtime.a and kit/include) is free software under the GNU General Public
License, version 3 or (at your option) any later version; its text is below. Its source:
https://github.com/$repo at $rev, also the "Source code" archives of each release.

The source of the GPL and LGPL parts below (XenonRecomp and FFmpeg's XMA decoder) is in
third-party-sources.tar.xz, attached to each release (packaging/installer/sources.sh makes it).
EOF
    text "Backbreaker's licence (LICENSE)" LICENSE

    part "Xenia (parts of the runtime kit and the installer derive from it)" \
        "kit/lib/libbackbreaker_runtime.a, Backbreaker-Recompiled.exe" "BSD-3-Clause" \
        "https://github.com/xenia-project/xenia (each file names what it derives from)"
    text "Xenia" docs/licenses/xenia-LICENSE.txt

    part "XenonRecomp" "xenonrecomp/XenonRecomp.exe, xenonrecomp/ppc_context.h" \
        "MIT; the program also has binutils' PowerPC disassembler (GPL-2.0-or-later) and libmspack's LZX decoder (LGPL-2.1) built in, so as a whole it is under GPL-2.0-or-later" \
        "https://github.com/hedge-dev/XenonRecomp at $(git -C $XENON rev-parse HEAD), with this repository's patches/xenonrecomp-backbreaker-instructions.patch; both in third-party-sources.tar.xz"
    printf '\nIts submodules at that commit:\n'
    git -C $XENON submodule status | sed 's/^[ +-]/  /'
    text "XenonRecomp" $XENON/LICENSE.md
    text "disasm (from binutils' ppc-dis.c, built into XenonRecomp)" <(comment $XENON/thirdparty/disasm/ppc-dis.c)
    text "libmspack (lzxd.c, built into XenonRecomp)" $XENON/thirdparty/libmspack/libmspack/COPYING.LIB
    text "tiny-AES-c (built into XenonRecomp)" $XENON/thirdparty/tiny-AES-c/unlicense.txt
    text "TinySHA1 (built into XenonRecomp)" <(comment $XENON/thirdparty/TinySHA1/TinySHA1.hpp)
    text "{fmt} (built into XenonRecomp)" $XENON/thirdparty/fmt/LICENSE
    text "toml++ (built into XenonRecomp)" $XENON/thirdparty/tomlplusplus/LICENSE
    text "xxHash (built into XenonRecomp)" $XENON/thirdparty/xxHash/LICENSE

    part "SIMDe" "kit/include/simde" "MIT" "XenonRecomp's thirdparty/simde submodule (above)"
    text "SIMDe" $XENON/thirdparty/simde/COPYING

    part "FFmpeg (libavcodec and libavutil, with only its XMA decoder)" "kit/lib/libavcodec.a, kit/lib/libavutil.a" \
        "LGPL-2.1-or-later" "$ffmpeg_url at $ffmpeg_rev (branch xmaframes), configured by this repository's scripts/windows_deps.sh; also in third-party-sources.tar.xz"
    text "FFmpeg (LICENSE.md)" "$FFMPEG/LICENSE.md"
    text "FFmpeg (COPYING.LGPLv2.1)" "$FFMPEG/COPYING.LGPLv2.1"

    part "SDL 3" "kit/lib/libSDL3.a" "Zlib" "https://github.com/libsdl-org/SDL $(pin SDL3_TAG) ($(git -C $SRC/sdl rev-parse HEAD))"
    text "SDL 3" $SRC/sdl/LICENSE.txt
    part "shaderc, with glslang and SPIRV-Tools, built against SPIRV-Headers" "kit/lib/libshaderc_combined.a" \
        "Apache-2.0 (shaderc, SPIRV-Tools); glslang's and SPIRV-Headers' own, below" \
        "https://github.com/google/shaderc $(pin SHADERC_TAG) ($(git -C $SRC/shaderc rev-parse HEAD))"
    for name in glslang spirv-tools spirv-headers; do
        printf '  %s: %s at %s, as shaderc %s pins it\n' "$name" "$(git -C "$SRC/shaderc/third_party/$name" remote get-url origin)" \
            "$(git -C "$SRC/shaderc/third_party/$name" rev-parse HEAD)" "$(pin SHADERC_TAG)"
    done
    text "shaderc" $SRC/shaderc/LICENSE
    text "glslang" $SRC/shaderc/third_party/glslang/LICENSE.txt
    text "SPIRV-Tools" $SRC/shaderc/third_party/spirv-tools/LICENSE
    text "SPIRV-Headers" $SRC/shaderc/third_party/spirv-headers/LICENSE

    part "Vulkan-Headers (the import library for vulkan-1.dll is made from their function list)" "kit/lib/libvulkan-1.a" \
        "Apache-2.0 OR MIT" "https://github.com/KhronosGroup/Vulkan-Headers $(pin VULKAN_HEADERS_TAG)"
    text "Vulkan-Headers" $SRC/vh/LICENSE.md

    part "llvm-mingw $tag: clang, lld, libc++, libunwind and compiler-rt (LLVM), and MinGW-w64's headers and libraries" \
        "toolchain/ (also linked into Backbreaker-Recompiled.exe, xenonrecomp/XenonRecomp.exe and the game the player builds: libc++, libunwind, compiler-rt's builtins and MinGW-w64's runtime)" \
        "Apache-2.0 with LLVM exceptions (LLVM); MinGW-w64's own, below (the Zope Public License 2.1 and public-domain and permissive notices; winpthreads is MIT and BSD-3-Clause)" \
        "https://github.com/mstorsjo/llvm-mingw/releases/tag/$tag (llvm-mingw-$tag-ucrt-x86_64.zip, unchanged files from it), which names the LLVM and MinGW-w64 commits it builds"
    text "LLVM (toolchain/LICENSE.TXT)" "$L/LICENSE.TXT"
    for f in COPYING COPYING.MinGW-w64.txt COPYING.MinGW-w64-runtime.txt COPYING.winpthreads.txt; do
        [ -f "$MINGW/$f" ] && text "MinGW-w64 (toolchain/share/mingw32/$f)" "$MINGW/$f"
    done
} >"$out"
