#pragma once
// Xenos GPU front end: register file, PM4 command processor and display interrupts.
// Phase 1 is headless: commands are executed for their memory/sync side effects only.
#include <cstdint>

namespace gpu
{
    // Register indices (dword offsets) used by the command processor
    namespace reg
    {
        constexpr uint32_t CP_RB_RPTR = 0x01C4;
        constexpr uint32_t CP_RB_WPTR = 0x01C5;
        constexpr uint32_t SCRATCH_UMSK = 0x01DC;
        constexpr uint32_t SCRATCH_ADDR = 0x01DD;
        constexpr uint32_t SCRATCH_REG0 = 0x0578;
        constexpr uint32_t COHER_STATUS_HOST = 0x0A31;
        constexpr uint32_t VGT_EVENT_INITIATOR = 0x21F9;
        constexpr uint32_t RB_EDRAM_TIMING = 0x0F00;
        constexpr uint32_t RB_BC_CONTROL = 0x0F01;
        constexpr uint32_t D1MODE_V_COUNTER = 0x194C;
        constexpr uint32_t D1MODE_INTERRUPT_STATUS = 0x1951;
        constexpr uint32_t D1MODE_VIEWPORT_SIZE = 0x1961;
        constexpr uint32_t kCount = 0x6000;
    }

    void init();

    uint32_t readRegister(uint32_t index);
    // The register file itself: big-endian dwords, indexed like readRegister (which swaps one).
    // For copying blocks of registers per draw without a call per register (#73).
    const uint32_t* registerFile();
    void writeRegister(uint32_t index, uint32_t value); // with side effects (scratch writeback etc.)

    // From the Vd* imports
    void setInterruptCallback(uint32_t callback, uint32_t userData);
    void initializeRingBuffer(uint32_t physAddr, uint32_t sizeLog2);
    void enableReadPointerWriteBack(uint32_t physAddr, uint32_t blockLog2);
    void wakeCommandProcessor();

    // Display size the game is told about
    constexpr uint32_t kDisplayWidth = 1280;
    constexpr uint32_t kDisplayHeight = 720;

    // PM4 opcode for the swap packet our VdSwap writes into the ring
    constexpr uint32_t PM4_XE_SWAP = 0x64;
}
