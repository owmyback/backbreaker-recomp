// The game's executable (XEX2) for the installer (#138), the C++ form of three tools:
//   decryptToPe()      tools/xex_decrypt.py: the retail-encrypted, basic-compressed XEX to its PE image
//   generateImports()  tools/gen_imports.py: runtime/generated/imports.cpp, the same text byte for byte
//   titleIcon()        tools/title_icon.py: the title's own PNG from its XDBF (SPA) resource
// plus executionInfo(), which holds the title ID the installer checks. Layouts: Xenia's
// src/xenia/kernel/util/xex2_info.h (BSD, docs/licenses/xenia-LICENSE.txt). Every offset read
// from the file is bounds-checked: a damaged executable gives a message, not a crash.
#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace installer
{
    struct ExecutionInfo // xex2_opt_execution_info (optional header 0x00040006)
    {
        uint32_t mediaId = 0;
        uint32_t version = 0;
        uint32_t baseVersion = 0;
        uint32_t titleId = 0;
        uint8_t discNumber = 0;
        uint8_t discCount = 0;
    };

    struct ExportName
    {
        std::string name;
        bool variable = false;
    };
    using ExportTable = std::map<uint32_t, ExportName>; // by ordinal

    // The kernel's and XAM's export tables, from XenonRecomp's XenonUtils/xbox/*_table.inc (the
    // files tools/gen_imports.py reads), by module file name: "xboxkrnl.exe", "xam.xex"
    const std::map<std::string, ExportTable>& kernelExports();

    class Xex
    {
    public:
        bool parse(std::vector<uint8_t> data, std::string& error);
        const std::vector<uint8_t>& bytes() const { return data_; }

        std::optional<uint32_t> header(uint32_t key) const; // an optional header's value or offset
        bool executionInfo(ExecutionInfo& info) const;
        uint32_t imageBase() const; // 0x00010201, else 0x82000000

        bool decryptToPe(std::vector<uint8_t>& pe, std::string& error) const;
        // stats: "function imports: N, variable slots: N, function-pointer slots: N" and the like
        bool generateImports(const std::vector<uint8_t>& pe, const std::map<std::string, ExportTable>& exports,
                             std::string& cpp, std::vector<std::string>& stats, std::string& error) const;

    private:
        std::vector<uint8_t> data_;
        std::map<uint32_t, uint32_t> headers_;
    };

    // The title image (XDBF section 2, id 0x8000) as PNG bytes, empty when there is none
    std::vector<uint8_t> titleIcon(const std::vector<uint8_t>& pe);
}
