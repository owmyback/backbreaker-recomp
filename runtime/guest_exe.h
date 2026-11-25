// Which build of the game's executable the runtime is built for: the disc's, or the disc's with
// Title Update #2 applied (CMake's BB_EXECUTABLE=tu2, which defines BB_TU2). The update is a relink,
// so nearly every function and global is at another address, and some structures grew.
//
// CMake includes this header first in every runtime source. The hooks keep the names and comments
// they were written with, which are the disc executable's:
//
// - A guest function's name, sub_XXXXXXXX, is its address on the disc. For the update,
//   sdk/tu2/guest_names.h (made by tools/match_functions.py) defines each name the runtime uses as
//   the function at its new address, so PPC_FUNC(sub_8241A410) replaces the same function in both.
// - Anything else that differs (a global's address, a return address, an offset in a structure that
//   grew) is written BB_EXE(disc, tu2) where it is used.
#pragma once

#if BB_TU2
#define BB_EXE(disc, tu2) (tu2)
#include "sdk/tu2/guest_names.h"
#else
#define BB_EXE(disc, tu2) (disc)
#endif
