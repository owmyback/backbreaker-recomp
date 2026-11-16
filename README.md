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
- `scripts/`      - contributor setup (`setup.sh`: your image to a build in one step)
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

## One-step setup
    scripts/setup.sh "<iso>"      # then: ./build/backbreaker --timeout 30

`scripts/setup.sh` runs the steps documented below in order - prerequisite checks, extraction,
decryption, the XenonRecomp clone + patch + build, the recompile, the import glue, the CMake
build and the game-free tests. A step whose output already exists is skipped, so re-running it
is cheap and safe; `--force` redoes everything, `--jobs N` limits the compile jobs and
`--regenerate` re-derives `config/backbreaker_switch_tables.toml` and the leaf-function block of
`config/backbreaker.toml` from your image (two recompile passes, see "Recompile"). The image path
goes only into the two extraction commands: it is never printed, written to a file or committed.

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
    # function_bounds.py adds leaf-function switch bounds, pointer-only and branch-only functions;
    # pass the ppc_func_mapping.cpp of a previous run so the analyzer's own functions are not duplicated
    python3 -I tools/function_bounds.py game/extracted/default.pe config/backbreaker_switch_tables.toml config/backbreaker.toml ppc/ppc_func_mapping.cpp
    mkdir -p ppc && third_party/XenonRecomp/build/XenonRecomp/XenonRecomp config/backbreaker.toml third_party/XenonRecomp/XenonUtils/ppc_context.h

## Tests
No game files needed; CI runs these on every push (`.github/workflows/ci.yml`):

    X=third_party/XenonRecomp
    mkdir -p build && clang++ -std=c++20 -O2 -march=x86-64-v3 -I tests/ppc_config_stub -I $X/XenonUtils -I $X/thirdparty/simde tests/instr_semantics.cpp -o build/instr_semantics && build/instr_semantics

