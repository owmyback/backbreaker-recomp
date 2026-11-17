# Project roadmap and phase tracker

Goal: run Backbreaker (Xbox 360, 2010, NaturalMotion) natively on Linux by statically
recompiling its PowerPC executable to C++ (XenonRecomp) and providing a host runtime that
stands in for the Xbox 360 kernel, GPU, audio and input.

**Keep this file current:** when a phase changes status, update its row, its section and the
"Current state" paragraph, and reference the commits.

Work that can be picked up is tracked as GitHub issues and linked from each phase below; see
[`CONTRIBUTING.md`](../CONTRIBUTING.md) for how to take one.

Last updated: 2026-11-17 (phase 9: the Deck is CPU-bound in matches, and a geometry cache cuts the command processor's work by
a quarter on the PC, #146, and thread timing reports the long frames that remain; phase 10: the AppImage runs on the Steam Deck, play-tested in Gaming Mode; the
achievements list opens there from R4 mapped to F1, #145; slow moments on the Deck, #146, now measurable with
a game-fps readout and a log file, #148; the installer, #138, started: RecompLauncher will be its front end,
and the runtime builds as a library without game files, the "runtime kit" a player's machine links
the game against; the extraction tools and the image check in C++; the installer builds the game
from the player's image with its own clang and sysroot, in each distribution's container; the
manifest that lets RecompLauncher set the game up and start it). Before that,
2026-11-16 (phase 10: portable build for Steam Deck and other distributions planned, an AppImage
with a mods folder, #134). Before that, 2026-11-16 (phase 7 done: every movie stopped after a second or two because of the
Win32 error codes, #132; `--fixed-seed` for test runs, #71). Before that, 2026-11-16 (phase 9 in progress: the frame-rate dips are fixed (#73), and heavy scenes
hold 60 fps at resolution scale 2 (#121); the Training Camp and second-match crashes are fixed (#129,
#127); phase 4: the crowd (#55), anisotropic filtering by default (#126) and matches rendered at a
native 1280x720 (#128); phase 11 done). Before that, 2026-11-15 (phase 12, online multiplayer, added
as the last phase, with its research in `docs/multiplayer.md`). Before that, 2026-11-14 (#3 closed; phase 4: window polish #9, validation clean #11 and the
screenshot key #10 done; setup script, phase 10 started; an Exhibition match loads and plays the kickoff;
EDRAM transfers between formats #36: the match's tile blocks are gone; clear rectangles bounded by
their vertices #37; resolves written back to guest memory #45: the grass renders; mip levels #46)

## Current state

The whole executable recompiles (0 warnings) and builds into one native binary. The runtime
boots the game through its full initialization and renders it with Vulkan: the middleware logo
screen, the attract movie and the title screen. With scripted button presses (`--press "a@20,a@26"`)
the game leaves the title screen, picks the (emulated) hard drive, creates or loads its saves
("Profile Options", "Custom Teams" in `game/saves/`) and reaches the **main menu** (Training
Camp, Exhibition, Leagues, Tackle Alley, Xbox LIVE). Controllers work (phase 5). **Exhibition**
goes through side select, team select and stadium options, loads the match ("Loading..." with
both teams' star players) and **plays**: the kickoff play call, the kicker's aim, the kickoff
itself, and the next play call (Pass / Run / Special / Ask Coach) with the clock running
(1st 4:55, 1st & 15). Memory stays flat during the match. The 3D view renders across the whole
screen (stadium, crowd, players, replays, the video board), with the artifacts in the blocker
below. Kicking works with the right stick pulled back and then pushed forward (a plain release
is not seen at the current frame rate, see phase 9). The game's mixed audio goes to the host's
playback device (#90), and XMA is decoded (#93) when the decoder is built
(`scripts/build_xma_decoder.sh`), so the game's music and sounds play.

**Current state of the picture (2026-11-16):** matches run at 60 fps, the default for windowed
play (#89), and hold it at resolution scale 2 on an RX 9070 XT (2560x1440), rain at night included
(#121: 59.9 fps over a play-tested match, lowest second 56). The match renders at a native 1280x720
instead of the game's 1152x640 stretched to 1280x720 (#128), so at scale 2 the picture is exactly
2560x1440. Textures get 16x anisotropic filtering (#126), so the field stays sharp toward the
horizon. The crowd is whole (#55), replays no longer show a grid (#44), and the player highlight
and the decal passes (kick meter, zones, routes) render (#60, #54). Training Camp drills and a
second match in one session no longer crash (#129, #127).

**Other machines (2026-11-17):** the portable AppImage (#134) runs on the Steam Deck in Gaming Mode.
There it is slow at moments (#146), and the achievements list needs R4 mapped to F1 (#145).

Fixed on the way here: GPU skinning through memory export; predicated tiling in the
command processor (without it a GPU->CPU callback re-ran D3D's worker job 3000+ times and the
side-select load hung); offline networking (XNet/WSA startup, DNS, and sockets as kernel objects so
the game's three socket uses get real handles instead of a stub returning 0); a byte-offset jump table
whose bounds check is on a copy of the index register; vtable thunks packed after a `bctr`; a
failed virtual reservation now returns `STATUS_NO_MEMORY` (the heap halves the request and
retries) instead of aborting; `KeSetAffinityThread` (CPU number in the PCR); and
`InterlockedPopEntrySList`/`InterlockedFlushSList`, which were unimplemented: PhysX keeps its
per-thread contexts in such a list, so every simulation step allocated a new one (128 KB + 64 KB
caches) until the heap ran out a minute into the match; `KeTlsFree`, which left the freed slot's
value in every thread, so a reused slot handed back freed data (#127); and a race in the game
itself between its update and render threads over a play's player slots, which the console's
timing hid (#129).

## Phase overview

| # | Phase | Status | Commits |
|---|-------|--------|---------|
| 0 | Feasibility and extraction | Done | `43b7032` |
| 1 | CPU recompilation (PowerPC -> C++) | Done | `5dad6d2`, `8efcafd`, `8400499`, `9667d0e` |
| 2 | Host runtime boot (loader, kernel, threads, files) | Done | `0507715`, `b3796cc`, `9d09a50` |
| 3 | Headless subsystems: game reaches the front end | Done | `8400499`, `9667d0e` |
| 4 | Rendering (Vulkan): see the game | **In progress** (steps 0-19 done: first picture, resolution scaling, tiled textures, EDRAM aliasing, render to texture, the match's 3D view, EDRAM transfers between formats, clear rectangles bounded by their vertices, resolves written back to guest memory, mip levels, player highlight, decal passes, the crowd, anisotropic filtering, native 1280x720 matches) | `258ec24`, `7bf4023`, `9ac4cb9`, `733e432`, `0ed897c` |
| 5 | Controller input (SDL, DualSense) | Done (DualSense tested; keyboard fallback not yet) | `607e8ce` |
| 6 | Audio output (real XMA decoding, mixing) | **Done** (output to SDL3, #90; XMA decoding, #93; confirmed by ear 2026-11-15) | #92, #95 |
| 7 | Movies (WMV / XMV playback) | **Done** (every movie stopped after a second or two: Win32 error codes, #132; boot logos skippable with `skip_intro`) | #132 |
| 8 | Saves and profile (XAM content) | **Saves done**; profile settings answered with the console defaults (#98) | `69ba008` |
| 9 | Gameplay correctness and performance | **In progress** (60 fps, #89; frame-rate dips fixed, #73; heavy scenes hold 60 at scale 2, #121; crashes fixed, #127, #129; Steam Deck CPU-bound in matches, geometry cache, #146) | `baa2b5f`, `2a16722`, `e859d75`, `97b8bdc` |
| 10 | Packaging and launcher | **Started** (one-step setup script done, #4; portable build for Steam Deck and other distributions merged and play-tested on the Deck, #134: #136 and #137; docker path untested; installer #138: RecompLauncher as the front end; the runtime kit, the tools in C++ and the headless install done) | `100ec86` |
| 11 | Achievements (unlocks, pop-ups, achievements screen) | Done (database, unlocks, store: #97; overlay list and pop-up: #101; pop-ups take turns, with a sound: #105) | `1a59660`, `83fce76`, `9b2c8bf` |
| 12 | Online multiplayer (session service, game browser) - last | Planned (research written, `docs/multiplayer.md`) | - |

Naming note: in early discussion, phase 3 was called "Phase 1 (headless GPU)" and phase 4
"Phase 2 (Vulkan renderer)". The numbering above replaces that.

## Goals beyond running the game

| Goal | Where | Notes |
|------|-------|-------|
| Higher internal resolution (1440p, 4K) | Phase 4 | Done for 16:9: `resolution_scale` 1-4 in `game/settings.ini` (#115) or `--resolution-scale` renders at that multiple while the game is still told 1280x720, and matches render at a native 1280x720 instead of a stretched 1152x640 (#128), so scale 2 is exactly 2560x1440. The game sizes its post-processing buffers from that view (640x360, 320x180). 2D UI scales as-is. Ultrawide (beyond 16:9) would need camera/layout patches. |
| Higher frame rate (60+ fps) | Phase 9 | 60 fps is the default for windowed play (#89; `docs/runtime.md`, "Frame rate"). The cap was D3D's present interval (2 vblanks a frame) and a fixed 1/30 s gameplay tick; both are set for 60, and PhysX steps 1/60 s either way. Play speed was wrong before because the guest clock ran at the host TSC rate, which overflowed the game's 32-bit timer frequency. Above 60 would need a faster guest vblank and a matching tick. Play-testing so far: a match at the right speed; anything that runs too fast or slow only at 60 is a follow-up. Heavy scenes (rain at night, 2,000+ draws a frame) hold 60 at resolution scale 2 (#121). |

---

## Phase 0 - Feasibility and extraction (done)

Goal: decide whether a recompilation is realistic before investing in it.

- `tools/xdvdfs.py` reads the Xbox disc file system (XGD2) straight from the ISO;
  `--all` extracts the full disc to `game/disc/` (1.07 GB of game data).
- `tools/xex_decrypt.py` decrypts `default.xex` (normal encryption, basic compression) into
  the in-memory PE image `game/extracted/default.pe` (0xE20000 bytes, base 0x82000000).
- Findings (`docs/feasibility.md`): ~2.2M instructions, 19,184 `.pdata` functions, imports only
  from `xam.xex` and `xboxkrnl.exe`; middleware is NaturalMotion's own engine (Euphoria,
  Morpheme), FMOD with XMA audio, WMV movies; no Havok/Bink/Scaleform.
- Verdict: CPU translation looks clean; the runtime (GPU, audio, kernel) is the bulk of the work.

## Phase 1 - CPU recompilation (done)

Goal: every guest function translates to C++ that compiles.

- XenonRecomp (pinned at `ddd128b`) with our patch `patches/xenonrecomp-backbreaker-instructions.patch`:
  - 62 PowerPC/VMX instructions it did not implement (update-form loads/stores, carry ops, CR
    logic, CTR branches, VMX integer saturation/packs/shifts, `vsro`, `vsel128`, `vctuxs` ...)
  - fixes: `bdnzf` assumed the `eq` bit; `vcmpgtuh.` record form; CTR branches into other
    functions; integer division guarded (x86 traps on /0 and INT_MIN/-1, PowerPC does not)
  - analyzer fixes (#52), for leaf functions (no `.pdata` entry) and the scan between
    functions: `bdzlr`/`bdnzlr` were taken for an unconditional `blr`, which cut the game's
    `memset` (`0x828E0440`) before its last two byte stores, so a fill whose length ends in 2
    or 3 bytes after the word loop left the last 1 or 2 bytes unwritten (1,150 call sites). A
    conditional branch to the next instruction cut `0x826EB500` (a tracker-music tremolo step)
    after its first case. The two words in front of every function with an exception handler
    (handler address and scope table) were decoded as code; that gave 66 bogus copies of those
    functions, cut into pieces whose jumps into each other were 57 of the 60 `// ERROR`s.
    Nothing ever called the copies.
  - a function whose last instruction can fall through (a conditional branch or return)
    now continues into the next function like the CPU does, instead of returning; one that
    cannot be resolved is logged as `ERROR: ... falls through`. Unresolved branches are logged
    too (`ERROR: branch at ...`), and `scripts/setup.sh` warns when `ppc/` has any `// ERROR`.
    Count: 0 (was 60).
- `tools/find_switch_tables.py`: XenonAnalyse found 0 jump tables (Backbreaker's compiler orders
  instructions differently and pads with nops); ours finds 439, including inline tables without
  a bounds check and bounds checks on the register the index was copied from (`mr r28,r3` then
  `cmplwi r3`; the one at `0x8230C9E4` runs while a match loads).
- `tools/function_bounds.py`: explicit bounds for leaf functions with switches (no `.pdata`
  entry) and functions reachable only through pointers (vtables, exception funclets, and vtable
  thunks packed back to back after a `bctr`, like `0x8243A4D8`, called when a play starts):
  56 entries in `config/backbreaker.toml`. Pointers into the body of a `.pdata` function are
  labels, not functions (the `__except` targets in exception scope tables, #52). Running it on
  the committed config with a mapping of a build of that same config writes those entries back
  unchanged (#29), so
  `scripts/setup.sh --regenerate` leaves `config/` clean; an entry that stops being re-derivable
  is reported rather than silently dropped.
- Register save/restore helpers located by byte pattern (`config/backbreaker.toml`).
- Result: 30,000+ functions -> 116 C++ files, 0 recompiler warnings, all compile with clang.
- Tests: `tests/instr_semantics.cpp` checks the byte-order-sensitive vector ops and carry
  arithmetic against hand-computed results (a deliberately broken variant is caught). Builds
  without game files against `tests/ppc_config_stub/` and runs in GitHub Actions on every
  push (`.github/workflows/ci.yml`), which also builds the patched XenonRecomp and lints
  `scripts/setup.sh` with shellcheck (#26).

Verify: see README "Recompile" and "Tests".

Open issues: #2 (tests for the remaining new instructions).

## Phase 2 - Host runtime boot (done)

Goal: load the image and run the entry point with real kernel services.

- `runtime/main.cpp`: 4 GB guest address space, image load, indirect-call table, crash handler.
- `runtime/kernel/`: all 228 imports have a logging stub (generated by `tools/gen_imports.py`);
  implemented: memory, XEX header queries, TLS, critical sections, spinlocks, events,
  semaphores, timers, waits (`objects.cpp`), real guest threads on host pthreads
  (`thread.cpp`), file system with case-insensitive device mapping (`files.cpp`), time,
  strings, the game's printf-style logging, and `KeTimeStampBundle` kept current by a host
  thread that rewrites InterruptTime, SystemTime and TickCount every millisecond (`#3`).
- Result: CRT init completes, the game starts its file-system and pool threads and opens its
  data archives.

Closed issues: #3 (keep `KeTimeStampBundle` updated, `00875cd`).

## Phase 3 - Headless subsystems (done)

Goal: everything the game needs to reach its main loop, without real output yet.

- GPU front end (`runtime/gpu/`): Vd* video imports; PM4 command processor executing the ring
  buffer for its side effects (register writes, fences, waits, interrupts, swaps); register
  file at 0x7FC80000; 60 Hz vblank interrupt. MMIO stores reach it via `runtime/ppc_hooks.h`,
  force-included into the generated code.
- Physical memory aliased at 0xA0/0xC0/0xE0 like the console (memfd).
- Audio (`runtime/apu/`): XAudio render-driver clock; **XMA decoder emulation** (context array,
  kick/lock/clear registers, decoder thread) producing silence of the correct length.
- XAM (`runtime/kernel/xam.cpp`): notifications, a local profile "Player", profile settings,
  console settings, XOVERLAPPED completion.
- Input (`runtime/hid/`): one connected pad in neutral state.
- Memory: first-fit heaps with coalescing; frees are honored.
- Debugging aids: profiler-scope names recovered from the game's compiled-out markers, per-thread
  crash/timeout reports, missing-function reporter, `GUEST_TRACE` wrappers, `tools/fninfo.py`.
- Milestones and what blocked them:
  - GPU init: needed real `ExGetXConfigSetting` values (video flags).
  - Voice engine (XHV2): needed notification listeners and two-step profile-settings reads.
  - First presented frames: needed `XamInputGetKeystrokeEx` to report "no keystroke" (the stub's
    success made the game read fake keystrokes forever), guarded integer division, and full
    `VdGetCurrentDisplayInformation` data.
  - Endless loading screen: FMOD waited on the XMA hardware decoder (contexts were handed out
    outside the hardware's context array, so kicks hit bogus registers).
  - Crash after ~47 s: allocators never reused memory.
- Result: loads in ~3 s, front end at a steady 30 fps (~65 draws per frame), memory flat over
  4 minutes (~135 MB virtual + 280 MB physical).

Verify: `ninja -C build && ./build/backbreaker --timeout 60` - the once-per-second `[gpu]` line
shows fps, draws and memory.

Open issues: #5 (virtual memory grows ~1 MB per 20 s on the title screen).

## Phase 4 - Rendering (in progress)

Goal: see the game, ideally at higher internal resolution than the console (see Goals).

**Step 0 - GPU frame capture (done, `258ec24`).** `--capture-frame N[,N...]` records a frame's draws,
shader loads, register changes, render targets and textures to `captures/frame_N/` (see
`docs/runtime.md`). Register names and field layouts come from Xenia's register table and
`xenos.h` (BSD). First findings:
- Attract-movie frames: ~51 draws, mostly single-point draws plus a few rectangle lists; bound
  textures are 8-bit planes at 1280x720 and 640x360 - the movie player's YUV planes and
  motion-compensation shaders running on the GPU.
- 3D logo frames between movie loops: ~65 draws, 4 large indexed triangle lists (9.5k-18.7k
  indices), 4x MSAA render targets at reduced size with resolves (EDRAM copies), tiled
  textures in 8_8_8_8, 4_4_4_4, 8 and DXT1 (including a cube map), 7 vertex + 5 pixel shaders.
- Front buffer is 1280x720 8_8_8_8 at physical 0x00840000.
- Movies are drawn by the GPU, not decoded by us: their pictures come with the renderer.

**Step 1 - Window + present (done, `6ad24c9`).** SDL3 window and Vulkan swapchain (FIFO) on the
process main thread; the game runs on its own host thread and never waits on the window. Each
guest swap publishes the front buffer; the presenter copies it into a staging buffer, uploads it
and blits it letterboxed into the swapchain image. Window title: game fps and display fps (30 and
240 on the dev machine). `--headless` keeps the old behavior. Findings:
- The front buffer is the destination of the frame's last draw, an EDRAM resolve
  (`RB_COPY_DEST_BASE` 0x00840000, pitch 1280, height 720, `RB_COPY_DEST_INFO`: format 8_8_8_8,
  red/blue swapped).
- `VdSwap`'s front-buffer texture fetch constant confirms: 8_8_8_8, **tiled**, pitch 1280,
  1280x720. The presenter copies raw for now, so it will need untiling once pixels exist.
- The picture is black: our command processor does not execute resolves or draws yet.

Open issues (separable from the renderer core): #12 (tested tiled/linear conversion). #11 (Vulkan
validation clean run) is settled: the layer runs with `BB_VK_VALIDATION=1`, and a run into an
Exhibition match reports no errors (docs/runtime.md, "Vulkan validation"; the two things it found -
render pass attachments the draw has no image for, and a shader feature enabled without asking the
GPU for it - are fixed). #10 (screenshot key) is done: F12 reads back the swapchain image the
presenter just drew and writes `screenshots/<date-time>.png` (what the window shows; `--screenshot N`
saves the renderer's front buffer instead). Window polish #9 is done (`bf0641c`): F11 or Alt+Enter
toggles borderless fullscreen, and the windowed size, position and fullscreen state are remembered
in `game/settings.ini` between runs (`--fullscreen` to start fullscreen). Only the windowed geometry
is remembered, so leaving fullscreen gives the same window back. The same file can keep the
resolution scale for windowed runs (`resolution_scale=2`, #115).

**Step 2 - Shader disassembler (done, `c5562a6`).** `runtime/gpu/ucode.h` decodes Xenos microcode
(control flow, vertex/texture fetch, paired vector+scalar ALU; layouts from Xenia's `ucode.h`),
`runtime/gpu/disasm.cpp` disassembles it and `build/xenos-disasm` summarizes coverage over a set
of shaders. `--dump-shaders` collects every shader a run loads. Coverage of the 58 unique shaders
(30 vertex, 28 pixel) loaded from boot through the title screen to the storage-device prompt:
- Control flow: straight-line `exec` blocks and `alloc` only - no loops, calls, jumps or
  predicated execs yet.
- Fetch: 72 vertex fetches (7 formats: 32_32/32_32_32/32_32_32_32/16_16/16_16_16_16 FLOAT,
  8_8_8_8 and 2_10_10_10 normalized), 75 texture fetches (2D and cube).
- Vector ALU (10 ops): mad, mul, max, add, cube, dp3, dp4, dp2add, min, cndeq.
- Scalar ALU (15 ops): rcp, maxs, mulsc, rsq, adds_prev, muls, muls_prev, subsc, sgts, setp_gt,
  log, exp, mins, addsc, setp_ge.
- Exports: oPos, interpolators o0-o4, color outputs oC0/oC1. 9 predicated ALU instructions,
  47 absolute-value sources. No pixel kills, relative addressing or memory export yet.
So the translator can start small; gameplay shaders will extend this (re-run the coverage once
the game gets further).

**Step 3 - Xenos state tracking (done, `a7b0bb8`).** `runtime/gpu/state.cpp` decodes the full draw
state from the register file at each draw (primitive/indices, EDRAM mode, four color targets with
format, EDRAM tile base, write mask and blending, depth/stencil front and back, rasterizer,
alpha test, viewport, window/screen scissor, shader register counts, export modes and constant
ranges) and builds a pipeline key from the fixed-function part. Register layouts are generated
from Xenia's `registers.h` (`tools/gen_gpu_registers.py`, 44 registers, 27 enums, each layout
checked against Xenia's bit-offset comments). The frame capture now prints the decoded state per
draw and the distinct pipeline states per frame. Findings from the 3D logo frame:
- 8 distinct pipeline states: 3D models (triangle lists, depth LessEqual + write, back-face
  culling), EDRAM clears (rectangle lists writing all four targets with depth Always and stencil
  Replace, also at 4x MSAA), resolves (rectangle lists in Copy mode), alpha-blended UI (triangle
  strips / a quad list with alpha test), and the movie decoder's point draws.
- EDRAM addressing checks out: a 256x256 8_8_8_8 target uses pitch 320 (80-pixel tiles) and its
  depth buffer starts at tile 64 = 256/16 rows x 4 tiles.
- Vertex shader constants live at 0-255 and pixel shader constants at 256-511.

**Step 4 - Shader translation (first version, `093c825`).** `runtime/gpu/shader_translator.cpp`
emits GLSL 4.50 for Vulkan, compiled to SPIR-V with glslc/shaderc. Design: vertices are fetched
in the vertex shader straight from guest memory (bound as one storage buffer, byte-swapped per
the fetch constant's endianness, formats unpacked in the shader) instead of Vulkan vertex input
state; cube-map fetches rebuild the direction from the `cube` op's (s, t, face) form;
multiplications follow Shader Model 3 (0 * anything = 0); unsupported features fail translation
with a reason. All 58 shaders of the current corpus translate, compile with glslc and pass
spirv-val. A hand-checked vertex shader confirmed the swizzle decoding: the game's compiler keeps
intermediate results in permuted component order and undoes it on export.
Not yet: loops/calls, LOD/gradient fetches,
texture fetch offsets, pixel shader param_gen, runtime compilation with shaderc + caching (comes
with draw execution).

**Step 5 - Vulkan renderer, first picture (`7bf4023`).** `runtime/gpu/renderer.cpp` runs the
game's draws on the host GPU. The attract movie renders correctly (checked with
`--screenshot`). Design:
- One Vulkan device shared by the window and the renderer (`runtime/video/vk_context.cpp`); the
  renderer submits each frame at the swap and publishes the front buffer image to the presenter.
- Guest data is snapshotted at draw time into a per-frame upload buffer: constants, indices
  (byte-swapped; quad lists expanded), and only the vertex range a draw uses (from its index
  range and the strides the translator reports). The vertex fetch constants in the uniform
  buffer are rewritten to point into it. When it fills (one loading phase issues ~10,600 draws
  without a swap), the work so far is submitted and recording continues.
- EDRAM render targets: one image per (base tile, pitch, MSAA, format), sized to what fits in
  EDRAM from the base tile; depth is D32_SFLOAT_S8 (AMD has no D24S8).
- Rectangle lists use a geometry shader; the guest viewport (`PA_CL_VTE_CNTL` formats, scale,
  offset, Direct3D pixel centers) is mapped to Vulkan in the vertex shader; alpha test in the
  pixel shader; blend/depth/stencil/cull from the draw state.
- Resolves copy (or MSAA-resolve) into images keyed by destination address, then clear if
  asked. Linear textures (8, 8_8, 8_8_8_8, DXT1/3/5) upload once per frame they are used.
- `--screenshot N` writes PNGs (headless too); `--no-render` turns the renderer off.

**Step 6 - Internal resolution scaling (`9ac4cb9`).** `--resolution-scale N` (1-4) multiplies
render targets, the host viewport, scissors, resolve rectangles and clears by N while the game
still renders "1280x720"; the guest viewport math stays in guest pixels. Checked: the attract
movie at 3x renders a full 3840x2160 front buffer at 30 fps.

**Tiled 2D addressing (`runtime/gpu/tiling.h`, issue #12, `f9566a7`).** The byte offset of a block in a tiled
surface, following Xenia's `texture_address::Tiled2D` (BSD, docs/licenses/xenia-LICENSE.txt): the
32x32 macro tile origin plus the micro tile position carried by the bank/pipe bits, for block sizes
1, 2, 4, 8 and 16 bytes. `tile2D`/`untile2D` move a whole surface between the tiled storage and
row-major order, and `tiledUpperBound2D` gives the bytes a tiled surface really occupies - the
address span (0xA00 and 0xC00 past a tile origin for 1 and 2 bytes per block), which is what buffer
sizing needs. Checked by `tests/tiling_test.cpp` and `tests/tiling_test_odr.cpp` (run in CI): spot
values hand-verified against Xenia, round-trip identity for aligned and non-aligned sizes, the
offsets of a pitch-aligned surface forming a permutation, and the header building in two
translation units (everything in it is inline/constexpr). Not used by the renderer or the presenter
yet.

**Step 7 - Tiled textures (`1f70840`).** Textures are untiled on upload with `runtime/gpu/tiling.h`
(after byte-swapping guest memory); 4_4_4_4 and 5_6_5 are expanded to RGBA8. The middleware
logo screen and the title screen ("BACKBREAKER", "(A) START") now render with their real images.
Visible gap: the game clears the screen with a rectangle drawn at pitch 640 with 4x MSAA, which
on the console covers the same EDRAM as the 1280x720 target; without EDRAM aliasing (step 2
below) that clear misses, so stale pixels show around the logos.

**Step 8 - EDRAM aliasing (`882ca1b`).** Each of the 2048 EDRAM tiles remembers the render target
image that wrote it last (draw scissor rectangles, resolve clears). Before a target is drawn into
or resolved from, tiles last written through another image are copied in by a transfer shader
that maps each sample to its EDRAM address in both layouts (pitch, MSAA 1x/2x/4x, 32/64-bit
samples, resolution scale); depth transfers keep stencil (VK_EXT_shader_stencil_export). With
the game's 640-pitch 4x MSAA screen clear now reaching the 1280x720 target, the logo screen is
clean black and the title screen shows its football-field background. Transfers between
different formats (including color <-> depth) came later, in step 11.

**Step 9 - Cube maps and render to texture on the GPU (`a450796`).** Cube textures (6 faces,
linear or tiled, base level) upload like 2D ones. Render to texture stays on the GPU: a 2D
8_8_8_8 texture whose address is a resolve destination samples the resolved image directly
(with the red/blue swap the resolve asked for, and at the internal resolution), and a cube map
whose six face slots were all resolved (the game renders environment cube maps at 256x256) is
assembled from the six resolved images. Seen working: the title scene's 256x256 render target,
several 128x128 and 64x64 ones, and two rendered cube maps. Not yet: textures that read resolved
data in another format or tiling than the resolve wrote, and CPU reads of resolved memory.

**Step 10 - The match's 3D view: more formats, depth resolves, tiled resolves.** Textures in
1_5_5_5, 2_10_10_10, 24_8 (depth, converted to float as Xenia's XeUNorm24To32), 16_16,
16_16_16_16, the 16-bit float and EXPAND formats, 32_FLOAT and 32_32_FLOAT upload like the
others, and every format gets Xenia's host swizzle composed with the fetch constant's
(`GuestToHostSwizzle`: one-channel formats read RRRR, 8_8 RGGG, 5_6_5 RGBB). A texture at a
resolve destination samples the resolved image whenever the channel layout matches, not only for
8_8_8_8: the match's HDR scene (16_16_16_16_FLOAT), its 32_FLOAT targets, and 8_8_8_8 targets
read as 1_5_5_5. Depth resolves exist: the depth plane (sample 0 of a multisampled target) goes
through a buffer into an R32_SFLOAT image at the destination, which 24_8 / 32_FLOAT textures
sample - the shadow maps (1024, 512 and 256 square) and the scene depth. The match renders its
1152x640 2x MSAA scene in two 576x640 tiles (predicated tiling) and resolves the second with
RB_COPY_DEST_BASE moved inside the same texture; such a resolve now lands at its place in the
existing image (the tiled offset, Xenia's Tiled2D, inverted). The shader translator gained jumps
(`jmp`/`cjmp`: a state machine over the control flow instructions), the vector predicate-push
ops and setp_inv/pop/rstr, so every shader the match uses translates. Seen working: the menus
with all their pictures, Team Select's 3D player, Stadium Select, the kickoff, live plays and
replays.

**Step 11 - EDRAM transfers between formats (#36).** The game puts different formats on the same
tiles all the time. On the Exhibition route, 268 distinct (owner -> target) pairs used to be
skipped. Most were a 4x MSAA D24S8 target at pitch N and a 1x 8_8_8_8 target at pitch 2N on the
same base. From the trace, the engine appears to clear a 4x surface through a 1x alias of it.
The others involved
2_10_10_10_FLOAT, 16_16, 16_16_16_16_FLOAT, 32_FLOAT and 32_32_FLOAT. When the formats differ, the
transfer shader now goes through the bits EDRAM would hold, as Xenia's ownership transfers do
(`vulkan_render_target_cache.cc`, `GetTransferShader`):
- depth is `depth24 << 8 | stencil` (D24S8 unorm, D24FS8 20e4)
- 8_8_8_8 has red in the low byte, and 2_10_10_10 is unorm
- 2_10_10_10_FLOAT is 7e3 RGB plus a 2-bit alpha
- 16-bit formats are their 16-bit integers, and 32-bit floats are their bits
- a 64bpp sample is two words of the tile row, the one holding R and G first

Checked offline: all 484 variants of the shader compile with glslc. With C++ copies of the shader
functions, unorm8, unorm10, 7e3 (also through float16, its host format) and 20e4 map every value
back to itself, and so does snorm16 except -32768, which SNORM stores as -32767. Depth does too,
for all 2^24 values, with `round(d * 2^24) - (n > 2^23)`; Xenia's `round(d * 0xFFFFFF)` in
float32 loses the low bit of 1 value in 8. NaN payloads in the float formats were not checked. Transfers now move whole tiles, because ownership is per tile, and they shade only the
bounding box of the tiles they copy. A resolve clear of part of a tile brings the rest of that
tile in first.

Result: the white and striped tile blocks over the field and the stands are gone. The route
creates 40 transfer pipelines, 29 of them converting, and validation is unchanged (0 errors).
Cost: about 180 transfers a frame in the match, and live play at 21.7 fps against 23.3 before
(headless, last 60 s of the route). Most of those transfers come before the engine's clear
rectangles, whose scissor spans all of EDRAM; bounding them by their vertex data is #37.

**Step 12 - Clear rectangles bounded by their vertices (#37).** The engine clears with
rectangle-list draws that have clipping off, the viewport transform off (screen-pixel positions)
and the scissor left at (0,0)-(8192,8192). The renderer clamped that scissor to the target image,
which runs to the end of EDRAM, so each clear first transferred in nearly every tile and then took
them all over. As Xenia's `DrawExtentEstimator` does (`execute_unclipped_draw_vs_on_cpu`), such
a draw is now bounded by its vertices:
- `analyzePositionFetch` (`shader_translator.cpp`) recognizes a vertex shader whose `oPos` is a
  fetched vertex moved through registers (straight-line code, no predication, moves only). That
  matches 22 of the 75 vertex shaders dumped from the route, the clear shader among them.
- For unclipped, unscissored, auto-indexed draws of up to 64 vertices with such a shader, the
  renderer reads the positions from guest memory. It applies the same W, viewport, window-offset
  and pixel-center math as the translated shader. The bounding box, plus a pixel of margin, is
  then what the draw syncs and takes ownership of.

Measured on the Exhibition route with a counting build, per frame in the match: about 81,000 tiles
transferred before this step and about 27,000 after; samples shaded went from 104 M to 37 M.
Transfer count is slightly up (~175 -> ~205), because ownership moves in smaller pieces. Live play
is back to 23.2 fps (23.3 before #36, 21.7 with it; headless, last 60 s of the route). Screenshots
are unchanged and validation is unchanged.

**Step 13 - Resolves written back to guest memory (#45).** The match's field was black. The grass
shader multiplies by a 512x1024 DXT1 texture (`05DC5000`) whose blocks were all DXT1's
transparent-black encoding. That texture is not in the game's pack. At boot, the game's "Stadium
texture generator" thread renders it (the pack has a `bb_fieldlightmap` shader), resolves it to
`08B14000` and compresses the resolved pixels to DXT1 on the CPU. Our resolves stayed on the GPU,
so the CPU compressed zeros. Now:
- A compute shader also writes each resolve into guest memory: tiled (`tiling.h`'s
  `tiledOffset2D`, ported to GLSL) and packed as `RB_COPY_DEST_INFO` says (format, endian,
  red/blue swap, exponent bias). It's the inverse of the texture upload. Formats: 8_8_8_8,
  2_10_10_10 and D24S8 depth (stencil written as 0). The route also resolves to 1_5_5_5, 16_16,
  16_16_16_16_FLOAT, 32_FLOAT and 32_32_FLOAT, and those stay on the GPU (logged once each).
- It writes through the buffer memory export uses. RADV can't import guest memory, so that's the
  512 MB staging copy: the destination range is copied in when the resolve is recorded and out at
  the sync. A writeback overlapping one still pending syncs first.
- The command processor calls `render::syncGuestMemory()` before each point where the CPU can see
  GPU progress: scratch-register fences, `EVENT_WRITE_SHD`/`EXT`, `MEM_WRITE`, `REG_TO_MEM`,
  `COND_WRITE`, `INTERRUPT`, and the read-pointer update after it drains the ring. If writebacks
  are pending, it submits, waits for the GPU and copies them out.
- Writing back every resolve took live play from 26.6 to 22.2 fps: about 34 resolves and 16 syncs
  per match frame. Those are the scene, shadow maps, bloom and front buffer, resolved every frame
  and sampled by the GPU. A destination resolved in each of the last 60 frames now stays on the GPU
  while that lasts (logged once per address). With that, live play is 26.9 fps (baseline 26.6,
  same session): 25,600 resolves written back over the route (mostly while loading), 209,000
  kept on the GPU, 59 destinations GPU-only. A CPU read of a resolve made every frame would need
  lazy writeback (write-protect the destination, write it back on the first CPU access); none is
  known yet.

Result: the grass, yard lines, end-zone logo and player shadows render, as do the coin toss
background and replays. In the route's four screenshots, near-black pixels in the middle band
of the picture went from 49-100% (main) to 5-18% (what remains is the play-call menu).

**Step 14 - Mip levels (#46).** Textures uploaded their base level only, so distant ones (the
grass, the crowd) shimmered and looked pixelated. A frame capture of the match shows most mipped
textures ask for trilinear filtering, and some for 4:1 anisotropic. None of the 368 texture
fetches in the match's shaders override the fetch constant's filters (`xenos-disasm` now prints
such overrides).
- `runtime/gpu/mip_layout.h` (header-only) says where each level is in guest memory, following
  Xenia's `GetGuestTextureLayout` / `GetPackedMipOffset`. Level 0 is at the base address with
  the fetch constant's pitch. Levels 1 and up follow each other from the mip address, laid out
  from the power-of-two size. With packed mips, the levels from the first one whose shorter side
  is 16 texels or less share one 32x32-block tail, each at its own offset. A tail at level 0
  also moves the base. Before this, tiny textures with packed mips were read from the tail's
  corner instead of their real place.
- `tests/mip_layout_test.cpp` (in CI) checks seven layouts against hand-computed values,
  including the two examples Xenia's comments describe (an 80x260 tail whose level 6 ends at
  Y = 36, and a 2x2 base at (16, 0) with its mip at (8, 0)).
- The renderer reads and untiles each region once per face and copies every level out of it.
  The level count comes from the fetch constant (`mip_max_level`, clamped to the size).
- Samplers take the mip filter (point, linear, or base map = base level only), the fetch
  constant's level range and LOD bias, and anisotropy up to the GPU's limit
  (`samplerAnisotropy`, now recorded in the Vulkan context).
- Render-to-texture images stay single-level.
- Textures are no longer re-uploaded every frame they're used. The renderer hashes the guest
  bytes of every level and face (a 4-lane 64-bit multiply hash), and uploads only when that
  changed since the last upload. Any byte change still re-uploads, so the result is the same as
  before.

Measured on the Exhibition route (same session, sequential runs, last 60 s): live play was
25.8 fps on main. With mips alone it fell to 22.5, since every frame re-uploaded about a third
more data. With the unchanged-texture skip it's 30.1 fps, the game's own 30 fps cap: 865 uploads
over the route, about 749,000 kept. In matching screenshots, the distant grass goes from grainy
noise to smooth, with clean yard lines. Validation: 0 errors, the same two pre-existing warnings.

**Step 15 - Player highlight: depth resolves keep their stencil (#60).** The red silhouette on
the selected player (and on the QB's target receiver) never appeared. In a frame capture,
`player_highlight.fx360`'s first pass samples the scene's depth resolve (`0ED6B000`, also read as
24_8 for depth by other passes) as **8_8_8_8**: the raw D24S8 words, with the stencil value that
marks the selected player in the low byte (the shader's component after the fetch swizzle). It
edge-detects that stencil at five taps and colors the edges from a 256x1 palette indexed by it.
Depth resolves kept only the depth plane (R32_SFLOAT), so there were no edges. Now:
- The multisample-to-sample-zero pass resolves stencil too, and `resolveDepth` copies the stencil
  aspect into an R8_UINT companion image of the destination.
- An 8_8_8_8 fetch of a depth resolve gets an RGBA8 image that a compute pass packs from both
  planes (X = stencil, then depth low to high byte), rebuilt only after a new resolve into it.

Verified in the window by the maintainer (2026-11-14): the silhouette appears on the player you
switch to, on offense and defense, and jumps between receivers as the QB changes target.
Validation is clean. The CPU writeback of depth resolves (step 13) still writes stencil as 0.

**Step 16 - Decal passes stop flickering: redraws of depth-written geometry get a depth bias
(#54).** The kicking power meter, the defensive zones and the play-call routes flickered in and
out from frame to frame. They're drawn into an offscreen texture, and `zone composite` (ps
`12a075e629d5d072`) lays that texture onto the field by drawing **the field's own quad again**
(same vertex buffer, same indices) with LessEqual and no depth write. A frame capture with the
vertex data showed both passes compute exactly the same depths in theory (margin 0). In practice
our two compiled shaders round differently by about an ulp. The field quad reaches behind the
camera, so it's clipped at the near plane, where that ulp becomes about a tenth of a pixel, and the
field's depth slope at that grazing angle is about 3,800 units of 2^-24 per pixel. So the whole
decal ended up a few hundred units in front of or behind the field, depending on the frame.
Biasing that pass by 1024 units stopped the flicker; 64 and 256 didn't. (Declaring the vertex
shaders' temporaries `precise` didn't help.)

The renderer now notes each depth-writing draw's geometry for the frame (primitive, indices, and
the vertex fetch constants its vertex shader reads). A later draw of the same geometry that tests
LessEqual or Equal without writing depth gets a depth bias toward the camera: half a pixel's depth
slope plus 16 units, which matters only where the two depths tie. In a headless kick-aim run (151
frames) the meter's arc was steady; without the change the same stretch flickered every few frames.

**Step 17 - Resolve images the guest overwrites (#114, #119).** A resolve image used as a texture
is now dropped when the CPU writes its memory, seen through the page write watch (#117: the
Controls screen sometimes showed the font sheet, #114); watching only images that went stale keeps
60 fps (#120).

**Step 18 - The crowd (#55).** The fans in the stands are billboards sampling an impostor atlas the
game renders every frame: 64 skinned fans in 128x128 tiles, a row of 8 at a time, each row resolved
as 1_5_5_5 into the same 1024x1024 texture 256 KB further on. A resolve into an earlier one's image
was placed by the EDRAM source's pixel size instead of the destination's, so every row landed on
the legs of the row before: torsos beside their legs, fans sunk to the shoulders. Resolve images now
record their guest bytes per pixel (#125, `733e432`).

**Step 19 - Sharpness: anisotropic filtering and native 1280x720 matches (#126, #128).** The game
asks for anisotropic filtering on few textures, so the field blurred toward the horizon:
`anisotropy=16` is now the default in `game/settings.ini` (`c034aa7`). And the game drew the match at
1152x640 and stretched it to 1280x720; its render view is now created at 1280x720, and the game
re-tiles the scene and sizes its post-processing from it (`0ed897c`). That exposed the resolve
rectangle: it was the scissor (whole EDRAM tile rows), so a 320x180 destination got 320x184 and its
texture sampled four rows of stale EDRAM into the bloom; resolves now copy the rectangle their
vertices span, within the scissor and the declared destination.

Remaining steps:
1. 3D (volume) textures, texture signedness (signed formats are read unsigned).
2. Resolves: written back to guest memory since step 13, except 16-bit, 16_16, 64-bit, float and
   D24FS8 destinations and destinations resolved every frame (lazy writeback would cover those).
   The resolve rectangle (step 19) and resolve clears in the target's own format (#43) are done.
3. Internal resolution scaling: done for render targets, viewports, scissors, resolves and
   clears (`resolution_scale` / `--resolution-scale`, step 6, #115). Matches are play-tested at
   scale 2 (bloom, depth of field and replays look right); shadow maps and half-pixel offsets in
   post-processing were not checked one by one. Still open: scaling resolves back down when they
   are written to guest memory.
4. Shader translation: remaining control flow and features as gameplay shaders need them;
   point sprites; pixel shader param_gen.
5. Performance: done in phase 9 (the texture write watch #103, batched memory-export barriers and
   the GPU time per kind of work #122, skipped EDRAM transfers #123, reused index and vertex copies
   #124).
This is the largest phase (weeks to months).

## Phase 5 - Controller input (done)

SDL3 gamepad backend behind `readPad()` in `runtime/hid/input.cpp` (DualSense over USB on the
dev machine), vibration via `XamInputSetState`, keyboard fallback.

Status (`607e8ce`, issue #6): up to four controllers through SDL3 (users 0-3, in SDL's order),
mapped to XInput (south/east/west/north = A/B/X/Y, stick Y inverted), keystrokes generated from
button, trigger and stick-direction edges, vibration forwarded with `SDL_RumbleGamepad`. With a
window the presenter's loop polls the controllers (SDL events stay on that thread); headless
runs use a small input thread. User 0 keeps a virtual pad in scripted and headless runs, so
`--press` works with or without a controller. Tested on 2026-11-13 by the maintainer with a
DualSense over USB in the window: the game is playable through the menus with the controller.
Unplug/replug not explicitly tested yet. Not yet: keyboard fallback, key repeat.

Sticks in a match (#47): the stick axes read 0 in gameplay. The controller code reads a full
stick axis (input ids 24-27: left X/Y, right X/Y, each with a dead zone that depends on the other
axis) through `sub_8262B1C0`, which tail-calls a small leaf function at `0x8262B0F8`. That
function has no `.pdata` entry, is never called with `bl` and has no pointer to it, and the
switch-table bound of `sub_8262AF48` ran over it. So XenonRecomp emitted `// ERROR 8262B0F8`
and a bare `return` there, and every full-axis read returned 0.0. The kick still worked because
it reads the stick-direction inputs (ids 14-23), which take another path.
`tools/function_bounds.py` now also declares
branch-only functions: tail-call targets right after padding or a `blr`, from another function's
range. That adds `0x8262B0F8` and shortens `0x8262AF48` to end where it starts. A debug build
logs the game reading left Y = 1.000 from `8262A05C` while the stick is held. Before, it read 0.
The 60 other `// ERROR` branches were fixed in the analyzer (#52, see Phase 1).

Done early: scripted presses for testing (`--press "a@20,a@26"`, see `docs/runtime.md`), reported
both as pad state and as keystroke events (`XamInputGetKeystrokeEx`). The game accepts them:
A on the title screen stops the attract loop and moves on to the storage-device prompt.

Open issues: #6 (SDL3 controller backend).

## Phase 6 - Audio output (done)

The game's audio engine is FMOD Ex (`media/sounds/XBox360/*.fev` + FSB4 banks). Nearly every sound
in the banks is XMA (FSB sample mode `FSOUND_XMA`); one ambience stream is IMA ADPCM. FMOD drives
the XMA hardware contexts itself, mixes in software and submits one 5.1 frame (256 samples) per
render-driver callback.

- Output (#90, done): `XAudioSubmitRenderDriverFrame` frames go to an SDL3 stream on the default
  playback device (5.1 for surround devices, a stereo downmix otherwise), with the callback steered
  by the stream's fill level so it follows the device clock; `--mute`, `--audio` (headless) and
  `--audio-dump <wav>`; `[apu]` status lines and per-channel levels in the exit report. The game
  mixes in 5.1 (all six channels carry sound once XMA decodes, #93). Headless check: 187.5 frames/s (= 48000 /
  256), callbacks = frames, the logo screens' sound in the dump. See docs/runtime.md, "Audio".
- Real XMA2 decoding (#93, in progress). Fedora's `ffmpeg-free` has no XMA decoder (WMA Pro family
  is excluded), and upstream FFmpeg only decodes whole multi-stream packets. Xenia decodes one XMA
  frame at a time with an `xmaframes` decoder from its FFmpeg fork; `scripts/build_xma_decoder.sh`
  builds just that decoder from a pinned commit (static, into `third_party/ffmpeg-xma`), and CMake
  links it when present. `runtime/apu/xma_context.h` is a port of Xenia's `XmaContextNew` (frames
  across packets and buffers, split frame headers, loops with subframe precision, subframe decode
  counts, output padding, merged write-back), checked on synthetic packets by
  `tests/xma_context_test.cpp`. A kick now runs one pass on the kicking thread, as in Xenia,
  instead of a polling thread. On the test route without the decoder: about 3,600 kicks/s and
  2,700 frames/s (played as silence), no parse errors, and the route plays as before. With the
  decoder (2026-11-15, main d4ba320 + #90 + #93): 486,300 frames decoded in 260 s with 0 decode
  and 0 parse errors, sound in every second except 4-9 s (between the logo screens and the title music), all six
  channels in use, 30.1 fps in the match (unchanged), memory flat, and about 23 ms/s of CPU in
  XMA passes in the match (12.4 ms/s of it in FFmpeg). The game's own mix sometimes goes above
  full scale (channel peaks up to 1.24), so a few dozen samples per run clip.
  Listening test in the window (2026-11-15, the maintainer): "audio works perfectly".

Left for later: a soft limiter if the occasional over-full-scale peaks ever become audible; the
XMA "interrupt when done" / "stop when done" bits (ignored, as in Xenia). Movies are phase 7.

## Phase 7 - Movies (done)

The logo and attract movies are `.wmv` files. The frame captures show the game's own movie player
decodes them and draws the YUV planes on the GPU (motion-compensation shaders), so pictures should
appear once the renderer handles those draws. This phase covers what remains: movie audio, timing,
and any decoding the player expects hardware for.

Status (#132, 2026-11-16): pictures, sound and timing needed nothing, but every movie stopped after
a second or two (the 505 logo at 2.6 s of 11.4 s, the NaturalMotion logo at 0.8 s of 9.9 s). The
player reads one block past the end of each file and expects `ERROR_HANDLE_EOF` from that read; our
`RtlNtStatusToDosError` answered 317 for every status, so the stream counted as broken and the video
decoder ended early. With the statuses mapped as Windows maps them (`runtime/kernel/ntstatus.h`), the
boot logos, the attract movie (after about two minutes on the title screen) and the bonus trailers
(Locker Room > Bonus > Media > Videos) play to the end, at the right speed and in sync, checked
against the files decoded with FFmpeg. The boot logos now take about 21 s: `skip_intro=1` in
`game/settings.ini` leaves them out, and headless runs skip them so scripted routes keep their
timing. See `docs/runtime.md`, "Movies".

## Phase 8 - Saves and profile (saves done)

XamContent* (save devices, content creation/enumeration) mapped to a host directory; persist
profile settings; device selector UI answered automatically.

Status - saves done (`69ba008`, issue #7): `runtime/kernel/xam_content.cpp` implements
content enumerators (`XamContentCreateEnumerator` / `XamEnumerate`), `XamContentCreateEx` /
`XamContentClose` (a package is mounted as a drive such as `save:` through a runtime device
mount + symbolic link in `files.cpp`), `XamContentGetCreator`, `XamContentSetThumbnail` and the
device queries, following Xenia's layouts. Packages live in
`game/saves/<content type>/<file name>/`, with the display name in `<file name>.name` and the
thumbnail in `<file name>.png`. The game creates "Profile Options" (`BBOPT15.SAV`, 1 KB) and
"Custom Teams" (`BBCUST14`, 4 MB) on the first run and reopens them on the next, then reaches
the main menu. Not yet: content on the disc device, `XamContentDelete`.

Profile settings (#98): the game reads 3 dashboard settings (the voice options) and 29 of its own
when the profile signs in (gamercard fields, controller and gamer preferences, three title-specific
blobs) and never writes any: there is no `XamUserWriteProfileSettings` import and no write
message, and its options live in its own save (`BBOPT15.SAV`). `XamUserReadProfileSettings`
returns the console's defaults for a fresh offline profile, as Xenia does (source "default",
values 0 or empty, controller vibration on, the default gamer picture key), and logs which
settings were asked for by name. The Options > Controls and Game screens look the same as before.

## Phase 9 - Gameplay correctness and performance (in progress)

Play-test menus and matches; watch Euphoria/Morpheme physics for floating-point differences;
profile hot paths; consider XenonRecomp's optimization flags once behavior is correct.
High-frame-rate support (see Goals): 60 fps since #89 (the present interval and the gameplay tick,
after fixing the guest clock); see `docs/runtime.md`, "Frame rate".

Performance (done):
- Frame-rate dips (#73, closed): the side-select freeze was shader compiles (a disk shader cache,
  #77); memory-export draws no longer wait for the GPU (#88); recorded work is submitted early (#94);
  GPU fences are written when the GPU gets there instead of stopping the command processor (#96);
  a fifth less renderer work per draw (#99); textures are known unchanged without reading them, a
  page write watch (#103).
- Heavy scenes at 60 fps and resolution scale 2 (#121, closed): rain at night ran 46-49 fps at
  2,000+ draws a frame. GPU time by kind of work (`BB_GPU_TIME`) showed three costs: render targets
  keyed by storage format, multipass export draws not rasterized, memory-export barriers batched
  (#122); EDRAM transfers skipped for tiles a clear overwrites (#123); and on the command processor
  thread, which decides whether a frame makes the vblank (`BB_CP_TIME`), index and vertex copies
  reused within a frame (#124). Window play-test: 59.9 fps, lowest second 56.

Performance (open): the Steam Deck is slow at moments (#146). The Deck's performance overlay can't
show it: it counts the window's presents, not the game's frames. To measure it there:
- `show_fps=1` puts the game's own frames, speed and longest frame in the corner of the window;
- the `[gpu]` status line ends with the second's longest frame and the game's speed;
- `tools/fps_report.py` totals the game time lost;
- the installed build keeps a log file in `~/.local/share/backbreaker-recomp/logs/`.

Measured on the Deck (2026-11-17, #146): in a match the game makes 26 fps and runs at 87% speed.
- **The limit is the CPU.** The command processor thread, which also runs the renderer, does
  28.8 ms of work a frame against a 33 ms budget, and the GPU is idle 28% of the time.
- **Uniformly slower.** Each step of that work costs about 3.3 times what it does on the PC.
- **The geometry cache (#146)** keeps indices and vertices that stay the same across frames. On the
  PC it takes the thread's work in an Exhibition match from 6.5 to 4.7 ms a frame (`docs/runtime.md`,
  "Renderer").

With the cache, the Deck holds 29.7 fps and 97% speed in a match, and the command processor does
20.5 ms of work a frame. What's left are single long frames of 100–150 ms: 17 in a 7-minute match.
- **Thread timing** (`BB_PERF=1`, `docs/runtime.md`, "Thread timing and hitch reports") reports
  what every thread did in such a frame.
- **On the PC it showed** the game's main thread spinning in D3D's wait for the GPU fence about 95%
  of the time, and the command processor polling for the game as much. At 30 fps both wait for the
  vblank on a full core each.

The Deck run with it (2026-11-17) answered that the hitches in play aren't a lack of free cores.
- **One thread's work:** the game's main thread runs the game's own code for 85–100 ms of the frame,
  roughly once a play, while the other threads wait for it.
- **The spinners don't explain them:** in play they leave the other threads only 30–60 ms a second
  waiting for a core.
- **Stack sampling:** the main thread's stacks are now sampled into the hitch reports.

Next: a Deck run with the sampling, to name the code. It's either the game's own logic or one of the
runtime's imports it calls, such as memory allocation.

Correctness: the analyzer fixes (#52); crashes fixed: a reused TLS slot that handed back freed data
on the second match load (#127), and the Training Camp passing drills, a race between the game's
update and render threads over a play's player slots (#129). #50 (an assert after stick input) was
closed as not reproducible. Test runs: `--fixed-seed` gives every run the same random numbers, so
the Exhibition route reaches the same coin toss, wind and kickoff (#71).

## Phase 10 - Packaging (started)

`scripts/setup.sh "<iso>"` (`100ec86`, #4) takes a checkout and an image of your own disc to a
built `build/backbreaker` in one step: prerequisite check with the package names for the
distribution, extraction, decryption, the XenonRecomp clone + patch + build at the pinned commit,
the recompile, the import glue, the CMake build and the game-free tests. It skips a step whose
output already exists, `--force` redoes everything, and `--regenerate` re-derives
`config/backbreaker_switch_tables.toml` and the leaf-function block of `config/backbreaker.toml`
from your image. The image path never reaches the output or a file. README ("One-step setup") and
`docs/runtime.md` ("Build and run") document it; the per-command sections stay as the reference.

Next in this phase: a portable build (#134), with an **AppImage** as the main output, that runs on
the Steam Deck (SteamOS), Debian/Ubuntu, Fedora and Arch. Status (2026-11-16): built, and checked
headless in containers of Ubuntu 22.04 and 24.04, Debian 12 and 13, Fedora and Arch (#136: where
files live, mods folder, CPU check; #137: the container, `setup.sh --portable`, the AppImage). On
2026-11-17 the maintainer's Steam Deck play-test ran the AppImage (`e795ba5`) in Gaming Mode as a
non-Steam game. Two follow-ups came out of it:
- #145: on the Deck, the achievements list needs a back grip mapped to F1 (README, step 5).
- #146: the game is slow at moments on the Deck. The Deck's performance overlay showed no drop below
  30 fps, because it counts the window's presents, not the game's frames.

Still open in #134: the docker path, since only podman has been used.

The native build (`scripts/setup.sh` without `--portable`) doesn't travel, which is why the portable
build exists:
- it needs the build machine's glibc (2.44 when built on Fedora 45);
- it links SDL3, shaderc and SPIRV-Tools from the system;
- it finds `game/` only in the working directory, and writes settings and saves there;
- `setup.sh` can't build natively on Debian 12 or Ubuntu 22.04/24.04, which don't package SDL3.

The plan:
- **A build container:** Ubuntu 22.04 for a glibc 2.35 floor, with SDL3, shaderc and the C++ runtime
  linked statically, and appimagetool with the static runtime, so hosts don't need `libfuse2`.
- **`setup.sh --portable`:** runs extraction, recompile, build and packaging in it with podman or
  docker, so any distribution with one of those works. The build folder stays in the checkout, so
  rebuilding after a code change stays incremental.
- **Outputs:** `Backbreaker-x86_64.AppImage`, plus the same as a plain folder or `.tar.gz`.
- **Where files live:** game data is found next to the executable or inside the AppImage. Settings
  and saves move to the per-user folders (`~/.config/backbreaker-recomp`,
  `~/.local/share/backbreaker-recomp`) for the AppImage and the folder; the checkout build keeps
  `game/`.
- **A mods folder** that overlays the disc's files without a rebuild.
- **A CPU check, and Steam Deck instructions.**

The binary and the AppImage contain the player's own recompiled game and disc files, so they are
for moving your own build between your own machines and are never published. The repo and the
container image hold no game content. The repo has no licence yet, which is the maintainer's
decision before it goes public.

**The installer (#138, started 2026-11-17):** for players who own the game. They choose their ISO, and
the game code is translated and compiled on their machine against a prebuilt runtime; nothing made
from the game is ever published.

Launchers checked first (2026-11-17):
- **RecompLauncher** (GPL v3, desktop app 0.90 beta) can host it, and is the chosen front end. A
  `recomplauncher.json` in our release declares the whole flow: the player drops in their disc image,
  `game.check` confirms it holds `default.xex`, a `prepare` step runs our own builder headless with the
  image's path (progress lines become its progress bar, the time limit is ours to set), `config` edits
  `settings.ini` for its settings page and controller mode, and Play starts the game. It installs
  and updates from our GitHub releases. It reruns `prepare` only when the manifest's `version`
  changes, so after a runtime update the game's start step relinks itself (seconds, see below).
- **Quiver Launcher** (source-available, BSL 1.1) installs and runs release files but has no setup
  step, so it can't build from the player's disc.
- So no GUI of our own for now: the builder also runs in a terminal (`--cli`), and a GUI comes back
  only if a Steam Deck test of the RecompLauncher flow shows it's needed.

The parts, in order:
1. **The runtime kit** (done): the runtime builds as a library without game files
   (`-DBB_RUNTIME_ONLY=ON`) and installs as a kit with its compile and link arguments, and
   `packaging/installer/link_game.sh` builds the game from it: all of `ppc/` in about 30 s with 16 jobs on
   the maintainer's desktop, a relink after a runtime update in under a second (docs/runtime.md, "Runtime kit").
2. **The extraction tools in C++** (done): `backbreaker-installer` checks the image (another game, a
   cut-short or damaged copy and an unknown release each get their own message and exit code) and does
   what `tools/xdvdfs.py`, `xex_decrypt.py`, `gen_imports.py` and `title_icon.py` do, byte for byte,
   with no Python or OpenSSL (docs/runtime.md, "Installer tools").
3. **The headless install** (done): `backbreaker-installer install <image>` checks the image, copies
   the game's files, runs XenonRecomp (2.4 s) and compiles and links the game with its bundled clang
   and lld against an Ubuntu 22.04 sysroot, into `~/.local/share/backbreaker-recomp/install`; `play`
   relinks after a runtime update (0.4 s) and starts the game. The installer AppImage
   (`packaging/installer/build_installer.sh`) is 109 MB, 416 MB unpacked, with no game content.
   From the ISO to a built game: 32 s on the maintainer's desktop (16 jobs; jobs are chosen by CPU
   threads and free memory at 1 GB each). In containers of Ubuntu 22.04 and 24.04, Debian 12 and 13,
   Fedora and Arch, none with a compiler, it installed from the image in 32 s each and the game
   reached the Exhibition kickoff (the title screen on lavapipe for Ubuntu 22.04 and Debian 12)
   (`packaging/portable/test_distros.sh --installer`).
   `ppc/` stays at `-O2` (2026-11-17): `-O1` compiles it with 23% less CPU (342 s against 441 s), but
   the game's own threads then need 28% more CPU by median, about 40% by mean, outside their waits
   and spins (the headless Exhibition route with `BB_PERF=1`, runs alternating `-O2`, `-O1`, `-O2`,
   `-O1`; both pairs agree). The Deck is short of CPU in matches already (#146). To redo if
   XenonRecomp's register options are turned on in `config/backbreaker.toml` (written up on #146):
   every `ppc/` file changes with them. Still open from part 6: the time on a Steam Deck.
4. **RecompLauncher's `recomplauncher.json`** (written, not yet tried in the launcher itself): in the
   installer AppImage, valid against the launcher's schema. RecompLauncher's own steps, run by hand
   from the manifest (a folder with a space, as its "Recomp Library" has) build the game in 33 s and
   start it, full screen in Big Picture mode (docs/runtime.md, "Installer tools"). Next: the
   maintainer's run of the launcher on the Steam Deck, Gaming Mode included, which also gives the
   Deck's build time.
5. Add to Steam, a menu entry and GitHub releases. Publishing waits for the licence decision and for
   the notices and sources of everything the kit and the builder ship (FFmpeg, glibc and libstdc++
   from the sysroot, XenonRecomp with binutils' PowerPC disassembler, the AppImage runtime's libfuse).

Also still open: an install prefix.

## Phase 11 - Achievements (done)

Goal: the game's Xbox 360 achievements work again - unlocks are recorded, an "Achievement
unlocked" pop-up appears in game, and an achievements screen lists them all.

What the game provides (checked 2026-11-13):
- Its achievement database is embedded in the executable as an XDBF (".spa") resource: 29
  achievements worth 1000 gamerscore, with name/description strings (per language) and images.
  It is read at startup from the user's own game files; nothing derived from it is committed.
- Unlocks are reported to the system through `XMsgStartIORequest`: the XGI app (0xFB), message
  0x000B0008 (`XUserWriteAchievements`, recompiled as `sub_828F7A30`; Xenia `xgi_app.cc`), and the
  game reads existing unlocks with `XamUserCreateAchievementEnumerator` when a match starts.
  (Corrected 2026-11-15: an earlier note here said 0x000B0025 and that the game imports
  `XamShowAchievementsUI`; it imports neither, and it has no achievements screen of its own.)

Status: parts 1 and 2 are done (#97, `1a59660`): the SPA reader (`runtime/kernel/spa.h`,
tested), the unlock path with a per-machine store (`game/achievements/`), the enumerator, and
`--achievements`. Confirmed in play: "First of Many" and "Quarter Pounder" unlocked, stored and
listed. Parts 3-5 (#101, `83fce76`): the overlay (`runtime/video/overlay.cpp`), the achievements
list on F1 / Guide, and the unlock pop-up, tested in the window. #105: simultaneous unlocks take
turns in the pop-up, and each plays a synthesized unlock sound. See docs/runtime.md,
"Achievements". All five parts below are done.

Planned parts:
1. Database reader: parse the XDBF resource (achievements, strings, images) from the loaded
   executable. No game files needed to test the parser beyond a tiny synthetic XDBF.
2. Unlock tracking: handle the XGI write-achievements message and the achievement enumerator,
   storing unlocks with timestamps per profile in a host file (next to the saves of phase 8),
   so the game sees its earlier unlocks.
3. Overlay layer: a small UI layer the presenter draws on top of the game at window
   resolution (also useful later for settings, an fps counter, controller prompts).
4. Pop-up: "Achievement unlocked" toast with icon, name and gamerscore, like the console's.
5. Achievements screen: opened with a key / controller button and whenever the game calls
   `XamShowAchievementsUI`; lists all achievements as locked or unlocked with descriptions,
   gamerscore, unlock dates and a total. Secret achievements stay hidden until unlocked.

A new entry inside the game's own menus would mean patching its menu code and assets, so the
screen is an overlay instead (as the console's Guide was).

Depends on: phase 5 (a button to open the screen), phase 8 (profiles and where unlocks are
stored). Parts 1 and 2 can start earlier and are good separable issues.

## Phase 12 - Online multiplayer (planned, last)

Goal: play Backbreaker's own online modes against other people running the recomp, with a way to
find players who are hosting. This phase comes last, after the game is complete offline. The full
research (feasibility, options, threat model, sources) is in
[`docs/multiplayer.md`](multiplayer.md).

What the game provides (checked 2026-11-15): online Exhibition and Tackle Alley as player and
ranked matches, a pregame lobby, invites, voice chat and leaderboards, all on NaturalMotion's
`NmgComms` layer over XSession, XNet QoS and sockets. It already has a **Hosted Games** list, so
the game browser can be the game's own menu, fed by our server. There is no system link, so LAN
tunneling (XLink Kai) can't work. Instead, the runtime has to stand in for the Xbox LIVE side.
One host runs the session, and the in-match message names (`kGameMessage_Sync_*`, remote
controllers) suggest the host simulates the match and sends its state. If that holds, two
separately compiled builds can play together, because they don't need to agree bit for bit as a
lockstep game would. Confirming it is the first research item.

Planned parts:
1. Research: confirm the netcode model (#108), list the online calls the menus make (#109), verify
   local two-controller play and document streamed play (Steam Remote Play Together, Parsec),
   which works with no network code (#110).
2. Security prerequisites, before any networking lands. Offline the guest is trusted; online, a
   peer's packets reach the game's parsers. Found so far: the indirect-call table (host function
   pointers) is writable from guest code (#111), `..` in guest paths escapes the mount root
   (#112), and imports trust guest lengths (#113). Plus a sandbox for the game process.
3. LAN proof: real sockets, XNADDR mapping, XSession calls answered by a local session service,
   online sign-in. Goal: two instances reach a pregame lobby and kick off.
4. Internet play: a self-hostable session service (registration, search, hole punching,
   per-session encryption keys), the Hosted Games table filled from it, and a version check on join.
5. Relay for unreachable hosts and for players who don't want to share their IP, voice,
   invites, and an external game browser (launcher or web) that joins through the game's own
   invite flow.
6. Maybe: stats and leaderboards, labelled unverified (anyone can claim any XUID, and the host
   controls the match).

Depends on: phases 4 and 9 (menus and matches render reliably at full speed; a slow host slows
everyone), 8 (profile and gamertag), 10 (if the browser lives in the launcher) and 11 (the overlay
for gamer cards, the keyboard and friends).

Open issues: #108-#113, all labelled `after the game is done`: none of them starts before the
game is complete offline. The later parts (3-6 and the sandbox) get issues when the research is in.

---

## Known shortcuts and technical debt

- `NtSuspendThread` only works before a thread starts; memory decommits are ignored.
- Timer APC routines are not delivered.
- XMA output is silence; XAudio frames are discarded.
- Sockets exist but carry no data offline (`NetDll_*`: connect refused, recv would block, sends
  dropped); XAM content, achievements and guide UI are stubs.
- `WAIT_REG_MEM` in the command processor spins with `yield`.
- The presenter falls back to the raw (tiled) front buffer when a swap was not rendered.
- Renderer shortcuts: see phase 4 steps 5-13 and the remaining steps (some resolves not written
  to memory, per-frame texture re-uploads).
- Virtual memory grows ~1 MB per 20 s in the front end (not investigated).
- The runtime trusts the guest. The indirect-call table is writable from guest memory, `..` in a
  guest path climbs out of the mount root, and imports don't bound guest lengths. That's fine
  while the guest is only ever the game; it must change before phase 12 (#111, #112, #113;
  `docs/multiplayer.md`, "Security").

## Open questions

- Which save/content calls the game makes after enumeration (expected: XamContentCreateEx).
- XMA2 decoder source for phase 6 (RPM Fusion FFmpeg vs. vendored decoder).
- Is online play host-authoritative or lockstep (phase 12)? It decides whether two separately
  compiled builds can play each other.
- High-level (hook Direct3D calls) vs. low-level (emulate Xenos PM4, like Xenia) rendering:
  the current command processor follows the low-level route, which needs no knowledge of the
  game's statically linked D3D library.

## Reproducing (for reviewers)

Game data is not in the repository: you need your own Backbreaker ISO. See `README.md` for
extraction, XenonRecomp setup, recompilation and tests, and `docs/runtime.md` for building and
running the runtime and for its debugging aids.

For the F12 screenshot key (#10): run without `--headless`, press F12 in the window (or focus it
and `xdotool key F12`). The log prints `[video] screenshot requested (WxH, format)` and
`[video] screenshot screenshots/<date-time>.png (...)`; the file is 1280x720 when the window is.
Press twice inside one second to see the `_2` suffix: every press gives its own file (holding the
key gives one). `--screenshot 140` as well writes the renderer's front buffer at that swap, for a
side-by-side look.
