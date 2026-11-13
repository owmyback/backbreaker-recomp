# Project roadmap and phase tracker

Goal: run Backbreaker (Xbox 360, 2010, NaturalMotion) natively on Linux by statically
recompiling its PowerPC executable to C++ (XenonRecomp) and providing a host runtime that
stands in for the Xbox 360 kernel, GPU, audio and input.

**Keep this file current:** when a phase changes status, update its row, its section and the
"Current state" paragraph, and reference the commits.

Work that can be picked up is tracked as GitHub issues and linked from each phase below; see
[`CONTRIBUTING.md`](../CONTRIBUTING.md) for how to take one.

Last updated: 2026-11-12 (phase 4 started)

## Current state

The whole executable recompiles (0 warnings) and builds into one native binary. The runtime
boots the game through its full initialization and renders it with Vulkan: the middleware logo
screen, the attract movie and the title screen. With scripted button presses (`--press "a@20,a@26"`)
the game leaves the title screen, picks the (emulated) hard drive, creates or loads its saves
("Profile Options", "Custom Teams" in `game/saves/`) and reaches the **main menu** (Training
Camp, Exhibition, Leagues, Tackle Alley, Xbox LIVE). Next: controller input (phase 5, #6) to
actually play. Audio is still silence.

## Phase overview

| # | Phase | Status | Commits |
|---|-------|--------|---------|
| 0 | Feasibility and extraction | Done | `43b7032` |
| 1 | CPU recompilation (PowerPC -> C++) | Done | `5dad6d2`, `8efcafd`, `8400499`, `9667d0e` |
| 2 | Host runtime boot (loader, kernel, threads, files) | Done | `0507715`, `b3796cc`, `9d09a50` |
| 3 | Headless subsystems: game reaches the front end | Done | `8400499`, `9667d0e` |
| 4 | Rendering (Vulkan): see the game | **In progress** (steps 0-9 done: first picture, resolution scaling, tiled textures, EDRAM aliasing, render to texture) | `258ec24`, `7bf4023`, `9ac4cb9` |
| 5 | Controller input (SDL, DualSense) | **In progress** (SDL3 backend done, hardware test pending) | `607e8ce` |
| 6 | Audio output (real XMA decoding, mixing) | Planned | - |
| 7 | Movies (WMV / XMV playback) | Planned | - |
| 8 | Saves and profile (XAM content) | **Saves done** (profile settings still empty) | `69ba008` |
| 9 | Gameplay correctness and performance | Planned | - |
| 10 | Packaging and launcher | Planned | - |
| 11 | Achievements (unlocks, pop-ups, achievements screen) | Planned | - |

Naming note: in early discussion, phase 3 was called "Phase 1 (headless GPU)" and phase 4
"Phase 2 (Vulkan renderer)". The numbering above replaces that.

## Goals beyond running the game

| Goal | Where | Notes |
|------|-------|-------|
| Higher internal resolution (1440p, 4K) | Phase 4 | The renderer can render at any internal resolution while the game is still told 1280x720. Needs consistent scaling of EDRAM resolves/copies and of resolution-dependent effects (blur, bloom, shadows, post-processing). 2D UI scales as-is. Ultrawide (beyond 16:9) would also need camera/layout patches. |
| Higher frame rate (60+ fps) | Phase 9 | The title loop runs at 30 fps; the source of the cap is not confirmed yet (attract video rate, vblank interval, or game logic). Uncapping is easy; the risk is game logic and Euphoria physics assuming a 30 Hz step. Fix by patching timing code (function overrides / mid-asm hooks; profiler-scope names help find it) or by a fixed 30 Hz simulation with interpolated rendering. Must be verified by play-testing. |

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
- `tools/find_switch_tables.py`: XenonAnalyse found 0 jump tables (Backbreaker's compiler orders
  instructions differently and pads with nops); ours finds 438, including inline tables without
  a bounds check.
- `tools/function_bounds.py`: explicit bounds for 48 leaf functions with switches (no `.pdata`
  entry) and 11 functions reachable only through pointers (vtables, exception funclets).
- Register save/restore helpers located by byte pattern (`config/backbreaker.toml`).
- Result: 30,000+ functions -> 116 C++ files, 0 recompiler warnings, all compile with clang.
- Tests: `tests/instr_semantics.cpp` checks the byte-order-sensitive vector ops and carry
  arithmetic against hand-computed results (a deliberately broken variant is caught). Builds
  without game files against `tests/ppc_config_stub/` and runs in GitHub Actions on every
  push (`.github/workflows/ci.yml`), which also builds the patched XenonRecomp.

Verify: see README "Recompile" and "Tests".

Open issues: #2 (tests for the remaining new instructions).

## Phase 2 - Host runtime boot (done)

Goal: load the image and run the entry point with real kernel services.

- `runtime/main.cpp`: 4 GB guest address space, image load, indirect-call table, crash handler.
- `runtime/kernel/`: all 228 imports have a logging stub (generated by `tools/gen_imports.py`);
  implemented: memory, XEX header queries, TLS, critical sections, spinlocks, events,
  semaphores, timers, waits (`objects.cpp`), real guest threads on host pthreads
  (`thread.cpp`), file system with case-insensitive device mapping (`files.cpp`), time,
  strings, the game's printf-style logging.
- Result: CRT init completes, the game starts its file-system and pool threads and opens its
  data archives.

Open issues: #3 (keep `KeTimeStampBundle` updated).

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

Open issues (separable from the renderer core): #9 (fullscreen toggle), #10 (screenshot key),
#11 (Vulkan validation clean run), #12 (tested tiled/linear conversion).

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
Not yet: loops/calls/jumps, vector predicate-push ops, LOD/gradient fetches, memory export,
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
clean black and the title screen shows its football-field background. Not yet: transfers
between different formats (including color <-> depth), which are skipped with a warning.

**Step 9 - Cube maps and render to texture on the GPU (`a450796`).** Cube textures (6 faces,
linear or tiled, base level) upload like 2D ones. Render to texture stays on the GPU: a 2D
8_8_8_8 texture whose address is a resolve destination samples the resolved image directly
(with the red/blue swap the resolve asked for, and at the internal resolution), and a cube map
whose six face slots were all resolved (the game renders environment cube maps at 256x256) is
assembled from the six resolved images. Seen working: the title scene's 256x256 render target,
several 128x128 and 64x64 ones, and two rendered cube maps. Not yet: textures that read resolved
data in another format or tiling than the resolve wrote, and CPU reads of resolved memory.

Remaining steps:
1. More texture formats, mip levels, 3D (volume) textures.
2. EDRAM transfers between different formats, resolves written back to guest memory (render-to-texture through memory, CPU reads), the resolve rectangle
   from the vertex data, depth resolves.
3. Internal resolution scaling: done for render targets, viewports, scissors, resolves and
   clears (`--resolution-scale N`, step 6). Still to check once 3D scenes render: effects that
   depend on pixel size (blur, bloom, shadow maps, half-pixel offsets in post-processing), and
   scaling resolves back down when they are written to guest memory.
4. Shader translation: remaining control flow and features as gameplay shaders need them;
   point sprites; pixel shader param_gen.
5. Performance: texture write-watching instead of per-frame uploads, finer barriers.
This is the largest phase (weeks to months).

## Phase 5 - Controller input (in progress)

SDL3 gamepad backend behind `readPad()` in `runtime/hid/input.cpp` (DualSense over USB on the
dev machine), vibration via `XamInputSetState`, keyboard fallback.

Status (`607e8ce`, issue #6): up to four controllers through SDL3 (users 0-3, in SDL's order),
mapped to XInput (south/east/west/north = A/B/X/Y, stick Y inverted), keystrokes generated from
button, trigger and stick-direction edges, vibration forwarded with `SDL_RumbleGamepad`. With a
window the presenter's loop polls the controllers (SDL events stay on that thread); headless
runs use a small input thread. User 0 keeps a virtual pad in scripted and headless runs, so
`--press` works with or without a controller. Waiting for: a hardware test with the DualSense
(title screen -> menus, unplug/replug). Not yet: keyboard fallback, key repeat.

Done early: scripted presses for testing (`--press "a@20,a@26"`, see `docs/runtime.md`), reported
both as pad state and as keystroke events (`XamInputGetKeystrokeEx`). The game accepts them:
A on the title screen stops the attract loop and moves on to the storage-device prompt.

Open issues: #6 (SDL3 controller backend).

## Phase 6 - Audio output (planned)

- Real XMA2 decoding. Fedora's `ffmpeg-free` has no XMA decoder (WMA Pro family is excluded);
  options: FFmpeg from RPM Fusion, or vendoring just the decoder.
- Feed decoded frames into the XMA output rings (replacing silence), mix XAudio render frames
  (`XAudioSubmitRenderDriverFrame`) to the host (SDL / PipeWire).

## Phase 7 - Movies (planned)

The logo and attract movies are `.wmv` files. The frame captures show the game's own movie player
decodes them and draws the YUV planes on the GPU (motion-compensation shaders), so pictures should
appear once the renderer handles those draws. This phase covers what remains: movie audio, timing,
and any decoding the player expects hardware for.

## Phase 8 - Saves and profile (planned)

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
the main menu. Not yet: stored profile settings (`XamUserReadProfileSettings` still returns
empty values), content on the disc device, `XamContentDelete`.

## Phase 9 - Gameplay correctness and performance (planned)

Play-test menus and matches; watch Euphoria/Morpheme physics for floating-point differences;
profile hot paths; consider XenonRecomp's optimization flags once behavior is correct.
High-frame-rate support (see Goals): find what caps the game at 30 fps, check whether game logic
and physics use real elapsed time, then patch timing or interpolate.

## Phase 10 - Packaging (planned)

Reproducible build from a user's own ISO (extract, decrypt, recompile, build) in one script;
optional integration with a launcher such as recomplauncher.com.

Open issues: #4 (one-step setup script).

## Phase 11 - Achievements (planned)

Goal: the game's Xbox 360 achievements work again - unlocks are recorded, an "Achievement
unlocked" pop-up appears in game, and an achievements screen lists them all.

What the game provides (checked 2026-11-13):
- Its achievement database is embedded in the executable as an XDBF (".spa") resource: 29
  achievements worth 1000 gamerscore, with name/description strings (per language) and images.
  It is read at startup from the user's own game files; nothing derived from it is committed.
- Unlocks are reported to the system: the game imports `XMsgStartIORequest` (the XGI app's
  write-achievements message, 0x000B0025 in Xenia's `xgi_app.cc`), and it lists existing unlocks
  with `XamUserCreateAchievementEnumerator`. Both are unimplemented today, so unlocks are lost.
- It imports `XamShowAchievementsUI` / `XamShowAchievementsUIEx` (the Guide's achievements
  screen on the console).

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

---

## Known shortcuts and technical debt

- `NtSuspendThread` only works before a thread starts; memory decommits are ignored.
- Timer APC routines are not delivered; `KeTimeStampBundle` is not updated.
- XMA output is silence; XAudio frames are discarded.
- Networking (`NetDll_*`), XAM content, achievements and guide UI are stubs.
- `WAIT_REG_MEM` in the command processor spins with `yield`.
- The presenter falls back to the raw (tiled) front buffer when a swap was not rendered.
- Renderer shortcuts: see phase 4 steps 5-8 and the remaining steps (EDRAM transfers only
  between same formats, resolves not written to memory, per-frame texture re-uploads).
- Profile settings are returned empty.
- Virtual memory grows ~1 MB per 20 s in the front end (not investigated).

## Open questions

- What caps the title loop at 30 fps (attract video rate, vblank interval, or game logic)?
- Which save/content calls the game makes after enumeration (expected: XamContentCreateEx).
- XMA2 decoder source for phase 6 (RPM Fusion FFmpeg vs. vendored decoder).
- High-level (hook Direct3D calls) vs. low-level (emulate Xenos PM4, like Xenia) rendering:
  the current command processor follows the low-level route, which needs no knowledge of the
  game's statically linked D3D library.

## Reproducing (for reviewers)

Game data is not in the repository: you need your own Backbreaker ISO. See `README.md` for
extraction, XenonRecomp setup, recompilation and tests, and `docs/runtime.md` for building and
running the runtime and for its debugging aids.
