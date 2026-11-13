#pragma once
// Xenos (Xbox 360 GPU) shader microcode decoding.
//
// Bit layouts, opcode numbers and mnemonics follow Xenia's src/xenia/gpu/ucode.h, ucode.cc and
// shader_translator.cc (Copyright Ben Vanik, BSD 3-clause; see docs/licenses/xenia-LICENSE.txt).
// Fields are extracted with explicit shifts from host-order dwords (shader microcode is stored
// big-endian in guest memory; swap each dword before decoding).
//
// Layout summary:
//  * A shader starts with control-flow (CF) instructions, 48 bits each, packed two per 3 dwords.
//  * Exec-type CF instructions point at instruction slots of 3 dwords (96 bits) each, counted
//    from the start of the shader; each slot is a fetch or an ALU instruction (per the exec's
//    sequence bits).
#include <cstdint>

namespace gpu::ucode
{
    inline uint32_t bits(uint32_t v, uint32_t lo, uint32_t n) { return (v >> lo) & ((1u << n) - 1); }
    inline int32_t sbits(uint32_t v, uint32_t lo, uint32_t n)
    {
        int32_t x = int32_t(bits(v, lo, n));
        return (x & (1 << (n - 1))) ? x - (1 << n) : x;
    }

    // ---- Control flow ----
    enum class CfOp : uint32_t
    {
        Nop = 0, Exec = 1, ExecEnd = 2, CondExec = 3, CondExecEnd = 4, CondExecPred = 5, CondExecPredEnd = 6,
        LoopStart = 7, LoopEnd = 8, CondCall = 9, Return = 10, CondJmp = 11, Alloc = 12,
        CondExecPredClean = 13, CondExecPredCleanEnd = 14, MarkVsFetchDone = 15,
    };
    inline const char* cfName(CfOp op)
    {
        static const char* n[16] = { "nop", "exec", "exece", "cexec", "cexece", "cexec_pred", "cexece_pred", "loop_start",
                                     "loop_end", "ccall", "ret", "cjmp", "alloc", "cexec_pred_clean",
                                     "cexece_pred_clean", "mark_vs_fetch_done" };
        return n[uint32_t(op) & 15];
    }
    inline bool isExec(CfOp op)
    {
        switch (op)
        {
        case CfOp::Exec: case CfOp::ExecEnd: case CfOp::CondExec: case CfOp::CondExecEnd: case CfOp::CondExecPred:
        case CfOp::CondExecPredEnd: case CfOp::CondExecPredClean: case CfOp::CondExecPredCleanEnd:
            return true;
        default:
            return false;
        }
    }
    inline bool endsShader(CfOp op)
    {
        return op == CfOp::ExecEnd || op == CfOp::CondExecEnd || op == CfOp::CondExecPredEnd ||
               op == CfOp::CondExecPredCleanEnd;
    }

    // One CF instruction: 32 low bits + 16 high bits.
    struct Cf
    {
        uint32_t lo, hi;
        CfOp op() const { return CfOp(bits(hi, 12, 4)); }
        bool absoluteAddressing() const { return bits(hi, 11, 1); }
        // exec family
        uint32_t execAddress() const { return bits(lo, 0, 12); }
        uint32_t execCount() const { return bits(lo, 12, 3); }
        bool execYield() const { return bits(lo, 15, 1); }
        uint32_t execSequence() const { return bits(lo, 16, 12); } // 2 bits per slot: fetch, serialize
        bool execPredicateClean() const { return bits(hi, 9, 1); } // Exec, CondExecPred*
        uint32_t condBoolAddress() const { return bits(hi, 2, 8); } // CondExec, CondCall, CondJmp
        bool condition() const { return bits(hi, 10, 1); }
        // loops, calls, jumps
        uint32_t target() const { return bits(lo, 0, 13); }
        bool loopIsRepeat() const { return bits(lo, 13, 1); }
        uint32_t loopId() const { return bits(lo, 16, 5); }
        bool loopPredicatedBreak() const { return bits(lo, 21, 1); }
        bool isUnconditional() const { return bits(lo, 13, 1); }
        bool isPredicated() const { return bits(lo, 14, 1); }
        bool jmpDirection() const { return bits(hi, 1, 1); }
        // alloc
        uint32_t allocSize() const { return bits(lo, 0, 3); }
        uint32_t allocType() const { return bits(hi, 9, 2); } // 1 = position/none, 2 = interpolators/colors, 3 = memory
    };

