# Title Update #2

Backbreaker got one patch on Xbox Live after launch, "Title Update #2" (the "Greathouse" patch:
its splash screen is signed Greathouse #3). It replaces the executable and adds an archive of its
own with 59 more offensive and 43 more defensive plays, changed plays, tackling and Training Camp
changes and a new play selection screen. The executable is a rebuild, not a small fix: the code
grew by about 47 KB and nearly every function moved.

The recomp can be built from that patched executable as well as from the disc's. The two builds
live side by side: the disc build is what `scripts/setup.sh` makes and what is released; the
update build is made by hand with the steps below, for now.

As with everything from the game, nothing the update holds is in this repository. You need the
update package yourself (`TU_134U1UQ_0000008000000.00000000000G2`, title ID `464F07DA`, 4,108,288
bytes).

## Building it

After a normal `scripts/setup.sh` (the disc build has to exist: the update patches the disc's
executable):

    # 1. the update's three files: default.xexp and PatchPackedOutputX360.pkh/.pkd
    tools/stfs.py /path/to/TU_134U1UQ_0000008000000.00000000000G2 game/extracted/tu2

    # 2. recompile. XenonRecomp applies default.xexp to the disc's default.xex itself and
    #    writes the patched executable to game/extracted/tu2/default.xex
    mkdir -p ppc-tu2
    third_party/XenonRecomp/build/XenonRecomp/XenonRecomp config/backbreaker_tu2.toml \
        third_party/XenonRecomp/XenonUtils/ppc_context.h

    # 3. the image the runtime loads
    tools/xex_decrypt.py game/extracted/tu2/default.xex game/extracted/tu2/default.pe

    # 4. the game, in a build folder of its own
    cmake -S . -B build-tu2 -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DBB_EXECUTABLE=tu2
    ninja -C build-tu2 backbreaker

    ./build-tu2/backbreaker

It uses the same disc files, saves, settings and mods folder as the disc build. The update's
archive is read from `game/extracted/tu2`, which the game sees as `update:\`, the drive the console
mounts an installed update on.

The update's files don't work with the disc's executable: with them in the mods folder the disc
build crashes while loading, with or without the update's play selection screen among them.

## What differs between the two builds

| | Disc | Title Update #2 |
|---|---|---|
| Recompiler config | `config/backbreaker.toml`, `backbreaker_switch_tables.toml` | `config/backbreaker_tu2.toml`, `backbreaker_tu2_switch_tables.toml` |
| Recompiled code | `ppc/` | `ppc-tu2/` |
| Layout the runtime is built for | `runtime/sdk/ppc_config.h` | `runtime/sdk/tu2/ppc_config.h` |
| Import glue | `runtime/generated/imports.cpp` | `runtime/generated/tu2/imports.cpp` (four more imports) |
| Image | `game/extracted/default.pe` | `game/extracted/tu2/default.pe` |

The runtime's source is the same for both. Where it knows the game by address,
`runtime/guest_exe.h` picks the right one:

- **Hooked functions** keep the name they have on the disc (`sub_8241A410`). For the update,
  `runtime/sdk/tu2/guest_names.h` defines each of those names as the same function's new address.
  It is generated: `tools/match_functions.py` pairs the two executables' functions up (25,825 of
  the disc's 28,927 are instruction for instruction the same once addresses are masked).
- **Everything else** (a global's address, a return address inside a function, an offset in a
  structure that grew) is written `BB_EXE(disc, tu2)` where it is used.

## Regenerating the generated parts

The switch tables and the function list in `config/backbreaker_tu2.toml`, as `scripts/setup.sh
--regenerate` does for the disc (start from an empty `functions = [ ]` block, or the analyzer's own
finds end up declared one by one):

    python3 -I tools/find_switch_tables.py game/extracted/tu2/default.pe config/backbreaker_tu2_switch_tables.toml
    # recompile, then:
    python3 -I tools/function_bounds.py game/extracted/tu2/default.pe config/backbreaker_tu2_switch_tables.toml \
        config/backbreaker_tu2.toml ppc-tu2/ppc_func_mapping.cpp
    # recompile again; repeat until function_bounds.py writes the same count twice

The import glue:

    python3 -I tools/gen_imports.py game/extracted/tu2/default.xex game/extracted/tu2/default.pe \
        third_party/XenonRecomp/XenonUtils/xbox/xboxkrnl_table.inc third_party/XenonRecomp/XenonUtils/xbox/xam_table.inc \
        runtime/generated/tu2/imports.cpp

The function names, after adding a hook that names a new guest function:

    python3 -I tools/match_functions.py game/extracted/default.pe ppc/ppc_func_mapping.cpp \
        game/extracted/tu2/default.pe ppc-tu2/ppc_func_mapping.cpp \
        --names runtime runtime/sdk/tu2/guest_names.h \
        --pair 822748A0=82276218 --pair 824972A8=82656E18

The two `--pair`s are functions the matcher can't pair on its own: `StartMatchStage`, which the
update changed, and the empty profiler-marker function, which the update's linker folded together
with every other empty function. Given addresses after the four files, the same tool says where a
function, a place inside one or a global went (`... 8236EA8C 82B5B7D8`). A function it reports as
`changed` needs reading before a hook on it is trusted: its instructions differ, if only by a
structure offset.

## What has been checked

On 2026-11-25, headless, on the update build:

- The Exhibition route of `packaging/portable/test_distros.sh`: boot, title screen, main menu, team
  and stadium selection, the match load, the coin toss, the kickoff's play selection and the kickoff
  itself, at 30 fps, and again at 60 fps with `--native-720p 1`.
- At 30 fps, on from there: the kickoff return, the replay after it, the update's play selection
  screen (Pass, Run, Special, Ask Coach, then the pass plays with their diagrams), a play called
  and the teams lined up for the snap.
- The hooks that log said what they do on the disc build: the boot logos skipped, present interval
  1 and the 1/60 s gameplay tick at 60 fps, the 1280x720 render views.
- The disc build, rebuilt with the same source, on the same route.

Not checked yet: the play-lock fix (Training Camp's passing drills) and the Tackle Alley fix at 60
fps, whose addresses and offsets were carried over by reading the code, not by playing them; the
achievements; a whole match; the installer, the AppImage, RecompLauncher and Windows, which all
still build the disc's executable only.
