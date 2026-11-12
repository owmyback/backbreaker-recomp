# Backbreaker (Xbox 360) static recompilation

Exploratory project to statically recompile Backbreaker (2010, NaturalMotion) for native Linux
using XenonRecomp. Requires a legally owned disc image.

## Layout
- `tools/`        - our own helper scripts (disc extraction, XEX decryption)
- `game/`         - extracted/decrypted game files (gitignored, copyrighted)
- `third_party/`  - upstream clones such as XenonRecomp (gitignored)
- `docs/`         - notes and analysis results

## Source ISO
An ISO image made from your own retail Backbreaker disc (its path is not recorded in this repository).

## Reproduce extraction
    python3 -I tools/xdvdfs.py "<iso>" game/extracted default.xex > docs/disc-listing.txt
    python3 -I tools/xex_decrypt.py game/extracted/default.xex game/extracted/default.pe

## XenonRecomp setup
    git clone --recursive https://github.com/hedge-dev/XenonRecomp.git third_party/XenonRecomp
    git -C third_party/XenonRecomp checkout ddd128b
    git -C third_party/XenonRecomp apply ../../patches/xenonrecomp-backbreaker-instructions.patch
    cmake -S third_party/XenonRecomp -B third_party/XenonRecomp/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
    ninja -C third_party/XenonRecomp/build

## Recompile
    python3 -I tools/find_switch_tables.py game/extracted/default.pe config/backbreaker_switch_tables.toml
    # function_bounds.py adds leaf-function switch bounds and pointer-only functions; pass the
    # ppc_func_mapping.cpp of a previous run so the analyzer's own functions are not duplicated
    python3 -I tools/function_bounds.py game/extracted/default.pe config/backbreaker_switch_tables.toml config/backbreaker.toml ppc/ppc_func_mapping.cpp
    mkdir -p ppc && third_party/XenonRecomp/build/XenonRecomp/XenonRecomp config/backbreaker.toml third_party/XenonRecomp/XenonUtils/ppc_context.h

## Tests
    X=third_party/XenonRecomp
    mkdir -p build && clang++ -std=c++20 -O2 -march=x86-64-v3 -I ppc -I $X/XenonUtils -I $X/thirdparty/simde tests/instr_semantics.cpp -o build/instr_semantics && build/instr_semantics
