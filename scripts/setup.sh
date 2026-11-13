#!/usr/bin/env bash
# One-step setup: from an image made from your own retail disc to a built build/backbreaker.
#
#   scripts/setup.sh <path-to-your-disc-image> [options]
#
# Steps are skipped when their output already exists, so re-running is cheap and safe.
# Nothing about the game is downloaded and the image path is never printed, stored or
# committed: it only appears in the command lines below, and any tool output that could
# quote it goes through scrub().
#
# The steps mirror README.md ("Reproduce extraction", "XenonRecomp setup", "Recompile")
# and docs/runtime.md ("Build and run"). See those for what each artifact is for.
set -euo pipefail

REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$REPO"

XENON=third_party/XenonRecomp
XENON_REV=ddd128b                                    # the pin README.md and CI use
XENON_BIN=$XENON/build/XenonRecomp/XenonRecomp
PATCH=patches/xenonrecomp-backbreaker-instructions.patch
DISC_STAMP=game/.disc-extracted                      # extraction has no other "done" marker

iso=""
jobs=""
force=0
regenerate=0
check_only=0

usage() {
    cat <<'TXT'
usage: scripts/setup.sh <path-to-your-disc-image> [options]

Options:
  --jobs N          build with N compile jobs (default: ninja's choice)
  --force           redo every step, even the ones whose output exists
  --regenerate      re-derive config/backbreaker_switch_tables.toml and the leaf-function
                    block of config/backbreaker.toml from your image (two recompile passes;
                    restores are one "git checkout config" away)
  --check-only      only check the prerequisites and exit
  --help            this text

The image is your own: nothing here tells you where to get a game, and no game file is
committed - game/, ppc/ and third_party/ are gitignored.
TXT
}

say() { printf '[setup] %s\n' "$*"; }
done_step() { printf '[setup] already done: %s\n' "$*"; }
warn() { printf '[setup] WARNING: %s\n' "$*" >&2; }
die() { printf '[setup] ERROR: %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --jobs) [ $# -ge 2 ] || die "--jobs needs a number"; jobs=$2; shift ;;
        --jobs=*) jobs=${1#*=} ;;
        --force) force=1 ;;
        --regenerate) regenerate=1 ;;
        --check-only) check_only=1 ;;
        --help | -h) usage; exit 0 ;;
        -*) die "unknown option '$1' (--help)" ;;
        *) [ -z "$iso" ] || die "give exactly one image path"; iso=$1 ;;
    esac
    shift
done

# Replace the image path in anything a tool might print (python tracebacks quote it).
scrub() {
    [ -n "$iso" ] || { cat; return; }
    local line
    while IFS= read -r line || [ -n "$line" ]; do
        printf '%s\n' "${line//"$iso"/<your image>}"
    done
}

# Run a command, keeping its stderr out of the log unless it fails (then print it scrubbed).
run_quiet() {
    local err rc=0
    err=$(mktemp)
    "$@" 2>"$err" || rc=$?
    if [ "$rc" -ne 0 ]; then
        printf '[setup] ERROR: %s failed (exit %s)\n' "${1##*/}" "$rc" >&2
        scrub <"$err" >&2
        rm -f "$err"
        exit 1
    fi
    rm -f "$err"
}

# --- prerequisites -------------------------------------------------------------------------

missing=()

pkg_hint() { # package names for the missing build dependencies, by distribution
    if [ -x /usr/bin/dnf ] || [ -x /usr/bin/dnf5 ]; then
        echo "  Fedora/Fedora Rawhide: sudo dnf install clang cmake ninja python3 git openssl-devel SDL3-devel vulkan-headers libshaderc-devel vulkan-loader-devel"
    elif [ -x /usr/bin/apt ]; then
        echo "  Debian/Ubuntu: sudo apt install clang cmake ninja-build python3 git libssl-dev libsdl3-dev libvulkan-dev libshaderc-dev"
    elif [ -x /usr/bin/pacman ]; then
        echo "  Arch: sudo pacman -S clang cmake ninja python3 git openssl sdl3 vulkan-headers shaderc"
    else
        echo "  install: clang 18+, cmake 3.20+, ninja, python3, git, openssl, and the SDL3, Vulkan and shaderc development packages"
    fi
}