    // Unpack the two CF instructions stored in three dwords.
    inline void unpackCf(const uint32_t* d, Cf out[2])
    {
        out[0] = { d[0], d[1] & 0xFFFF };
        out[1] = { (d[1] >> 16) | (d[2] << 16), d[2] >> 16 };
    }

    // ---- Fetch ----
    enum class FetchOp : uint32_t
    {
        VertexFetch = 0, TextureFetch = 1, GetTextureBorderColorFrac = 16, GetTextureComputedLod = 17,
        GetTextureGradients = 18, GetTextureWeights = 19, SetTextureLod = 24, SetTextureGradientsHorz = 25,
        SetTextureGradientsVert = 26,
    };
    inline const char* fetchName(uint32_t op)
    {
        switch (op)
        {
        case 0: return "vfetch";
        case 1: return "tfetch";
        case 16: return "getBCF";
        case 17: return "getCompTexLOD";
        case 18: return "getGradients";
        case 19: return "getWeights";
        case 24: return "setTexLOD";
        case 25: return "setGradientH";
        case 26: return "setGradientV";
        default: return "fetch_?";
        }
    }
    inline const char* vertexFormatName(uint32_t f)
    {
        switch (f)
        {
        case 6: return "8_8_8_8"; case 7: return "2_10_10_10"; case 16: return "10_11_11"; case 17: return "11_11_10";
        case 25: return "16_16"; case 26: return "16_16_16_16"; case 31: return "16_16_FLOAT";
        case 32: return "16_16_16_16_FLOAT"; case 33: return "32"; case 34: return "32_32"; case 35: return "32_32_32_32";
        case 36: return "32_FLOAT"; case 37: return "32_32_FLOAT"; case 38: return "32_32_32_32_FLOAT";
        case 57: return "32_32_32_FLOAT";
        default: return "format_?";
        }
    }

    struct Fetch
    {
        uint32_t d[3];
        uint32_t op() const { return bits(d[0], 0, 5); }
        uint32_t srcReg() const { return bits(d[0], 5, 6); }
        bool srcRegRelative() const { return bits(d[0], 11, 1); }
        uint32_t dstReg() const { return bits(d[0], 12, 6); }
        bool dstRegRelative() const { return bits(d[0], 18, 1); }
        uint32_t constIndex() const { return bits(d[0], 20, 5); }
        uint32_t dstSwizzle() const { return bits(d[1], 0, 12); } // 3 bits per component: 0-3 xyzw, 4 = 0, 5 = 1, 7 = keep
        bool isPredicated() const { return bits(d[1], 31, 1); }
        bool predCondition() const { return bits(d[2], 31, 1); }
        // vertex fetch
        uint32_t vfConstIndexSel() const { return bits(d[0], 25, 2); }
        uint32_t vfFetchSlot() const { return constIndex() * 3 + vfConstIndexSel(); } // 2-dword vertex fetch constant
        uint32_t vfSrcSwizzle() const { return bits(d[0], 30, 2); }
        bool vfFormatCompAll() const { return bits(d[1], 12, 1); } // signed
        bool vfNumFormatAll() const { return bits(d[1], 13, 1); }  // 1 = integer, 0 = fraction
        bool vfIsIndexRounded() const { return bits(d[1], 15, 1); }
        uint32_t vfFormat() const { return bits(d[1], 16, 6); }
        int32_t vfExpAdjust() const { return sbits(d[1], 24, 6); }
        bool vfIsMini() const { return bits(d[1], 30, 1); }
        uint32_t vfStride() const { return bits(d[2], 0, 8); }   // dwords
        int32_t vfOffset() const { return sbits(d[2], 8, 23); } // dwords
        // texture fetch
        bool tfCoordDenormalized() const { return bits(d[0], 25, 1); }
        uint32_t tfSrcSwizzle() const { return bits(d[0], 26, 6); } // 2 bits per coordinate
        uint32_t tfMagFilter() const { return bits(d[1], 12, 2); }
        uint32_t tfMinFilter() const { return bits(d[1], 14, 2); }
        uint32_t tfMipFilter() const { return bits(d[1], 16, 2); }
        uint32_t tfAnisoFilter() const { return bits(d[1], 18, 3); }
        bool tfUseComputedLod() const { return bits(d[1], 28, 1); }
        bool tfUseRegisterLod() const { return bits(d[1], 29, 1); }
        bool tfUseRegisterGradients() const { return bits(d[2], 0, 1); }
        int32_t tfLodBias() const { return sbits(d[2], 2, 7); }
        uint32_t tfDimension() const { return bits(d[2], 14, 2); } // 0 1D, 1 2D, 2 3D/stacked, 3 cube
        int32_t tfOffsetX() const { return sbits(d[2], 16, 5); }
        int32_t tfOffsetY() const { return sbits(d[2], 21, 5); }
        int32_t tfOffsetZ() const { return sbits(d[2], 26, 5); }
    };

