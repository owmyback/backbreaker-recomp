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
//                                     Dword 0 of each vertex fetch slot the shader reads holds the dword index of
//                                     that vertex data in `mem` (rewritten by the renderer; not the guest value)
//   set 0, binding 4: storage buffer  `uint gmem[]`     - guest physical memory, writable (memory export only)
//   set 1, binding N: texture for fetch constant slot N (sampler1D/2D/3D/Cube by fetch dimension)
//   push constant:    system constants (struct SystemConstants below)
// Vertex shader outputs and pixel shader inputs are the block XeInterpolators { vec4 v[16]; }
// at location 0 (16 locations).
#include "disasm.h"

#include <cstdint>
#include <string>
#include <vector>

namespace gpu
{
    // Where a vertex shader's position comes from when all it does with it is move a fetched
    // vertex into oPos (screen-space rectangles such as the engine's clears): the renderer reads
    // those positions on the CPU to bound the EDRAM a draw touches.
    struct PositionFetch
    {
        bool valid = false;
        uint32_t slot = 0, stride = 0, format = 0; // vertex fetch constant slot, dwords per vertex, xenos::VertexFormat
        int32_t offset = 0;                        // dwords into the vertex
        uint8_t comp[4] = { 6, 6, 6, 6 };          // oPos.xyzw: fetched component 0-3, 4 = 0.0, 5 = 1.0, 6 = unknown
    };

    struct TranslatedShader
    {
        bool ok = false;
        std::string glsl;
        std::string error;           // why translation failed (unsupported feature), if !ok
        uint32_t textureSlots = 0;   // bit N set: uses texture fetch constant slot N
        uint32_t textureDims[32]{};  // per used slot: 0 1D, 1 2D, 2 3D, 3 cube
        bool writesDepth = false;
        bool kills = false;          // pixel shaders: may discard (kill instructions)
        // Memory export (eA / eM0-4): the shader writes guest memory through set 0 binding 4.
        // memExportStreams: float constants used as stream constants (`mad eA, r#, const0100, c#`)
        bool memExport = false;
        uint32_t memExportStreams[8]{}; // bit N of [N / 32]: c<N>
        uint32_t colorOutputs = 0;   // bit N: writes oCN
        uint32_t vertexFetchSlots[3]{}; // bit N of [N / 32]: reads vertex fetch slot N (fetch constant dwords 2N, 2N+1)
        // Per vertex fetch slot: stride and the dwords an element reaches (max offset + size), so the
        // renderer can copy only the vertices a draw uses
        uint8_t vertexStride[96]{}, vertexExtent[96]{};
        // Every vertex fetch is indexed by the untouched vertex index (r0.x): the fetched range
        // follows from the draw's index range
        bool vertexIndexDirect = true;
        PositionFetch position; // vertex shaders: see PositionFetch
    };

    // Push constants of translated shaders (std430 layout, 48 bytes)
    struct SystemConstants
    {
        float ndcScale[4];          // host position.xyz = guest.xyz * ndcScale + w * ndcOffset
        float ndcOffset[4];
        int32_t vertexIndexOffset;  // VGT_INDX_OFFSET
        uint32_t flags;             // kSysFlag*
        uint32_t alphaFunc;         // VkCompareOp order; 7 (always) disables the alpha test
        float alphaRef;
    };
    constexpr uint32_t kSysFlagXYDividedByW = 1, kSysFlagZDividedByW = 2, kSysFlagWNotReciprocal = 4;

    // words: host-order dwords of the shader microcode (a vertex shader).
    PositionFetch analyzePositionFetch(const std::vector<uint32_t>& words);

    // words: host-order dwords of the shader microcode.
    TranslatedShader translateToGlsl(const std::vector<uint32_t>& words, ShaderType type);
}