version_at_least() { # version_at_least 3.20 4.3 -> true (major.minor comparison)
    awk -v need="$1" -v got="$2" 'BEGIN { split(need, n, "."); split(got, g, ".")
        exit !(g[1] > n[1] || (g[1] == n[1] && g[2] >= n[2])) }'
}

check_prerequisites() {
    local cv cm
    for tool in git cmake ninja python3 openssl; do
        command -v "$tool" >/dev/null 2>&1 || missing+=("$tool is not installed or not on PATH")
    done
    if command -v clang++ >/dev/null 2>&1; then
        cv=$(clang++ -dumpversion 2>/dev/null || echo 0)
        version_at_least 18.0 "$cv" ||
            missing+=("clang 18+ is needed (found $cv): the recompiled code is built with clang and uses C++20")
    else
        missing+=("clang++ 18+ is not installed (the recompiled code only builds with clang)")
    fi
    if command -v cmake >/dev/null 2>&1; then
        cm=$(cmake --version 2>/dev/null | head -1 | grep -oE '[0-9]+\.[0-9]+' | head -1)
        version_at_least 3.20 "$cm" || missing+=("cmake 3.20+ is needed (found $cm)")
    fi

    # The runtime needs these development packages; a small probe says what is missing
    # before CMake gets as far as a wall of errors.
    local probe rc=0 flags
    probe=$(mktemp --suffix=.cpp)
    printf '#include <SDL3/SDL.h>\n#include <vulkan/vulkan.h>\n#include <shaderc/shaderc.hpp>\nint main() { return 0; }\n' >"$probe"
    if command -v pkg-config >/dev/null 2>&1; then
        flags=$(pkg-config --cflags --libs sdl3 vulkan shaderc 2>/dev/null) || flags=""
    else
        flags="-I/usr/include/SDL3 -lSDL3 -lvulkan -lshaderc_shared"
    fi
    if [ -n "$flags" ]; then
        # $flags is deliberately unquoted: it is a list of compiler switches.
        clang++ -std=c++20 -o "$probe.bin" "$probe" $flags 2>/dev/null || rc=1
    else
        rc=1 # pkg-config could not find them; ask the compiler with the plain library names
        clang++ -std=c++20 -I/usr/include/SDL3 -o "$probe.bin" "$probe" -lSDL3 -lvulkan -lshaderc_shared 2>/dev/null || rc=1
    fi
    [ "$rc" -eq 0 ] || missing+=("the SDL3, Vulkan and shaderc development packages (the window and the renderer need them)")
    rm -f "$probe" "$probe.bin"

    if [ ${#missing[@]} -ne 0 ]; then
        printf '[setup] missing prerequisites:\n' >&2
        for m in "${missing[@]}"; do printf '  - %s\n' "$m" >&2; done
        printf '%s\n' "$(pkg_hint)" >&2
        exit 1
    fi
    say "prerequisites look good (clang $(clang++ -dumpversion), cmake $(cmake --version | head -1 | grep -oE '[0-9]+\.[0-9]+' | head -1), ninja $(ninja --version), python $(python3 -V 2>&1 | cut -d' ' -f2))"
}

# --- steps ---------------------------------------------------------------------------------

check_image() { # a plain message instead of a python traceback that would quote the path
    [ -n "$iso" ] || die "this step needs the path of your disc image as an argument"
    local out
    out=$(python3 -I - "$iso" <<'PY' | scrub
import sys
SEC, MAG = 2048, b'MICROSOFT*XBOX*MEDIA'
try:
    iso = open(sys.argv[1], 'rb')
except OSError as e:
    print('cannot open the image (%s)' % e.strerror); raise SystemExit
iso.seek(0, 2)
size = iso.tell()
for off in (0x0, 0xFD90000, 0x2080000, 0x18300000):  # same offsets tools/xdvdfs.py probes
    if size >= off + 33 * SEC:
        iso.seek(off + 32 * SEC)
        if iso.read(20) == MAG:
            print('ok'); break
else:
    print('no Xbox 360 file system at the usual offsets (image is %d bytes)' % size)
PY
)
    [ "$out" = "ok" ] || die "the image is not usable: $out"
}

step_xex() {
    if [ $force -eq 0 ] && [ -s game/extracted/default.xex ]; then
        done_step "game/extracted/default.xex"
        return
    fi
    check_image
    mkdir -p game/extracted
    # The same pass prints the disc's file-name listing; keep it where README.md keeps it,
    # but never overwrite the one that is already committed.
    local listing=/dev/null
    [ -f docs/disc-listing.txt ] || listing=docs/disc-listing.txt
    say "extracting default.xex from your image (file-name listing -> $listing)"
    run_quiet python3 -I tools/xdvdfs.py "$iso" game/extracted default.xex >"$listing"
}

step_pe() {
    if [ $force -eq 0 ] && [ -s game/extracted/default.pe ] &&
        [ game/extracted/default.pe -nt game/extracted/default.xex ]; then
        done_step "game/extracted/default.pe (decrypted executable)"
        return
    fi
    say "decrypting the executable (needs openssl)"
    run_quiet python3 -I tools/xex_decrypt.py game/extracted/default.xex game/extracted/default.pe
}

disc_extracted() {
    # tools/xdvdfs.py leaves no marker, so this step has its own stamp file; an extraction
    # done by hand as README.md describes it counts too (the disc holds 24 files, so a
    # truncated one is missing most of them).
    [ -f "$DISC_STAMP" ] && return 0
    [ -f game/disc/default.xex ] && [ "$(find game/disc -type f | wc -l)" -ge 20 ]
}

step_disc() {
    # The runtime reads game/disc at run time for the "game:" device.
    if [ $force -eq 0 ] && disc_extracted; then
        done_step "game/disc (the whole disc)"
        return
    fi
    check_image
    mkdir -p game/disc
    say "extracting the whole disc to game/disc (about 1.1 GB, a few minutes)"
    run_quiet python3 -I tools/xdvdfs.py "$iso" game/disc --all >/dev/null
    say "disc extracted: $(du -sh game/disc | cut -f1)"
    : >"$DISC_STAMP"
}

step_xenonrecomp() {
    if [ ! -d $XENON/.git ]; then
        say "cloning XenonRecomp at $XENON_REV with its submodules"
        run_quiet git clone --recursive https://github.com/hedge-dev/XenonRecomp.git "$XENON"
        run_quiet git -C "$XENON" checkout --quiet "$XENON_REV"
        run_quiet git -C "$XENON" submodule update --init --recursive
    elif [ "$(git -C "$XENON" rev-parse HEAD)" != "$(git -C "$XENON" rev-parse "$XENON_REV")" ]; then
        say "XenonRecomp is at another commit - checking out $XENON_REV"
        run_quiet git -C "$XENON" checkout --quiet "$XENON_REV"
        run_quiet git -C "$XENON" submodule update --init --recursive
    fi

    if git -C "$XENON" apply --reverse --check "$REPO/$PATCH" >/dev/null 2>&1; then
        done_step "the instruction patch"
    else
        say "applying $(basename "$PATCH") (the instructions Backbreaker needs)"
        run_quiet git -C "$XENON" apply "$REPO/$PATCH"
    fi

    if [ $force -eq 0 ] && [ -x "$XENON_BIN" ] && [ "$XENON_BIN" -nt "$PATCH" ]; then
        done_step "XenonRecomp ($XENON_BIN)"
        return
    fi
    say "building XenonRecomp"
    run_quiet cmake -S "$XENON" -B "$XENON/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
    run_quiet ninja -C "$XENON/build" ${jobs:+-j"$jobs"}
}

recompile() { # one XenonRecomp pass over the image with the config in config/
    mkdir -p ppc build
    say "recompiling the executable to ppc/ (a few minutes)"
    run_quiet "$XENON_BIN" config/backbreaker.toml "$XENON/XenonUtils/ppc_context.h" \
        | tee build/recompile.log
    say "recompiled: $(ls ppc/ppc_recomp.*.cpp | wc -l) C++ files, $(grep -cE '\{ 0x[0-9A-Fa-f]+, sub_' ppc/ppc_func_mapping.cpp || true) functions, $(grep -ciE 'warning|error' build/recompile.log || true) warnings (full output: build/recompile.log)"
}

step_recompile() {
    if [ $force -eq 0 ] && [ $regenerate -eq 0 ] && [ -s ppc/ppc_func_mapping.cpp ]; then
        done_step "ppc/ (recompiled output)"
        return
    fi
    recompile
    if [ $regenerate -eq 0 ]; then
        return
    fi
    # Two passes: tools/function_bounds.py needs the function map of a previous run, so the
    # first pass above uses the committed config and the second one uses the regenerated one.
    say "regenerating config/backbreaker_switch_tables.toml and the leaf-function block"
    run_quiet python3 -I tools/find_switch_tables.py game/extracted/default.pe config/backbreaker_switch_tables.toml
    run_quiet python3 -I tools/function_bounds.py game/extracted/default.pe \
        config/backbreaker_switch_tables.toml config/backbreaker.toml ppc/ppc_func_mapping.cpp
    warn "config/*.toml were rewritten: check 'git diff config' and commit them only if they are better than the checked-in ones ('git checkout config' restores them)."
    recompile
}

step_imports() {
    if [ $force -eq 0 ] && [ -f runtime/generated/imports.cpp ] &&
        [ runtime/generated/imports.cpp -nt game/extracted/default.xex ]; then
        done_step "runtime/generated/imports.cpp"
        return
    fi
    say "generating the import glue (runtime/generated/imports.cpp)"
    run_quiet python3 -I tools/gen_imports.py game/extracted/default.xex game/extracted/default.pe \
        "$XENON/XenonUtils/xbox/xboxkrnl_table.inc" "$XENON/XenonUtils/xbox/xam_table.inc" \
        runtime/generated/imports.cpp
    if ! git diff --quiet runtime/generated/imports.cpp 2>/dev/null; then
        warn "runtime/generated/imports.cpp changed from the committed version - check 'git diff runtime/generated/imports.cpp'."
    fi
}

step_build() {
    say "configuring and building the runtime (build/backbreaker)"
    run_quiet cmake -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
    run_quiet ninja -C build ${jobs:+-j"$jobs"}
}

step_tests() { # same as CI: these need no game files
    local X=$XENON
    say "running the instruction and tiling tests (no game files, same as CI)"
    run_quiet clang++ -std=c++20 -O2 -march=x86-64-v3 -I tests/ppc_config_stub \
        -I "$X/XenonUtils" -I "$X/thirdparty/simde" tests/instr_semantics.cpp -o build/instr_semantics
    run_quiet ./build/instr_semantics
    run_quiet clang++ -std=c++20 -Wall -Wextra -O2 tests/tiling_test.cpp tests/tiling_test_odr.cpp \
        -o build/tiling_test
    run_quiet ./build/tiling_test
}

# --- main ----------------------------------------------------------------------------------

check_prerequisites
if [ $check_only -eq 1 ]; then
    exit 0
fi
if [ -z "$iso" ]; then
    if [ -s game/extracted/default.xex ]; then
        say "no image given and game/extracted/default.xex already exists - continuing with the files in game/"
    else
        usage
        die "give the path of an image made from your own retail disc"
    fi
fi

# Cheap checks before touching the image, so a wrong path fails before a long build.
if [ -n "$iso" ]; then
    # Check the path before any long step; the path itself is never printed on failure.
    [ -e "$iso" ] || die "there is no file at that image path (it is never printed or stored)"
    [ -r "$iso" ] || die "the image is not readable (permissions?)"
fi

step_xex
step_pe
step_disc
step_xenonrecomp
step_recompile
step_imports
step_build
step_tests

say "done."
say "run it from this directory: ./build/backbreaker --timeout 30   (Ctrl-C stops it)"
say "docs/runtime.md lists the other options; --press \"a@20,a@26\" gets past the title screen."
