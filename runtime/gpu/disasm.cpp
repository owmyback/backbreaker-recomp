// Xenos shader disassembler. Operand decoding follows Xenia's shader_translator.cc
// (BSD 3-clause, see docs/licenses/xenia-LICENSE.txt); see ucode.h for the bit layouts.
#include "disasm.h"
#include "ucode.h"

#include <cstdarg>
#include <cstdio>
#include <set>

namespace gpu
{
    using namespace ucode;

    namespace
    {
        const char kComp[] = "xyzw";

        std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
        std::string fmt(const char* f, ...)
        {
            char buf[512];
            va_list ap;
            va_start(ap, f);
            vsnprintf(buf, sizeof(buf), f, ap);
            va_end(ap);
            return buf;
        }

        std::string mask(uint32_t m)
        {
            std::string s;
            for (int i = 0; i < 4; i++) s += (m >> i) & 1 ? kComp[i] : '_';
            return s;
        }

        std::string exportName(uint32_t reg, ShaderType type)
        {
            if (reg == 32) return "eA";
            if (reg >= 33 && reg <= 37) return fmt("eM%u", reg - 33);
            if (type == ShaderType::Vertex)
            {
                if (reg == 62) return "oPos";
                if (reg == 63) return "oPts";
                if (reg < 16) return fmt("o%u", reg);
            }
            else
            {
                if (reg < 4) return fmt("oC%u", reg);
                if (reg == 61) return "oDepth";
            }
            return fmt("export%u", reg);
        }

        // ALU source operand i (1..3) with `count` swizzle components (4, or 1/2 for scalar ops)
        std::string aluSource(const Alu& a, int i, int count, ShaderCoverage* cov)
        {
            uint32_t reg = a.srcReg(i);
            std::string s = a.srcNegate(i) ? "-" : "";
            bool absolute;
            if (a.srcIsTemp(i))
            {
                s += fmt("r%u", reg & 0x3F);
                if (reg & 0x40) { s += "[aL]"; if (cov) cov->add(cov->features, "temp register loop-relative"); }
                absolute = reg & 0x80;
            }
            else
            {
                s += fmt("c%u", reg);
                if (a.srcConstIsAddressed(i))
                {
                    s += a.constAddressRegisterRelative() ? "[a0]" : "[aL]";
                    if (cov) cov->add(cov->features, a.constAddressRegisterRelative() ? "constant a0-relative" : "constant loop-relative");
                }
                absolute = a.absConstants();
            }
            uint32_t sw = a.srcSwizzle(i);
            std::string comps;
            if (count == 4) for (uint32_t c = 0; c < 4; c++) comps += kComp[swizzledComponent(sw, c)];
            else if (count == 1) comps = kComp[swizzledComponent(sw, 3)];
            else { comps += kComp[swizzledComponent(sw, 3)]; comps += kComp[swizzledComponent(sw, 0)]; }
            if (comps != "xyzw") s += "." + comps;
            if (absolute) { s = "|" + s + "|"; if (cov) cov->add(cov->features, "absolute value source"); }
            return s;
        }

