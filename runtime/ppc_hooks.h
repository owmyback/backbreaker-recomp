#pragma once
// Force-included into every recompiled file (see CMakeLists.txt) before ppc_context.h.
// XenonRecomp emits PPC_MM_STORE_* for stores followed by `eieio`, i.e. MMIO writes.
#include <cstdint>

void GuestMmioStore32(uint8_t* base, uint32_t address, uint32_t value);

#define PPC_MM_STORE_U32(x, y) GuestMmioStore32(base, uint32_t(x), uint32_t(y))