Tiled 2D addressing (`runtime/gpu/tiling.h`, issue #12) is header-only, so nothing else is needed.
Build both translation units - `tests/tiling_test_odr.cpp` is what catches a definition in the
header that is not inline:

    mkdir -p build && clang++ -std=c++20 -Wall -Wextra -O2 tests/tiling_test.cpp tests/tiling_test_odr.cpp -o build/tiling_test && build/tiling_test

The mip level layout (`runtime/gpu/mip_layout.h`, issue #46) is header-only too:

    mkdir -p build && clang++ -std=c++20 -Wall -Wextra -O2 tests/mip_layout_test.cpp -o build/mip_layout_test && build/mip_layout_test

So are the resolve clear values per render target format (`runtime/gpu/clear_value.h`, issue #43):

    mkdir -p build && clang++ -std=c++20 -Wall -Wextra -O2 tests/clear_value_test.cpp -o build/clear_value_test && build/clear_value_test

And the audio frame conversion (`runtime/apu/frame_convert.h`, issue #90: the game's planar
big-endian 5.1 frames to interleaved 5.1 and to the stereo downmix):

    mkdir -p build && clang++ -std=c++20 -Wall -Wextra -O2 tests/audio_convert_test.cpp -o build/audio_convert_test && build/audio_convert_test

The XMA hardware context's packet and frame walk (`runtime/apu/xma_context.h`, issue #93) runs on
synthetic XMA2 packets with a fake frame decoder (no FFmpeg, no game data): frames split across
packets and input buffers, a split frame header, loops with subframe precision, a ring that is
not a multiple of a frame, and a frame the decoder rejects:

    mkdir -p build && clang++ -std=c++20 -Wall -Wextra -O2 tests/xma_context_test.cpp -o build/xma_context_test && build/xma_context_test

The achievement database parser (`runtime/kernel/spa.h`, issue #97) reads hand-built XDBF blobs:
achievements, their strings per language with the fallback to the title's default, the image
table, UTF-8 to UTF-16, and blobs that are cut short or claim more than they hold:

    mkdir -p build && clang++ -std=c++20 -Wall -Wextra -O2 tests/spa_test.cpp -o build/spa_test && build/spa_test

The unlock sound (`runtime/apu/system_sound.h`, issue #105) is synthesized, so its length, peak,
silent ends and note order are checked directly:

    mkdir -p build && clang++ -std=c++20 -Wall -Wextra -O2 tests/system_sound_test.cpp -o build/system_sound_test && build/system_sound_test

The PNG writer (`runtime/debug/png.cpp`, issue #76) is checked by writing images and reading them
back: chunk framing and CRC-32, the IHDR fields, the IDAT zlib stream and its Adler-32, and every
pixel. It links zlib to do the inflating (`zlib1g-dev` on Ubuntu, `zlib-devel` on Fedora), so it
covers IDAT however it is encoded - stored blocks today, real deflate if the writer changes:

    mkdir -p build && clang++ -std=c++20 -Wall -Wextra -O2 tests/png_test.cpp runtime/debug/png.cpp -lz -o build/png_test && build/png_test

It writes its test images to a temporary directory and removes them again; `PNG_TEST_VERBOSE=1`
prints one line per image (file size, IDAT bytes, which row filters it met).

The texture write watch (`runtime/memory.cpp`, #73) is tested on the real memory layout: writes
through each physical view, two textures sharing a page, file reads into protected pages, and many
threads writing while the pages are re-protected:

    mkdir -p build && clang++ -std=c++20 -O2 -pthread -I runtime tests/memory_watch_test.cpp runtime/memory.cpp -o build/memory_watch_test && build/memory_watch_test

The settings file (`runtime/settings.cpp`, #115) is read and written in a temporary directory:
defaults, a file from before `resolution_scale` existed, save-then-load of every key, hand-edited
lines, and out-of-range or non-numeric scales:

    mkdir -p build && clang++ -std=c++20 -Wall -Wextra -O2 -I runtime tests/settings_test.cpp runtime/settings.cpp -o build/settings_test && build/settings_test

To test against a recompile of the real game, replace `-I tests/ppc_config_stub` with `-I ppc`.

`scripts/setup.sh` is linted as well, with shellcheck (same CI file; `apt-get install shellcheck`
if your distro does not ship it):

    shellcheck scripts/setup.sh

## Build and run the runtime
See `docs/runtime.md` (import glue generation, CMake/Ninja build, `./build/backbreaker --timeout N`).

## Play the newest build
Once `build/` exists, updating is two commands from the repo root:

    git pull
    cmake --build build -j 8

If the pull changed `config/` or `patches/`, the recompiled code under `ppc/` no longer matches.
`git diff --stat ORIG_HEAD HEAD -- config patches` right after the pull says whether it did. Then
run `scripts/setup.sh` (no image path needed once `game/` exists) instead of the `cmake` line: it
re-applies a changed patch to XenonRecomp and rebuilds it, recompiles `ppc/` whenever it is older
than XenonRecomp or the config, and builds. The other steps see their output and skip.

Run it from the repo root too - the game files are found by paths relative to it (`game/disc`,
`game/extracted`):

    ./build/backbreaker                          # windowed, at the game's own 1280x720
    ./build/backbreaker --resolution-scale 2     # 3D at 2560x1440 - sharper, costs frame rate
    ./build/backbreaker --anisotropy 16          # keeps the field sharp toward the horizon

To keep a resolution scale without typing the flag, add a line to `game/settings.ini` (create
the file if it isn't there yet; the window writes the rest when it closes):

    resolution_scale=2

`--resolution-scale` still overrides it for one run, and headless runs ignore it.

Both flags only change 3D scenes (`--resolution-scale` goes to 4, `--anisotropy` to 16); what each
does to the picture - and what it leaves alone, like the 1280x720 attract movie and UI textures -
is in `docs/runtime.md`. On a Radeon RX 9070 XT neither costs measurable frame rate: headless over
the Exhibition route, scale 1, scale 2 with `--anisotropy 16` and 4K (scale 3) with
`--anisotropy 16` all hold the game's 30 fps cap, within 0.4% of each other in frames rendered
([issue #82](https://github.com/owmyback/backbreaker-recomp/issues/82)). A weaker GPU will show a
difference sooner: the runtime prints an `[gpu] N fps` line once a second, so try the flag and
watch that number.

In the window:
- **F12** - save the picture you see as `screenshots/<date-time>.png`
- **F10** - GPU frame capture (draws, shaders, render targets) of the next frame to `captures/frame_N/`
- **F11** or **Alt+Enter** - borderless fullscreen; window size, position and fullscreen state are
  remembered in `game/settings.ini`, next to the `resolution_scale` above

Everything else - headless runs, screenshots by frame number or by second, scripted input,
controllers, the renderer's debug aids - is in [`docs/runtime.md`](docs/runtime.md).

To report a bug: open an issue here with what you expected, what you got, and the F12 screenshot;
for anything that looks wrong on screen, also attach the F10 capture taken at that moment (its
`summary.txt` and `draws.txt` are what make a rendering bug fixable). Both folders are gitignored:
attach the files to the issue, never commit them.
