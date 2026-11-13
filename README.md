# Backbreaker (Xbox 360) static recompilation

Exploratory project to statically recompile Backbreaker (2010, NaturalMotion) for native Linux
using XenonRecomp. Requires a legally owned disc image; no game data is in this repository.

## Start here
- `docs/ROADMAP.md` - phases, current status, what each phase did and what is next
- `docs/runtime.md` - building and running the runtime, its layout and debugging aids
- `docs/feasibility.md` - the original analysis and recompiler notes
- `CONTRIBUTING.md` - rules (no game files, ever), workflow and testing for contributors

## Layout
- `tools/`        - our own helper scripts (disc extraction, XEX decryption, switch tables, function bounds)
- `config/`       - XenonRecomp configuration and generated switch tables
- `patches/`      - our changes to XenonRecomp
- `runtime/`      - the host runtime (kernel, GPU, audio, input)
- `tests/`        - instruction semantics tests
- `game/`         - extracted/decrypted game files (gitignored, copyrighted)
- `ppc/`          - recompiled C++ output (gitignored, derived from game code)
- `third_party/`  - upstream clones such as XenonRecomp (gitignored)
- `docs/`         - roadmap, notes and analysis results

## Game files
You need an ISO image made from your own retail Backbreaker (Xbox 360) disc. No game files are
included or provided here. Below, `<iso>` is the path to your image.

## Reproduce extraction
    mkdir -p game/extracted game/disc
    python3 -I tools/xdvdfs.py "<iso>" game/extracted default.xex > docs/disc-listing.txt
    python3 -I tools/xex_decrypt.py game/extracted/default.xex game/extracted/default.pe
    python3 -I tools/xdvdfs.py "<iso>" game/disc --all     # full disc, used by the runtime

## XenonRecomp setup
    git clone --recursive https://github.com/hedge-dev/XenonRecomp.git third_party/XenonRecomp
    git -C third_party/XenonRecomp checkout ddd128b
    git -C third_party/XenonRecomp submodule update --init --recursive   # submodules as of ddd128b
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
No game files needed; CI runs this on every push (`.github/workflows/ci.yml`):

    X=third_party/XenonRecomp
    mkdir -p build && clang++ -std=c++20 -O2 -march=x86-64-v3 -I tests/ppc_config_stub -I $X/XenonUtils -I $X/thirdparty/simde tests/instr_semantics.cpp -o build/instr_semantics && build/instr_semantics

To test against a recompile of the real game, replace `-I tests/ppc_config_stub` with `-I ppc`.

## Build and run the runtime
See `docs/runtime.md` (import glue generation, CMake/Ninja build, `./build/backbreaker --timeout N`).
