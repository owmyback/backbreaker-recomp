// Second translation unit that includes runtime/gpu/tiling.h. Its only job is to make a
// non-inline definition in the header fail to link: the renderer and the presenter will both
// include the header, so these must be inline/constexpr. Build both units together, see the CI
// "Build tiling test" step.
#include "../runtime/gpu/tiling.h"

int odrCheck()
{
    // Same value the main test computes for block (1,1) of a 32-block pitch at 1 byte per block.
    return int(gpu::tiledOffset2D(1, 1, 32, 1)) + int(gpu::tiledUpperBound2D(32, 32, 32, 1)) +
           int(gpu::tiledPitchAligned(1280)) + int(gpu::tiledBlocksAligned(33));
}
