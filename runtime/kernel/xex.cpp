// The game's own module: XEX header in guest memory and the module-query imports.
#include "import_support.h"
#include "kernel.h"
#include "../log.h"

#include <cstring>
#include <fstream>
#include <vector>

using namespace mem;

namespace
{
    uint32_t g_header = 0; // guest copy of the XEX header
    uint32_t g_ldr = 0;    // LDR_DATA_TABLE_ENTRY-like module handle
    constexpr uint32_t kLdrXexHeaderBase = 0x58;

    // Returns the optional header value (key & 0xFF == 0) or a guest pointer to its data.
    bool findOptionalHeader(uint32_t header, uint32_t key, uint32_t& out)
    {
        uint32_t count = read32(header + 0x14);
        for (uint32_t i = 0; i < count; i++)
        {
            uint32_t k = read32(header + 0x18 + i * 8);
            uint32_t v = read32(header + 0x1C + i * 8);
            if (k != key)
                continue;
            out = (key & 0xFF) == 0 ? v : header + v;
            return true;
        }
        return false;
    }
}

namespace kernel
{
    void loadXexHeader(const std::string& path)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f)
            fatal("cannot open %s", path.c_str());
        uint8_t first[0x18];
        f.read((char*)first, sizeof(first));
        uint32_t peOffset = __builtin_bswap32(*(uint32_t*)(first + 8));
        std::vector<char> hdr(peOffset);
        f.seekg(0);
        f.read(hdr.data(), peOffset);

        g_header = kernelAlloc(peOffset, 0x1000);
        memcpy(host(g_header), hdr.data(), peOffset);
        g_ldr = kernelAlloc(0x100);
        write32(g_ldr + kLdrXexHeaderBase, g_header);
        write32(g_ldr + 0x18, PPC_IMAGE_BASE); // DllBase
        write32(g_ldr + 0x20, PPC_IMAGE_SIZE); // SizeOfImage
        info("XEX header (0x%X bytes) at %08X, module handle %08X", peOffset, g_header, g_ldr);
    }

    uint32_t moduleHandle() { return g_ldr; }

    uint32_t xexHeaderField(uint32_t key)
    {
        uint32_t out = 0;
        findOptionalHeader(g_header, key, out);
        return out;
    }
}

// void* RtlImageXexHeaderField(void* XexHeaderBase, u32 ImageField)
GUEST_IMPORT(RtlImageXexHeaderField)
{
    uint32_t header = ctx.r3.u32 ? ctx.r3.u32 : g_header;
    uint32_t out = 0;
    findOptionalHeader(header, ctx.r4.u32, out);
    ctx.r3.u64 = out;
}

// NTSTATUS XexGetModuleHandle(const char* Name, void** Handle)
GUEST_IMPORT(XexGetModuleHandle)
{
    uint32_t name = ctx.r3.u32;
    if (name && strcasecmp((const char*)host(name), "default.xex") != 0)
    {
        warn("XexGetModuleHandle(\"%s\") - only the game module exists", (const char*)host(name));
        write32(ctx.r4.u32, 0);
        ctx.r3.u64 = STATUS_OBJECT_NAME_NOT_FOUND;
        return;
    }
    write32(ctx.r4.u32, g_ldr);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// BOOL XexCheckExecutablePrivilege(u32 Privilege)
GUEST_IMPORT(XexCheckExecutablePrivilege)
{
    uint32_t flags = 0;
    findOptionalHeader(g_header, 0x30000, flags); // XEX_HEADER_SYSTEM_FLAGS
    uint32_t bit = ctx.r3.u32;
    ctx.r3.u64 = bit < 32 ? (flags >> bit) & 1 : 0;
}
