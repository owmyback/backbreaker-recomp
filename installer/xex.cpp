#include "xex.h"

#include "crypto.h"

#include <cstdio>
#include <cstring>

namespace installer
{
    namespace
    {
        // The retail XEX key (also in tools/xex_decrypt.py and Xenia's xex_module.cc): it decrypts
        // the per-title key stored in the executable's security info
        constexpr uint8_t kRetailKey[16] = {0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3,
                                            0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF, 0x91};

        constexpr uint32_t kFileFormatInfo = 0x000003FF;
        constexpr uint32_t kImportLibraries = 0x000103FF;
        constexpr uint32_t kImageBase = 0x00010201;
        constexpr uint32_t kExecutionInfo = 0x00040006;

        bool be32(const std::vector<uint8_t>& d, size_t at, uint32_t& v)
        {
            if (at > d.size() || d.size() - at < 4) return false;
            v = uint32_t(d[at]) << 24 | uint32_t(d[at + 1]) << 16 | uint32_t(d[at + 2]) << 8 | d[at + 3];
            return true;
        }
        bool be16(const std::vector<uint8_t>& d, size_t at, uint16_t& v)
        {
            if (at > d.size() || d.size() - at < 2) return false;
            v = uint16_t(d[at] << 8 | d[at + 1]);
            return true;
        }
    }

    const std::map<std::string, ExportTable>& kernelExports()
    {
        // XE_EXPORT(module, ordinal, name, kind) lines, Xenia's export tables as XenonRecomp carries them
        // (BSD, docs/licenses/xenia-LICENSE.txt); a later duplicate ordinal replaces an earlier one, as
        // in tools/gen_imports.py's dict
        static const std::map<std::string, ExportTable> tables = [] {
            struct Row { uint32_t ordinal; const char* name; bool variable; };
            enum { kFunction, kVariable };
#define XE_EXPORT(MODULE, ORDINAL, NAME, KIND) {ORDINAL, #NAME, (KIND) == kVariable} // the .inc adds the comma
            static const Row krnl[] = {
#include "xbox/xboxkrnl_table.inc"
            };
            static const Row xam[] = {
#include "xbox/xam_table.inc"
            };
#undef XE_EXPORT
            std::map<std::string, ExportTable> t;
            for (const Row& r : krnl) t["xboxkrnl.exe"][r.ordinal] = {r.name, r.variable};
            for (const Row& r : xam) t["xam.xex"][r.ordinal] = {r.name, r.variable};
            return t;
        }();
        return tables;
    }

    bool Xex::parse(std::vector<uint8_t> data, std::string& error)
    {
        data_ = std::move(data);
        headers_.clear();
        uint32_t count = 0;
        if (data_.size() < 24 || std::memcmp(data_.data(), "XEX2", 4) != 0 || !be32(data_, 20, count)) {
            error = "default.xex is not an Xbox 360 executable (no XEX2 header): the image is damaged";
            return false;
        }
        if (count > 256 || 24 + size_t(count) * 8 > data_.size()) {
            error = "default.xex's header is damaged";
            return false;
        }
        for (uint32_t i = 0; i < count; i++) {
            uint32_t key = 0, value = 0;
            be32(data_, 24 + 8 * i, key);
            be32(data_, 28 + 8 * i, value);
            headers_[key] = value;
        }
        return true;
    }

    std::optional<uint32_t> Xex::header(uint32_t key) const
    {
        auto it = headers_.find(key);
        if (it == headers_.end()) return std::nullopt;
        return it->second;
    }

    bool Xex::executionInfo(ExecutionInfo& info) const
    {
        const auto at = header(kExecutionInfo);
        if (!at || *at > data_.size() || data_.size() - *at < 24) return false;
        be32(data_, *at, info.mediaId);
        be32(data_, *at + 4, info.version);
        be32(data_, *at + 8, info.baseVersion);
        be32(data_, *at + 12, info.titleId);
        info.discNumber = data_[*at + 18];
        info.discCount = data_[*at + 19];
        return true;
    }

    uint32_t Xex::imageBase() const
    {
        return header(kImageBase).value_or(0x82000000);
    }

