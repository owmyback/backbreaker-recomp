# Runtime

`build/backbreaker` runs the recompiled game on the host.

## Build and run
The one step (`scripts/setup.sh "<iso>"` from the repo root, see README) does everything below;
the commands are still listed individually because they are also useful on their own.

    python3 -I tools/xdvdfs.py "<iso>" game/disc --all        # once: extract the disc
    python3 -I tools/gen_imports.py game/extracted/default.xex game/extracted/default.pe \
        third_party/XenonRecomp/XenonUtils/xbox/xboxkrnl_table.inc \
        third_party/XenonRecomp/XenonUtils/xbox/xam_table.inc runtime/generated/imports.cpp
    cmake -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
    ninja -C build
    ./build/backbreaker --timeout 15     # run from the repo root

By default a window opens (SDL3 + Vulkan) showing what the game renders; its title shows the
revision it was built from, the game's frame rate and the display rate. Press **F12** in that window to save the picture you see as
`screenshots/<date-time>.png` (`_2`, `_3` ... for repeats within one second): the presenter copies
the swapchain image it has just drawn and a helper thread writes the PNG, so no guest thread is
involved and presentation does not stop. That read-back needs the swapchain image to be usable as a
copy source, which is something the surface decides: where it will not allow it, the presenter says
so once and F12 does nothing. `--headless` runs without a window (the renderer still
runs if a Vulkan GPU is available, for screenshots). F11 or Alt+Enter toggles borderless
fullscreen; window size, position and fullscreen state are remembered in `game/settings.ini`
(written on toggle and on close - `game/` is gitignored), and `--fullscreen` starts fullscreen
whatever that file says. Build dependencies: Vulkan headers/loader, SDL3 and shaderc (Fedora:
`libshaderc-devel`) development packages. Set `BB_VK_VALIDATION=1` to
enable the Vulkan validation layer (needs it installed; Fedora: `vulkan-validation-layers`).

Every build stamps in where it came from. `cmake/git_version.cmake` asks git for the short
revision, the branch and whether the tree had uncommitted edits, and writes them to
`build/generated/build_version.h` (`runtime/build_id.cpp` is the only file that reads it). That
string is printed by `--version` (which exits before any game file is opened), is the first line of
every run's log, is the second word of the window title, and heads every
`captures/frame_N/summary.txt`, so a log, a screenshot of the window and a capture folder all trace
back to one checkout. The stamp is redone on every build, so it follows `HEAD`, the branch or a new
edit straight away; a source tarball, or a machine without git, says `unknown` rather than failing
the build.

### Runtime kit (#138)
The build has two halves. The **runtime** (`backbreaker_runtime`, everything in `runtime/` except
the import glue) is a static library that needs no game files: it is compiled against
`runtime/sdk/ppc_config.h`, its own copy of the executable's four layout values, and configure stops
if `ppc/ppc_config.h` says something else. The **game** is `ppc/` plus the import glue
(`runtime/generated/imports.cpp`), both made from the player's executable, linked with the whole
runtime library: the runtime replaces recompiled functions with strong definitions of the same
name (`play_lock.cpp`, `fixed_seed.cpp`, `thread_times.cpp` ...), and from an ordinary archive the
linker would keep ppc/'s weak versions instead (8 of the 14 hooks were dropped when tried that way).

    cmake -B build-rt -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DBB_RUNTIME_ONLY=ON
    ninja -C build-rt                                                   # no ppc/, no game/ needed
    cmake --install build-rt --component runtime --prefix <kit>         # the runtime kit
    packaging/installer/link_game.sh --kit <kit> --ppc ppc --imports runtime/generated/imports.cpp \
        --out <dir>/backbreaker                                         # the game, from the kit

