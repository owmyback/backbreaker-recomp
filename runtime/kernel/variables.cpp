// Kernel data exports and import-slot patching.
#include "import_support.h"
#include "kernel.h"
#include "../log.h"

#include <cstring>
#include <string_view>

using namespace mem;

namespace
{
    // Guest address of KeTimeStampBundle, kept so the updater thread can find it (see kernel.h).
    uint32_t g_timeStampBundle = 0;

    uint32_t makeVariable(std::string_view name)
    {
        if (name == "XboxHardwareInfo")
        {
            // { u32 Flags; u8 NumberOfProcessors; u8 PCIBridgeRevisionID; u8 Reserved[6]; u16 BldrMagic; u16 BldrFlags; }
            uint32_t a = kernelAlloc(0x10);
            write32(a, 0x00000020);
            write8(a + 4, 6);
            return a;
        }
        if (name == "XboxKrnlVersion")
        {
            // { u16 Major, Minor, Build; u8 ApprovalType, Qfe } - a late retail dashboard
            uint32_t a = kernelAlloc(8);
            write16(a, 2);
            write16(a + 2, 0);
            write16(a + 4, 17559);
            write16(a + 6, 0x8000);
            return a;
        }
        if (name == "KeTimeStampBundle")
        {
            // { u64 InterruptTime; u64 SystemTime; u32 TickCount; u32 }, all big-endian. The fields
            // are refreshed by a host thread: see kernel::startTimeStampThread (system.cpp).
            g_timeStampBundle = kernelAlloc(0x18);
            return g_timeStampBundle;
        }
        if (name == "XexExecutableModuleHandle")
        {
            // The variable holds a pointer to the module's loader entry.
            uint32_t a = kernelAlloc(4);
            write32(a, kernel::moduleHandle());
            return a;
        }
        if (name == "ExLoadedCommandLine")
        {
            static const char cmd[] = "\"default.xex\"";
            uint32_t a = kernelAlloc(0x200);
            memcpy(host(a), cmd, sizeof(cmd));
            return a;
        }
        if (name == "VdGpuClockInMHz")
        {
            uint32_t a = kernelAlloc(4);
            write32(a, 500);
            return a;
        }
        // Object types, device globals, monitor data: zeroed storage is enough for now.
        return kernelAlloc(0x40);
    }
}

namespace kernel
{
    uint32_t timeStampBundleAddress() { return g_timeStampBundle; }

    void patchImports()
    {
        size_t vars = 0, fptrs = 0;
        for (const ImportVariable* v = g_importVariables; v->name; v++, vars++)
            write32(v->slot, makeVariable(v->name));
        for (const ImportFunctionPointer* p = g_importFunctionPointers; p->name; p++, fptrs++)
            write32(p->slot, p->thunk);
        info("patched %zu kernel variables and %zu import function pointers", vars, fptrs);
    }
}