        std::string disasmAlu(const Alu& a, ShaderType type, ShaderCoverage* cov)
        {
            std::string out;
            std::string pred = a.isPredicated() ? (a.predCondition() ? "(p) " : "(!p) ") : "";
            if (a.isPredicated() && cov) cov->add(cov->features, "predicated ALU");
            const VectorOpInfo& vi = vectorOpInfo(a.vectorOp());
            const ScalarOpInfo& si = scalarOpInfo(a.scalarOp());
            bool exp = a.isExport();
            uint32_t vmask = a.vectorWriteMask(), smask = a.scalarWriteMask();

            // Vector half (skipped when it writes nothing and is a plain no-side-effect op)
            bool vectorUsed = vmask || exp || (a.vectorOp() >= 20 && a.vectorOp() <= 27) || a.vectorOp() == 29;
            if (vectorUsed)
            {
                std::string dst = exp ? exportName(a.vectorDest(), type) : fmt("r%u", a.vectorDest());
                if (a.vectorDestRelative() && !exp) dst += "[aL]";
                out += pred + vi.name + (a.vectorClamp() ? "_sat " : " ") + dst + "." + mask(vmask);
                for (uint32_t s = 1; s <= vi.operands; s++) out += ", " + aluSource(a, int(s), 4, cov);
                if (cov)
                {
                    cov->add(cov->vector, vi.name);
                    if (exp) cov->add(cov->exports, exportName(a.vectorDest(), type));
                }
            }
            // Scalar half
            bool scalarUsed = smask || exp || a.scalarOp() == 50 || (a.scalarOp() >= 27 && a.scalarOp() <= 39);
            if (scalarUsed && a.scalarOp() != 50) // retain_prev with nothing written is a no-op
            {
                if (!out.empty()) out += "  +  ";
                std::string dst = exp ? exportName(a.vectorDest(), type) : fmt("r%u", a.scalarDest());
                if (a.scalarDestRelative() && !exp) dst += "[aL]";
                out += pred + si.name + (a.scalarClamp() ? "_sat " : " ") + dst + "." + mask(smask);
                if (si.operands == 1)
                    out += ", " + aluSource(a, 3, si.twoComponent ? 2 : 1, cov);
                else if (si.operands == 2)
                {
                    // constant (W of src3) and temp register (X of src3's swizzle)
                    std::string neg = a.srcNegate(3) ? "-" : "";
                    out += fmt(", %sc%u.%c, %sr%u.%c", neg.c_str(), a.srcReg(3), kComp[swizzledComponent(a.srcSwizzle(3), 3)],
                               neg.c_str(), a.scalarConstOpTempReg(), kComp[swizzledComponent(a.srcSwizzle(3), 0)]);
                }
                if (cov)
                {
                    cov->add(cov->scalar, si.name);
                    if (exp && !vectorUsed) cov->add(cov->exports, exportName(a.vectorDest(), type));
                }
            }
            if (out.empty()) out = "nop";
            return out;
        }

        std::string fetchDst(const Fetch& f)
        {
            static const char comp[8] = { 'x', 'y', 'z', 'w', '0', '1', '?', '_' };
            std::string s = fmt("r%u%s.", f.dstReg(), f.dstRegRelative() ? "[aL]" : "");
            for (int c = 0; c < 4; c++) s += comp[(f.dstSwizzle() >> (3 * c)) & 7];
            return s;
        }

        std::string disasmFetch(const Fetch& f, ShaderCoverage* cov)
        {
            std::string pred = f.isPredicated() ? (f.predCondition() ? "(p) " : "(!p) ") : "";
            uint32_t op = f.op();
            if (cov)
            {
                cov->add(cov->fetch, fetchName(op));
                if (f.isPredicated()) cov->add(cov->features, "predicated fetch");
            }
            if (op == uint32_t(FetchOp::VertexFetch))
            {
                // Integer/normalized and signed/unsigned only apply to fixed-point formats
                std::string format = vertexFormatName(f.vfFormat());
                if (format.find("FLOAT") == std::string::npos)
                    format += std::string(f.vfNumFormatAll() ? " int" : " norm") + (f.vfFormatCompAll() ? " signed" : " unsigned");
                if (cov) cov->add(cov->vertexFormats, format);
                return pred + fmt("vfetch%s %s, r%u.%c, vf%u (%s stride=%u offset=%d%s)", f.vfIsMini() ? "_mini" : "", fetchDst(f).c_str(),
                                  f.srcReg(), kComp[f.vfSrcSwizzle()], f.vfFetchSlot(), format.c_str(), f.vfStride(), f.vfOffset(),
                                  f.vfExpAdjust() ? fmt(" exp_adjust=%d", f.vfExpAdjust()).c_str() : "");
            }
            static const char* dims[4] = { "1D", "2D", "3D", "cube" };
            std::string coords;
            for (int c = 0; c < 3; c++) coords += kComp[(f.tfSrcSwizzle() >> (2 * c)) & 3];
            if (op == uint32_t(FetchOp::TextureFetch) && cov)
            {
                cov->add(cov->textureDimensions, dims[f.tfDimension()]);
                if (f.tfUseRegisterLod()) cov->add(cov->features, "texture LOD from register");
                if (f.tfUseRegisterGradients()) cov->add(cov->features, "texture gradients from register");
                if (f.tfOffsetX() || f.tfOffsetY() || f.tfOffsetZ()) cov->add(cov->features, "texture fetch offset");
                if (f.tfCoordDenormalized()) cov->add(cov->features, "unnormalized texture coordinates");
            }
            return pred + fmt("%s %s, r%u.%s, tf%u (%s%s%s)", fetchName(op), fetchDst(f).c_str(), f.srcReg(), coords.c_str(), f.constIndex(),
                              dims[f.tfDimension()], f.tfCoordDenormalized() ? " unnormalized" : "",
                              f.tfLodBias() ? fmt(" lod_bias=%d", f.tfLodBias()).c_str() : "");
        }
    }