    // ---- ALU ----
    struct VectorOpInfo { const char* name; uint32_t operands; };
    inline const VectorOpInfo& vectorOpInfo(uint32_t op)
    {
        static const VectorOpInfo t[32] = {
            {"add", 2}, {"mul", 2}, {"max", 2}, {"min", 2}, {"seq", 2}, {"sgt", 2}, {"sge", 2}, {"sne", 2},
            {"frc", 1}, {"trunc", 1}, {"floor", 1}, {"mad", 3}, {"cndeq", 3}, {"cndge", 3}, {"cndgt", 3},
            {"dp4", 2}, {"dp3", 2}, {"dp2add", 3}, {"cube", 2}, {"max4", 1}, {"setp_eq_push", 2},
            {"setp_ne_push", 2}, {"setp_gt_push", 2}, {"setp_ge_push", 2}, {"kill_eq", 2}, {"kill_gt", 2},
            {"kill_ge", 2}, {"kill_ne", 2}, {"dst", 2}, {"maxa", 2}, {"opcode_30", 0}, {"opcode_31", 0} };
        return t[op & 31];
    }

    // operands: 0 none, 1 = one operand (src3; .w, or .wx when twoComponent), 2 = constant + temp
    struct ScalarOpInfo { const char* name; uint32_t operands; bool twoComponent; };
    inline const ScalarOpInfo& scalarOpInfo(uint32_t op)
    {
        static const ScalarOpInfo t[64] = {
            {"adds", 1, true}, {"adds_prev", 1, false}, {"muls", 1, true}, {"muls_prev", 1, false},
            {"muls_prev2", 1, true}, {"maxs", 1, true}, {"mins", 1, true}, {"seqs", 1, false}, {"sgts", 1, false},
            {"sges", 1, false}, {"snes", 1, false}, {"frcs", 1, false}, {"truncs", 1, false}, {"floors", 1, false},
            {"exp", 1, false}, {"logc", 1, false}, {"log", 1, false}, {"rcpc", 1, false}, {"rcpf", 1, false},
            {"rcp", 1, false}, {"rsqc", 1, false}, {"rsqf", 1, false}, {"rsq", 1, false}, {"maxas", 1, true},
            {"maxasf", 1, true}, {"subs", 1, true}, {"subs_prev", 1, false}, {"setp_eq", 1, false},
            {"setp_ne", 1, false}, {"setp_gt", 1, false}, {"setp_ge", 1, false}, {"setp_inv", 1, false},
            {"setp_pop", 1, false}, {"setp_clr", 0, false}, {"setp_rstr", 1, false}, {"kills_eq", 1, false},
            {"kills_gt", 1, false}, {"kills_ge", 1, false}, {"kills_ne", 1, false}, {"kills_one", 1, false},
            {"sqrt", 1, false}, {"opcode_41", 0, false}, {"mulsc", 2, false}, {"mulsc", 2, false}, {"addsc", 2, false},
            {"addsc", 2, false}, {"subsc", 2, false}, {"subsc", 2, false}, {"sin", 1, false}, {"cos", 1, false},
            {"retain_prev", 0, false}, {"opcode_51", 0, false}, {"opcode_52", 0, false}, {"opcode_53", 0, false},
            {"opcode_54", 0, false}, {"opcode_55", 0, false}, {"opcode_56", 0, false}, {"opcode_57", 0, false},
            {"opcode_58", 0, false}, {"opcode_59", 0, false}, {"opcode_60", 0, false}, {"opcode_61", 0, false},
            {"opcode_62", 0, false}, {"opcode_63", 0, false} };
        return t[op & 63];
    }

