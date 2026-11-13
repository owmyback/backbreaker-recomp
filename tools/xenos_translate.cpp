// xenos-translate: translate captured Xenos shaders to GLSL (for checking the shader translator).
//
// Usage: xenos-translate OUT_DIR FILE_OR_DIR...
//   Writes OUT_DIR/<name>.vert / .frag for each shader that translates, and prints a line per
//   shader that does not (with the reason). Compile the output with glslc to check it.
#include "../runtime/gpu/shader_translator.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

int main(int argc, char** argv)
{
    if (argc < 3) { std::fprintf(stderr, "usage: xenos-translate OUT_DIR FILE_OR_DIR...\n"); return 2; }
    fs::path outDir = argv[1];
    fs::create_directories(outDir);
    std::vector<fs::path> files;
    for (int i = 2; i < argc; i++)
    {
        fs::path p = argv[i];
        if (fs::is_directory(p))
            for (auto& e : fs::recursive_directory_iterator(p))
                if (e.is_regular_file() && e.path().extension() == ".bin") files.push_back(e.path());
        if (fs::is_regular_file(p)) files.push_back(p);
    }
    std::sort(files.begin(), files.end());
    int ok = 0, failed = 0;
    for (const fs::path& p : files)
    {
        std::ifstream f(p, std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::vector<uint32_t> words(bytes.size() / 4);
        for (size_t i = 0; i < words.size(); i++) { uint32_t be; std::memcpy(&be, &bytes[i * 4], 4); words[i] = __builtin_bswap32(be); }
        std::string name = p.stem().string();
        bool pixel = name.rfind("ps_", 0) == 0;
        gpu::TranslatedShader t = gpu::translateToGlsl(words, pixel ? gpu::ShaderType::Pixel : gpu::ShaderType::Vertex);
        if (!t.ok) { std::printf("FAIL %s: %s\n", name.c_str(), t.error.c_str()); failed++; continue; }
        std::ofstream(outDir / (name + (pixel ? ".frag" : ".vert"))) << t.glsl;
        ok++;
    }
    std::printf("%d translated, %d failed\n", ok, failed);
    return failed ? 1 : 0;
}
