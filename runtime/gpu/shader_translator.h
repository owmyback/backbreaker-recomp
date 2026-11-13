#pragma once
// Xenos shader microcode -> GLSL 4.50 (Vulkan), compiled to SPIR-V by shaderc/glslc.
// Roadmap phase 4, "Shader translation". Semantics follow Xenia's shader translators
// (BSD 3-clause, see docs/licenses/xenia-LICENSE.txt).
//
// Resource layout used by translated shaders:
//   set 0, binding 0: storage buffer  `uint mem[]`      - all of guest physical memory (vertex fetch)
//   set 0, binding 1: uniform buffer  vec4 c[256]      - vertex shader float constants
//   set 0, binding 2: uniform buffer  vec4 c[256]      - pixel shader float constants
//   set 0, binding 3: uniform buffer  uvec4 boolc[2], loopc[8], fetchc[48]
//                                     - bool constants (256 bits), loop constants (32), fetch constants (32 x 6 dwords)
//   set 1, binding N: texture for fetch constant slot N (sampler1D/2D/3D/Cube by fetch dimension)
//   push constant:    int vertexIndexOffset (VGT_INDX_OFFSET)
// Vertex shader outputs and pixel shader inputs are 16 vec4 interpolators at locations 0-15.
#include "disasm.h"

#include <cstdint>
#include <string>
#include <vector>

namespace gpu
{
    struct TranslatedShader
    {
        bool ok = false;
        std::string glsl;
        std::string error;           // why translation failed (unsupported feature), if !ok
        uint32_t textureSlots = 0;   // bit N set: uses texture fetch constant slot N
        uint32_t textureDims[32]{};  // per used slot: 0 1D, 1 2D, 2 3D, 3 cube
        bool writesDepth = false;
        uint32_t colorOutputs = 0;   // bit N: writes oCN
    };

    // words: host-order dwords of the shader microcode.
    TranslatedShader translateToGlsl(const std::vector<uint32_t>& words, ShaderType type);
}
