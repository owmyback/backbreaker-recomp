// The executable's layout, as XenonRecomp writes it to ppc/ppc_config.h for Backbreaker's
// default.xex: the runtime is compiled against this copy, so it builds without game files and can
// be shipped prebuilt (the runtime kit, #138). The four values are addresses and sizes of the
// image and its code section - the same kind of fact as the function addresses runtime/*.cpp
// already hook - not game content.
//
// CMakeLists.txt compares these values with ppc/ppc_config.h whenever ppc/ exists, and
// packaging/installer/link_game.sh does the same before linking: a runtime built for another
// layout would index the function table wrong (PPC_LOOKUP_FUNC), so a mismatch stops the build.
#pragma once
#ifndef PPC_CONFIG_H_INCLUDED
#define PPC_CONFIG_H_INCLUDED

#define PPC_IMAGE_BASE 0x82000000ull
#define PPC_IMAGE_SIZE 0xE20000ull
#define PPC_CODE_BASE 0x82150000ull
#define PPC_CODE_SIZE 0x863EC8ull

#endif