    struct Alu
    {
        uint32_t d[3];
        // dword 0
        uint32_t vectorDest() const { return bits(d[0], 0, 6); }
        bool vectorDestRelative() const { return bits(d[0], 6, 1); }
        bool absConstants() const { return bits(d[0], 7, 1); }
        uint32_t scalarDest() const { return bits(d[0], 8, 6); }
        bool scalarDestRelative() const { return bits(d[0], 14, 1); }
        bool isExport() const { return bits(d[0], 15, 1); }
        uint32_t vectorWriteMask() const { return bits(d[0], 16, 4); }
        uint32_t scalarWriteMask() const { return bits(d[0], 20, 4); }
        bool vectorClamp() const { return bits(d[0], 24, 1); }
        bool scalarClamp() const { return bits(d[0], 25, 1); }
        uint32_t scalarOp() const { return bits(d[0], 26, 6); }
        // dword 1 (src index 1..3)
        uint32_t srcSwizzle(int i) const { return bits(d[1], 8 * (3 - i), 8); }
        bool srcNegate(int i) const { return bits(d[1], 24 + (3 - i), 1); }
        bool predCondition() const { return bits(d[1], 27, 1); }
        bool isPredicated() const { return bits(d[1], 28, 1); }
        bool constAddressRegisterRelative() const { return bits(d[1], 29, 1); }
        bool const1RelAbs() const { return bits(d[1], 30, 1); }
        bool const0RelAbs() const { return bits(d[1], 31, 1); }
        // dword 2
        uint32_t srcReg(int i) const { return bits(d[2], 8 * (3 - i), 8); }
        uint32_t vectorOp() const { return bits(d[2], 24, 5); }
        bool srcIsTemp(int i) const { return bits(d[2], 29 + (3 - i), 1); }

        // Constant sources are addressed by const 0 / const 1 flags in order of appearance
        bool srcConstIsAddressed(int i) const
        {
            if (i == 1) return const0RelAbs();
            if (i == 2) return srcIsTemp(1) ? const0RelAbs() : const1RelAbs();
            return (srcIsTemp(1) && srcIsTemp(2)) ? const0RelAbs() : const1RelAbs();
        }
        // Temp register of the constant+temp scalar ops (mulsc, addsc, subsc)
        uint32_t scalarConstOpTempReg() const { return (scalarOp() & 1) | (uint32_t(srcIsTemp(3)) << 1) | (srcSwizzle(3) & 0x3C); }
    };

    // Component-relative swizzle: component c of the source comes from ((s >> 2c) + c) & 3
    inline uint32_t swizzledComponent(uint32_t swizzle, uint32_t c) { return ((swizzle >> (2 * c)) + c) & 3; }
}
