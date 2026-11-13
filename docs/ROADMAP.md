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
boots the game through its full initialization, runs through the two logo movies (timing only,
nothing shown), loads all sound banks, and reaches the title screen, where it loops the attract
movie (`attract.wmv`) with a 3D logo scene in between at a steady 30 fps. With scripted button
presses (`--press`), the game leaves the title screen, asks for a storage device and starts
looking for saves - where it stops, because save handling (phase 8) is not implemented yet.
**Nothing is visible or audible yet**: GPU commands are executed only for their side effects and
audio is silence. Rendering (phase 4) has started with a GPU frame-capture tool.

## Phase overview

| # | Phase | Status | Commits |
|---|-------|--------|---------|
| 0 | Feasibility and extraction | Done | `43b7032` |
| 1 | CPU recompilation (PowerPC -> C++) | Done | `5dad6d2`, `8efcafd`, `8400499`, `9667d0e` |
| 2 | Host runtime boot (loader, kernel, threads, files) | Done | `0507715`, `b3796cc`, `9d09a50` |
| 3 | Headless subsystems: game reaches the front end | Done | `8400499`, `9667d0e` |
| 4 | Rendering (Vulkan): see the game | **In progress** (steps 0-1 done) | `258ec24` |
| 5 | Controller input (SDL, DualSense) | Planned (scripted presses done) | - |
| 6 | Audio output (real XMA decoding, mixing) | Planned | - |
| 7 | Movies (WMV / XMV playback) | Planned | - |
| 8 | Saves and profile (XAM content) | **Next blocker** (device selector done) | - |
| 9 | Gameplay correctness and performance | Planned | - |
| 10 | Packaging and launcher | Planned | - |

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
  arithmetic against hand-computed results (a deliberately broken variant is caught).

Verify: see README "Recompile" and "Tests".

Open issues: #1 (tests without game files + CI), #2 (tests for the remaining new instructions).

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

Remaining steps:
1. Shader disassembler for the captured microcode (which Xenos instructions this game uses).
2. Xenos state tracking: render targets (EDRAM), viewport, blend/depth state from the register file.
3. Shader translation: Xenos microcode (vertex/pixel) -> SPIR-V. Xenia's shader translator is the
   reference.
4. Render targets in EDRAM, resolves to memory (including the front buffer); internal
   resolution scaling.
5. Textures: tiled formats, untiling, format conversion; vertex fetch.
6. Draw calls (`DRAW_INDX`, `DRAW_INDX_2`). First picture expected: the attract movie.
This is the largest phase (weeks to months).

## Phase 5 - Controller input (planned)

SDL3 gamepad backend behind `readPad()` in `runtime/hid/input.cpp` (DualSense over USB on the
dev machine), vibration via `XamInputSetState`, keyboard fallback.

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

Status: the device selector is answered (one dummy hard drive, as in Xenia) and the XMP music
player query is handled. The game then enumerates saves with `XamContentCreateEnumerator` /
`XamEnumerate` and crashes on the stubs - this is the current blocker after the title screen.
Next: enumerators (empty at first), `XamContentCreateEx` / `XamContentClose` mounting a save
container as a drive (e.g. `save:`) backed by a host folder, `XamContentGetDeviceData`.

Open issues: #7 (save system).

## Phase 9 - Gameplay correctness and performance (planned)

Play-test menus and matches; watch Euphoria/Morpheme physics for floating-point differences;
profile hot paths; consider XenonRecomp's optimization flags once behavior is correct.
High-frame-rate support (see Goals): find what caps the game at 30 fps, check whether game logic
and physics use real elapsed time, then patch timing or interpolate.

## Phase 10 - Packaging (planned)

Reproducible build from a user's own ISO (extract, decrypt, recompile, build) in one script;
optional integration with a launcher such as recomplauncher.com.

Open issues: #4 (one-step setup script).

---

## Known shortcuts and technical debt

- `NtSuspendThread` only works before a thread starts; memory decommits are ignored.
- Timer APC routines are not delivered; `KeTimeStampBundle` is not updated.
- XMA output is silence; XAudio frames are discarded.
- Networking (`NetDll_*`), XAM content, achievements and guide UI are stubs.
- `WAIT_REG_MEM` in the command processor spins with `yield`.
- The presenter shows the front buffer raw (tiled, no format conversion).
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
