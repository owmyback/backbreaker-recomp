#!/usr/bin/env bash
# shellcheck disable=SC2016 # dpkg-query's '${Field}' formats aren't shell expansions
# The installer bundle's third-party notices (#160): NOTICES.txt, which goes in the bundle beside
# MANIFEST.txt and is attached to each release. For every part of the AppImage that isn't this
# project's own code: where it is, its licence as its project ships it, and where its source is.
#
#   packaging/installer/notices.sh <out> <package>...
#
# <package>... are the Ubuntu and apt.llvm.org packages build_installer.sh copied files from. Runs
# in the build container (packaging/portable/Containerfile), which has their Debian copyright
# files, and the licences of what it builds from source in /usr/local/share/licenses.
set -euo pipefail

REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$REPO"
out=$1
shift
XENON=third_party/XenonRecomp
FFMPEG=third_party/ffmpeg-xma-portable/share/licenses/FFmpeg
LIC=/usr/local/share/licenses

die() { printf '[notices] ERROR: %s\n' "$*" >&2; exit 1; }
for f in LICENSE "$XENON/LICENSE.md" "$FFMPEG/COPYING.LGPLv2.1" "$LIC/SDL3/LICENSE.txt" "$LIC/shaderc/LICENSE"; do
    [ -f "$f" ] || die "$f is missing (the build container or scripts/build_xma_decoder.sh is older than notices.sh)"
done

repo=$(python3 -I -c 'import json, sys; print(json.load(open(sys.argv[1]))["repo"])' packaging/installer/recomplauncher.json)
rev=$(git rev-parse HEAD 2>/dev/null || echo unknown)

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
Backbreaker installer: third-party notices

Backbreaker-Recompiled-x86_64.AppImage builds the game from the player's own disc image. It holds
no game content: its programs, the runtime kit the game links against, and the open-source
toolchain and libraries below. Paths are inside the AppImage, under usr/lib/backbreaker-installer/
unless they say otherwise; MANIFEST.txt beside this file lists every package and version.

This project's own code (the installer, usr/bin/backbreaker-installer, and the runtime kit,
kit/lib/libbackbreaker_runtime.a and kit/include) is free software under the GNU General Public
License, version 3 or (at your option) any later version; its text is below. Its source:
https://github.com/$repo at $rev, also the "Source code" archives of each release.

The source of the GPL and LGPL parts below that no distribution keeps (XenonRecomp, FFmpeg's XMA
decoder and the AppImage runtime's libfuse) is in third-party-sources.tar.xz, attached to each
release beside the AppImage (packaging/installer/sources.sh makes it). The Ubuntu packages' source
is on Launchpad, linked with each one.
EOF
    text "Backbreaker's licence (LICENSE)" LICENSE

    part "Xenia (parts of the runtime kit and the installer derive from it)" \
        "kit/lib/libbackbreaker_runtime.a, usr/bin/backbreaker-installer" "BSD-3-Clause" \
        "https://github.com/xenia-project/xenia (each file names what it derives from)"
    text "Xenia" docs/licenses/xenia-LICENSE.txt

    part "XenonRecomp" "xenonrecomp/XenonRecomp, xenonrecomp/ppc_context.h" \
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
        "LGPL-2.1-or-later" "$(cat "$FFMPEG/SOURCE"), configured by this repository's scripts/build_xma_decoder.sh; also in third-party-sources.tar.xz"
    text "FFmpeg (LICENSE.md)" "$FFMPEG/LICENSE.md"
    text "FFmpeg (COPYING.LGPLv2.1)" "$FFMPEG/COPYING.LGPLv2.1"

    part "SDL 3" "kit/lib/libSDL3.a" "Zlib" "$(cat $LIC/SDL3/SOURCE)"
    text "SDL 3" $LIC/SDL3/LICENSE.txt
    part "shaderc, with glslang and SPIRV-Tools, built against SPIRV-Headers" "kit/lib/libshaderc_combined.a" \
        "Apache-2.0 (shaderc, SPIRV-Tools); glslang's and SPIRV-Headers' own, below" "$(cat $LIC/shaderc/SOURCE)"
    for name in glslang SPIRV-Tools SPIRV-Headers; do
        printf '  %s: %s\n' "$name" "$(cat "$LIC/$name/SOURCE")"
    done
    text "shaderc" $LIC/shaderc/LICENSE
    text "glslang" $LIC/glslang/LICENSE.txt
    text "SPIRV-Tools" $LIC/SPIRV-Tools/LICENSE
    text "SPIRV-Headers" $LIC/SPIRV-Headers/LICENSE

    part "The AppImage runtime" "the start of the AppImage file, before its file system" \
        "MIT, with LGPL-2.1, BSD and MIT libraries built in" "below; libfuse's, with the runtime's patch, also in third-party-sources.tar.xz"
    text "The AppImage runtime" packaging/installer/licenses/appimage-runtime.txt

    part "Ubuntu 22.04 and apt.llvm.org packages" \
        "toolchain/ (clang and lld, their headers and the libraries they load), sysroot/ (glibc, libstdc++, the Linux headers, the Vulkan loader)" \
        "each package's own, from its Debian copyright file below" "each package's below"
    # One copyright file per source package and text: a library and its -dev package share one
    declare -A shown=()
    licences=()
    for p in $(printf '%s\n' "$@" | sort -u); do
        version=$(dpkg-query -W -f='${Version}' "$p")
        src=$(dpkg-query -W -f='${source:Package}' "$p")
        srcversion=$(dpkg-query -W -f='${source:Version}' "$p")
        case $src in
            llvm-toolchain-*)
                commit=$(sed -n 's/.*+\([0-9a-f]\{12\}\)-.*/\1/p' <<<"$srcversion")
                where="apt.llvm.org's $src $srcversion, LLVM at https://github.com/llvm/llvm-project commit ${commit:-(see the version)}" ;;
            *) where="https://launchpad.net/ubuntu/+source/$src/$srcversion" ;;
        esac
        printf '\n%s %s: from %s\n' "$p" "$version" "$where"
        f=/usr/share/doc/$p/copyright
        [ -f "$f" ] || die "$f is missing"
        key=$(sha256sum <"$f" | cut -c1-16)
        if [ -n "${shown[$key]:-}" ]; then
            printf '  (its copyright file is the same as %s'"'"'s, above)\n' "${shown[$key]}"
            continue
        fi
        shown[$key]=$p
        text "$p (/usr/share/doc/$p/copyright)" "$f"
        while IFS= read -r l; do licences+=("$l"); done < <(grep -o '/usr/share/common-licenses/[A-Za-z0-9.+_-]*[A-Za-z0-9+_-]' "$f" || true)
    done

    if [ ${#licences[@]} -gt 0 ]; then
        printf '\n\n==================================================================================\n'
        printf 'The licence texts the copyright files above refer to (/usr/share/common-licenses)\n'
        # Each text once: GPL is a link to GPL-3, LGPL to LGPL-3
        declare -A names=()
        for l in $(printf '%s\n' "${licences[@]}" | sort -u); do
            [ -f "$l" ] || continue
            r=$(readlink -f "$l")
            names[$r]="${names[$r]:+${names[$r]}, }$l"
        done
        for r in $(printf '%s\n' "${!names[@]}" | sort); do
            case ", ${names[$r]}," in
                *", $r,"*) text "${names[$r]}" "$r" ;;
                *) text "${names[$r]} (the text of $r)" "$r" ;;
            esac
        done
    fi
} >"$out"
