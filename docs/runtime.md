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
  the grass is black. The command processor waits for these writes before anything the CPU can
  see (fences, interrupts, the read pointer). Formats written: 8_8_8_8, 2_10_10_10 and D24S8
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
  3 = 3840x2160. The game still sees 1280x720; render targets, viewports, scissors, resolves and
  clears are scaled. (The attract movie itself is a 1280x720 video, so it only gets smoother
  scaling; 3D scenes are where the extra resolution shows.)
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
  and fullscreen state are read from `game/settings.ini` (issue #9)
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
- `runtime/apu/` - XAudio render driver clock (256 samples @ 48 kHz), frames discarded
- `runtime/apu/xma.cpp` - XMA decoder: 320-context array (register 0x600), kick/lock/clear
  registers (0x650/0x690/0x6A0), decoder thread that walks packets frame by frame and writes
  SILENCE of the right length (real XMA2 decoding needs a WMA Pro capable decoder)
- `runtime/hid/` - one neutral pad on user 0, no keystrokes
- `runtime/kernel/xam.cpp` - notifications, local profile "Player", profile settings (empty values)
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

## Renderer performance line
`BB_PERF=1` adds one `[perf]` line per second (issue #73) that says where the renderer's time went:
swaps and draws, the CPU time of the command processor thread (which also runs the renderer) and
how much of it was spent polling in `WAIT_REG_MEM`, i.e. waiting for the game rather than working,
GPU waits and flushes, texture bytes hashed and the time it took, texture uploads, resolves and
writebacks, memory export draws (and how many waited for the GPU), syncs that had pending writes,
GPU vertex copies and draws recorded again after a sync. Pair it with `tools/fps_report.py` on the
same log for the fps side.

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
writebacks: the command processor syncs them before anything the CPU can see (fences, interrupts,
the read pointer). A later draw that fetches vertices from a pending range gets them copied GPU to
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
- Networking carries no data (socket objects exist, nothing is ever connected); movies (XMV) and
  real audio output are stubs; XMA output is silence
- Profile settings are returned empty; WAIT_REG_MEM spins with yield
- Renderer: resolve destination images are never freed (one per guest address; ~375 during the
  first load); resolves take their rectangle from the window scissor; resolves to formats other
  than 8_8_8_8, 2_10_10_10 and D24S8 depth are not written back to guest memory (depth stencil is
  written as 0), nor are destinations resolved every frame; a resolve's own format conversion is
  skipped on the GPU side (textures sample the resolve image in the render target's format); 1D/3D
  textures and some formats are placeholders; signed texture formats are read unsigned; textures
  are hashed once per frame they are used and re-uploaded only when their bytes changed (no
  write-watching); depth is 32-bit float instead of 24-bit
