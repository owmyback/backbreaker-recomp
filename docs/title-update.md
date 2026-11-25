# Title Update #2

Backbreaker got one patch on Xbox Live after launch, "Title Update #2" (the "Greathouse" patch:
its splash screen is signed Greathouse #3). It replaces the executable and adds an archive of its
own with 59 more offensive and 43 more defensive plays, changed plays, tackling and Training Camp
changes and a new play selection screen. The executable is a rebuild, not a small fix: the code
grew by about 47 KB and nearly every function moved.

The game can be built from that patched executable as well as from the disc's, and the two builds
live side by side: you choose by giving the installer the update's package, or taking it away.

As with everything from the game, nothing the update holds is in this repository or in a release.
You need the update's package yourself: the file Xbox Live delivered to the console,
`TU_134U1UQ_0000008000000.00000000000G2` (title ID `464F07DA`, 4,108,288 bytes).

## Playing with it

With the game installed (the AppImage, the Windows zip or RecompLauncher), either:

- **Give the installer the package:** `./Backbreaker-Recompiled-x86_64.AppImage title-update <the
  package>`. On Windows, drag the package onto `Backbreaker-Recompiled.exe`. It checks the package,
  keeps a copy in the update folder and builds the updated game, showing its progress. For a game
  RecompLauncher set up, add `--prefix` with the game's `build` folder, as its start command does.
- **Or put the package in the update folder yourself** and start the game through the installer:
  `~/.local/share/backbreaker-recomp/update` (`$XDG_DATA_HOME`), or
  `%LOCALAPPDATA%\backbreaker-recomp\update` on Windows. It is beside the mods folder, which
  RecompLauncher's **Open Mods Folder** opens, and it is made the first time the installer starts
  the game. That start then builds the updated game (on Linux without showing progress).

**The installer is what looks in the update folder**, each time it starts the game: RecompLauncher's
Play, a double-click on the AppImage or on `Backbreaker-Recompiled.exe`, or the `play` command. The
applications-menu entry, the Start-menu shortcut and the Steam shortcut start the game's program
itself, which is the version built last: after adding or removing a package, start the game through
the installer once, and the shortcuts start that version from then on.

Building takes about as long as installing did: half a minute on a fast PC, a few minutes on a Steam
Deck. Saves, settings and mods are shared by both versions.

**To go back to the disc's version,** take the package out of the update folder and start the game
through the installer. Both versions stay built, so switching either way takes about a second. While a package is in the
folder that can't be used (another game's, a damaged copy), the installer says so and starts the
disc's version.

What the installer checks: the file is an Xbox 360 package for Backbreaker, it holds the three
files the update is made of (`default.xexp`, the patch for the executable, and
`PatchPackedOutputX360.pkh`/`.pkd`, the update's archive), and the patch is the one this project is
made for (its SHA-256, as with the disc's executable). They are copied to `game/extracted/tu2` in
the install folder, where the build and the game read them.

## What it changes in the game

Found by comparing the two versions' files; the numbers are the game's own ratings, from its
worst-rated player to its best. What each rating does is read from its name.

- **Playbook:** 59 more offensive plays (79 to 138) and 43 more defensive plays (57 to 100), in
  formations the disc doesn't have: Bunch, Strong I, Spread, T, Ace and Empty on offense; 3-3, 4-4,
  3 Deep and 4-6 on defense. 98 of the disc's plays are changed, and the play selection screen is new.
- **Catching:** the single catch rating (0.70 to 0.95) becomes one for standing catches (0.65 to
  0.95) and one for catches on the move (0.60 to 0.90). The fumble check has a new case for a
  player hit in the first half second after a catch. The rating for interceptions on the move is
  lower too (0.40-0.70 becomes 0.30-0.55).
- **Kicking:** kick speed goes from 0.7-1.0 to 1.3-1.45, with a new hold speed to match, and the
  punt formation's blocking is rebuilt.
- **Player ratings:** acceleration, throwing speed and getting back up come from Agility instead of
  Speed. Juke speed depends more on Agility (0.8-1.1 becomes 0.7-1.2).
- **Passing:** the pocket holds for less time (3-5 becomes 2.5-4), weak-armed quarterbacks throw
  farther (0.55-1.0 becomes 0.75-1.0), and defensive linemen can bat a pass down with the right stick.
- **Tackling:** the tackle force on the quarterback goes from 15 to 22, and another tackle type
  gets a tackle force of 50 it didn't have.
- **Before the snap:** the defense can show or hide a blitz; plays gain a handoff setting.
- **AI:** separate audible ratings for offense and defense, new zone coverage ratings, and fixes for
  a "NanoBlitz" exploit and a spin-tackle exploit.
- **Replays:** a choice of cameras (free, action, ball, player, two end zone, two sideline).
- **Training Camp:** the blitz drill gets a setting that stops the quarterback's pass, and the
  coverage drills' instructions say to hold Focus to intercept.
- **Loading screens:** about a hundred gameplay tips.

## Building it from a checkout

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
mounts an installed update on. `scripts/setup.sh` builds the disc's version only.

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
| In the installer's kit | `lib/libbackbreaker_runtime.a`, `share/backbreaker-runtime/` | `lib/libbackbreaker_runtime_tu2.a`, `share/backbreaker-runtime/tu2/` |
| In an install folder | `ppc/`, `obj/` | `ppc-tu2/`, `obj-tu2/` |

The installed game is one program, `bin/backbreaker`, whichever version was linked last
(`installer/build.cpp`); the menu entry and the Steam shortcut stay as they are.

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

The installer, the same day, in scratch install folders with an update folder of their own
(`--update-dir`):

- Linux, from the bundle `packaging/installer/build_installer.sh` makes: `title-update` with the
  package built the updated game in 31 s (16 threads) and `play` started it; with the package taken
  out, `play` linked the disc's version and started that; with it put back, the update's again,
  each switch a link and nothing else; a cut-short package was reported and the disc's version
  started. The Exhibition route to the kickoff, on both versions built this way.
- Windows, from `packaging/installer/build_installer_windows.sh`'s zip under GE-Proton 11-12: the
  same `title-update` (27 s), `play` on the update, the package taken out (the disc's version) and
  put back, headless each time; and the package dropped on the program with nothing installed.

Not checked yet: the play-lock fix (Training Camp's passing drills) and the Tackle Alley fix at 60
fps, whose addresses and offsets were carried over by reading the code, not by playing them; the
achievements; a whole match; a real Windows PC; a Steam Deck; RecompLauncher starting a game with a
package in the folder.