    std::string disassemble(const std::vector<uint32_t>& w, ShaderType type, ShaderCoverage* cov)
    {
        std::string out;
        if (cov) cov->shaders++;
        size_t cfEnd = w.size(); // dword index where instruction slots begin
        std::vector<Cf> cfs;
        for (size_t i = 0; i + 3 <= w.size() && i < cfEnd; i += 3)
        {
            Cf pair[2];
            unpackCf(&w[i], pair);
            bool end = false;
            for (const Cf& cf : pair)
            {
                cfs.push_back(cf);
                if (isExec(cf.op()) && cf.execCount() && cf.execAddress() * 3 < cfEnd)
                    cfEnd = cf.execAddress() * 3;
                end = end || endsShader(cf.op());
            }
            if (end) break;
        }

        std::set<uint32_t> listed;
        for (size_t n = 0; n < cfs.size(); n++)
        {
            const Cf& cf = cfs[n];
            CfOp op = cf.op();
            if (cov) cov->add(cov->controlFlow, cfName(op));
            out += fmt("  cf%-3zu %s", n, cfName(op));
            if (isExec(op))
            {
                if (op == CfOp::CondExec || op == CfOp::CondExecEnd)
                    out += fmt(" %sb%u", cf.condition() ? "" : "!", cf.condBoolAddress());
                else if (op >= CfOp::CondExecPred && op != CfOp::Exec && op != CfOp::ExecEnd)
                    out += cf.condition() ? " p" : " !p";
                out += fmt("  addr=%u count=%u%s\n", cf.execAddress(), cf.execCount(), cf.execYield() ? " yield" : "");
                for (uint32_t k = 0; k < cf.execCount(); k++)
                {
                    uint32_t slot = cf.execAddress() + k;
                    if (size_t(slot) * 3 + 3 > w.size()) { out += "        <slot out of range>\n"; break; }
                    bool isFetch = (cf.execSequence() >> (2 * k)) & 1;
                    bool serialize = (cf.execSequence() >> (2 * k + 1)) & 1;
                    std::string text;
                    if (isFetch)
                        text = disasmFetch(Fetch{ { w[slot * 3], w[slot * 3 + 1], w[slot * 3 + 2] } }, cov);
                    else
                        text = disasmAlu(Alu{ { w[slot * 3], w[slot * 3 + 1], w[slot * 3 + 2] } }, type, cov);
                    if (cov) cov->instructions++;
                    out += fmt("        %3u%s %s\n", slot, serialize ? "s" : " ", text.c_str());
                    listed.insert(slot);
                }
            }
            else if (op == CfOp::LoopStart || op == CfOp::LoopEnd || op == CfOp::CondCall || op == CfOp::CondJmp)
            {
                out += fmt(" target=cf%u", cf.target());
                if (op == CfOp::LoopStart || op == CfOp::LoopEnd) out += fmt(" i%u", cf.loopId());
                else if (!cf.isUnconditional())
                    out += cf.isPredicated() ? (cf.condition() ? " p" : " !p") : fmt(" %sb%u", cf.condition() ? "" : "!", cf.condBoolAddress());
                if (cov) cov->add(cov->features, op == CfOp::LoopStart ? "loops" : op == CfOp::CondCall ? "subroutine calls" : op == CfOp::CondJmp ? "jumps" : "loops");
                out += "\n";
            }
            else if (op == CfOp::Alloc)
            {
                const char* types[4] = { "none", type == ShaderType::Vertex ? "position" : "?", type == ShaderType::Vertex ? "interpolators" : "colors", "memory" };
                out += fmt(" %s size=%u\n", types[cf.allocType()], cf.allocSize());
                if (cov && cf.allocType() == 3) cov->add(cov->features, "memory export");
            }
            else
                out += "\n";
        }
        return out;
    }
}
