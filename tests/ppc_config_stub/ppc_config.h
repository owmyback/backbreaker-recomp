// Stub stand-in for the ppc_config.h that XenonRecomp generates from the game's
// executable. Lets tests/instr_semantics.cpp build without any game files: the
// tests only need the macros in ppc_context.h, and the real image addresses only
// matter to the recompiled game code, not to these tests.
// Build with -I tests/ppc_config_stub instead of -I ppc (see docs/testing.md).
#pragma once

#ifndef PPC_CONFIG_H_INCLUDED
#define PPC_CONFIG_H_INCLUDED

#define PPC_IMAGE_BASE 0x82000000ull
#define PPC_IMAGE_SIZE 0x2000000ull
#define PPC_CODE_BASE  0x821E0000ull
#define PPC_CODE_SIZE  0x1000000ull

#endif