    bool Xex::decryptToPe(std::vector<uint8_t>& pe, std::string& error) const
    {
        // tools/xex_decrypt.py: the file format info says how the payload is stored; this handles
        // what retail discs use, normal (AES) encryption or none, with basic compression - runs of
        // data followed by runs of zeros
        uint32_t peOffset = 0, securityOffset = 0;
        be32(data_, 8, peOffset);
        be32(data_, 16, securityOffset);
        const auto ff = header(kFileFormatInfo);
        uint32_t infoSize = 0;
        uint16_t encryption = 0, compression = 0;
        if (!ff || !be32(data_, *ff, infoSize) || !be16(data_, *ff + 4, encryption) || !be16(data_, *ff + 6, compression)) {
            error = "default.xex has no readable file format header: the image is damaged";
            return false;
        }
        if (encryption == 0 && compression == 0) {
            // What XenonRecomp writes after applying a title update's patch: the image as it is,
            // after the headers
            if (peOffset + 2 > data_.size() || data_[peOffset] != 'M' || data_[peOffset + 1] != 'Z') {
                error = "the patched executable has no program after its headers";
                return false;
            }
            pe.assign(data_.begin() + peOffset, data_.end());
            return true;
        }
        if (encryption > 1 || compression != 1) {
            error = "default.xex is stored in a way this installer doesn't handle (encryption " + std::to_string(encryption) +
                    ", compression " + std::to_string(compression) + "): not the retail disc's executable";
            return false;
        }
        if (peOffset > data_.size()) {
            error = "default.xex is cut short: the image is damaged";
            return false;
        }
        std::vector<uint8_t> payload(data_.begin() + peOffset, data_.end());
        payload.resize(payload.size() / 16 * 16);
        if (encryption == 1) {
            if (size_t(securityOffset) + 0x160 > data_.size()) {
                error = "default.xex's security info is cut short: the image is damaged";
                return false;
            }
            uint8_t titleKey[16], iv[16] = {};
            Aes128Decryptor(kRetailKey).decryptBlock(data_.data() + securityOffset + 0x150, titleKey);
            Aes128Decryptor(titleKey).decryptCbc(payload.data(), payload.size(), iv);
        }
        pe.clear();
        size_t p = 0;
        const uint32_t blocks = infoSize >= 8 ? (infoSize - 8) / 8 : 0;
        for (uint32_t i = 0; i < blocks; i++) {
            uint32_t dataSize = 0, zeroSize = 0;
            if (!be32(data_, *ff + 8 + 8 * i, dataSize) || !be32(data_, *ff + 12 + 8 * i, zeroSize)) {
                error = "default.xex's block list is cut short: the image is damaged";
                return false;
            }
            if (dataSize > payload.size() - p) {
                error = "default.xex is cut short: the image is damaged";
                return false;
            }
            pe.insert(pe.end(), payload.begin() + p, payload.begin() + p + dataSize);
            pe.insert(pe.end(), zeroSize, 0);
            p += dataSize;
        }
        if (pe.size() < 2 || pe[0] != 'M' || pe[1] != 'Z') {
            error = "default.xex didn't decrypt to a program: the image is damaged";
            return false;
        }
        return true;
    }

