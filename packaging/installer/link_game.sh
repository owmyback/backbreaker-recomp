#!/usr/bin/env bash
# Links the game against a prebuilt runtime kit (#138): the step a player's machine runs after it has
# recompiled its own executable into ppc/ and made the import glue, and again, in seconds, whenever a
# new runtime comes out. ppc/ is compiled only when its objects are missing or older than their
# source or the kit's compile arguments.
#
#   packaging/installer/link_game.sh --kit <kit> --ppc <ppc dir> --imports <imports.cpp> --out <binary>
#       [--ppc-lib <libbackbreaker_ppc.a>]  use these already-compiled ppc/ objects instead
#       [--objdir <dir>]                     where compiled objects are kept (default: <out>.objs)
#       [--jobs N] [--cxx <clang++>]
#
# The kit comes from `cmake --install <build> --component runtime --prefix <kit>` (CMakeLists.txt,
# "The runtime kit"); its share/backbreaker-runtime/*.args files hold every compile and link
# argument, so nothing here repeats CMake's flags. This script is the prototype of what the
# installer does (packaging/installer/, #138) and the check that the kit's arguments stay in step
# with CMake's own link of build/backbreaker.
set -euo pipefail

kit="" ppc="" imports="" out="" ppc_lib="" objdir="" jobs="" cxx=${CXX:-clang++}

say() { printf '[link-game] %s\n' "$*"; }
die() { printf '[link-game] ERROR: %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --help | -h) sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        --*) [ $# -ge 2 ] || die "$1 needs a value" ;;&
        --kit) kit=$2 ;;
        --ppc) ppc=$2 ;;
        --imports) imports=$2 ;;
        --out) out=$2 ;;
        --ppc-lib) ppc_lib=$2 ;;
        --objdir) objdir=$2 ;;
        --jobs) jobs=$2 ;;
        --cxx) cxx=$2 ;;
        *) die "unknown argument '$1' (--help)" ;;
    esac
    shift 2
done
[ -n "$kit" ] && [ -n "$ppc" ] && [ -n "$imports" ] && [ -n "$out" ] || die "--kit, --ppc, --imports and --out are needed (--help)"
kit=$(cd -- "$kit" && pwd -P) || die "no kit folder"
ppc=$(cd -- "$ppc" && pwd -P) || die "no ppc/ folder" # -P: ppc/ is often a symlink, which find would not enter
share=$kit/share/backbreaker-runtime
[ -f "$share/game-link.args" ] || die "$kit is not a runtime kit (no share/backbreaker-runtime/game-link.args)"
[ -f "$imports" ] || die "no import glue at $imports"
objdir=${objdir:-$out.objs}
mkdir -p "$objdir"
jobs=${jobs:-$(nproc)}

# The layout check CMakeLists.txt does: the runtime indexes the function table with these values
layout() { grep -E '^#define PPC_(IMAGE|CODE)_(BASE|SIZE)[[:space:]]' "$1" | tr -s ' \t' ' ' | sort; }
[ -f "$ppc/ppc_config.h" ] || die "$ppc has no ppc_config.h"
if [ "$(layout "$ppc/ppc_config.h")" != "$(layout "$share/ppc_config.h")" ]; then
    die "ppc/ was recompiled from another executable than this runtime is built for (ppc_config.h differs)"
fi

# An .args file as an array: one argument per line, with the kit's placeholders filled in
read_args() { # read_args <file> <array name>
    local -n into=$2
    local line
    into=()
    while IFS= read -r line || [ -n "$line" ]; do
        [ -n "$line" ] || continue
        line=${line//@KIT@/$kit}
        line=${line//@PPC@/$ppc}
        into+=("$line")
    done <"$1"
}

# Compile one source to an object (skipped by the caller when the object is current)
compile() { # compile <args file> <source> <object>
    local args
    read_args "$1" args
    "$cxx" "${args[@]}" -c "$2" -o "$3.tmp" && mv "$3.tmp" "$3"
}

game=()
if [ -n "$ppc_lib" ]; then
    [ -f "$ppc_lib" ] || die "no ppc/ library at $ppc_lib"
    say "ppc/: the compiled objects in $ppc_lib"
    game+=("$ppc_lib")
else
    mapfile -t sources < <(find "$ppc" -maxdepth 1 -name '*.cpp' | sort)
    [ ${#sources[@]} -gt 0 ] || die "$ppc has no .cpp files"
    # Every ppc/ object depends on the kit's compile arguments and force-included header and on
    # ppc/'s own headers: when any of them changes, all of ppc/ is compiled again
    inputs=$(cat "$share/ppc-compile.args" "$kit/include/ppc_hooks.h" "$ppc"/*.h | sha256sum | cut -c1-64)
    if [ "$(cat "$objdir/ppc-inputs.sha256" 2>/dev/null)" != "$inputs" ]; then
        rm -f "$objdir"/ppc_*.o "$objdir/ppc-inputs.sha256"
    fi
    stale=()
    for s in "${sources[@]}"; do
        o=$objdir/$(basename "$s" .cpp).o
        [ "$o" -nt "$s" ] || stale+=("$s")
        game+=("$o")
    done
    say "ppc/: ${#sources[@]} files, ${#stale[@]} to compile ($jobs jobs)"
    if [ ${#stale[@]} -gt 0 ]; then
        export -f compile read_args
        export kit ppc cxx
        printf '%s\n' "${stale[@]}" | xargs -P "$jobs" -I{} bash -c \
            'compile "$0" "$1" "$2/$(basename "$1" .cpp).o"' "$share/ppc-compile.args" {} "$objdir" ||
            die "compiling ppc/ failed"
    fi
    echo "$inputs" >"$objdir/ppc-inputs.sha256"
fi

say "import glue: $imports" # one file: always compiled
compile "$share/imports-compile.args" "$imports" "$objdir/imports.o" || die "compiling the import glue failed"
game=("$objdir/imports.o" "${game[@]}")

read_args "$share/game-link.args" link
final=()
for a in "${link[@]}"; do
    if [ "$a" = "@GAME@" ]; then final+=("${game[@]}"); else final+=("$a"); fi
done
say "linking against the runtime $(cat "$share/version.txt" 2>/dev/null || echo '(no version.txt)')"
"$cxx" "${final[@]}" -o "$out.tmp" || die "linking failed"
mv "$out.tmp" "$out"
say "done: $out"
