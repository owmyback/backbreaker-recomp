#pragma once
// Xenos shader disassembler and instruction-coverage statistics (roadmap phase 4, step 2).
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace gpu
{
    enum class ShaderType { Vertex = 0, Pixel = 1 };

    // Counts of what a set of shaders uses: drives what the shader translator must support.
    struct ShaderCoverage
    {
        uint64_t shaders = 0, instructions = 0;
        std::map<std::string, uint64_t> controlFlow, fetch, vector, scalar, exports, features;
        std::map<std::string, uint64_t> vertexFormats, textureDimensions;
        void add(std::map<std::string, uint64_t>& m, const std::string& k) { m[k]++; }
    };

    // words: host-order dwords of the shader microcode.
    std::string disassemble(const std::vector<uint32_t>& words, ShaderType type, ShaderCoverage* coverage);
}
