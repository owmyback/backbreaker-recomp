# Modding Backbreaker

Plays, the AI's audibles, the minigames and the menus are text files you can edit in any text
editor, and a mod is shared as the changes you made, never as the game's files (#169). This guide
covers those data mods, and, for programmers, changing the game's code through the runtime.

The rule for both: nothing taken from the game is ever committed to this repository or uploaded
anywhere. Not the files `tools/pkd.py` extracts, not edited copies of them, not code from `ppc/`.
Everyone makes their own from their own disc.

## The mods folder

A file in the mods folder replaces the game's file at the same path, from the next launch, with no
rebuild. Paths are matched without regard to case, as on the disc.

| How the game is set up | The mods folder |
|------------------------|-----------------|
| RecompLauncher, the installer, the AppImage | `~/.local/share/backbreaker-recomp/mods` (`$XDG_DATA_HOME`). RecompLauncher's **Open Mods Folder** opens it. |
| A checkout (`scripts/setup.sh`) | `game/mods` |
| `--user-dir <dir>` | `<dir>/mods` |

Most of the game is in one archive on the disc, `packedfilesx360/PackedOutputX360` (an index,
`.pkh`, and the data, `.pkd`: 2,035 entries). A file in the mods folder named like one of its
entries replaces that entry:

    mods/media/PlayDatabase/Offense/Dagger.pd      replaces the archive's media\PlayDatabase\Offense\Dagger.pd
    mods/media/Movies/Xbox360/505.wmv              replaces the disc's own file (the 505 logo movie)

- **Restart the game after a change.** The game reads its files when it first needs them, and
  some of them only at boot (see "Checking that a mod loaded").
- **To remove a mod,** delete its files from the mods folder.
- **New files** work too, once a file the game reads refers to them. A new play is a new `.pd`
  file listed in a playbook: the game looks for it in the archive, doesn't find it there, and
  opens it from the disc's path, where the mods folder supplies it. (Tried on 2026-11-18: the
  game opened the new play at boot. Whether it then shows up in the play menu hasn't been checked.)
- **Folder listings** stay the disc's, so a file can't be added where the game finds files by
  listing a folder.

## Getting the files: tools/pkd.py

`tools/pkd.py` (Python 3, nothing else; SteamOS has it) lists the archive and copies entries out.
From a checkout run it as `tools/pkd.py`; otherwise download that one file from this repository and
run it with `python3 pkd.py`.

    pkd.py list 'media/PlayDatabase/*'                  names and sizes (-l: offsets too)
    pkd.py extract --mods 'media/PlayDatabase/Offense/Dagger.pd'
                                                        into the mods folder, ready to edit
    pkd.py extract ~/bb-files 'media/ai/*'              anywhere else, to read

