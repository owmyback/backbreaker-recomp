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

## Renderer
`runtime/gpu/renderer.cpp` executes the game's draws with Vulkan (one device shared with the
window, `runtime/video/vk_context.cpp`): shaders are translated (see below) and compiled with
shaderc on first use, EDRAM render targets are Vulkan images, resolves copy into images keyed by
their guest address, and the presenter shows the image resolved to the front buffer.
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
  after that time; the log line names the frame and the exact time. Neither form makes two runs
  of the Exhibition route show the same scenes everywhere: the **coin toss is random** (from about
  125 s, one run kicks off and the other receives), loading screens pick random art, and a slower
  build (e.g. `BB_VK_VALIDATION=1`) boots and loads slower, so it reaches each menu later in both
  seconds and frames. Before the coin toss, runs of the same build match with either form; avoid
  times right at a scripted press (the frame before and after it differ).
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
- `--anisotropy N` (2-16) gives every texture the game minifies with linear filtering and mips at
  least N:1 anisotropic filtering (16 is the usual choice). The game asks for it on only a few
  textures, so the field blurs toward the horizon without it. UI textures (base level only) are
  left alone. Needs the GPU's `samplerAnisotropy`, and the log says if it's missing.
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

    [rt] [gpu] 30 fps, 900 draws/s, 30 frames total, front buffer 00840000 1280x720, 390 xma frames, mem 43+62 MB

`fps` and `draws/s` are that second's counts, `frames total` is what the guest has finished since
the run started (so comparing it across two runs of the same timed route is the frame-rate ratio),
the front buffer is the guest's own surface address and size, and the two numbers after `mem` are
guest virtual and physical bytes in use, in MB. `[rt] ` is the prefix on every log line (`info()`
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
- `runtime/generated/imports.cpp` - weak logging stub for every import (from tools/gen_imports.py)

## Status (2026-11-12)
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

## Controllers
Controllers are read through SDL3 (users 0-3 = the first four controllers SDL lists; a
DualSense's Cross/Circle/Square/Triangle are A/B/X/Y, Create/Options are Back/Start). The log
shows `[hid] controller on user 0: ...` when one is found. On KDE Plasma 6.7+, turn off System
Settings -> Game Controller -> pointer navigation, or the stick also moves the mouse.

## Scripted input
`--press "start@12,a@20,down@24.5"` presses each button that many seconds after launch (held
150 ms), reported as pad state and as keystrokes, added to any real controller on user 0.
Buttons: a b x y start back up down left right lb rb ls rs. Example that gets past the title
screen to the main menu: `--press "a@20,a@26"`. Sticks: `ls_` or `rs_` plus `left`, `right`, `up`
or `down`, as `stick@seconds[:duration]` - full deflection of that axis for the duration (150 ms
by default), then released; reported as pad state only (no stick keystrokes). The kickoff of the
Exhibition route: `rs_down@196:1.0,rs_up@197:0.3` pulls the right stick back and pushes it forward.

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
GPU vertex copies and draws recorded again after a sync, and the write watch: texture checks
answered without reading, write faults and pages watched. Pair it with `tools/fps_report.py` on the
same log for the fps side.

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
instead, so it does not fault on every write. `tests/memory_watch_test.cpp` checks all of this on
the real memory layout, including the race between a fault and the renderer re-protecting a page.

Resolve images use the same watch (#63, #114). A texture at a resolve destination samples the
resolve image (render to texture), but the game can load something else into that memory later:
in a match, the pause menu's Controls screen loads its controller picture where the team-coloured
jersey font was rendered. After each resolve, or once its writeback has landed (landing counts as a
write), the pages of an image used as a texture are protected, all but a last one that another
allocation may share. A lookup that finds one of them written since drops the image, and the
texture reads the guest's memory. An image is watched from its first use as a texture, and after
that from each resolve into it. Until its first use, #65's sampled fingerprint covers it (256 bytes
at the start of each of the first 16 pages). That fingerprint was the whole check before: the new
picture could match it by chance, since both images are mostly empty, so the font sheet stayed in
some sessions. Only images used as textures are watched. And like a texture that keeps changing, an
image whose memory the game writes between one resolve and the next, four times in a row, goes
back to the fingerprint for 300 frames, then 600, and so on. In the menus, two 1024x1024 targets are
resolved every frame over memory the CPU rewrites every frame, which otherwise faults 61,000 times
a second.

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
and wait for each one was most of the renderer's GPU waiting). Its ranges are pending like resolve
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
  Fixtures it is checked against: `tests/data/fps_report_sample*.log`.
- Sampling a busy thread: `eu-stack -1 -p <tid>` (recompiled frames are named sub_XXXXXXXX)
- Crash report: host backtrace, guest registers, recent profiler scopes and import calls of the
  crashing thread. Indirect calls to addresses without a recompiled function are reported with
  the target and caller.
- `--timeout N`: after N seconds print every thread's recent scopes and calls, then exit with 124.
  `kill <pid>` (SIGTERM) and Ctrl+C (SIGINT) end a run the same way, headless or windowed, with
  `==== SIGTERM received ====` (or `SIGINT`) and exit code 143 (130). A second signal while the
  report prints ends the process at once.

## Known shortcuts
- Kernel objects (KTHREAD, PCR, TLS slots, events) are never freed; only a thread's stack is
- NtSuspendThread only works before a thread starts; decommits are ignored (memory stays backed)
- Timer APC routines are not delivered
- Networking carries no data (socket objects exist, nothing is ever connected); movies (XMV) are
  stubs; XMA plays as silence in a build without the decoder (`scripts/build_xma_decoder.sh`); the
  XMA "interrupt when done" and "stop when done" bits are ignored
- WAIT_REG_MEM spins with yield
- Renderer: resolve destination images are never freed (one per guest address; ~375 during the
  first load); resolves take their rectangle from the window scissor; resolves to formats other
  than 8_8_8_8, 2_10_10_10 and D24S8 depth are not written back to guest memory (depth stencil is
  written as 0), nor are destinations resolved every frame; a resolve's own format conversion is
  skipped on the GPU side (textures sample the resolve image in the render target's format); 1D/3D
  textures and some formats are placeholders; signed texture formats are read unsigned; textures
  are re-uploaded only when their bytes changed, which is known from the write watch below or,
  for textures whose bytes change all the time, from hashing them once per frame; depth is 32-bit float instead of 24-bit
