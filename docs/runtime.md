# Runtime

`build/backbreaker` runs the recompiled game on the host.

## Build and run
    python3 -I tools/xdvdfs.py "<iso>" game/disc --all        # once: extract the disc
    python3 -I tools/gen_imports.py game/extracted/default.xex game/extracted/default.pe \
        third_party/XenonRecomp/XenonUtils/xbox/xboxkrnl_table.inc \
        third_party/XenonRecomp/XenonUtils/xbox/xam_table.inc runtime/generated/imports.cpp
    cmake -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
    ninja -C build
    ./build/backbreaker --timeout 15     # run from the repo root

By default a window opens (SDL3 + Vulkan) showing what the game renders; its title shows the
game's frame rate and the display rate. `--headless` runs without a window (the renderer still
runs if a Vulkan GPU is available, for screenshots). Build dependencies: Vulkan headers/loader,
SDL3 and shaderc (Fedora: `libshaderc-devel`) development packages. Set `BB_VK_VALIDATION=1` to
enable the Vulkan validation layer (needs it installed; Fedora: `vulkan-validation-layers`).

## Renderer
`runtime/gpu/renderer.cpp` executes the game's draws with Vulkan (one device shared with the
window, `runtime/video/vk_context.cpp`): shaders are translated (see below) and compiled with
shaderc on first use, EDRAM render targets are Vulkan images, resolves copy into images keyed by
their guest address, and the presenter shows the image resolved to the front buffer.
- `--screenshot N[,N...]` writes `screenshots/frame_N.png` (the front buffer of swap N; works
  headless). `screenshots/` is gitignored - pictures of the game are not committed.
- `--resolution-scale N` (1-4) renders at N times the game's resolution: 2 = 2560x1440,
  3 = 3840x2160. The game still sees 1280x720; render targets, viewports, scissors, resolves and
  clears are scaled. (The attract movie itself is a 1280x720 video, so it only gets smoother
  scaling; 3D scenes are where the extra resolution shows.)
- `--no-render` turns the renderer off (command stream side effects only, as in phase 3).
- Unsupported textures (1D/3D/cube and some formats, for now) show as magenta, and unsupported
  features log one `[render] WARNING` each.

On a crash or timeout it prints the host backtrace (sub_XXXXXXXX names are guest functions),
the last 32 kernel calls and a per-import usage summary.

## Layout
- `runtime/main.cpp` - loader, indirect call table, crash handler
- `runtime/memory.*` - 4 GB guest address space and allocators
- `runtime/kernel/` - kernel/XAM imports
  - `objects.*` handles, events, semaphores, waits
  - `thread.cpp` guest threads (PCR, KTHREAD, TLS, host pthreads)
  - `files.cpp` file system (game:\ -> game/disc, cache:\ -> game/cache)
  - `memory_imports.cpp`, `sync.cpp`, `strings.cpp`, `system.cpp`, `xex.cpp`, `variables.cpp`
- `runtime/generated/imports.cpp` - weak logging stub for every import (from tools/gen_imports.py)

## Status (2026-11-12)
Phase 3 (headless subsystems, see docs/ROADMAP.md) works end to end: the game boots, initializes graphics, audio, voice
(XHV2), comms and input, plays the 505 and NaturalMotion logo movies at ~30 fps, loads its sound
banks (~3 s), then runs its front end at a steady 30 fps (~65 draws per frame, likely the game's
own cap). Tested for 4 minutes: memory stays flat (~135 MB virtual + 280 MB physical). Nothing is
drawn: the command processor executes PM4 packets only for their side effects.

The earlier "slow menu" was a loading screen stuck forever: FMOD's sound-bank loader waited on the
XMA hardware decoder, which did not exist. With XMA emulated (silent output) loading completes.

Subsystems:
- `runtime/video/presenter.cpp` - window and presentation: the process main thread runs SDL and
  Vulkan, the guest's main thread runs on its own host thread (16 MB stack); at each swap the
  command processor only publishes the front-buffer address/size (lock-free) and the presenter
  copies it on its own schedule, so the game never waits on the window. Currently a raw copy:
  the picture is black because nothing renders into the front buffer yet
- `runtime/gpu/` - Vd* imports, PM4 command processor, register file at 0x7FC80000, 60 Hz vblank
  interrupt; MMIO stores reach it through `runtime/ppc_hooks.h` (force-included into ppc/)
- `runtime/apu/` - XAudio render driver clock (256 samples @ 48 kHz), frames discarded
- `runtime/apu/xma.cpp` - XMA decoder: 320-context array (register 0x600), kick/lock/clear
  registers (0x650/0x690/0x6A0), decoder thread that walks packets frame by frame and writes
  SILENCE of the right length (real XMA2 decoding needs a WMA Pro capable decoder)
