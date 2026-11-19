# Backbreaker (Xbox 360) static recompilation

Exploratory project to statically recompile Backbreaker (2010, NaturalMotion) for native Linux
using XenonRecomp. Requires a legally owned disc image; no game data is in this repository.

## Playing it
- **With RecompLauncher:** add this repository's link in RecompLauncher. It downloads the installer
  from the [latest release](https://github.com/owmyback/backbreaker-recomp/releases/latest), asks
  for your disc image, builds the game on your machine and adds it to Steam.
- **From a terminal:** the release's `Backbreaker-Recompiled-x86_64.AppImage install <your disc image>`
  does the same; the release notes have the steps.
- **Achievements:** F1, or the controller's Guide (PS) button, opens the list of achievements over
  the game, and the same key or button closes it. On a Steam Deck the Steam button belongs to Steam
  and there's no F1 key: in Steam, open the game's controller settings and map the back grip R4 to
  the keyboard key F1 (#145).
- **Mods:** plays, the AI's audibles, minigames and menus can be changed; see `docs/modding.md`.

## Start here
- `docs/ROADMAP.md` - phases, current status, what each phase did and what is next
- `docs/runtime.md` - building and running the runtime, its layout and debugging aids
- `docs/modding.md` - changing plays, the AI's audibles, minigames and menus, and sharing it as changes
- `docs/feasibility.md` - the original analysis and recompiler notes
- `CONTRIBUTING.md` - rules (no game files, ever), workflow and testing for contributors

## Layout
- `tools/`        - our own helper scripts (disc extraction, XEX decryption, switch tables, function bounds, the game's archive)
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

## Steam Deck and other distributions
    scripts/setup.sh --portable "<iso>"

builds, in a container, a version that runs on SteamOS (Steam Deck), Debian 12+, Ubuntu 22.04+,
Fedora, Arch and other current distributions (#134). The machine you build on needs only podman or
docker and git, whatever its distribution; the first run builds the container (about 15 minutes),
later runs rebuild only what changed. It produces:
- `dist/Backbreaker-x86_64.AppImage` (about 1 GB): one file, ready to run.
- `dist/backbreaker-linux-x86_64/`: the same as a folder, started with `./backbreaker`, and
  `dist/backbreaker-linux-x86_64.tar.gz` of it, for systems without FUSE.

**Both hold your game** - code recompiled from your disc, and the disc's files. They are for your
own machines: never upload or share them. Anyone else makes their own from their own disc with the
same command.

Where they keep things: settings in `~/.config/backbreaker-recomp/settings.ini`; saves,
achievements, screenshots, the mods folder and the logs of the last three runs (`logs/`) in
`~/.local/share/backbreaker-recomp/`. On the machine
you build on, `--portable` copies the checkout's settings and saves there once. Needs on the
target: an x86-64 CPU with AVX2 (2013 or newer; the game says so if not), a Vulkan 1.3 driver
(Mesa or NVIDIA) and glibc 2.35+.

On a desktop, put the AppImage in `~/Applications` and give it a menu entry (it carries one, with
the game's own icon). On a Steam Deck:
1. Copy the AppImage over (USB stick, network share, or `scp`), in Desktop Mode.
2. Make it executable: right-click > Properties > Permissions, or `chmod +x`.
3. Steam (desktop) > Games > Add a Non-Steam Game to My Library > the AppImage. It is then in your
   library in Gaming Mode; the controls work through Steam Input.
4. Start with `fps=30` and `resolution_scale=1` in `~/.config/backbreaker-recomp/settings.ini`; the
   Deck's GPU is far smaller than a desktop's.
5. For the achievements list, open the game's controller settings in Steam and map the back grip
   **R4** to the keyboard key **F1**. Steam keeps the Steam (Guide) button for its own menu, and the
   Deck has no F1 key, so the list has no other way to open there (#145). R4 then opens and
   closes it, and the d-pad moves through it.

After changing the code, run `scripts/setup.sh --portable` again: it recompiles only what changed
and repacks the AppImage (about a minute and a half for a change to the runtime).

If the game plays slowly at times (#146), add `show_fps=1` to `settings.ini`. The top-left corner
then shows the game's own frames a second, its speed (below 100% it runs in slow motion) and its
longest frame. Steam's performance overlay can't show this: it counts the window's redraws, which
stay at 30 while the game slows down. Every run also leaves its log in
`~/.local/share/backbreaker-recomp/logs/backbreaker.log`; the two runs before it are `.1.log` and
`.2.log`. Copy it to a PC and run `tools/fps_report.py` on it. For more detail in the log, set the
game's launch options in Steam to `BB_PERF=1 %command%`.

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

NTSTATUS to Win32 error codes (`runtime/kernel/ntstatus.h`, #132), what the game's own file calls
see when one fails:

    mkdir -p build && clang++ -std=c++20 -Wall -Wextra -O2 tests/ntstatus_test.cpp -o build/ntstatus_test && build/ntstatus_test

Archive entries replaced from the mods folder (`runtime/kernel/pack_overlay.cpp`, #169), on archives
the test makes up: the rewritten index, the appended data and reads across it, case-insensitive
names, both byte orders, and an index that doesn't parse:

    mkdir -p build && clang++ -std=c++20 -Wall -Wextra -O2 tests/pack_overlay_test.cpp runtime/kernel/pack_overlay.cpp -o build/pack_overlay_test && build/pack_overlay_test

The tools' own tests (`tests/test_*.py`; `tests/test_pkd.py` for `tools/pkd.py`) need no game files either:

    python3 -m unittest discover -s tests -p 'test_*.py'

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
    ./build/backbreaker --anisotropy 1           # the game's own texture filtering (the default is 16)

To keep a resolution scale without typing the flag, add a line to `game/settings.ini` (create
the file if it isn't there yet; the window writes the rest when it closes):

    resolution_scale=2

`--resolution-scale` still overrides it for one run, and headless runs ignore it.

Matches render at 1280x720 instead of the game's 1152x640 stretched to 1280x720, which keeps the
ball and the grass sharp. `native_720p=0` in `game/settings.ini` (or `--native-720p 0` for one run)
gives the game's own size back.

The game runs at 60 frames a second, twice the console's 30. For the console's rate, add `fps=30`
to `game/settings.ini` the same way, or pass `--fps 30` for one run (`--fps 60` likewise).
Headless runs use 30 unless given `--fps`. What 60 changes in the game, and how:
`docs/runtime.md`, "Frame rate" ([issue #89](https://github.com/owmyback/backbreaker-recomp/issues/89)).

Files in `game/mods/` replace the disc's files at the same path, without a rebuild: for example
`game/mods/media/Movies/Xbox360/505.wmv` replaces the 505 logo movie, and
`game/mods/media/PlayDatabase/Offense/Dagger.pd` replaces that play inside the game's archive
(`tools/pkd.py extract --mods` copies it out to edit). The log names each replaced file. Editing
and sharing mods: `docs/modding.md`; where an installed build keeps its files: `docs/runtime.md`,
"Where files live".

The game opens with two logo movies (505 Games, NaturalMotion; about 21 s). `skip_intro=1` in
`game/settings.ini` (or `--skip-intro 1` for one run) leaves them out; headless runs skip them unless
given `--skip-intro 0`. The attract movie (after about two minutes on the title screen) and the
trailers in Locker Room > Bonus > Media > Videos always play.

Textures get 16x anisotropic filtering, which keeps the field sharp toward the horizon (the game
asks for it on only a few textures). `anisotropy=N` in `game/settings.ini` (1-16; 1 is the game's
own) or `--anisotropy N` for one run changes it; headless runs use 1 unless given the flag.

`show_fps=1` in `game/settings.ini` (or `--show-fps 1` for one run) shows the game's own frames a
second, its speed and its longest frame in the top-left corner of the window (#146). Speed is game
time over real time: the game counts a frame as at most 1/30 s, so a long frame slows it down even
when the frame count looks fine (`docs/runtime.md`, "Frame rate").

Both flags only change 3D scenes (`--resolution-scale` goes to 4, `--anisotropy` to 16); what each
does to the picture - and what it leaves alone, like the 1280x720 attract movie and UI textures -
is in `docs/runtime.md`. On a Radeon RX 9070 XT neither costs measurable frame rate: headless over
the Exhibition route, scale 1, scale 2 with `--anisotropy 16` and 4K (scale 3) with
`--anisotropy 16` all hold the console's 30 fps, within 0.4% of each other in frames rendered
([issue #82](https://github.com/owmyback/backbreaker-recomp/issues/82)). A weaker GPU will show a
difference sooner: the runtime prints an `[gpu] N fps` line once a second, so try the flag and
watch that number.

In the window:
- **F12** - save the picture you see as `screenshots/<date-time>.png`
- **F10** - GPU frame capture (draws, shaders, render targets) of the next frame to `captures/frame_N/`
- **F11** or **Alt+Enter** - borderless fullscreen; window size, position and fullscreen state are
  remembered in `game/settings.ini`, next to the `resolution_scale`, `native_720p`, `fps`, `anisotropy`, `skip_intro` and `show_fps` above

Everything else - headless runs, screenshots by frame number or by second, scripted input,
controllers, the renderer's debug aids - is in [`docs/runtime.md`](docs/runtime.md).

To report a bug: open an issue here with what you expected, what you got, and the F12 screenshot;
for anything that looks wrong on screen, also attach the F10 capture taken at that moment (its
`summary.txt` and `draws.txt` are what make a rendering bug fixable). Both folders are gitignored:
attach the files to the issue, never commit them.

## Licence
This project's code is free software: you can redistribute it and modify it under the terms of the
GNU General Public License as published by the Free Software Foundation, version 3 or (at your
option) any later version ([`LICENSE`](LICENSE)). It comes with no warranty. Files derived from
Xenia keep its BSD 3-Clause licence ([`docs/licenses/xenia-LICENSE.txt`](docs/licenses/xenia-LICENSE.txt)).
The installer's AppImage also holds open-source tools and libraries under their own licences:
`NOTICES.txt`, attached to each release, lists them and where their source is. The source of the
GPL and LGPL ones no distribution keeps (XenonRecomp, the FFmpeg fork, the AppImage runtime's
libfuse) is attached too, as `third-party-sources.tar.xz`.

The licence covers this project's own code, never the game. Backbreaker and everything on its disc
belong to their owners. This project isn't affiliated with or endorsed by them, includes none of
the game, and needs your own copy of it.