Patterns use `*`, `?` and `[...]`, are matched without regard to case, and take `/` or `\`
between folders; quote them so the shell leaves them alone. `extract` keeps a file that's already
there and differs, so it never overwrites your edits (`--force` does).

**Where it looks for the archive:** a checkout's `game/disc/packedfilesx360/`, then the
installer's default, `~/.local/share/backbreaker-recomp/install/game/disc/packedfilesx360/`. A game
set up by RecompLauncher has it in the game's folder in the launcher's library, under
`build/game/disc/packedfilesx360/`: pass `--pack` with the `.pkh` file's path, before the command
(`pkd.py --pack <path to PackedOutputX360.pkh> list`). **The mods folder** it uses is the
checkout's `game/mods` when the archive is the checkout's, otherwise
`~/.local/share/backbreaker-recomp/mods`; `--mods-dir` names another.

It won't write the game's files anywhere in a checkout that git doesn't ignore (`game/` is
ignored, so `game/mods` is fine).

## What's in the archive

Text, ready to edit:

| Entries | What they are |
|---------|---------------|
| `media\PlayDatabase\*_Plays.xml` | The playbooks (offense, defense, special teams, the minigames): each lists its plays' `.pd` files and their pictures. |
| `media\PlayDatabase\**\*.pd` | 401 plays, XML. Each `<Player>` has a role, a `<Position>` in the formation, and a `<Route>` of waypoints (position, speed, goal), with blocking and timing settings. The first `<Position>` number is across the field: changing it in the kickoff moved the kicking team sideways (tried 2026-11-18). |
| `media\ai\AudiblesReactions.xml` | Audibles: for each kind of play one side has called and the kind the other side shows, the kinds of play it can switch to. |
| `media\macrobehaviours\*_360.xml` | The players' behaviours, assembled from smaller ones. (`*_PS3.xml` are the PlayStation 3 versions.) |
| `media\minigame\*.xml` | Practice, Sandbox, Tackle Alley and Tackle Alley Versus. |
| `media\ui\**\*.xui` | The menus, as XUI scenes (XML): controls, their sizes and positions, and animations. A position an animation drives is set by its keyframes, not by the control's `<Position>`. |
| `media\Stadiums\`, `media\PostProcess\` | Stadium lists, lighting, and each stadium's and weather's post-processing. |
| `media\options\`, `media\unlockables\`, `media\credits\`, and others | Option lists, unlockables, the credits. |

Binary, not worked out yet (each its own task): `media\content\clubdata.dat` (6 MB; teams and
players, presumably), `media\ai.dat`, the AI's play selection tables (`media\ai\*.xls.cxml`),
NaturalMotion's euphoria and morpheme data, the models and animations (`.bof`), the jumbotron
animations (`.jad`), the `.cut` files, `translations.dat` (the game's text), and the textures
(`.dds`, `.tga`; replacing one hasn't been tried).

## Checking that a mod loaded

The log says what each file in the mods folder replaced when the game starts, and when the game
first reads it. In a terminal the log is on screen; the installed game also keeps it in
`~/.local/share/backbreaker-recomp/logs/backbreaker.log`, with the two runs before it as
`backbreaker.1.log` and `backbreaker.2.log`.

    [rt] [mods] files in .../mods replace the disc's at the same path, and the archive's entries
    [rt] [mods] .../mods/media/PlayDatabase/Special/KickOff.pd replaces media\PlayDatabase\Special\KickOff.pd in PackedOutputX360
    [rt] [mods] the game read media\PlayDatabase\Special\KickOff.pd from the mods folder
    [rt] [mods] .../mods/media/PlayDatabase/Offense/My Play.pd adds \Device\Cdrom0\media\PlayDatabase\Offense\My Play.pd

A "replaces" line with no "the game read" line means the game didn't need that file in that run.
When the game reads its files (measured on 2026-11-18, the Exhibition route):
- **At boot, before the title screen:** every play and playbook (the 391 plays the playbooks list;
  the other 10 are never read), the stadium list, the AI's audibles, the behaviours.
- **A menu,** when it's first shown: the title screen's scene just before the title appears.
- **Later:** `media\minigame\Practice.xml` wasn't read by the time the menus were reached;
  presumably it is when Practice starts.

## Sharing a mod: changes, not files

Make the mod from your mods folder:

    pkd.py diff -o my-mod.patch

That's a unified diff of every text entry your mods folder changes, each against the original in
your archive, with that original's SHA-256 so it's applied to the same version. Like any diff it
holds a few unchanged lines around each change, but never the rest of a file. Binary entries are
left out, and so are whole-file replacements of the disc's own files.

Whoever gets it applies it from their own copy of the game:

    pkd.py apply my-mod.patch

That writes the changed files into their mods folder. Nothing is written if their copy of an entry
isn't the one the mod was made from, or if any part of the mod doesn't fit. A file already in the
mods folder (from another mod) gets the changes on top, if the lines they change are still there.
It also takes patches from `diff -ru` (GNU diff), and ones that lost their carriage returns on the
way. To undo it, delete the files it wrote.

## A whole new archive

    pkd.py repack ~/my-files ~/new-archive/PackedOutputX360

writes a complete archive (`.pkh` and `.pkd`), with each file in `~/my-files` in place of the entry
it's named like, laid out as the shipped one: repacked with nothing replaced, the shipped archive
comes back byte for byte. That's for where there's no mods folder (another emulator, say). The
mods folder does the same for this game without it, and an archive is the game's content, so it's
never shared.

## How the runtime does it

The game's own archive reader is left alone. It reads the whole index once at boot into a table of
offsets, sizes and names (NaturalMotion's `NmgPackedFileArchive`, `sub_82650A80`), opens the data
file, and reads each entry by its offset. When the mods folder has files named like entries, the
runtime's file layer (`runtime/kernel/pack_overlay.cpp`, used by `runtime/kernel/files.cpp`) shows
the game a rewritten index, the same size, in which those entries point past the end of the
original data, and a data file that is the original followed by the replacement files, each at a
2048-byte boundary as the shipped entries are. Reads past the original data come straight from the
files in the mods folder; nothing is copied. The game's file size queries see the larger sizes.
`tests/pack_overlay_test.cpp` checks it on a made-up archive, and `tests/test_pkd.py` the tool.

## Code mods: replacing a game function

What the data can't express needs a change to the game's code, made in the runtime. That means a
checkout and a build (README, "One-step setup"; `docs/runtime.md`, "Build and run"), and some
reading of PowerPC.

Every function of the game is C++ in `ppc/`, made by XenonRecomp from your own executable (it's
the game's code, so it's never committed or shared). Each is named after its address,
`sub_8215B710`, and defined *weak*, under a second name, `__imp__sub_8215B710`. A function the
runtime defines with the same name replaces it everywhere the game calls it, and can still call the
original:

    extern "C" PPC_FUNC(__imp__sub_8215B710); // the original: the play-flow state machine's update

    PPC_FUNC(sub_8215B710)
    {
        // ctx holds the guest registers (ctx.r3.u32 is the first argument, here `this`);
        // base is guest memory, big-endian: guest address a is base + a
        if (/* the case to change */)
            return;                // skip the original this time
        __imp__sub_8215B710(ctx, base);
    }

`runtime/tackle_alley.cpp` is a complete one (#139): it holds the play flow back for a tick while a
fade to black finishes, which fixed Tackle Alley at 60 fps; its header comment says how the
function and the addresses were found. `runtime/intro_movies.cpp`, `frame_rate.cpp` and
`play_lock.cpp` are others. A new `.cpp` in `runtime/` is built in by the next `ninja -C build`.

Finding the function to change:
- **`tools/fninfo.py game/extracted/default.pe ppc ADDR...`** lists a function's strings, the
  functions it calls and its imports. Strings are the way in: the game's file names, its debug
  messages (`NmgPackedFileArchive::NmgPackedFileArchive`) and XML element names. Search
  `game/extracted/default.pe` for one, then for the functions that load its address
  (`lis`/`addi` pairs in `ppc/` give it in two halves).
- **`--sample-threads N`** prints the guest call stacks each thread is in, N seconds after
  launch: what runs during a moment of play.
- **A crash report** has the call stack (the game's functions show as `sub_XXXXXXXX`), the guest
  registers and the crashing thread's recent kernel calls.
- **For anything drawn:** GPU frame captures (`docs/runtime.md`, "GPU frame capture"), and
  `BB_DEBUG_WATCH=<address>` to log which frames change a block of guest memory and which GPU copies
  landed there (`docs/runtime.md`, "Renderer debug aids").

A code mod is shared as a change to the runtime (a branch, a pull request or a patch of
`runtime/`), which holds addresses and offsets but none of the game's code.
