// Vd* (video driver) imports. Signatures and behavior follow Xenia's xboxkrnl_video.cc.
#include "gpu.h"
#include "../kernel/import_support.h"
#include "../kernel/kernel.h"
#include "../log.h"

#include <cstring>

using namespace mem;

namespace
{
    uint32_t g_gpuIdentifierAddress = 0;
}

// BOOL VdInitializeEngines(u32 Unknown, void* Callback, void* Arg, u32* PfpMicrocode, u32* MeMicrocode)
GUEST_IMPORT(VdInitializeEngines) { ctx.r3.u64 = 1; }
GUEST_IMPORT(VdShutdownEngines) {}

// void VdSetGraphicsInterruptCallback(void* Callback, void* UserData)
GUEST_IMPORT(VdSetGraphicsInterruptCallback) { gpu::setInterruptCallback(ctx.r3.u32, ctx.r4.u32); }

// void VdInitializeRingBuffer(u32 PhysicalAddress, u32 SizeLog2)
GUEST_IMPORT(VdInitializeRingBuffer) { gpu::initializeRingBuffer(ctx.r3.u32, ctx.r4.u32); }

// void VdEnableRingBufferRPtrWriteBack(u32 PhysicalAddress, u32 BlockLog2)
GUEST_IMPORT(VdEnableRingBufferRPtrWriteBack) { gpu::enableReadPointerWriteBack(ctx.r3.u32, ctx.r4.u32); }

// void VdGetSystemCommandBuffer(void* Buffer, u32* Value)
GUEST_IMPORT(VdGetSystemCommandBuffer)
{
    memset(host(ctx.r3.u32), 0, 0x94);
    write32(ctx.r3.u32, 0xBEEF0000);
    write32(ctx.r4.u32, 0xBEEF0001);
}

GUEST_IMPORT(VdSetSystemCommandBufferGpuIdentifierAddress) { g_gpuIdentifierAddress = ctx.r3.u32; }

GUEST_IMPORT(VdQueryVideoMode) { kernel::fillVideoMode(ctx.r3.u32); }

// Flags: 1 = widescreen, 2 = >= 1024 wide, 4 = >= 1920 wide
GUEST_IMPORT(VdQueryVideoFlags) { ctx.r3.u64 = 1 | 2; }

// void VdGetCurrentDisplayInformation(X_DISPLAY_INFO* Info)  (0x58 bytes, layout from Xenia)
GUEST_IMPORT(VdGetCurrentDisplayInformation)
{
    uint32_t d = ctx.r3.u32, w = gpu::kDisplayWidth, h = gpu::kDisplayHeight;
    memset(host(d), 0, 0x58);
    write16(d + 0x00, w);   // front buffer width
    write16(d + 0x02, h);   // front buffer height
    write8(d + 0x04, 6);    // front buffer color format
    write8(d + 0x05, 0x1A); // front buffer pixel format
    // scaler parameters @ 0x08: source rect (x1, y1, x2, y2), scaled output w/h, filters
    write32(d + 0x10, w);
    write32(d + 0x14, h);
    write32(d + 0x18, w);   // scaled output width
    write32(d + 0x1C, h);   // scaled output height
    write32(d + 0x20, 1);   // vertical filter type
    write32(d + 0x30, 1);   // horizontal filter type
    // overscan @ 0x40..0x46 left at 0
    write16(d + 0x48, w);   // display width
    write16(d + 0x4A, h);   // display height
    write32(d + 0x4C, 0x42700000); // refresh rate 60.0f
    write32(d + 0x50, 0);   // interlaced
    write16(d + 0x56, w);   // actual display width
}

// void VdGetCurrentDisplayGamma(u32* Type, float* Power)
GUEST_IMPORT(VdGetCurrentDisplayGamma)
{
    write32(ctx.r3.u32, 2);
    write32(ctx.r4.u32, 0x400E38E4); // 2.2222223f
}

GUEST_IMPORT(VdSetDisplayMode) { ctx.r3.u64 = 0; }
GUEST_IMPORT(VdPersistDisplay) { ctx.r3.u64 = 0; }
GUEST_IMPORT(VdEnableDisableClockGating) { ctx.r3.u64 = 0; }
GUEST_IMPORT(VdIsHSIOTrainingSucceeded) { ctx.r3.u64 = 1; }
GUEST_IMPORT(VdRetrainEDRAM) { ctx.r3.u64 = 0; }
GUEST_IMPORT(VdRetrainEDRAMWorker) { ctx.r3.u64 = 0; }
GUEST_IMPORT(VdCallGraphicsNotificationRoutines) { ctx.r3.u64 = 0; }

// u32 VdInitializeScalerCommandBuffer(u32 SrcXY, u32 SrcWH, u32 DstXY, u32 DstWH, u32, u32 SplitWidth, u32, u32, void* Dest)
GUEST_IMPORT(VdInitializeScalerCommandBuffer)
{
    // The game only checks for success; fill the buffer with type-2 NOPs.
    constexpr uint32_t kWords = 0x1CC / 4;
    uint32_t dest = stackArg(ctx, 8);
    for (uint32_t i = 0; i < kWords; i++)
        write32(dest + i * 4, 0x80000000);
    ctx.r3.u64 = kWords;
}

// void VdSwap(u32* RingSpace, void* FetchConstant, void*, void*, void*, u32* FrontBuffer,
//             u32* TextureFormat, u32* ColorSpace, u32* Width, u32* Height)
// D3D reserves 64 dwords in the ring for us; we put a single swap packet there.
GUEST_IMPORT(VdSwap)
{
    uint32_t ring = ctx.r3.u32, pFront = ctx.r8.u32;
    uint32_t pWidth = stackArg(ctx, 8), pHeight = stackArg(ctx, 9);
    uint32_t front = pFront ? read32(pFront) : 0;
    uint32_t width = pWidth > 0x10000 ? read32(pWidth) : gpu::kDisplayWidth;
    uint32_t height = pHeight > 0x10000 ? read32(pHeight) : gpu::kDisplayHeight;

    write32(ring + 0, (3u << 30) | ((63u - 1) << 16) | (gpu::PM4_XE_SWAP << 8)); // type 3, 63 payload dwords
    write32(ring + 4, physicalAddress(front));
    write32(ring + 8, width);
    write32(ring + 12, height);
    for (uint32_t i = 4; i < 64; i++)
        write32(ring + i * 4, 0);
    ctx.r3.u64 = 0;
}