- `runtime/hid/` - one neutral pad on user 0, no keystrokes
- `runtime/kernel/xam.cpp` - notifications, local profile "Player", profile settings (empty values)
- `runtime/kernel/scopes.cpp` - the game's compiled-out profiler markers carry function names;
  crash and timeout reports list each thread's recent scopes
- `runtime/debug/guest_trace.cpp` - GUEST_TRACE(addr) wraps a guest function to log args/result

## Scripted input
`--press "start@12,a@20,down@24.5"` presses each button that many seconds after launch (held
150 ms), reported as pad state and as keystrokes. Buttons: a b x y start back up down left right
lb rb ls rs. Example that gets past the title screen: `--press "a@20,a@26"`.

## GPU frame capture
`./build/backbreaker --capture-frame 1200` (or a list: `--capture-frame 300,1200,2400`) records the
frame ending with that swap into `captures/frame_N/` (gitignored):
- `summary.txt` - draw count, primitive types, shaders used, render-target setups, textures
  (format, dimension, size, tiling), distinct pipeline states (fixed-function state per draw,
  with draw counts), PM4 packet histogram
- `draws.txt` - every draw: packet, current vertex/pixel shader, the decoded draw state
  (`runtime/gpu/state.cpp`: primitive and indices, EDRAM mode, render targets with formats, tile
  bases, write masks and blending, depth/stencil, rasterizer, alpha test, viewport, scissor,
  shader setup), then the raw registers changed since the previous draw (named from Xenia's
  register table) and the bound texture fetch constants; shader loads are listed in order
- `registers_start.txt` - non-zero GPU registers at the start of the frame
- `shaders/vs_*.bin`, `shaders/ps_*.bin` - shader microcode used in the frame (big-endian dwords)

`--dump-shaders` writes every unique shader the game loads during a run to `captures/shaders/`.

## Shader disassembler
`./build/xenos-disasm captures/shaders` (files or directories, searched recursively) prints a
listing of each shader - control flow, vertex/texture fetches, paired vector+scalar ALU
instructions, exports - followed by a coverage summary (which opcodes, export targets, vertex
formats, texture dimensions and features appear, and how often). `--quiet` prints only the
summary. The decoder (`runtime/gpu/ucode.h`) follows Xenia's `ucode.h`/`shader_translator.cc`
(BSD) and will be shared with the shader translator.

## Shader translator
`runtime/gpu/shader_translator.cpp` translates Xenos microcode to GLSL 4.50 for Vulkan (resource
layout documented in `shader_translator.h`: guest memory as a storage buffer for vertex fetch,
float/bool/loop/fetch constants as uniform buffers, one texture binding per fetch slot, 16 vec4
interpolators). Check it over a shader dump:

    ./build/xenos-translate /tmp/glsl captures/shaders
    for f in /tmp/glsl/*; do glslc --target-env=vulkan1.3 -o "$f.spv" "$f" && spirv-val "$f.spv"; done

Unsupported features (loops, calls, jumps, vector predicate-push ops, LOD/gradient fetch ops,
memory export, texture fetch offsets) make translation fail with a reason instead of producing
wrong code.

Register field layouts come from `runtime/gpu/generated/registers.h`, generated by
`tools/gen_gpu_registers.py` from Xenia's `registers.h`, `xenos.h` and `register_table.inc` (it
checks that every layout covers 32 bits and matches Xenia's bit-offset comments).
Register names come from `runtime/gpu/generated/register_names.cpp`, generated by
`tools/gen_register_names.py` from Xenia's `register_table.inc` (BSD, `docs/licenses/xenia-LICENSE.txt`).

## Debugging aids
- `tools/fninfo.py default.pe ppc ADDR...` - string references, imports and callees of guest functions
- Sampling a busy thread: `eu-stack -1 -p <tid>` (recompiled frames are named sub_XXXXXXXX)
- Crash report: host backtrace, guest registers, recent profiler scopes and import calls of the
  crashing thread. Indirect calls to addresses without a recompiled function are reported with
  the target and caller.
- `--timeout N`: after N seconds print every thread's recent scopes and calls.

## Known shortcuts
- NtSuspendThread only works before a thread starts; decommits are ignored (memory stays backed)
- Physical memory is not aliased across 0xA0/0xC0/0xE0 ranges yet
- KeTimeStampBundle is not updated; timer APC routines are not delivered
- XAM content (saves), networking, movies (XMV) and real audio output are stubs; XMA output is silence
- Profile settings are returned empty; WAIT_REG_MEM spins with yield
- Renderer: resolve destination images are never freed (one per guest address; ~375 during the
  first load); EDRAM aliasing only transfers between targets of the same format; resolves are not written back to guest
  memory and take their rectangle from the window scissor; 1D/3D/cube and some texture formats are
  placeholders; textures are re-uploaded once per frame they are used (no
  write-watching); only the base mip level; depth is 32-bit float instead of 24-bit
