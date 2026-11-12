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
Phase 1 (headless GPU) works: the game boots, initializes graphics, audio, voice (XHV2), comms
and input, plays the 505 and NaturalMotion logo movies at ~30 fps (~1500 draws/s), loads the
front-end sound banks, then runs a main loop at 4-11 fps with ~59 draws per frame (likely the
front end; the low rate is not investigated yet). Nothing is drawn: the command processor
executes PM4 packets only for their side effects (registers, fences, waits, interrupts).

Subsystems:
- `runtime/gpu/` - Vd* imports, PM4 command processor, register file at 0x7FC80000, 60 Hz vblank
  interrupt; MMIO stores reach it through `runtime/ppc_hooks.h` (force-included into ppc/)
- `runtime/apu/` - XAudio render driver clock (256 samples @ 48 kHz), frames discarded; XMA contexts
- `runtime/hid/` - one neutral pad on user 0, no keystrokes
- `runtime/kernel/xam.cpp` - notifications, local profile "Player", profile settings (empty values)
- `runtime/kernel/scopes.cpp` - the game's compiled-out profiler markers carry function names;
  crash and timeout reports list each thread's recent scopes
- `runtime/debug/guest_trace.cpp` - GUEST_TRACE(addr) wraps a guest function to log args/result

## Debugging aids
- Crash report: host backtrace, guest registers, recent profiler scopes and import calls of the
  crashing thread. Indirect calls to addresses without a recompiled function are reported with
  the target and caller.
- `--timeout N`: after N seconds print every thread's recent scopes and calls.

## Known shortcuts
- Allocators never free; NtSuspendThread only works before a thread starts
- Physical memory is not aliased across 0xA0/0xC0/0xE0 ranges yet
- KeTimeStampBundle is not updated; timer APC routines are not delivered
- XAM content (saves), networking, movies (XMV) and real audio output are stubs
- Profile settings are returned empty; WAIT_REG_MEM spins with yield