The kit holds `lib/libbackbreaker_runtime.a` (plus SDL3 and shaderc in a `BB_PORTABLE` build, and the
XMA decoder's FFmpeg libraries), the headers `ppc/` and the import glue are compiled with, and
`share/backbreaker-runtime/`: `version.txt` (the line `--version` prints), the runtime's
`ppc_config.h`, and `ppc-compile.args`, `imports-compile.args` and `game-link.args`, every
argument CMake uses, one per line (`@KIT@`, `@PPC@`, `@GAME@` are filled in by the linker step).
`link_game.sh` compiles only the `ppc/` files whose objects are missing or older than their source,
and all of them when the kit's compile arguments or headers change; after a runtime update it
relinks in about a second. It is the prototype of the installer's build step (#138); the kit holds
no game content.

### Installer tools (#138)
`backbreaker-installer` (`installer/`) takes a player from their own disc image to a built game
with nothing from the host but glibc, a Vulkan driver and the kernel: no Python, OpenSSL or compiler.

    ./Backbreaker-Installer-x86_64.AppImage install <image> [--prefix <dir>] [--jobs <n>] [--progress]
    ./Backbreaker-Installer-x86_64.AppImage play [--prefix <dir>] [-- <game options>]
    ./Backbreaker-Installer-x86_64.AppImage rebuild [--prefix <dir>]
    ./Backbreaker-Installer-x86_64.AppImage add-to-steam [--prefix <dir>]
    ./Backbreaker-Installer-x86_64.AppImage

The program inside is `backbreaker-installer`, but nothing puts it on the PATH: it runs as the
AppImage, and its messages name the AppImage's own path (`$APPIMAGE`). With no command it plays,
once the game is installed in the default folder, and prints its help otherwise.

`install` from a terminal also adds Backbreaker to the applications menu:
`~/.local/share/applications/backbreaker-recomp.desktop` (`$XDG_DATA_HOME`, `installer/menu_entry.h`),
which starts `<prefix>/bin/backbreaker` with the title's icon, `<prefix>/game/icon.png`. The icon is
made from the player's own executable (the `icon` command's, `tools/title_icon.py`'s), by `install`
or, for an older install, by `play`. `--no-menu-entry` leaves the entry out, and `--progress` (a
launcher's install) never adds one, since the launcher has its own entry for the game. `TryExec` hides
the entry once the game's folder is gone; deleting the `.desktop` file removes it.

**Add to Steam** (#160): `install` from a terminal, and `add-to-steam` on its own, add Backbreaker
to Steam as a non-Steam game, for every Steam account on the machine, in
`userdata/<account>/config/shortcuts.vdf` (`installer/steam_shortcut.h`). The entry starts
`<prefix>/bin/backbreaker`, in quotes, with no launch options and the title's icon,
`<prefix>/game/icon.png`. Steam is found at `~/.steam/steam` and the other folders its launcher script
uses, and Flathub's Steam at `~/.var/app/com.valvesoftware.Steam/.local/share/Steam`. On a Steam
Deck (2026-11-18), after a fresh `install` with Steam closed, Gaming Mode's library had the one
entry, with its icon, and it started the game. Details:
- **Steam has to be closed:** it reads the file when it starts and writes its own list back when it
  closes. While a process named `steam` of the player's is running, nothing is written: `install`
  says so and names the `add-to-steam` command to run once Steam is closed, and `add-to-steam`
  itself stops with exit code 1. In a Steam Deck's Gaming Mode, Steam is always running, so this is
  done from Desktop Mode.
- **No duplicates:** an entry that already starts the same program is updated, and so is one named
  Backbreaker, in any case, that starts something else (the portable AppImage, say). The entry keeps
  its `appid`, which Steam keys the game's playtime, controller settings and library artwork by. It
  also keeps its other keys and, when it already started this program, the player's launch options.
  Run again with nothing to change, it writes nothing.
- **A new entry** has the keys, order and defaults of Steam's own "Add a Non-Steam Game", and
  `crc32(Exe + AppName) | 0x80000000` as its `appid`, as Steam ROM Manager and RecompLauncher give
  it. Steam stores the id in the entry and uses it as stored. An entry it adds itself gets an id
  of its own: on the maintainer's PC (Steam client 1792618285, October 2026), neither shortcut's
  id was that checksum.
- **A file it can't read**, a value type it doesn't know or a file cut short, is left as it is, and
  so is a path with `"`, `` ` ``, `$` or `\` in it. The file as it was before the first change is
  kept beside it as `shortcuts.vdf.before-backbreaker`.
- **Flathub's Steam** can only start a program its sandbox can see. When it gets the shortcut, the
  installer prints the line that lets it read the game's folder:
  `flatpak override --user --filesystem=<prefix>:ro com.valvesoftware.Steam`. Started there, the
  game should keep its player files in that sandbox's own folders
  (`~/.var/app/com.valvesoftware.Steam/data/backbreaker-recomp`, Flatpak's `$XDG_DATA_HOME`). Not
  tried with Flathub's Steam yet: only the files it writes, under a temporary HOME.
- **Leaving it out:** `--no-steam` leaves the shortcut out, and `--progress` never adds one, since
  RecompLauncher has its own Add to Steam and the game would be there twice. Removing the game in
  Steam removes the shortcut. Library artwork (cover, banner, logo) is left empty (decided
  2026-11-18, #160): nothing from the game can ship, and the title's icon is too small for a cover.

`install` checks the image (below), copies the game's files, runs XenonRecomp, compiles `ppc/` and
links the game, into `~/.local/share/backbreaker-recomp/install` by default (`$XDG_DATA_HOME`; the
player's saves and settings stay where the installed layout puts them, "Where files live"). That
folder has the checkout's layout, which XenonRecomp's config expects: `config/`, `game/disc`,
`game/extracted`, `ppc/` (only its headers once compiled: XenonRecomp makes the sources again in
seconds), `obj/` and `bin/backbreaker`. `play` builds whatever changed and starts the game with
`--data-dir <prefix>/game`: a new runtime kit relinks in about a second, and `ppc/` is compiled again
only when XenonRecomp's source, the config, the kit's compile arguments or the executable changed
(`obj/build.stamp`). XenonRecomp counts by `xenonrecomp/SOURCE`, its commit and the patch's SHA-256,
not by the program's bytes: a newer build container's clang makes a different program from the same
source, and it translates the same way. `ppc-inputs` prints the bundle's part of that as one hash
(Releases, below). Parallel compiles: the CPU threads, or fewer when free memory is short (1 GB a
job; the biggest file peaks at about 720 MB). `ppc/` is compiled with the kit's arguments plus
`-g0`: crash backtraces use the exported symbol names, not debug info, and without it the biggest
file takes 11 s instead of 13 s. The optimisation level stays the kit's `-O2`: `-O1` compiles `ppc/` with
23% less CPU but makes the game's threads need 28-40% more (ROADMAP, phase 10). A second installer
started on the same folder while one is working
stops with a message (`<prefix>/.lock`).

**From Steam by hand**: without `add-to-steam`, in "Add a Non-Steam Game", pick Backbreaker from the
list (the menu entry, with its icon), or browse to `<prefix>/bin/backbreaker` (by default
`~/.local/share/backbreaker-recomp/install/bin/backbreaker`) or the AppImage, with no launch options;
a shortcut made by browsing has no icon until one is picked in its properties
(`<prefix>/game/icon.png`). The game finds its files from its own path, `<prefix>/game` beside its
`bin/` once `game/.complete` is there ("Where files live"), so the folder Steam starts it in doesn't
matter. The AppImage also relinks after a runtime update; `bin/backbreaker` keeps the runtime it was
linked with until `play` runs. On the maintainer's Steam Deck (2026-11-18), after `install` in a
terminal, Steam's list had Backbreaker without browsing, its library entry shows the icon beside the
name, and it started. Steam's library artwork (the cover and banner) stays empty either way: Steam
takes it only from files named after the shortcut's id (ROADMAP, phase 10, part 5).

Everything it builds with comes from its bundle, `build-installer/AppDir/usr/lib/backbreaker-installer`
(`installer/build.h` has the layout), made in the portable build container by
`packaging/installer/build_installer.sh`: the runtime kit (`BB_PORTABLE`), XenonRecomp with
`patches/`, `config/`, clang and lld 23 with the libraries they load, and a sysroot of Ubuntu 22.04's
glibc, libstdc++ and Vulkan loader packages, so the game it builds needs glibc 2.35, as the portable
build does. `MANIFEST.txt` lists every package and version in it, and `NOTICES.txt`
(`packaging/installer/notices.sh`) every third-party part's licence and source: the Debian copyright
files of the Ubuntu and apt.llvm.org packages, the licences of what the container builds from source
(`/usr/local/share/licenses`), XenonRecomp's and FFmpeg's from their checkouts, and the AppImage
runtime's from `packaging/installer/licenses/`. The bundle is about 420 MB, and the
AppImage, `build-installer/Backbreaker-Installer-x86_64.AppImage`, about 110 MB; it holds no game
content. `packaging/portable/test_distros.sh --installer <AppImage> --image <your image>` installs and
runs the game with it in each distribution's container, none of which has a compiler.

**RecompLauncher** (#138, part 4) is the front end players will use: it takes their disc image,
runs the installer, shows the settings and starts the game. `packaging/installer/recomplauncher.json`
tells it how, and `build_installer.sh` puts it at the AppImage's root, where the launcher reads it
(its `docs/DEVELOPERS.md`, "Where the file goes"):
- `game`: an `.iso` holding `default.xex` (`"xdvdfs"`), with the USA/Europe dump's size and CRC32
  for its "verified" badge.
- `prepare`: `install {game} --prefix {install}/build --progress`, so the built game lives in the
  launcher's own folder for the game and goes when the player removes it there; done when
  `bin/backbreaker` and `game/.complete` exist. Its `version` makes players who set up already build
  again; a new runtime only needs `play`'s relink, so it changes only when the install itself does.
- `start`: `play --prefix {install}/build --`, with the player's launch options after the `--`. In
  Big Picture mode (`RECOMPLAUNCHER_MODE=bigpicture`) `play` adds `--fullscreen`.
- `config` and `settings`: `{config}/backbreaker-recomp/settings.ini` (resolution, frame rate,
  window, texture filtering, intro logos, the frame-rate counter; native 720p under Advanced),
  edited line by line, so the keys the game writes itself (window size and position) stay.
- `saves`, `folders` and `actions`: saves, achievements and settings for its backups; Open Mods
  Folder and Open Screenshots; the log folder for crash reports. `exit_codes` explain `play`'s 7 and 8.

**Releases** (`.github/workflows/release.yml`, #160): a `v*` tag pushed, or the workflow run by hand
(Actions, Release, Run workflow, with the tag to make), builds the AppImage in the build container
(cached between runs) and attaches it, `MANIFEST.txt`, `NOTICES.txt` and `third-party-sources.tar.xz`
to a **draft** release, with `packaging/installer/release_notes.md` as its text. The maintainer
looks the draft over and publishes it on GitHub. Run again for the same tag, it replaces a draft's
files; a published release is left alone. Before the draft:
- **The AppImage is checked:** it starts (unpacked, since a runner has no FUSE), its manifest names
  `Backbreaker-Installer-x86_64.AppImage`, the name RecompLauncher looks for, and it holds no disc
  image, executable, translated code or title image.
- **The sources archive** (`packaging/installer/sources.sh`): the source of the GPL and LGPL parts
  that no distribution keeps, so it comes from the same place as the AppImage: XenonRecomp at its
  pin with its submodules (binutils' disassembler, libmspack) and our patch, the FFmpeg fork at its
  pin, and the libfuse release and patch the AppImage runtime is built with (checked against their
  SHA-256). About 19 MB, and the same bytes from the same inputs. glibc's and GCC's come from
  Ubuntu's source packages on Launchpad, which `NOTICES.txt` links at their versions.
- **`prepare.version`:** RecompLauncher reruns `prepare` only when it changes, and its start step
  runs `play`, which would recompile `ppc/` for minutes with nothing on screen. So when the
  `ppc inputs` line of `MANIFEST.txt` differs from the last published release's and
  `prepare.version` doesn't, the run stops and says to raise it in `recomplauncher.json`.

A pull request that changes `packaging/`, the decoder's build or the workflow builds and checks the
AppImage the same way, and uploads nothing.

To try it before there is a release: put `Backbreaker-Installer-x86_64.AppImage` alone in a folder
whose name has the word "Installer" in it (`Backbreaker Installer`, say), then in RecompLauncher
(0.90 beta 63 or newer) Settings, Bring Your Games, "Pick a Folder…", that folder, and "Add Without
Updates" when it asks which game it is. Its folder scan skips a program named like an installer
unless the folder's name has that word too (its `install.rs`, `NOT_THE_GAME`), and says "There's no
program in that folder." It reads the manifest from the AppImage, and shows the game as
"Backbreaker" with our settings page when it did; named after the folder, with no settings, it
didn't. On the Deck (2026-11-18, RecompLauncher 0.90.0-beta.64) the first try was the second
kind: its AppImage reader (the backhand crate, 0.21) can't read an extended symlink inode, which
`mksquashfs` writes for a symlink with extended attributes, and every file built on the
maintainer's Fedora PC has an SELinux label. `build_installer.sh` now passes `-no-xattrs`, and the
release workflow checks the image has none. Built that way, the whole flow worked on the Deck: the
settings page, the disc image asked for and the game built (3-4 minutes, offline), Play, the folder
actions, and its Add to Steam, from which the game started in Gaming Mode (ROADMAP, phase 10, part
4). The Steam entry's icon is RecompLauncher's to set (its catalog's, or a picture the player
picks under Pictures, such as `build/game/icon.png` in the game's folder), not the manifest's. A verified developer's account can use Settings, Developers, "Add a build"
instead, which adds the folder the same way (its `devtools.rs`, `add_folder`) and has a Developer tab
listing what it read; other accounts don't see the Developers section (its `ui/app.js`, `devAllowed`).

The single steps are commands too:

    backbreaker-installer check <image>                 is it Backbreaker, the release supported?
    backbreaker-installer game-files <image> <game dir> [--progress]   disc/ + extracted/default.{xex,pe}
    backbreaker-installer list <image>                  | extract <image> <dir> --all | <file>...
    backbreaker-installer decrypt <default.xex> <default.pe>
    backbreaker-installer imports <default.xex> <default.pe> <imports.cpp>
    backbreaker-installer icon <default.pe> <icon.png>
    backbreaker-installer ppc-inputs [--bundle <dir>]   the bundle's part of what ppc/ is built from (Releases)

`list`, `extract`, `decrypt`, `imports` and `icon` write the same bytes as `tools/xdvdfs.py`,
`xex_decrypt.py`, `gen_imports.py` and `title_icon.py` (AES-128 and SHA-256 are in
`installer/crypto.cpp`, checked against the standards' test vectors by `tests/installer_test.cpp`;
the kernel's export tables are compiled in from XenonRecomp's `XenonUtils/xbox/*_table.inc`).
`check` (also the first step of `game-files`) reads only the file system and `default.xex`, and
refuses, each with its own message and exit code:
- 3: the image can't be opened (missing, unreadable, a folder);
- 4: not an Xbox 360 game disc (an archive or a content package is named as such), or no `default.xex`;
- 5: another game (its title ID; Backbreaker's is `464F07DA`);
- 6: damaged: a file or folder runs past the end of the image (a cut-short copy), or `default.xex`
  isn't a known release (`kKnownReleases` in `installer/game_check.cpp`: its SHA-256).
- 7: (`play`, `rebuild`) the game isn't installed in that folder; 8: the build failed (the message
  ends with the compiler's or linker's last lines; the whole log is in `<prefix>/obj/`).

It doesn't read the rest of the disc, so damage elsewhere goes unnoticed until the file is used.
Messages never contain the image's path. `--progress` prints `progress <0-1> <step>` lines, the form
RecompLauncher shows as a progress bar. `scripts/setup.sh` still uses the Python tools.

## Where files live
`runtime/paths.cpp` (#134). Two layouts:
- **Checkout** (the default, as before): everything relative to the working directory, so run from
  the repo root. The game's files and the player's settings, saves (`game/saves`), the game's cache
  partition (`game/cache`), achievements and a custom unlock sound (`game/sounds/achievement.wav`) are
  in `game/`; screenshots and captures go to `screenshots/` and `captures/` beside it.
- **Installed**: inside an AppImage (its runtime sets `APPIMAGE` and `APPDIR`; the game's files are
  the AppImage's `game/`), when a launcher passes `--data-dir <dir>` (a directory with `disc/` and
  `extracted/`), or when the program is the installer's `<prefix>/bin/backbreaker` and
  `<prefix>/game/.complete` exists (#138: found from `/proc/self/exe`, so a shortcut needs no options;
  a checkout's `build/backbreaker` never matches). The game's files may then be read-only.

The window's icon is `icon.png` in the game's files, when it's there: the title's own image, which the
installer, `scripts/setup.sh` (`game/icon.png`) and `scripts/package_portable.sh` make from the
player's executable. X11 and KDE Plasma (Wayland's `xdg_toplevel_icon_v1`) show it; GNOME shows the
menu entry's icon, found by the window's app id, `backbreaker-recomp` (`SDL_SetAppMetadata`), the name
of the installer's and the portable AppImage's `.desktop` files. The log says `[video] window icon`. The player's go to the per-user folders:
  `~/.config/backbreaker-recomp/settings.ini` and `~/.local/share/backbreaker-recomp/` (`saves/`,
  `cache/`, `achievements/`, `sounds/`, `mods/`, `screenshots/`, `captures/`, `logs/`), following
  `$XDG_CONFIG_HOME` and `$XDG_DATA_HOME`. The log's `[paths]` line names all three.

`--user-dir <dir>` puts all of the player's files in one directory instead, in either layout. The
shader cache is in `~/.cache/backbreaker-recomp` in both.

**Log file** (`runtime/log_file.cpp`, #146): in the installed layout and with `--user-dir`, each run's
log is also kept in `logs/backbreaker.log`, with the two runs before it as `backbreaker.1.log` and
`backbreaker.2.log`. That's the log of a run started from Steam or a launcher, which has no terminal.
It holds everything the process writes to stderr: the runtime's log, a crash report, SDL's and the
Vulkan driver's messages. The file is stderr itself, so a crash report is complete in it. A thread
copies what it gets on to the terminal or pipe the run started with, every 20 ms, and the last of it
just before the process ends, so the console still shows the whole log. A console that goes away (a
closed pipe) no longer ends the game: SIGPIPE is ignored once the file is open. At 256 MB the file
stops growing and the rest goes to the console only. The checkout layout writes no file.

**Mods folder:** `mods/` in the player's directory (`game/mods` in the checkout layout) overlays the
disc. A file there replaces the disc's file at the same path, matched without regard to case like
the disc itself, from the next launch, with no rebuild: `mods/media/Movies/Xbox360/505.wmv` replaces
the 505 logo, for example. Only files are replaced; directory listings stay the disc's, so a mod
can't add files the game finds by listing a directory. The log names each file a mod replaces
(`[mods] ... replaces ...`). Most of the game's content is inside
`packedfilesx360/PackedOutputX360.pkd`; the whole archive can be replaced this way, but editing what
is inside it needs a tool that doesn't exist yet.

**Portable build** (`scripts/setup.sh --portable`, #134; README, "Steam Deck and other distributions"):
- The container (`packaging/portable/Containerfile`): Ubuntu 22.04, so glibc 2.35 is the floor;
  clang 23 from apt.llvm.org; SDL3 3.4.20, shaderc 2026.2 and Vulkan-Headers 1.4.350 built from
  pinned tags; AppImage's static type2 runtime at a dated release, checked against its SHA-256.
  Tagged by the Containerfile's hash, so editing it builds a new image. It holds tools only.
- `setup.sh --portable` runs the same steps inside it (`--in-container`) with its own build
  directories: `third_party/XenonRecomp/build-portable`, `third_party/ffmpeg-xma-portable` and
  `build-portable/`. `ppc/` and `game/` are shared with the native build; `ppc/` counts as current
  when it is newer than the config and the XenonRecomp patch, so neither build recompiles the other's
  output. Symlinked `game`, `ppc` or `third_party`, and a git worktree's repository, are mounted at
  their own paths so they resolve in the container. The image, if given, is mounted read-only as
  `/image`.
- `-DBB_PORTABLE=ON` links SDL3 (static; it still loads X11, Wayland, PipeWire, PulseAudio and ALSA
  at run time), `shaderc_combined` and the C++ runtime in. The binary needs only `libvulkan.so.1`,
  `libm`, `libc` and the loader: `objdump -p` and `ldd` list nothing else, and its newest glibc
  symbol is 2.35.
- `scripts/package_portable.sh` makes the folder (a launcher passing `--data-dir`, `bin/`, `game/` as
  hard links to the checkout's), the AppImage (an `AppRun`, a `.desktop` entry, the title's own
  icon from its XDBF resource via `tools/title_icon.py`, a zstd squashfs) and the `.tar.gz`.
- `packaging/portable/test_distros.sh` runs the AppImage headless in containers of Ubuntu 22.04
  and 24.04, Debian 12 and 13, Fedora and Arch, each with its own Vulkan loader and Mesa: the
  Exhibition route on the GPU where the distribution's Mesa supports it, a boot to the title on
  lavapipe (software Vulkan) where it doesn't.

**CPU check:** the build targets x86-64-v3 (AVX2, FMA, BMI1/2, F16C, MOVBE). `runtime/cpu_check.cpp`,
built for baseline x86-64, runs before anything else and on a CPU without them prints what is
missing, also in a message box, and exits with code 1, instead of the first such instruction ending
the process with SIGILL.

## Renderer
`runtime/gpu/renderer.cpp` executes the game's draws with Vulkan (one device shared with the
window, `runtime/video/vk_context.cpp`): shaders are translated (see below) and compiled with
shaderc on first use, EDRAM render targets are Vulkan images, resolves copy into images keyed by
their guest address, and the presenter shows the image resolved to the front buffer.
- A render target is one image per EDRAM base, pitch, MSAA mode and *storage* format (Xenia's
  GetStorageFormat): the `AS_` color formats only change what a resolve writes, and 8_8_8_8_GAMMA is
  stored as 8_8_8_8. The game draws the same tiles as 2_10_10_10_FLOAT and as
  2_10_10_10_FLOAT_AS_16_16_16_16 several times a frame; with an image for each, every switch was an
  EDRAM transfer (#121: 1.5 ms of GPU time a frame in the rain at resolution scale 2).
- A draw that writes every sample of a tile skips the transfer of that tile (#121). D3D clears a
  target by drawing one rectangle into a 4x MSAA view of the same tiles at a quarter of the pixels,
  with depth and stencil forced, so what the tiles held before the clear does not matter. The draw
  qualifies when it is one screen-aligned rectangle (its covered pixels are known from the vertex
  data), every sample passes (no depth or stencil test that can fail, alpha test, alpha to coverage
  or kill), and for each target: all four color channels written with blending off, or depth
  written with the test always passing and all 8 stencil bits replaced. Tiles only partly covered
  are transferred as before. In the rain at scale 2 this skips about 300,000 of 1.1 million tiles a
  second.
- Each draw's constants, indices and vertices are copied out of guest memory into the frame's
  upload buffer, where the GPU reads them. The shadow, reflection and main passes draw the same
  meshes, so a draw whose indices or vertex range are the same bytes an earlier draw of the frame
  copied uses that copy instead (#121), as long as the guest cannot have changed them since. The
  guest rewrites memory the GPU reads only after the command processor has shown it the GPU is
  past those reads (a fence or other memory write, a scratch register, the read pointer, an
  interrupt), and every one of those makes earlier copies unusable. GPU writes to guest memory
  (resolve writebacks, memory export) do the same for the pages they touch, and ranges with a GPU
  write pending are copied every time. In the heaviest rain frames this reuses 84% of the index
  bytes and 62% of the vertex bytes and takes the command processor thread's work from 11.4 to
  8.5 ms a frame.
- **The geometry cache (#146).** Indices and vertices that stay the same from frame to frame are
  kept in a cache: `runtime/gpu/geometry_cache.h`, and `geomCached` in the renderer.
  - **Why.** The copies above were reused only within a frame, and only until the guest next saw
    progress. In a match that happens about 300 times a frame, nearly all of it `EVENT_WRITE_EXT`
    screen extents. Yet 70% of the vertex bytes copied, and nearly all of the index bytes, were the
    same as the last copy of that range. On the Steam Deck the copying was a third of the command
    processor thread's work, and that thread decides the frame rate there.
  - **Where.** The cache is a 64 MB ring at the start of the one upload buffer; the frames' parts
    come after it. A cached copy is good while the write watch (below) says its guest pages are
    unwritten, and no GPU write has been recorded over them. Progress signals don't matter to it.
  - **What gets cached.** A range of at least 1 KB, copied alike (by a fingerprint) in 3 frames in a
    row, with no GPU write over it. Only then are its pages protected, because a write fault costs
    the game's thread about 90 µs (#119) and some buffers are rewritten every frame. A cached range
    found written is left uncached for 300 frames, then 600, up to 2,400.
  - **Why the ring can't overwrite data in use.** Each frame may write at most an eighth of the
    ring, and a copy is used only while it is less than half the ring behind the newest; an older
    one is copied forward. With two frames in flight, data a frame reads can't be overwritten
    before the GPU has run that frame. `tests/geometry_cache_test.cpp` simulates that.
  - **Result,** measured on the PC with `packaging/portable/test_distros.sh`'s route (`--fixed-seed`):
    - **Exhibition** (started from a saved profile): the command processor's work went from 6.5 to
      4.7 ms a frame in the match. Indices fell from 17.7 to 2.2 ms a second, vertices from 48.7 to
      14.4.
    - **Training Camp's passing drill** (a new profile, which the same presses take there): 7.3 to
      5.3 ms a frame.
    - Write faults and the frame count stayed the same.
  - **Switches.** `BB_GEOM_CACHE=0` turns the cache off. `BB_GEOM_CHECK=1` checks every cached copy
    it hands out against a host copy of the guest bytes taken when it was cached, and logs any that
    differ: a write the watch missed.
  - The vertex loop also reads its fetch constants from the register file, not back out of the
    upload buffer. That buffer is write-combined memory, so every CPU read of it misses the cache.
- A resolve whose destination lies inside an earlier resolve's (predicated tiling, or a texture
  atlas filled a strip at a time) goes into that image, at the place its address has in the
  destination's tiled layout. That layout is in the *destination* format's pixel size, which a
  resolve can change: the crowd's impostor atlas (#55) is 64 fans drawn as 8_8_8_8 a row at a time
  and resolved as 1_5_5_5, each row 256 KB further on. Counted in 4-byte pixels that was 64 rows
  instead of 128, so every row of fans landed on the legs of the row before and the stands showed
  torsos beside their legs and fans sunk to the shoulders.
- A resolve copies the rectangle its vertices span (Xenia's GetResolveInfo), within the scissor -
  which is often a power of two or whole EDRAM tile rows bigger - and within the destination the
  game declared (`RB_COPY_DEST_PITCH`), so a resolve image is exactly as big as the textures that
  read it. Sized by the scissor, a 320x180 destination came out 320x184 (four rows of whatever EDRAM
  held), and the 320x180 texture that sampled all of it spread those rows into the bloom.
- Resolves are also written to guest memory (#45), tiled and in the destination's format, by a
  compute shader. The CPU reads some of them: the game's "Stadium texture generator" thread
  renders the field lightmap and compresses the resolved picture to DXT1 itself, so without this
  the grass is black. These writes reach guest memory before anything that tells the CPU the GPU
  got that far (fences, interrupts, the read pointer; see "GPU fences" below). Formats written: 8_8_8_8, 2_10_10_10 and D24S8
  depth. Others (the log says which, once each) stay on the GPU. Where guest memory can't be
  imported as a Vulkan buffer (it fails on RADV), the writes go through memory export's staging
  buffer and are copied out. Each write costs a GPU sync, so a destination resolved in each of the
  last 60 frames (scene, shadow maps, bloom, front buffer) stays on the GPU for as long as it keeps
  being resolved every frame. The log names each one (`resolve writeback: <address> is resolved
  every frame`).
- `--screenshot N[,N...]` writes `screenshots/frame_N.png` (the front buffer of swap N; works
  headless). `screenshots/` is gitignored - pictures of the game are not committed. F12 in the
  window writes the same kind of PNG from the swapchain image instead, i.e. what the window showed
  (letterboxed and at the window's size).
- `--screenshot-at S[,S...]` takes seconds since launch instead, on the clock `--press` uses, and
  writes `screenshots/t_S.png` (as typed: `t_200.png`, `t_212.5.png`) at the first swap at or
  after that time; the log line names the frame and the exact time. On their own, neither form
  makes two runs of the Exhibition route show the same scenes everywhere: the **coin toss is
  random** (from about 125 s, one run kicks off and the other receives), and so are the loading
  screen's art and tip, the wind and the weather. With `--fixed-seed` (see "Scripted input") those
  come out the same: in five runs the toss screen matched pixel for pixel and the kickoff had the
  same kicking side, wind and returner. The play itself still differs in detail, since it depends
  on frame timing. A slower build (e.g. `BB_VK_VALIDATION=1`) boots and loads slower, so it reaches
  each menu later in both seconds and frames. Avoid times right at a scripted press (the frame
  before and after it differ).
- `--resolution-scale N` (1-4) renders at N times the game's resolution: 2 = 2560x1440,
  3 = 3840x2160. To keep a scale for every windowed run, put `resolution_scale=N` in
  `game/settings.ini` (#115); the flag overrides the file for one run and doesn't change it, and
  headless runs ignore the file, so scripted routes and screenshots stay at 1 unless they pass the
  flag. The log says where a scale other than 1 came from (`[gpu] resolution scale 2
  (game/settings.ini)`). The game still sees 1280x720; render targets, viewports, scissors, resolves and
  clears are scaled. (The attract movie itself is a 1280x720 video, so it only gets smoother
  scaling; 3D scenes are where the extra resolution shows.) Measured on a Radeon RX 9070 XT
  (issue #82, main at 2a16722, headless Exhibition route, 245 s, ABBA): scale 1 rendered 7,265 and
  7,268 frames, scale 2 with `--anisotropy 16` 7,251 and 7,261, scale 3 with `--anisotropy 16`
  7,248 and 7,241 - all at the 30 fps cap. The renderer's cost is per draw on the CPU, not per
  pixel, so resolution only starts to matter once a GPU is the limit.
- Matches render at 1280x720 (`native_720p=1` in `game/settings.ini`, the default; `--native-720p
  0|1` for one run; headless runs keep the game's size unless given the flag). With a 1280x720
  display the game draws the match at 1152x640 and stretches it to 1280x720, as on the console
  (`sub_82381928` picks 1152x640 for displays up to 1280 wide, 1920x1080 above), which softens the
  whole 3D picture a little: the ball and the grass at kickoff most visibly. The runtime asks
  `sub_827A09B0` for a 1280x720 view instead (and 1280x360 for the two-player split), and
  everything after follows the view's size - the game re-tiles the scene for EDRAM (four 320-wide
  tiles instead of two 576-wide ones) and sizes its post-processing buffers from it (640x360,
  320x180) - so nothing is stretched, and at resolution scale 2 the picture is exactly 2560x1440.
  About 25% more pixels to draw; the rain route still holds 60 fps at scale 2 on the RX 9070 XT.
- Anisotropic filtering: every texture the game minifies with linear filtering and mips gets at
  least N:1 anisotropic filtering. The game asks for it on only a few textures, so without it the
  field blurs toward the horizon, worst from the low kickoff camera. Windowed runs use
  `anisotropy=N` in `game/settings.ini` (1-16, default 16: no measurable cost, #82); `--anisotropy N`
  overrides it for one run, and headless runs stay at 1 (the game's own) unless they pass the flag.
  The log says which (`[gpu] anisotropic filtering 16x (game/settings.ini)`). UI textures (base
  level only) are left alone. Needs the GPU's `samplerAnisotropy`, and the log says if it's missing.
  Two other things soften the 3D picture and are not filtering: unless `native_720p` (above) is on, the game
  renders the match at 1152x640 and stretches it to 1280x720 (draw `post_process` 12e50a79),
  and a window shorter than the scaled frame gets it shrunk to fit (2560x1440 into a maximized
  2560x1362 window is 0.95x); fullscreen (F11) shows the frame 1:1. Depth of field and motion blur
  (`bb_post_process`) are off for still pictures such as the kick aim: checked with `BB_DEBUG_EXPR`.
- Compiled shaders are cached on disk: `$XDG_CACHE_HOME/backbreaker-recomp/spirv/` (else
  `~/.cache/...`), one SPIR-V file per shader, keyed by a hash of its GLSL and compile options, so a
  changed translation simply gets a new file. Compiling with shaderc was nearly all of a new shader's
  cost (1.8 s for the 61 shaders the side select needs, a freeze, every run); with the cache only the
  very first run pays it. At the end of a frame that needed new shaders or pipelines (at most once a
  second, adding up everything since the last one) the renderer logs a `first use` line: how many
  it translated, how many shaderc compiled, how many came from the cache and how many pipelines it
  created, each with the time it took. Deleting the folder is always safe.
  `--no-shader-cache` compiles everything.
- `--no-render` turns the renderer off (command stream side effects only, as in phase 3).
- Unsupported textures (1D/3D/cube and some formats, for now) show as magenta, and unsupported
  features log one `[render] WARNING` each.

On a crash, timeout, `kill` or Ctrl+C it prints the host backtrace (sub_XXXXXXXX names are guest functions),
the last 32 kernel calls, a per-import usage summary, and the allocations still held grouped by
the import that made them.

While a run is up, the GPU thread prints one status line per second on stderr, and that line is the
frame trace every frame-rate question here starts from:

    [rt] [gpu] 30 fps, 900 draws/s, 30 frames total, front buffer 00840000 1280x720, 390 xma frames, mem 43+62 MB, longest frame 33 ms, speed 100%

`fps` and `draws/s` are that second's counts, `frames total` is what the guest has finished since
the run started (so comparing it across two runs of the same timed route is the frame-rate ratio),
the front buffer is the guest's own surface address and size, and the two numbers after `mem` are
guest virtual and physical bytes in use, in MB. `longest frame` and `speed` (#146) are the longest
gap between two of the game's swaps in that second and the game's speed over it (see "Frame rate",
"Speed"); they come last, so tools that read the line from its start still work with older logs.
`[rt] ` is the prefix on every log line (`info()`
in `runtime/log.h`); the tag after it says who wrote it - `[gpu]`, `[render]`, `[iso host]`,
`[video]`, `[capture]`. `tools/fps_report.py` reads a saved log of these lines into a dip report;
it is under "Debugging aids" below.

## Vulkan validation
`BB_VK_VALIDATION=1` runs the process under the Khronos validation layer; `2` adds the layer's
info messages. `runtime/video/vk_context.cpp` installs one `VK_EXT_debug_utils` messenger, both
chained into `VkInstanceCreateInfo::pNext` (so instance creation itself is validated) and created
with `vkCreateDebugUtilsMessengerEXT` (device-level messages). Messages go through the `[rt]`
logger, not the layer's own stdout messenger; an identical message is logged three times and then
counted only. Every way the process ends prints one line:

    [rt] [vk-val] 1 validation message(s), 1 distinct: 0 error(s), 1 warning(s)

A pipeline is named by its handle in these messages, and the `[render] pipeline N 0x...` line the
renderer prints when it compiles one carries the same handle, which is how an error gets tied back
to the guest state that made it.

That is what a run has to look like to call itself clean, and the route it is measured on goes into
a match: `--press "a@20,a@26,a@42,right@47,a@51,a@80,a@86,a@100,a@106,a@112,a@118,a@124,a@130,a@170,a@180,a@190,a@200"`
is boot, the movies, the saves, the menus, Exhibition's side, team and stadium selection, the match
load and the match itself, so the 3D scene's draws, the skinning export draws and the HDR and depth
targets are added to the front end's transfers and clears. A headless run of it under the layer
(8020 frames) ends with `5 validation message(s), 2 distinct: 0 error(s), 5 warning(s)`: the
loader note below and one SPIR-V interface note the layer itself calls "not invalid" (a vertex
shader output that one pixel shader does not read). It is not the whole check: a shader that
fails to translate or compile is not a validation message - the renderer logs a `[render]` warning
and carries on without those draws, which takes the front buffer and the screenshots with it. The
log has to be read as well as the count line. On the route above the `[render]` warnings left are all
"not supported yet" ones - texture formats and depth resolves - each logged once per thing
missing, none of them API misuse.

What is left in a clean run is loader noise, not our API use: `terminator_CreateInstance: ... libvulkan_dzn.so` (that driver cannot open
here) and, at level 2, which ICDs were found and how the physical devices sorted.

What the renderer keeps in step for this:
- A dynamic render pass carries the color slots up to the last one that has an image
  (`colorSlots` in `renderer.cpp`), and the pipeline for it is built with the same number; the two
  counts have to be equal anyway (VUID-VkGraphicsPipelineCreateInfo-renderPass-06055,
  VUID-vkCmdDraw-colorAttachmentCount-06179). A slot inside that range with no image keeps its entry:
  no format (`VK_FORMAT_UNDEFINED`) in the pipeline, and no image view in the pass, in which case the
  spec ignores the attachment's other members and discards writes to it.
- A pixel shader used in a pass that has no image for one of the outputs it writes is compiled again
  without that output (`BB_COLOR_MASK`, `Shader::colorVariant`): a write to a location the pass has
  no attachment for is an undefined value, which the layer calls
  `Undefined-Value-ShaderOutputNotConsumed-DynamicRendering`.
- `shaderDemoteToHelperInvocation` is enabled only when the GPU offers it, and shaderc is told to
  target Vulkan 1.2 on a GPU that does not (a 1.3 module that declares the demote capability is
  invalid unless the device has it, VUID-VkShaderModuleCreateInfo-pCode-08740).
- Features the renderer asks the device for are read with `vkGetPhysicalDeviceFeatures2` and only
  requested when present, instead of asking for names the GPU may not have.
- A pipeline with rasterizer discard (a memory export draw with no render targets) leaves the
  fragment stage out: such a pipeline may not carry one
  (VUID-VkGraphicsPipelineCreateInfo-pStages-06894), and the export comes from the vertex shader,
  which runs anyway.
- Every pipeline states its viewport, scissor, stencil masks and reference, constant blend color and
  depth bias dynamically - the EDRAM transfer pipeline included, so a transfer after a scene draw is
  a draw like any other - and a draw then sets a state only where the bound pipeline uses it: the
  stencil ones only when its stencil test is on (a depth buffer used without a stencil test has no
  stencil state, and setting it anyway is counted as drawing with a state the pipeline states statically,
  VUID-vkCmdDraw-None-08608), the depth bias only on a polygon offset.

Clean has to hold while the window is being driven, not only boot to movie: 1280x720 to 900x520 to
1600x900 (each size goes through `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED`, which tears the swapchain
down and makes another one after `vkDeviceWaitIdle`), iconified and back (the minimized path fails
`createSwapchain` and retries every 50 ms), and closed. With the layer on, all of that ends
with the same line: 0 errors, and only the loader's own note left.

## Layout
- `runtime/main.cpp` - loader, indirect call table, crash handler
- `runtime/memory.*` - 4 GB guest address space and allocators; every tracked allocation records
  the import that made it, which is what the report above can group by
- `runtime/kernel/` - kernel/XAM imports
  - `objects.*` handles, events, semaphores, waits
  - `thread.cpp` guest threads (PCR, KTHREAD, TLS, host pthreads); the stack is returned to the
    heap when the thread exits
  - `files.cpp` file system (game:\ -> game/disc, cache:\ -> game/cache; saves mounted at runtime)
  - `xam_content.cpp` saves: content packages in game/saves/<type>/<name>/, mounted as save:\
  - `memory_imports.cpp`, `sync.cpp`, `strings.cpp`, `system.cpp`, `xex.cpp`, `variables.cpp`
- `runtime/generated/imports.cpp` - weak logging stub for every import (from tools/gen_imports.py);
  compiled into the game, not the runtime library (see "Runtime kit")
- `runtime/sdk/ppc_config.h` - the executable's layout the runtime library is built for
- `installer/` - `backbreaker-installer`: the image check and the game-file steps in C++ (see "Installer tools")

## Status (2026-11-12, headless phase 3; the current state is in docs/ROADMAP.md)
Phase 3 (headless subsystems, see docs/ROADMAP.md) works end to end: the game boots, initializes graphics, audio, voice
(XHV2), comms and input, plays the 505 and NaturalMotion logo movies at ~30 fps, loads its sound
banks (~3 s), then runs its front end at a steady 30 fps (~65 draws per frame, likely the game's
own cap). Tested for 5 minutes: memory stays flat (~106 MB virtual + 276 MB physical) once the
load is done. It did not use to: a thread's stack was never returned, so the front end's movie
player grew the virtual heap 1 MB per loop (issue #5). Nothing is
drawn: the command processor executes PM4 packets only for their side effects.

The earlier "slow menu" was a loading screen stuck forever: FMOD's sound-bank loader waited on the
XMA hardware decoder, which did not exist. With XMA emulated (silent output) loading completes.

Subsystems:
- `runtime/video/presenter.cpp` - window and presentation: the process main thread runs SDL and
  Vulkan, the guest's main thread runs on its own host thread (16 MB stack); at each swap the
  command processor only publishes the front-buffer address/size (lock-free) and the presenter
  copies it on its own schedule, so the game never waits on the window. Currently a raw copy:
  the picture is black because nothing renders into the front buffer yet. Window size, position
  and fullscreen state are read from `game/settings.ini` (issue #9); `runtime/settings.cpp` reads
  and writes that file with every key it knows, the resolution scale (#115) included
- `runtime/gpu/` - Vd* imports, PM4 command processor, register file at 0x7FC80000, 60 Hz vblank
  interrupt; MMIO stores reach it through `runtime/ppc_hooks.h` (force-included into ppc/).
  `GuestMmioStore32` only warns about a store no device handles inside the device window
  0x7F000000-0x7FFFFFFF (`mem::kDeviceBase`/`kDeviceEnd`): XenonRecomp turns every store followed
  by `eieio` into `PPC_MM_STORE_U32`, and D3D also uses `eieio` after plain writes to physical
  memory, so those are stored as normal memory with no warning (issue #25).
  Predicated tiling is honored: `SET_BIN_MASK`/`SET_BIN_SELECT` (and their `_LO`/`_HI` halves)
  set 64-bit masks, and a packet with the predicate bit (header bit 0) is skipped unless
  `select & mask` is non-zero, as in Xenia. D3D's replayed command buffers depend on it: without
  it, the GPU->CPU callback that queues D3D's worker job fired on every replay (3000+ times),
  the job's pending count went negative and match loading hung
- `runtime/apu/audio.cpp` - XAudio render driver: the game's (FMOD's) render callback every 256
  samples at 48 kHz, submitted frames to SDL3 and `--audio-dump` (see "Audio" below)
- `runtime/apu/xma.cpp` - XMA decoder: 320-context array (register 0x600), kick/lock/clear
  registers (0x650/0x690/0x6A0); a kick runs one decoder pass on the kicking thread
  (`xma_context.h`, frames decoded by `xma_frame_decoder.cpp`; see "XMA" below)
- `runtime/hid/` - one neutral pad on user 0, no keystrokes
- `runtime/kernel/xam.cpp` - notifications, local profile "Player", profile settings (the console's
  defaults for a fresh offline profile, logged by name; the game never writes any, #98)
- `runtime/kernel/xnet.cpp` - networking, offline: XNet/WSA startup succeeds, no Ethernet link,
  DNS lookups fail (the game looks up "localhost" when a match loads), `WSACreateEvent` returns a
  real event (as Xenia's xam_net.cc). Sockets are kernel objects with handles from the same table,
  so `socket`/`closesocket`/`setsockopt`/`ioctlsocket` (FIONBIO)/`bind`/`listen` succeed and keep the
  state asked for, but nothing is ever connected or received: `connect` is refused, `select` reports
  nothing ready after at most 100 ms, `recv`/`recvfrom`/`WSARecvFrom`/`accept` give WSAEWOULDBLOCK,
  sends succeed and are dropped, and an unknown handle gives WSAENOTSOCK. No host socket is opened.
  Offline is the only mode this has ever been run in. To try online, each socket object would carry
  a host socket (family and type as asked for, addresses converted since the guest keeps them in
  network order), and `select`/`recv`/`accept` would report real readiness instead of inventing a
  timeout - which also means waking on the guest's signalling rules through `kernel::waitForObjects`
  rather than sleeping. Even then a match could not be hosted or joined: the Xbox Live half (XNet
  signing, XNADDR/XID mapping, QoS, the `XMsg*` requests) is still stubbed.
- `runtime/kernel/scopes.cpp` - the game's compiled-out profiler markers carry function names;
  crash and timeout reports list each thread's recent scopes
- `runtime/debug/guest_trace.cpp` - GUEST_TRACE(addr) wraps a guest function to log args/result

## Audio
The game mixes its own audio (FMOD Ex, `media/sounds/XBox360/*.fev/.fsb`) and hands the result
to the render driver one frame at a time: `XAudioSubmitRenderDriverFrame` gets 6 channels x 256
samples of big-endian float, one channel after another, in the 5.1 order FL FR FC LFE BL BR
(`runtime/apu/frame_convert.h`). The runtime queues each frame on an SDL3 audio stream on the
default playback device: as 5.1 when the device has more than two channels (SDL maps it to the
device layout), otherwise as a stereo downmix (centre and surrounds at -3 dB, LFE dropped, front
channels at full level). FMOD mixes in 5.1 even though `XAudioGetSpeakerConfig` says stereo: with
XMA decoded (#93) all six channels carry sound on the test route; only the logo movie is front-only.

The render callback runs every 5.33 ms on its own guest thread. With a device open, the stream's
fill level steers it: below 4 frames (21 ms) an extra callback runs, at 12 frames (64 ms) one is
skipped, so the game's audio clock follows the device's clock and the queue can neither run dry
nor grow. Without a device the timer alone paces it.

- Windowed runs play audio; `--mute` opens no device (the game's audio still runs).
- Headless runs open no device unless `--audio` is given.
- `--audio-dump <file.wav>` writes the stereo downmix of every frame as a 32-bit float WAV, with or
  without a device (the header is rewritten about once a second, so a killed run leaves a valid file).
  `SDL_AUDIO_DRIVER=disk SDL_AUDIO_DISK_OUTPUT_FILE=<file>` with `--audio` exercises the device path
  (pacing, underrun counting) without sound.
- Every 5 s the log has `[apu] N frames (+n, a audible), queue q, underruns u, skipped s, extra e;
  xma kicks k, frames d decoded s silent, errors e decode p parse, t ms/s in passes (u decoding)`
  (queue in frames, -1 without a device; "audible" = a sample above 1/32768; xma counts are totals
  since launch; the times are CPU time per second of the last 5 s, summed over the game threads
  that kick); the exit report adds per-channel peaks and RMS levels.

### XMA
Nearly every sound in the banks is XMA (FSB sample mode `FSOUND_XMA`). FMOD drives the decoder
hardware itself: it fills a context (two input buffers of 2 KB packets, an output ring of 256-byte
blocks, loop points, subframe counts) and kicks it through the kick register; it then reads 16-bit
big-endian PCM from the ring, moves the read offset, sets the output valid again and kicks again.

A kick runs one decoder pass on the kicking thread (`runtime/apu/xma_context.h`, a port of Xenia's
`XmaContextNew`): frame by frame, following frames across packets and into the other input buffer,
until the output ring is full or the input runs out; a full ring is reported by clearing the
output valid bit. Lock waits for a pass in progress on that context, and a pass's result is merged
into the context as it is in memory at the end (only the fields the decoder owns), so the game's
own writes meanwhile survive. `tests/xma_context_test.cpp` checks the walk on synthetic packets.

Each frame goes to `runtime/apu/xma_frame_decoder.cpp`: FFmpeg's `xmaframes` decoder (one frame
per packet, preceded by a byte with the bit padding), built from the FFmpeg fork Xenia uses by
`scripts/build_xma_decoder.sh` into `third_party/ffmpeg-xma` (static, decoder only, LGPL-2.1+;
`scripts/setup.sh` runs it, `--no-xma-decoder` skips it). CMake links it when it is there and
says so; without it the build still works and every frame plays as silence (the log's first
`[xma]` line says which). A frame the decoder rejects also plays as silence; the first 20 are
logged with FFmpeg's error.

One difference from Xenia: a pass that starts with no valid input still delivers the rest of the
stream's last frame (Xenia's starved-input branch resets the write offset and drops it).

## Movies
The game plays its movies with its own player, Microsoft's WMV decoder (VC-1 video, WMA Pro audio)
linked into the executable: the CPU decoding is recompiled code, and the picture is assembled by
GPU shaders the renderer runs. All are 1280x720 at 29.97 fps with stereo sound at 48 kHz:
- `505.wmv` (11.4 s) and `nmlogo.wmv` (9.9 s), the boot logos, played by `NmgMovie::Play`
  (`sub_82804660`) from the boot code while the game loads. `skip_intro=1` in `game/settings.ini`
  or `--skip-intro 1` leaves them out (`runtime/intro_movies.cpp`); headless runs skip them unless
  given `--skip-intro 0`, so the title screen comes about 4 s after launch, as scripted routes expect.
- `attract.wmv` (30.5 s), on the title screen after about two minutes without input.
- `Bonus/Trailer1-5.wmv` (69-85 s; Trailer5 is a copy of the attract movie), in Locker Room > Bonus >
  Media > Videos (`media\bonusVideo\bonusVideo_list.xml`).

Every movie used to stop after a second or two (#132). The player keeps eight 128 KB reads ahead of
its parser on each of its two file handles and reads one block past the end of the file; it expects
`ERROR_HANDLE_EOF` from that read and treats any other error as a broken stream. The Win32 error
comes from `RtlNtStatusToDosError`, which answered 317 (`ERROR_MR_MID_NOT_FOUND`) for every status.
The parser, still a second or two of video behind its reads, then failed its next refill
(`0x80500001` from `sub_828188E8`), the video decoder reported end of stream, and the player said it
was done. `runtime/kernel/ntstatus.h` now maps the statuses as Windows does (tested by
`tests/ntstatus_test.cpp`). Checked headless against the files decoded with FFmpeg: the boot logos,
the attract movie and trailer 1 play to the end, and their sound matches the soundtrack sample for
sample at a constant offset over the whole movie, so speed and A/V sync are right (the picture
follows the same clock, within a frame or two in screenshots).

## Achievements
The game's 29 achievements (1000 G) come from its own SPA resource: an XDBF blob in the XEX,
named after the title id 464F07DA, at 0x82E00000 in the loaded image (`runtime/kernel/spa.h`
parses it; `tests/spa_test.cpp` checks the parser on hand-built blobs). Only English strings are
in it.

The game reports an unlock with the XDK's `XUserWriteAchievements` (recompiled as `sub_828F7A30`),
which is `XMsgStartIORequest(0xFB, 0x000B0008, overlapped, {count, X_USER_ACHIEVEMENT*}, 8)`: a
message to XAM's XGI app, not an import of its own. `runtime/kernel/xam_achievements.cpp` handles
it: the first time an id is reported, the log says
`[xam] achievement unlocked: First of Many (10 G)` and the unlock is written to
`game/achievements/464F07DA.txt` (`<id> <unix time>  # name` per line; gitignored with the rest
of `game/`). When a match starts the game reads the list back through
`XamUserCreateAchievementEnumerator` (all 29, with texts), and earned ones carry the "achieved"
flag and their unlock time, as in Xenia, so the game knows they are done.

The game has no achievements screen of its own: on the console that was the system Guide's, and
the game does not even import `XamShowAchievementsUI`. The runtime draws one instead (#101,
`runtime/video/overlay.cpp`):
- **F1**, or the controller's **Guide (PS) button**, opens the achievements list over the game:
  every achievement with its icon (from the SPA), gamerscore, description, and the unlock date of
  the earned ones; secret ones show as "Secret achievement" until earned; totals at the top.
  Up/Down (d-pad, left stick or arrow keys) moves, LB/RB or PgUp/PgDn a page, Home/End; B, Back,
  Esc, F1 or Guide closes it. While it is open the game sees a neutral controller (scripted
  `--press` input still goes through).
- **On a Steam Deck** the Guide button is the Steam button. Steam keeps it for its own menu and the
  game never sees the press, and the Deck has no F1 key. The way in is a back grip mapped to F1 in
  the game's Steam Input layout: R4 → keyboard F1, tested in Gaming Mode on 2026-11-17 (#145; README,
  "Steam Deck and other distributions", step 5).
- An unlock shows an "Achievement unlocked" pop-up (icon, name, gamerscore) for 5 s, at the top of
  the screen, or at the bottom of the list when the list is open, with the unlock sound. Several
  unlocks at once take turns, and the pop-up says how many more are waiting (#105).
- The unlock sound is the runtime's own (the console's belonged to the dashboard, not the game): a
  short (0.45 s) bright "pip-pop" synthesized in `runtime/apu/system_sound.h`: a soft B5 pickup,
  then an E6 tone that glides up into pitch over a quiet shimmer. It is in the spirit of the
  console's sound (short, bright, rising) but its own notes and shape. Peak -4.4 dBFS,
  mixed into the front channels of the game's audio output (device and `--audio-dump`).
- The game is turned down 10 dB under the sound (ducking: down over 15 ms, back up over 0.3 s
  after the sound ends, about 0.75 s in all). In play the game runs at about -18 dB RMS and
  reaches -10 dB under a touchdown crowd, which buried the sound when it was simply added on top;
  ducked, it sits 7-8 dB over the game and the mix stays under 0 dBFS.
  `BB_DEBUG_CHIME=<seconds>` plays it at that time in any run, to check it in a dump.
- A sound of your own replaces the chime: put it at `game/sounds/achievement.wav` (any WAV that
  SDL reads; `ffmpeg -i <your file> game/sounds/achievement.wav` converts other formats). It is
  converted to 48 kHz mono and scaled to the chime's peak, and cut at 10 s; the game is ducked under
  it the same way. The log says which
  sound is in use (`[apu] unlock sound: ...`). Like everything under `game/`, it is never committed.
- F12 screenshots include what the overlay shows.
- `--overlay-preview <file.png>[@WxH]` draws the list and a pop-up over a grey backdrop into a PNG
  (default 1920x1080) and exits: the drawing can be checked without a window or a GPU.

How it is drawn: on the CPU with SDL3's software renderer (its built-in 8x8 font, scaled by
2/3/4/6 at 720p/1080p/1440p/2160p; icons decoded with `SDL_LoadPNG_IO`), only when what is shown
changes. The presenter then calls `overlay::record` once, after the game's blit and before the
F12 read-back: it copies the new picture into an RGBA8 image when it changed and blits the
visible panels (opaque) onto the swapchain image, in the presenter's own command buffer. Nothing is
created until something is first shown (then one image and one staging buffer the size of the
window), and nothing is recorded while nothing is shown: a hidden overlay costs one atomic load per
present (the unlock counter).

`--achievements` prints the same list in the terminal and exits without starting the game (run it
from the repo root).

The other XGI messages the game sends: `XUserSetContext` (0x000B0006) and `XUserSetPropertyEx`
(0x000B0007) for presence and stats (nothing to do offline), and `XUserReadStats` (0x000B0021),
which is refused as "not signed in to LIVE" (0x80151802, as Xenia). Any other `XMsgStartIORequest`
is reported as done, and every message is logged once.

## Frame rate
Windowed runs default to 60 frames a second, twice the console's 30 (#89). `fps=30` in
`game/settings.ini` or `--fps 30` gives the console's rate. Headless runs use 30 unless they are
given `--fps`, so test routes and their frame-numbered screenshots stay as they were. The log says
which applies: `[rt] 60 fps (game/settings.ini)`.

**The guest clock.** The game times its frames with `mftb` (the PowerPC time base) and the
performance frequency (`KeQueryPerformanceFrequency`). XenonRecomp emits `mftb` as the host TSC,
and the runtime used to report the TSC rate as the frequency. Backbreaker's frame timer
(sub_82649BE0, its struct at 0x82CD0138) keeps only the low 32 bits of that frequency. A 4.7 GHz
TSC leaves 405 MHz, so every frame measured 11.6 times too long. The game clock (0x82C7B9D0)
clamps a frame to 1/30 s, so every frame counted as exactly 1/30 s. That was right at 30 fps and
twice the speed at 60, whatever the frame actually took. Now the instruction patch emits `mftb` as
a call to the runtime's `ppcQueryTimeBase()` (`runtime/kernel/system.cpp`), a 50 MHz count, the
console's rate, scaled from the TSC. `KeQueryPerformanceFrequency` says 50 MHz too. A slow frame
now counts as what it took, up to the clamp.

**What makes it 30, and what 60 changes** (`runtime/frame_rate.cpp`):
- *The present interval.* Before each swap, the command stream has the CPU call D3D's
  sub_8241A410 (through an interrupt) with an argument whose bits 8-11 say how many vblanks a
  frame stays up. The game asks for 2 (argument 0x264): 30 fps on the 60 Hz vblank. At 60 it gets
  1. The log shows each new argument the game passes (`[gpu] swap argument 00000264: present
  interval 1 (the game asks for 2)`). Through the route and a match, it never changes.
- *The gameplay tick.* The game object (constructed by sub_82274628) runs the simulation in fixed
  steps. Each frame adds the game clock's time to an accumulator (+168) and runs one step for each
  +172 in it, 1/30 s. At 60 frames a second, every step would be drawn twice. At 60 the step is
  1/60 s. PhysX is set to take 1/60 s substeps (`NxScene::setTiming`, maximum 6), so it now does
  one per step instead of two.

Measured over the Exhibition route at 60: 60 PhysX steps a second of 1/60 s each, adding up to
1.000 s of game time per second, as at 30. A frame that runs late (the game clock's time above
1/60 s) gets two steps, so a dip below 60 fps keeps the game's speed. Below 30 fps the clamp
slows the game, as on the console.

Code that counts ticks rather than time runs twice as fast at 60. Tackle Alley's between-attempts
sequence raced a time-based fade that way and never finished (#139, fixed: "Fixes to the game's own
code"). Not covered yet: other code with its own idea of 30 Hz. The constant 1/30 (0x8200E744) is used in
about 50 functions, mostly as a clamp on a frame's time. One forces 1/30 into an update in
sub_82277110 while the byte 0x82C75097 is set. The code only ever clears that byte and nothing
points at it, so it looks unused, and it was 0 throughout the route and a play-test. Above 60
fps would need a faster guest vblank (`runtime/gpu/`, 60 Hz) and a matching tick.

**Speed** (#146, `runtime/gpu/frame_stats.h`). Because the game clock clamps a frame to 1/30 s, a
frame that takes longer loses game time: one 50 ms frame loses 17 ms, and the game plays in slow
motion. A second can do that and still count 29 or 30 frames. So each second also reports:
- the longest gap between two of the game's swaps;
- the game's speed, game time over real time: each gap of dt advances the game by min(dt, 1/30 s).
  A gap still open when the second closes counts toward that second, so a freeze shows in every
  second it lasts.

It's an estimate from the swaps' host times. The game measures its frames with its own timer, which
runs at the same pace, so the two agree while frames are steady. A loading screen that stops drawing
reads as slow too. Both numbers end the `[gpu]` status line (`longest frame 52 ms, speed 94%`).
`tools/fps_report.py` sums the game time lost and lists the slow seconds that aren't fps dips.

**The fps readout.** `show_fps=1` in `settings.ini` (or `--show-fps 1` for one run) shows the game's
frames, speed and longest frame over the last second in the top-left corner of the window, and turns
the speed gold below 98%. It's the number to watch on a Steam Deck. Its performance overlay counts
the window's presents, and the presenter presents on every vsync, showing the last picture again
when the game hasn't finished a new one. So under Steam's 30 fps limit that overlay reads 30 even
while the game slows down. The readout is drawn by the overlay (`runtime/video/overlay.cpp`) and
hidden while the achievements list is open. It redraws once a second and then uploads only its own
rectangle. Windowed runs only. `--overlay-preview` draws it with sample numbers when it's on.

## Fixes to the game's own code
- Tackle Alley at 60 fps (`runtime/tackle_alley.cpp`, #139): after a tackle the next attempt never
  started (the player stood still, the clock frozen). The between-attempts stage (43) starts a
  0.25 s fade to black, and Tackle Alley sets up the next attempt when that fade has finished,
  while still in stage 43. Meanwhile the play-flow state machine (sub_8215B710) steps one state a
  tick and requests the next stage after about 14 ticks: 0.47 s at 30 fps, but 0.23 s at 60, before
  the fade is done. Its fade back in then cuts the fade to black off, and the attempt is never set
  up. The play flow now waits in that state (64) while the fade to black runs in stage 43, which
  restores the order the console's timing gave; at 30 fps it never has to.
- A play's player slots (`runtime/play_lock.cpp`): a play builds its list of 11 player slots on
  first use under the play lock (`sub_82323D28`), and the render thread does that when it looks a
  play up to draw the route display. The drill set-up in `sub_8236E8D0` has the same build inlined
  without the lock, so on a Training Camp retry both threads could build the same play: 22 slots,
  and `sub_823DBCD0` crashed handing out the team's 11 players (a read at guest 0x0000FB00, most
  passing-drill retries). That one build now takes the play lock too and checks the play's "built"
  flag inside it.

## Controllers
Controllers are read through SDL3 (users 0-3 = the first four controllers SDL lists; a
DualSense's Cross/Circle/Square/Triangle are A/B/X/Y, Create/Options are Back/Start). The log
shows `[hid] controller on user 0: ...` when one is found. On KDE Plasma 6.7+, turn off System
Settings -> Game Controller -> pointer navigation, or the stick also moves the mouse.

## Scripted input
`--press "start@12,a@20,down@24.5"` presses each button that many seconds after launch (held
150 ms), reported as pad state and as keystrokes, added to any real controller on user 0. Routes
count on the title screen about 4 s after launch: headless runs skip the two boot logo movies
(about 21 s) unless given `--skip-intro 0` (see "Movies").
Buttons: a b x y start back up down left right lb rb lt rt ls rs (`lt`/`rt` pull the trigger fully
for the hold). Example that gets past the title
screen to the main menu: `--press "a@20,a@26"`. Sticks: `ls_` or `rs_` plus `left`, `right`, `up`
or `down`, as `stick@seconds[:duration]` - full deflection of that axis for the duration (150 ms
by default), then released; reported as pad state only (no stick keystrokes). The kickoff of the
Exhibition route: `rs_down@196:1.0,rs_up@197:0.3` pulls the right stick back and pushes it forward.

`--fixed-seed` gives a run the same random numbers as every other run with the flag (#71), so a
route's coin toss, wind, weather and loading screens come out the same. For test runs only.
- Most of the game's randomness comes from one generator (multiply-with-carry, state at
  `0x82CC5430`), which the game seeds from the wall clock and re-seeds when a match loads. A fixed
  seed alone doesn't help: it is drawn about 10,000 times a second, much of it once per frame and
  from up to five threads, so how many draws come before the toss depends on timing. Under the flag
  every call site gets its own stream instead (`runtime/fixed_seed.cpp`), and the re-seed restarts
  them all. A one-off pick such as the toss gets the first number of its own stream, however long
  the menus took.
- The C runtime's `rand()`, seeded with `time()` and `GetTickCount()` at startup, gets the same
  seeds: the guest date starts at 2010-07-01 12:00:00.5 UTC, and the boot clock holds at 0 for its
  first second.
- Still different between runs: whatever depends on frame timing or on the frame a press lands on,
  such as physics and how a play unfolds.

## GPU frame capture
`./build/backbreaker --capture-frame 1200` (or a list: `--capture-frame 300,1200,2400`) records the
frame ending with that swap into `captures/frame_N/` (gitignored). `--capture-at S[,S...]` takes
seconds since launch instead, like `--screenshot-at`; the folder is still `frame_N`, and the log
says which (`[capture] --capture-at 230: frame 6512`). In the window, **F10** captures
a frame right after the press (the log names its folder), for a moment that is hard to reach by
frame number:
- `summary.txt` - draw count, primitive types, shaders used, render-target setups, textures
  (format, dimension, size, tiling), distinct pipeline states (fixed-function state per draw,
  with draw counts), PM4 packet histogram
- `draws.txt` - every draw: packet, current vertex/pixel shader, the decoded draw state
  (`runtime/gpu/state.cpp`: primitive and indices, EDRAM mode, render targets with formats, tile
  bases, write masks and blending, depth/stencil, rasterizer, alpha test, viewport, scissor,
  shader setup), then the raw registers changed since the previous draw (named from Xenia's
  register table), the ALU constants that changed, as floats (`vs c12 = (...)`, `ps c38 = (...)`),
  and the bound texture fetch constants; shader loads are listed in order. Each texture line ends
  with what guest memory holds at its address (`data: N/M nonzero, fnv <hash>, first bytes <hex>`,
  over the first 256 KB): a texture the game renders on the GPU and reads back, or never fills,
  shows as all zeros or as the same pattern every frame
- `registers_start.txt` - non-zero GPU registers at the start of the frame
- `shaders/vs_*.bin`, `shaders/ps_*.bin` - shader microcode used in the frame (big-endian dwords)

`--dump-shaders` writes every unique shader the game loads during a run to `captures/shaders/`.

To compare two of those folders: `python3 -I tools/capture_diff.py captures/frame_1006
captures/frame_1026` (issue #79). It prints only the parts that moved - the counts, the sets of
render targets, textures, pipeline states and shaders (including whether the microcode behind a
shader name matches the bytes the name hashes to), the EDRAM copy list, the frame-start registers,
and the draw sequence - and stays silent about the parts that did not. What moved is what changed
in what the renderer asked for: a pipeline state only `summary.txt` records, a texture the frame
stopped sampling, one shader's microcode, one uniform, one draw's blending.

Two frames of the same scene differ in things that say nothing about what was drawn, so by default
it ignores the draw numbers (`draw 133` in `draws.txt`), the raw register deltas, and the base
addresses in the index-buffer pointer and in the texture lines; `--raw` puts those back when where
something was allocated is the thing being chased. Two of the things printed among those register
deltas are not registers, and stay in either way: the shader constants (`vs c4 = (...)` is a
uniform, and it says where an object stands and what colour it is) and the textures, which are
compared as what they are - format, dimension, size, tiling - rather than the address they were
uploaded to. Two things there are left with the deltas: the `(N ALU constant dwords changed)`
marker, which counts what moved since the draw *before* this one and so changes when that draw
changes; and the vertex fetch constants, whose `data=` hash is taken, by design, over the first
64 KB at the address rather than over the vertices the draw reads, so it moves between two frames
of a still picture. `--raw` prints both as written. Draws line up on what
they are rather than on exact text, so a draw whose blending changed is printed as that one field,
and one extra draw is printed as one insertion instead of the rest of the frame having changed.
`--brief` names what moved without printing the draw blocks, `--max N` caps the items per section,
`--fields N` the changed fields of one draw, `--all` shows the sections that match too.

Exit status is 0 if the frames agree, 1 if they differ, 2 if a folder is not a capture, so a bisect
can be driven by it: capture the same frame on two builds and compare the folders (the `built from`
line each capture's summary starts with is shown in the header and never counted as a difference). A capture that
contradicts its own files (a `draws.txt` short of the draw count in `summary.txt`, a shader list
that does not match `shaders/`) is reported as a problem instead of being trusted, and a section of
`summary.txt` this script does not read is reported rather than skipped, so a capture from a newer
runtime cannot hide a difference. Those rules are checked by `tests/test_capture_diff.py` against
the small capture pairs in `tests/data/`, which CI runs.

## Which effect a captured shader came from
`python3 tools/shader_effects.py captures/frame_1006` (or single `.bin` files) answers that
question, which a capture cannot answer by itself: it names a shader only by a hash of its
microcode. The answers come from the game's shader pack, `PackedOutputX360.pkh` (index) +
`PackedOutputX360.pkd` (data) under `game/disc/packedfilesx360/` - so it needs the extracted
game files, which are gitignored and therefore not in CI. 2035 entries, 57 of them `.fx360`
effect files, the units the renderer names (`player_highlight.fx360`, `bb_ball.fx360`);
`--effects` lists them and `--extract DIR` writes them out (game data, never commit it).

Over the 173 distinct shaders in the captures here (84 pixel, 89 vertex), 82 pixel shaders sit
verbatim inside one effect, at a byte offset partway into the entry, and 2 are in the pack
nowhere. No captured vertex shader matches its pack copy byte for byte: the tool instead reports
the longest unbroken run of dwords it shares with an effect as `[partial]` (`96/126 dwords
agree: shader 30-125 is pack 559-654`), which is a shortlist rather than an answer. That shader
shares 96 dwords with `bb_satin` and 94 with `bb_cloth`, `bb_foam`, `bb_leather` and
`bb_armour_leather`, because those effects ship the same vertex shader; the difference in
agreement is only how far each effect's copy was relinked, which says nothing about which of
them drew the triangle, so the line names the effects it cannot separate with their counts
(`4 more within 10 dwords: bb_armour_leather.fx360 94, bb_cloth.fx360 94, ...`) instead of
picking one. 12 of the 62 partial vertex shaders read like that. Agreement runs 50-88% of the
shader's dwords (median 71%), and where there is a run it ends where the captured microcode does
and starts partway into it (anywhere from dword 12 to 93): the capture holds extra dwords in
front of the pack's copy, and the tool says so on the line.

The other 27 vertex shaders name no effect, and the report says how much they agree with
anything in the pack rather than leaving that out: 21 share 9 dwords with some effect (the
prologue they have in common) and nothing more, 3 share 5, one shares nothing, and 2 agree over
12 of their 30 dwords - that clears `--near 12` but is less than half the shader, and the line
says which of the two cuts it failed (`shorter than half of the 30 dwords it is`) because
"below --near" would be false. So the `--near` threshold is not what leaves 25 of the 27
unnamed; there is a clean gap, and nothing sits just under it.

Exit status is 0 when every shader was found, 1 when some were not, 2 for a missing pack or a
bad input, so it can gate a check.

## Renderer debug aids
Environment variables for chasing a wrong picture (`runtime/gpu/debug_aids.cpp`, issue #40). Each
is off unless set, they're read once, and their log lines start with `[debug]`. Shader hashes are
the ones in a capture's `draws.txt` (`ps=...`); addresses are hex, physical or in any of the
A0/C0/E0000000 views.
- `BB_DEBUG_PS=<hash>[,<hash>...]` paints those pixel shaders' color 0 in a flat color, by position
  in the list: magenta, cyan, yellow, green, red, blue, white, orange. A hash can be shortened to
  its first hex digits (`2bf5593d`). A draw that shows up in the screenshot in its color is that
  shader's; one that stays hidden is covered, clipped or not drawn.
- `BB_DEBUG_EXPR=<GLSL vec4>` (with `BB_DEBUG_PS`) outputs that expression instead, to look at one
  value: `vec4(r[4].xyz, 1.0)` (a temporary register), `vec4(psc.c[38].xyz, 1.0)` (a constant).
  The names are the translator's (`xenos-translate` prints a shader's GLSL).
- `BB_DEBUG_DUMP=<address>` also saves the 8_8_8_8 resolve image at that address on `--screenshot`
  and `--screenshot-at` frames, as `screenshots/resolve_<address>_<frame>.png`, and logs its channel means (an image that
  is all transparent black, for example, shows as `A 0.000`). The PNG is in the image's own channel
  order; the log says when the guest reads it with red and blue swapped.
- `BB_DEBUG_WATCH=<address>[,<frames>]` hashes the 256 KB there at every swap and logs each frame
  where it changed (nonzero bytes, first bytes). It logs every resolve whose destination overlaps
  those 256 KB as it happens. With each change it also lists the resolves of the last `<frames>`
  frames (default 30) wherever they went, one line per destination, largest first (top 16), since
  the CPU may copy or compress a resolve into the watched range: the field lightmap in issue #45 was
  resolved to 08B14000 and compressed to 05DC5000.
- `BB_DEBUG_RESOLVE_CHECK=1` checks the resolve-image check (below) against a hash of the image's
  whole memory, every time one is looked up. It logs each case once per address: a change that the
  sampled fingerprint alone would have missed (#114), a write that left the bytes the same (the
  image is dropped anyway), or a change that was missed and the image kept (a bug). Slow: it hashes
  every resolve image the game samples, every frame.

## GPU fences
The game learns how far the GPU has got from fences: `EVENT_WRITE_SHD` packets that write a value
to memory, D3D's scratch registers, and the ring's read pointer. On the console the command
processor writes them when the GPU reaches them. The runtime does the same (issue #73): the
renderer counts its submits on a Vulkan timeline semaphore, each pending GPU write (resolve
writebacks, memory export) carries the submit that makes it, and a fence written after pending
writes is queued with that submit instead of stopping the command processor. Once the GPU has
finished it, the pending writes are copied to guest memory and then the fence is written, in
order. The command processor applies finished ones between packets, while it waits in
`WAIT_REG_MEM` (the game may be waiting for one of them) and while the ring is idle. Everything
else the guest can see a GPU result through (`MEM_WRITE`, `REG_TO_MEM`, `COND_WRITE`,
`EVENT_WRITE_EXT`, interrupts) still waits for all of it first. The renderer also submits its
work in pieces as it records (after resolves and memory export draws, and before the command
processor waits for the game), so the GPU runs while the CPU records instead of after it. Without
timeline semaphores (a log line says so) the command processor waits at every fence instead.

## Renderer performance line
`BB_PERF=1` adds one `[perf]` line per second (issue #73) that says where the renderer's time went:
swaps and draws, the CPU time of the command processor thread (which also runs the renderer) and
how much of it was spent polling in `WAIT_REG_MEM`, i.e. waiting for the game rather than working,
GPU waits and flushes, texture bytes hashed and the time it took, texture uploads, resolves and
writebacks, memory export draws (and how many waited for the GPU), syncs that had pending writes,
GPU vertex copies and draws recorded again after a sync, index and vertex copies reused (see
"Renderer"), and the write watch: texture checks answered without reading, write faults and pages
watched. Pair it with `tools/fps_report.py` on the same log for the fps side.

A second line covers the geometry cache (#146, see "Renderer"):
- hits, with their bytes, and new copies into the cache;
- copies moved forward in the ring, because they were too far behind;
- cached copies found written, which are then dropped;
- copies that didn't fit the frame's ring budget and went to the frame's part instead;
- how many vertex and index ranges it is tracking, and the megabytes written through the ring;
- with `BB_GEOM_CHECK=1`, how many cached copies it handed out that differed from guest memory.

`BB_CP_TIME=1` (with `BB_PERF=1`) adds a line with the command processor thread's time in draws,
split by step: setup, constants, indices, vertices, textures, EDRAM sync, pass and recording, and
memory export and after, with the bytes each copied into the upload buffer (#121). In heavy frames
that thread, not the GPU, decides whether a frame is ready by the next vblank: the game's present
waits for the GPU to drain, then for the vblank, so the CP's work per frame plus that drain has to
fit in 16.7 ms.

`BB_GPU_TIME=1` (with `BB_PERF=1`) adds where the GPU's time went (issue #121). The renderer writes
a Vulkan timestamp each time the kind of work it records changes, and reads them back once the
frame's GPU work is done. The time from one timestamp to the next is charged to the kind of work
that started at the first, so one more line a second gives:
- busy time;
- "between submits": time the GPU spent between two command buffers, idle or running the
  presenter's work;
- passes, full barriers and EDRAM transfers (and their tiles);
- milliseconds and stretches per kind: draws by pass (named by their attachments: format/guest
  format, pitch, MSAA, base tile), EDRAM transfers, resolve copies, MSAA resolves, depth
  resolves, writebacks, resolve clears, texture uploads, depth packing, vertex copies.

Every layout change is a full barrier, so the GPU runs these kinds one after another and the split
is close to what each costs. `BB_GPU_TIME=2` splits EDRAM transfers by source and destination
target, and `BB_GPU_TIME=3` also splits draws by pixel shader, blending and primitive type. Both
add a `calls by kind` line with how many of each there were, with their tiles or indices.

## Thread timing and hitch reports
`BB_PERF=1` also times every thread (#146; `runtime/debug/thread_times.cpp`, logic in
`thread_times_core.h`). `BB_THREAD_TIMES=0` turns that part off.
- **Why:** on the Steam Deck the remaining slow moments are single long frames, and the renderer's
  own lines couldn't say what the game's threads were doing during them.

What it measures:
- **From the kernel,** every 20 ms, for every thread: time on a CPU, and time runnable but waiting
  for one (`/proc/self/task/<tid>/schedstat`). Waiting for a core is what too few cores look like.
- **From the runtime:** each thread's wall-clock time inside the calls that block or spin, by kind:
  - guest waits (the four wait imports), sleeps (`KeDelayExecutionThread`), file opens and reads;
  - spinning on a held critical section (`RtlEnterCriticalSection` spins with `sched_yield`);
  - write-watch faults (the handler's part only; the kernel's share of a fault isn't seen);
  - the command processor polling `WAIT_REG_MEM`;
  - **the GPU fence spin:** D3D's wait for the GPU, `sub_8240F1B8`. It reads the fence and spins
    in `sub_82419CB0`, whose delay hints (`db16cyc`) are recompiled to nothing.
  - For each kind, the longest call and its guest caller are kept.
- **Thread names:** the threads carry them (`bb-gpu-cp`, `bb-vblank`, `bb-audio`, `bb-timestamp`,
  `bb-g<N>` for guest thread N), so `top -H` tells them apart. The names are set without
  `BB_PERF` too. Reports also use the game's own names ("Pool thread 0", "RenderThread",
  "Morpheme Update 2", "File-System thread").

What it prints:
- **`[hitch]`:** a report for every frame of 80 ms or more, measured over the 20 ms snapshots
  just around it:
  - a header with how many cores were busy, and the total time threads waited for a core;
  - a line for the game's main thread (guest 1), the command processor, and any thread that was
    busy for a tenth of the frame, waited for a core, read files, spun on a lock or faulted 50
    times. Each line gives CPU time, time waiting for a core, the time inside each kind of call,
    and the time outside them.
- **`[perf]   threads:`** each second: the same for the second, in short.
- **What the game's main thread ran.** Each `[hitch]` report also gives:
  - guest 1's stacks: the share of samples in the GPU fence spin, then its commonest stacks, innermost
    first. Each stack starts with the import or runtime function it stopped in, if any, such as
    `[MmAllocatePhysicalMemoryEx]` or `[__mprotect]`, followed by up to four recompiled
    functions (`sub_...`);
  - guest 1's functions: the recompiled functions on its stack in the most samples outside the
    fence spin, whole calls included. The main loop's outer frames are in all of them.
- **How the sampling works:** a timer on guest 1's own CPU-time clock signals it every 2 ms of CPU
  it uses, so a waiting thread isn't interrupted. The handler records the host return addresses in
  a ring of the last ~20 s, and a report symbolizes the samples of its window (`dladdr`, as
  `--sample-threads` does).
  - **Switches:** `BB_THREAD_STACKS=0` turns the sampling off, and `BB_HITCH_MS=<ms>` (at least 20)
    reports shorter frames than 80 ms, for a faster machine where the same work takes less time.
- **Why the main thread:** on the Deck the hitches in play are bursts of the game's main thread
  working through the whole frame, roughly once a play, while the other threads wait for it.

What it showed on the PC (Exhibition route, 30 fps, match seconds):
- The game's main thread spends about 950 ms a second in the GPU fence spin, and the command
  processor about 930 ms polling for the game.
- So at 30 fps two cores spin nearly all the time, each waiting for the other until the next
  vblank. With 16 hardware threads that costs the others about 30 ms a second of waiting for a core.

The Steam Deck has 8 hardware threads; its log is what tells whether this matters there.

What it costs, on the PC's route (`BB_THREAD_TIMES=0` against on, back to back):
- the sampler thread used about 1 ms of CPU a second;
- frames rendered were 4,724 against 4,703, and the command processor's work 4.5 against 4.8 ms a
  frame, both within run-to-run noise.

## Texture write watch
Finding out whether a texture changed used to mean reading all of its bytes every frame it was
used: 1.3 GB a second in a match, about a third of the renderer's time (issue #73). Now, before the
renderer reads a texture's guest memory, it write-protects those pages (`mem::watch`,
`runtime/memory.cpp`), in all three physical views (0xA0000000, 0xC0000000, 0xE0000000), since
any of them can write the same RAM. A write afterwards - by the game, a file read, the GPU results
the runtime copies back - faults once: the SIGSEGV hook in `main.cpp` stamps the page with the next
value of a global counter and makes it writable again, and the store is retried. A texture is
unchanged if every page it reads is still protected and carries a stamp older than the one taken
when the renderer last read it; the stamps keep two textures that share a page apart. File reads
into physical memory go through a host buffer (`mem::readFile`), because `pread` into a
protected page would fail with EFAULT instead of faulting. A texture that changed four times in a
row (a movie frame, an image the CPU draws) is left unwatched for 300 frames and hashed every frame
instead, so it does not fault on every write. The geometry cache (#146, see "Renderer") watches the
indices and vertices that stay the same the same way. Each protected run splits a memory mapping,
and Linux limits a process's mappings (`vm.max_map_count`, 65,530 on some distributions). In a match
the game holds about 1,500, so it's far from the limit. If `mprotect` fails anyway, the watch says so
once and stops protecting, and pages it couldn't protect never read as unchanged.
`tests/memory_watch_test.cpp` checks all of this on
the real memory layout, including the race between a fault and the renderer re-protecting a page.

Resolve images use the same watch (#63, #114, #119). A texture at a resolve destination samples
the resolve image (render to texture), but the game can load something else into that memory later:
in a match, the pause menu's Controls screen loads its controller picture where the team-coloured
jersey font was rendered. #65's check is a fingerprint, 256 bytes at the start of each of the first
16 pages, taken after each resolve (or once its writeback has landed). The new picture could match it
by chance, since both images are mostly empty, so the font sheet stayed in some sessions (#114). So
a lookup also watches a *stale* image: once its last resolve is 30 frames ago, its pages are
protected, all but a last one that another allocation may share. A later lookup that finds one of
them written drops the image, and the texture reads the guest's memory. Images resolved (nearly)
every frame are never watched: they are new each time, and a write fault costs the game's thread
about 90 µs. Watching every image used as a texture (#117) took 4,800 faults a second in a menu and
caused dips to 40 fps in matches at 60 fps (#119). The jersey font sheet is rendered once, when the
match loads. Like a texture that keeps changing, an image whose memory the game writes between one
resolve and the next, four times in a row, goes back to the fingerprint for 300 frames, then 600,
and so on. `release()`, used when a writeback lands over watched pages, unprotects runs of pages
with one `mprotect` per view instead of one per page.

## Shader disassembler
`./build/xenos-disasm captures/shaders` (files or directories, searched recursively) prints a
listing of each shader - control flow, vertex/texture fetches, paired vector+scalar ALU
instructions, exports - followed by a coverage summary (which opcodes, export targets, vertex
formats, texture dimensions and features appear, and how often). `--quiet` prints only the
summary. Texture fetches show the filters an instruction sets itself (`mag=`, `min=`, `mip=`,
`aniso=`); ones left to the fetch constant aren't printed (none of the match's shaders set any).
The decoder (`runtime/gpu/ucode.h`) follows Xenia's `ucode.h`/`shader_translator.cc` (BSD) and
will be shared with the shader translator.

## Shader translator
`runtime/gpu/shader_translator.cpp` translates Xenos microcode to GLSL 4.50 for Vulkan (resource
layout documented in `shader_translator.h`: guest memory as a storage buffer for vertex fetch,
float/bool/loop/fetch constants as uniform buffers, one texture binding per fetch slot, 16 vec4
interpolators). Check it over a shader dump:

    ./build/xenos-translate /tmp/glsl captures/shaders
    for f in /tmp/glsl/*; do glslc --target-env=vulkan1.3 -o "$f.spv" "$f" && spirv-val "$f.spv"; done

Unsupported features (loops, calls, LOD/gradient fetch ops)
make translation fail with a reason instead of producing wrong code.

Memory export (`eA`/`eM0-4`, used by the game's GPU skinning) writes guest memory from the shader
through set 0 binding 4, flushed at each `alloc` and at the end as Xenia describes (xenos.h). The
renderer runs such draws with rasterization discarded when they have no render target. Guest
physical memory is imported into Vulkan when the driver allows it (VK_EXT_external_memory_host);
RADV refuses the runtime's shared-memory mapping, so a 512 MB host-visible staging buffer at the
same addresses is used and the stream ranges (from the `mad eA, r#, const0100, c#` stream
constants) are copied in when the draw is recorded and out when they are synced. An export draw
does not wait for the GPU (issue #73: the match records about 130 of them per frame, and a submit
and wait for each one was most of the renderer's GPU waiting). Nor does the GPU wait between them
(#121): export draws that write separate ranges follow each other without a barrier, and one
barrier goes in before anything else is recorded, before an export draw that writes bytes a
pending one wrote, and before the command buffer ends (the host reads the ranges after that). A
barrier after each one cost 3.5 ms of GPU time a frame at resolution scale 2. A vertex shader in
export mode 7 (multipass) is not rasterized at all, as in Xenia, whatever render targets are
bound: the game's skinning draws are such point lists, and they used to be drawn into a 1280-wide
color target, with the EDRAM transfers that came with it. Its ranges are pending like resolve
writebacks: they reach guest memory before any fence after them does (see "GPU fences"). A later draw that fetches vertices from a pending range gets them copied GPU to
GPU from that buffer, and a draw whose textures or indices come from one syncs first and is
recorded again. `BB_EXPORT_WAIT=1` brings back the wait after every export draw, for comparisons.

Register field layouts come from `runtime/gpu/generated/registers.h`, generated by
`tools/gen_gpu_registers.py` from Xenia's `registers.h`, `xenos.h` and `register_table.inc` (it
checks that every layout covers 32 bits and matches Xenia's bit-offset comments).
Register names come from `runtime/gpu/generated/register_names.cpp`, generated by
`tools/gen_register_names.py` from Xenia's `register_table.inc` (BSD, `docs/licenses/xenia-LICENSE.txt`).

## Debugging aids
- `--sample-threads N`: N seconds after launch, every running guest thread is sampled 50 times
  (20 ms apart) and the most frequent guest call stacks are printed per thread, innermost first,
  with the host function it was in (e.g. `[pthread_cond_wait]` = parked in a wait). A thread
  stuck in a polling loop shows one stack at ~100% with no host function. Use it on hangs.
- `tools/fninfo.py default.pe ppc ADDR...` - string references, imports and callees of guest functions
- `tools/fps_report.py LOG` - a run log is already a frame trace, one `[gpu] N fps` line per second:
  this prints its mean and worst seconds, the share below 25 and below 20, and for every dip second
  below `--threshold` (25) what else was logged in it - first-use shader work, new pipelines, new
  resolve destinations, screenshots and captures (those two cost frames themselves, and are marked
  as overhead). `--from S --to S` narrows the seconds, `--compare OTHER.log` lines two runs up and
  ends with the ratio of frames rendered, which is the frame-rate number a headless A/B run is after.
  For logs that carry the speed (#146), it also prints the mean speed and the game time lost, and
  lists the seconds below `--slower-than` (95%) that aren't fps dips: long single frames.
  Fixtures it is checked against: `tests/data/fps_report_sample*.log`, `tests/data/fps_report_speed.log`.
- Sampling a busy thread: `eu-stack -1 -p <tid>` (recompiled frames are named sub_XXXXXXXX)
- Crash report: host backtrace, all the guest registers, recent profiler scopes and import calls of
  the crashing thread. Indirect calls to addresses without a recompiled function are reported with
  the target and caller.
- `--timeout N`: after N seconds print every thread's recent scopes and calls, then exit with 124.
  `kill <pid>` (SIGTERM) and Ctrl+C (SIGINT) end a run the same way, headless or windowed, with
  `==== SIGTERM received ====` (or `SIGINT`) and exit code 143 (130). A second signal while the
  report prints ends the process at once.

## Known shortcuts
- Kernel objects (KTHREAD, PCR, TLS slots, events) are never freed; only a thread's stack is
- NtSuspendThread only works before a thread starts; decommits are ignored (memory stays backed)
- Timer APC routines are not delivered
- Networking carries no data (socket objects exist, nothing is ever connected); XMA plays as silence
  in a build without the decoder (`scripts/build_xma_decoder.sh`); the XMA "interrupt when done" and
  "stop when done" bits are ignored
- File reads and writes complete before the call returns, also on handles opened for overlapped I/O
  (the console returns `STATUS_PENDING` there and completes later); the result is in the I/O status
  block and the event either way
- WAIT_REG_MEM spins with yield
- Renderer: resolve destination images are never freed (one per guest address; ~375 during the
  first load); resolves to formats other
  than 8_8_8_8, 2_10_10_10 and D24S8 depth are not written back to guest memory (depth stencil is
  written as 0), nor are destinations resolved every frame; a resolve's own format conversion is
  skipped on the GPU side (textures sample the resolve image in the render target's format); 1D/3D
  textures and some formats are placeholders; signed texture formats are read unsigned; textures
  are re-uploaded only when their bytes changed, which is known from the write watch below or,
  for textures whose bytes change all the time, from hashing them once per frame; depth is 32-bit float instead of 24-bit
