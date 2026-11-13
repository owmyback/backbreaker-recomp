// xenos-disasm: disassemble captured Xenos shader microcode and summarize instruction coverage.
//
// Usage: xenos-disasm [--quiet] FILE_OR_DIR...
//   Files are the shader dumps written by the frame capture (vs_*.bin / ps_*.bin, big-endian
//   dwords); directories are searched recursively. Duplicate shaders are counted once.
//   --quiet prints only the coverage summary.
#include "../runtime/gpu/disasm.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static void printTable(const char* title, const std::map<std::string, uint64_t>& m)
{
    if (m.empty()) return;
    std::printf("\n%s:\n", title);
    std::vector<std::pair<uint64_t, std::string>> rows;
    for (auto& [k, v] : m) rows.push_back({ v, k });
    std::sort(rows.rbegin(), rows.rend());
    for (auto& [v, k] : rows) std::printf("  %-36s %llu\n", k.c_str(), (unsigned long long)v);
}

int main(int argc, char** argv)
{
    bool quiet = false;
    std::vector<fs::path> files;
    for (int i = 1; i < argc; i++)
    {
        if (!std::strcmp(argv[i], "--quiet")) { quiet = true; continue; }
        fs::path p = argv[i];
        if (fs::is_directory(p))
        {
            for (auto& e : fs::recursive_directory_iterator(p))
                if (e.is_regular_file() && e.path().extension() == ".bin") files.push_back(e.path());
        }
        else
            files.push_back(p);
    }
    if (files.empty())
    {
        std::fprintf(stderr, "usage: xenos-disasm [--quiet] FILE_OR_DIR...\n");
        return 2;
    }
    std::sort(files.begin(), files.end());

    gpu::ShaderCoverage cov;
    std::set<std::string> seen; // by file name: the capture names shaders by content hash
    for (const fs::path& p : files)
    {
        std::string name = p.filename().string();
        if (!seen.insert(name).second) continue;
        std::ifstream f(p, std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::vector<uint32_t> words(bytes.size() / 4);
        for (size_t i = 0; i < words.size(); i++)
        {
            uint32_t be;
            std::memcpy(&be, &bytes[i * 4], 4);
            words[i] = __builtin_bswap32(be);
        }
        gpu::ShaderType type = name.rfind("ps_", 0) == 0 ? gpu::ShaderType::Pixel : gpu::ShaderType::Vertex;
        std::string text = gpu::disassemble(words, type, &cov);
        if (!quiet)
            std::printf("== %s (%s, %zu dwords)\n%s\n", name.c_str(), type == gpu::ShaderType::Pixel ? "pixel" : "vertex",
                        words.size(), text.c_str());
    }

    std::printf("==== coverage: %llu unique shaders, %llu instructions ====\n", (unsigned long long)cov.shaders,
                (unsigned long long)cov.instructions);
    printTable("control flow", cov.controlFlow);
    printTable("fetch", cov.fetch);
    printTable("vector ALU", cov.vector);
    printTable("scalar ALU", cov.scalar);
    printTable("exports", cov.exports);
    printTable("vertex formats", cov.vertexFormats);
    printTable("texture dimensions", cov.textureDimensions);
    printTable("features", cov.features);
    return 0;
}
