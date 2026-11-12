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
Boots through CRT init, starts the file-system thread and 4 pool threads, opens
PackedOutputX360.pkh/.pkd, then calls VdInitializeEngines (GPU) - unimplemented, so the
game tears video down and crashes on a null pointer. Next: GPU (Vd*) interface.

## Known shortcuts
- Allocators never free; NtSuspendThread only works before a thread starts
- Physical memory is not aliased across 0xA0/0xC0/0xE0 ranges yet
- KeTimeStampBundle is not updated; timers (NtCreateTimer/NtSetTimerEx) are stubs
- XAM (users, profiles, notifications, content), networking and audio are stubs