    bool Xex::generateImports(const std::vector<uint8_t>& pe, const std::map<std::string, ExportTable>& exports,
                              std::string& cpp, std::vector<std::string>& stats, std::string& error) const
    {
        // tools/gen_imports.py, kept line for line so the output is the same text
        const auto il = header(kImportLibraries);
        uint32_t stringSize = 0, libraryCount = 0;
        if (!il || !be32(data_, *il + 4, stringSize) || !be32(data_, *il + 8, libraryCount) || libraryCount > 64 ||
            size_t(*il) + 12 + stringSize > data_.size()) {
            error = "default.xex has no readable import table: the image is damaged";
            return false;
        }
        std::vector<std::string> names;
        size_t p = 0;
        const uint8_t* raw = data_.data() + *il + 12;
        for (uint32_t i = 0; i < libraryCount; i++) {
            const void* zero = p < stringSize ? std::memchr(raw + p, 0, stringSize - p) : nullptr;
            if (!zero) {
                error = "default.xex's import names are damaged";
                return false;
            }
            const size_t e = size_t(static_cast<const uint8_t*>(zero) - raw);
            names.emplace_back(reinterpret_cast<const char*>(raw + p), e - p);
            p = (e + 1 + 3) & ~size_t(3);
        }

        const uint32_t base = imageBase();
        std::map<std::string, uint32_t> functions; // name -> thunk address (record type 1)
        struct Slot { uint32_t address; std::string name; };
        std::vector<Slot> variables, pointers;
        std::vector<std::string> unknown;
        size_t lib = size_t(*il) + 12 + stringSize;
        for (const std::string& library : names) {
            uint32_t librarySize = 0;
            uint16_t count = 0;
            if (!be32(data_, lib, librarySize) || !be16(data_, lib + 0x26, count) || librarySize == 0) {
                error = "default.xex's import table is damaged";
                return false;
            }
            static const ExportTable none;
            auto t = exports.find(library);
            const ExportTable& table = t == exports.end() ? none : t->second;
            for (uint32_t k = 0; k < count; k++) {
                uint32_t address = 0, record = 0;
                if (!be32(data_, lib + 0x28 + 4 * k, address) || address < base || !be32(pe, address - base, record)) {
                    error = "default.xex's import table points outside the program: the image is damaged";
                    return false;
                }
                const uint32_t type = record >> 24, ordinal = record & 0xFFFF;
                std::string name;
                bool variable = false;
                auto e = table.find(ordinal);
                if (e != table.end()) {
                    name = e->second.name;
                    variable = e->second.variable;
                } else {
                    char fallback[64];
                    std::snprintf(fallback, sizeof(fallback), "%s_%03X", library.substr(0, library.find('.')).c_str(), ordinal);
                    name = fallback;
                    unknown.push_back(name);
                }
                if (type == 1) functions[name] = address;
                else if (variable) variables.push_back({address, name});
                else pointers.push_back({address, name});
            }
            lib += librarySize;
        }

        char line[256];
        cpp = "// Generated by tools/gen_imports.py - do not edit\n";
        cpp += "#include \"kernel/import_support.h\"\n\n";
        for (const auto& [name, address] : functions) cpp += "PPC_WEAK_IMPORT(" + name + ")\n";
        cpp += "\nconst ImportFunction g_importFunctions[] = {\n";
        for (const auto& [name, address] : functions) {
            std::snprintf(line, sizeof(line), "    { 0x%08X, \"%s\" },\n", address, name.c_str());
            cpp += line;
        }
        cpp += "    { 0, nullptr },\n};\n\n";
        cpp += "const ImportVariable g_importVariables[] = {\n";
        for (const Slot& s : variables) {
            std::snprintf(line, sizeof(line), "    { 0x%08X, \"%s\" },\n", s.address, s.name.c_str());
            cpp += line;
        }
        cpp += "    { 0, nullptr },\n};\n\n";
        cpp += "const ImportFunctionPointer g_importFunctionPointers[] = {\n";
        for (const Slot& s : pointers) {
            auto f = functions.find(s.name);
            std::snprintf(line, sizeof(line), "    { 0x%08X, 0x%08X, \"%s\" },\n", s.address,
                          f == functions.end() ? 0u : f->second, s.name.c_str());
            cpp += line;
        }
        cpp += "    { 0, 0, nullptr },\n};\n";

        std::string libs;
        for (const std::string& n : names) libs += (libs.empty() ? "" : ", ") + n;
        stats.push_back("libraries: " + libs);
        stats.push_back("function imports: " + std::to_string(functions.size()) + ", variable slots: " +
                        std::to_string(variables.size()) + ", function-pointer slots: " + std::to_string(pointers.size()));
        if (!unknown.empty()) {
            std::string u;
            for (const std::string& n : unknown) u += (u.empty() ? "" : ", ") + n;
            stats.push_back("unknown ordinals: " + u);
        }
        return true;
    }

    std::vector<uint8_t> titleIcon(const std::vector<uint8_t>& pe)
    {
        // tools/title_icon.py: every "XDBF" in the image is tried; layout in runtime/kernel/spa.h
        constexpr uint64_t kTitleImage = 0x8000;
        static const uint8_t kPng[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
        for (size_t at = 0; at + 24 <= pe.size(); at++) {
            if (std::memcmp(pe.data() + at, "XDBF", 4) != 0) continue;
            uint32_t entryCount = 0, entryUsed = 0, freeCount = 0;
            be32(pe, at + 8, entryCount);
            be32(pe, at + 12, entryUsed);
            be32(pe, at + 16, freeCount);
            const uint64_t data = uint64_t(at) + 24 + uint64_t(entryCount) * 18 + uint64_t(freeCount) * 8;
            const uint32_t used = entryUsed < entryCount ? entryUsed : entryCount;
            for (uint32_t i = 0; i < used; i++) {
                const size_t e = at + 24 + size_t(i) * 18;
                uint16_t section = 0;
                uint32_t idHigh = 0, idLow = 0, offset = 0, size = 0;
                if (!be16(pe, e, section) || !be32(pe, e + 2, idHigh) || !be32(pe, e + 6, idLow) ||
                    !be32(pe, e + 10, offset) || !be32(pe, e + 14, size))
                    break;
                const uint64_t id = uint64_t(idHigh) << 32 | idLow;
                if (section != 2 || id != kTitleImage) continue;
                const uint64_t start = data + offset;
                if (start >= pe.size()) continue;
                const uint64_t end = start + size < pe.size() ? start + size : pe.size(); // a Python slice clamps
                if (end - start >= 8 && std::memcmp(pe.data() + start, kPng, 8) == 0)
                    return std::vector<uint8_t>(pe.begin() + long(start), pe.begin() + long(end));
            }
        }
        return {};
    }
}
