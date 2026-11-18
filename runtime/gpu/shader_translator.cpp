// Xenos -> GLSL shader translator. Instruction semantics follow Xenia's
// spirv_shader_translator*.cc and shader_translator.cc (BSD 3-clause, see
// docs/licenses/xenia-LICENSE.txt); see shader_translator.h for the resource layout.
#include "shader_translator.h"
#include "ucode.h"

#include <algorithm>
#include <cmath>
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
            char buf[1024];
            va_list ap;
            va_start(ap, f);
            vsnprintf(buf, sizeof(buf), f, ap);
            va_end(ap);
            return buf;
        }

        // GLSL helpers shared by all translated shaders
        const char* kPrelude = R"(
// Shader Model 3 multiply: +-0 * anything (including inf/NaN) = 0
float mul0(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }
vec4 mul0(vec4 a, vec4 b) { return mix(a * b, vec4(0.0), bvec4(vec4(equal(a, vec4(0.0))) + vec4(equal(b, vec4(0.0))))); }
// Shader Model 3 max/min: a >= b ? a : b (NaN handling as on the console)
float smax(float a, float b) { return a >= b ? a : b; }
float smin(float a, float b) { return a < b ? a : b; }
vec4 smax(vec4 a, vec4 b) { return mix(b, a, greaterThanEqual(a, b)); }
vec4 smin(vec4 a, vec4 b) { return mix(b, a, lessThan(a, b)); }
float dot4(vec4 a, vec4 b) { vec4 m = mul0(a, b); return m.x + m.y + m.z + m.w; }
float dot3(vec4 a, vec4 b) { vec4 m = mul0(a, b); return m.x + m.y + m.z; }
// cube: from direction d returns (tc, sc, 2 * major axis, face id)
vec4 cubeOp(vec3 d) {
  vec3 a = abs(d);
  if (a.z >= a.x && a.z >= a.y) return vec4(-d.y, d.z < 0.0 ? -d.x : d.x, 2.0 * d.z, d.z < 0.0 ? 5.0 : 4.0);
  if (a.y >= a.x) return vec4(d.y < 0.0 ? -d.z : d.z, d.x, 2.0 * d.y, d.y < 0.0 ? 3.0 : 2.0);
  return vec4(-d.y, d.x < 0.0 ? d.z : -d.z, 2.0 * d.x, d.x < 0.0 ? 1.0 : 0.0);
}
// tfetchCube coordinates (s, t in 1..2, face) back to a direction (inverse of cubeOp)
vec3 cubeDir(vec3 c) {
  float s = c.x * 2.0 - 3.0, t = c.y * 2.0 - 3.0;
  int f = clamp(int(floor(c.z)), 0, 5);
  if (f == 0) return vec3(1.0, -t, -s);
  if (f == 1) return vec3(-1.0, -t, s);
  if (f == 2) return vec3(s, 1.0, t);
  if (f == 3) return vec3(s, -1.0, -t);
  if (f == 4) return vec3(s, -t, 1.0);
  return vec3(-s, -t, -1.0);
}
uint gpuSwap(uint v, uint endian) {
  if (endian == 1u) return ((v >> 8) & 0x00FF00FFu) | ((v << 8) & 0xFF00FF00u);
  if (endian == 2u) return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
  if (endian == 3u) return (v >> 16) | (v << 16);
  return v;
}
float rcpc(float x) { return clamp(1.0 / x, -3.402823466e38, 3.402823466e38); }
float rcpf(float x) { float r = 1.0 / x; return isinf(r) ? (r < 0.0 ? -0.0 : 0.0) : r; }
float rsqc(float x) { return clamp(inversesqrt(x), -3.402823466e38, 3.402823466e38); }
float rsqf(float x) { float r = inversesqrt(x); return isinf(r) ? 0.0 : r; }
float logc(float x) { float r = log2(x); return isinf(r) && r < 0.0 ? -3.402823466e38 : r; }
)";

        // Memory export (Xenia's xenos.h describes the format): eA = (base address in dwords,
        // 2^23 + element index, 2^23 + format bit field, 2^23 + element count) as float bits; each
        // written eM# stores element index + #. Elements are packed per color format and number
        // format, red/blue swapped and endian swapped like resolves, and written straight to guest
        // memory (gmem: little-endian view of the big-endian guest bytes).
        const char* kMemExport = R"(
layout(set = 0, binding = 4, std430) buffer GuestPhysical { uint gmem[]; };
vec4 xe_eA = vec4(0.0);
vec4 xe_eM[5] = vec4[5](vec4(0.0), vec4(0.0), vec4(0.0), vec4(0.0), vec4(0.0));
uint xe_eMWritten = 0u;
uint xeSwap32(uint v, uint e) {
  if (e == 1u) return ((v >> 8) & 0x00FF00FFu) | ((v << 8) & 0xFF00FF00u);
  if (e == 2u || e >= 4u) return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
  if (e == 3u) return (v >> 16) | (v << 16);
  return v;
}
// One integer component of `bits` (< 32) bits; nfmt: 0 unorm, 1 snorm, 2 uint, 3 sint
uint xeComp(float v, uint bits, uint nfmt) {
  uint mask = (1u << bits) - 1u;
  if (nfmt == 0u) return uint(round(clamp(v, 0.0, 1.0) * float(mask)));
  if (nfmt == 1u) return uint(int(round(clamp(v, -1.0, 1.0) * float(mask >> 1)))) & mask;
  if (nfmt == 2u) return uint(clamp(round(v), 0.0, float(mask)));
  if (nfmt == 3u) { float h = float(1u << (bits - 1u)); return uint(int(clamp(round(v), -h, h - 1.0))) & mask; }
  return floatBitsToUint(v) & mask;
}
uint xeComp32(float v, uint nfmt) {
  if (nfmt == 2u) return uint(max(round(v), 0.0));
  if (nfmt == 3u) return uint(int(round(v)));
  return floatBitsToUint(v);
}
uint xeElementSize(uint fmt) {
  if (fmt == 2u) return 1u;                                          // 8
  if (fmt == 24u || fmt == 30u) return 2u;                           // 16, 16_FLOAT
  if (fmt == 6u || fmt == 7u || fmt == 25u || fmt == 31u || fmt == 33u || fmt == 36u) return 4u;
  if (fmt == 26u || fmt == 32u || fmt == 34u || fmt == 37u) return 8u;
  if (fmt == 35u || fmt == 38u) return 16u;                          // 32_32_32_32 (_FLOAT)
  return 0u;
}
void xeMemExportElement(uint addr, vec4 v, uint fmt, uint nfmt, uint endian, uint size) {
  uvec4 d = uvec4(0u);
  if (fmt == 2u) d.x = xeComp(v.x, 8u, nfmt);
  else if (fmt == 24u) d.x = xeComp(v.x, 16u, nfmt);
  else if (fmt == 30u) d.x = packHalf2x16(vec2(v.x, 0.0)) & 0xFFFFu;
  else if (fmt == 6u) d.x = xeComp(v.x, 8u, nfmt) | (xeComp(v.y, 8u, nfmt) << 8) | (xeComp(v.z, 8u, nfmt) << 16) | (xeComp(v.w, 8u, nfmt) << 24);
  else if (fmt == 7u) d.x = xeComp(v.x, 10u, nfmt) | (xeComp(v.y, 10u, nfmt) << 10) | (xeComp(v.z, 10u, nfmt) << 20) | (xeComp(v.w, 2u, nfmt) << 30);
  else if (fmt == 25u) d.x = xeComp(v.x, 16u, nfmt) | (xeComp(v.y, 16u, nfmt) << 16);
  else if (fmt == 31u) d.x = packHalf2x16(v.xy);
  else if (fmt == 26u) d.xy = uvec2(xeComp(v.x, 16u, nfmt) | (xeComp(v.y, 16u, nfmt) << 16), xeComp(v.z, 16u, nfmt) | (xeComp(v.w, 16u, nfmt) << 16));
  else if (fmt == 32u) d.xy = uvec2(packHalf2x16(v.xy), packHalf2x16(v.zw));
  else if (fmt == 36u) d.x = floatBitsToUint(v.x);
  else if (fmt == 37u) d.xy = floatBitsToUint(v.xy);
  else if (fmt == 38u) d = floatBitsToUint(v);
  else if (fmt == 33u) d.x = xeComp32(v.x, nfmt);
  else if (fmt == 34u) d.xy = uvec2(xeComp32(v.x, nfmt), xeComp32(v.y, nfmt));
  else if (fmt == 35u) d = uvec4(xeComp32(v.x, nfmt), xeComp32(v.y, nfmt), xeComp32(v.z, nfmt), xeComp32(v.w, nfmt));
  if (size >= 4u) {
    uint n = size / 4u, base = addr >> 2;
    uvec4 o;
    if (endian == 4u && n >= 2u) o = uvec4(xeSwap32(d.y, 2u), xeSwap32(d.x, 2u), xeSwap32(d.w, 2u), xeSwap32(d.z, 2u)); // 8in64
    else if (endian == 5u && n == 4u) o = uvec4(xeSwap32(d.w, 2u), xeSwap32(d.z, 2u), xeSwap32(d.y, 2u), xeSwap32(d.x, 2u)); // 8in128
    else o = uvec4(xeSwap32(d.x, endian), xeSwap32(d.y, endian), xeSwap32(d.z, endian), xeSwap32(d.w, endian));
    gmem[base] = o.x;
    if (n > 1u) gmem[base + 1u] = o.y;
    if (n > 2u) { gmem[base + 2u] = o.z; gmem[base + 3u] = o.w; }
  } else {
    // 8/16-bit element: the swap moves its byte lane (and swaps a 16-bit element's bytes)
    uint a = addr, v = d.x;
    if (size == 2u) {
      if (endian == 1u || endian == 2u) v = ((v >> 8) & 0xFFu) | ((v & 0xFFu) << 8);
      if (endian == 2u || endian == 3u) a ^= 2u;
    } else {
      a ^= endian == 1u ? 1u : endian == 2u ? 3u : endian == 3u ? 2u : 0u;
    }
    uint shift = (a & 3u) * 8u, mask = (size == 1u ? 0xFFu : 0xFFFFu) << shift;
    atomicAnd(gmem[a >> 2], ~mask);
    atomicOr(gmem[a >> 2], (v << shift) & mask);
  }
}
void xeMemExportFlush() {
  if (xe_eMWritten == 0u) return;
  uint ay = floatBitsToUint(xe_eA.y);
  if ((ay & 0xFF800000u) == 0x4B000000u) { // index must have exponent 23 and sign 0
    uint ax = floatBitsToUint(xe_eA.x), az = floatBitsToUint(xe_eA.z), aw = floatBitsToUint(xe_eA.w);
    uint index = ay & 0x7FFFFFu, count = aw & 0x7FFFFFu, base = (ax & 0x3FFFFFFFu) << 2;
    uint endian = az & 7u, fmt = (az >> 8) & 0x3Fu, nfmt = (az >> 16) & 7u, size = xeElementSize(fmt);
    bool swapRB = ((az >> 19) & 1u) != 0u;
    for (uint i = 0u; i < 5u; i++) {
      uint e = index + i, addr = base + e * size;
      if ((xe_eMWritten & (1u << i)) == 0u || e >= count || size == 0u || addr + size > 0x20000000u) continue;
      xeMemExportElement(addr, swapRB ? xe_eM[i].zyxw : xe_eM[i], fmt, nfmt, endian, size);
    }
  }
  xe_eMWritten = 0u;
}
)";

        struct Translator
        {
            const std::vector<uint32_t>& w;
            ShaderType type;
            TranslatedShader out;
            std::string body;
            std::set<uint32_t> fetchConstantsUsed;
            bool usesA0 = false, failed = false;
            int indent = 1;

            Translator(const std::vector<uint32_t>& words, ShaderType t) : w(words), type(t) {}

            void fail(const std::string& why)
            {
                if (!failed) out.error = why;
                failed = true;
            }
            void emit(const std::string& line) { body += std::string(indent * 2, ' ') + line + "\n"; }

            const char* constBlock() const { return type == ShaderType::Vertex ? "vsc" : "psc"; }

            // ---- ALU operands ----
            std::string source(const Alu& a, int i, int components /* 4, 1 (W), 2 (W,X) */)
            {
                uint32_t reg = a.srcReg(i);
                std::string v;
                bool absolute;
                if (a.srcIsTemp(i))
                {
                    v = (reg & 0x40) ? fmt("r[%u + aL]", reg & 0x3F) : fmt("r[%u]", reg & 0x3F);
                    absolute = reg & 0x80;
                }
                else
                {
                    std::string index = fmt("%u", reg);
                    if (a.srcConstIsAddressed(i))
                    {
                        if (a.constAddressRegisterRelative()) { index += " + a0"; usesA0 = true; }
                        else index += " + aL";
                    }
                    v = fmt("%s.c[%s]", constBlock(), index.c_str());
                    absolute = a.absConstants();
                }
                uint32_t sw = a.srcSwizzle(i);
                std::string comps;
                if (components == 4) for (uint32_t c = 0; c < 4; c++) comps += kComp[swizzledComponent(sw, c)];
                else if (components == 1) comps = kComp[swizzledComponent(sw, 3)];
                else { comps += kComp[swizzledComponent(sw, 3)]; comps += kComp[swizzledComponent(sw, 0)]; }
                if (comps != "xyzw") v += "." + comps;
                if (absolute) v = "abs(" + v + ")";
                if (a.srcNegate(i)) v = "-" + v;
                return v;
            }

            std::string exportTarget(uint32_t reg)
            {
                if (type == ShaderType::Vertex)
                {
                    if (reg == 62) return "o_pos";
                    if (reg == 63) return "o_pts";
                    if (reg < 16) return fmt("o_int[%u]", reg);
                }
                if (reg == 32) { out.memExport = true; return "xe_eA"; }              // memory export address
                if (reg >= 33 && reg <= 37) { out.memExport = true; return fmt("xe_eM[%u]", reg - 33); } // data
                else
                {
                    if (reg < 4) { out.colorOutputs |= 1u << reg; return fmt("o_col[%u]", reg); }
                    if (reg == 61) { out.writesDepth = true; return "o_depth"; }
                }
                fail(fmt("export to register %u (memory export or unknown) not supported", reg));
                return "o_unused";
            }

            void alu(const Alu& a)
            {
                const VectorOpInfo& vi = vectorOpInfo(a.vectorOp());
                const ScalarOpInfo& si = scalarOpInfo(a.scalarOp());
                bool exp = a.isExport();
                uint32_t vmask = a.vectorWriteMask(), smask = a.scalarWriteMask();
                if (a.isPredicated()) { emit(fmt("if (%sp) {", a.predCondition() ? "" : "!")); indent++; }

                // Vector op -> vres (sources read before any write, like the hardware)
                uint32_t vop = a.vectorOp();
                bool vectorActive = vmask || exp || (vop >= 24 && vop <= 27) || vop == 29;
                if (vectorActive)
                {
                    std::string s1 = vi.operands >= 1 ? source(a, 1, 4) : "", s2 = vi.operands >= 2 ? source(a, 2, 4) : "",
                                s3 = vi.operands >= 3 ? source(a, 3, 4) : "";
                    std::string e;
                    switch (vop)
                    {
                    case 0: e = s1 + " + " + s2; break;
                    case 1: e = "mul0(" + s1 + ", " + s2 + ")"; break;
                    case 2: e = "smax(" + s1 + ", " + s2 + ")"; break;
                    case 3: e = "smin(" + s1 + ", " + s2 + ")"; break;
                    case 4: e = "vec4(equal(" + s1 + ", " + s2 + "))"; break;
                    case 5: e = "vec4(greaterThan(" + s1 + ", " + s2 + "))"; break;
                    case 6: e = "vec4(greaterThanEqual(" + s1 + ", " + s2 + "))"; break;
                    case 7: e = "vec4(notEqual(" + s1 + ", " + s2 + "))"; break;
                    case 8: e = "fract(" + s1 + ")"; break;
                    case 9: e = "trunc(" + s1 + ")"; break;
                    case 10: e = "floor(" + s1 + ")"; break;
                    case 11: e = "mul0(" + s1 + ", " + s2 + ") + " + s3; break;
                    case 12: e = "mix(" + s3 + ", " + s2 + ", equal(" + s1 + ", vec4(0.0)))"; break;
                    case 13: e = "mix(" + s3 + ", " + s2 + ", greaterThanEqual(" + s1 + ", vec4(0.0)))"; break;
                    case 14: e = "mix(" + s3 + ", " + s2 + ", greaterThan(" + s1 + ", vec4(0.0)))"; break;
                    case 15: e = "vec4(dot4(" + s1 + ", " + s2 + "))"; break;
                    case 16: e = "vec4(dot3(" + s1 + ", " + s2 + "))"; break;
                    case 17: e = "vec4(mul0((" + s1 + ").x, (" + s2 + ").x) + mul0((" + s1 + ").y, (" + s2 + ").y) + (" + s3 + ").x)"; break;
                    case 18:
                        // Operand components z_xy: the direction is (src.z, src.w, src.x) of the swizzled operand
                        emit("cube_src = " + s1 + ";");
                        e = "cubeOp(vec3(cube_src.z, cube_src.w, cube_src.x))";
                        break;
                    case 19: e = "vec4(smax(smax((" + s1 + ").x, (" + s1 + ").y), smax((" + s1 + ").z, (" + s1 + ").w)))"; break;
                    case 20: case 21: case 22: case 23:
                    {
                        // setp_{eq,ne,gt,ge}_push, as Xenia's ucode.h describes them:
                        // p0 = src0.w == 0 && (src1.w <op> 0); dest = (src0.x == 0 && (src1.x <op> 0)) ? 0 : src0.x + 1
                        static const char* cmp[4] = { "==", "!=", ">", ">=" };
                        std::string c = cmp[vop - 20];
                        emit("p = (" + s1 + ").w == 0.0 && (" + s2 + ").w " + c + " 0.0;");
                        e = "vec4(((" + s1 + ").x == 0.0 && (" + s2 + ").x " + c + " 0.0) ? 0.0 : (" + s1 + ").x + 1.0)";
                        break;
                    }
                    case 24: case 25: case 26: case 27:
                    {
                        static const char* cmp[4] = { "equal", "greaterThan", "greaterThanEqual", "notEqual" };
                        emit(std::string("if (any(") + cmp[vop - 24] + "(" + s1 + ", " + s2 + "))) discard;");
                        out.kills = true;
                        e = "vec4(0.0)";
                        break;
                    }
                    case 28: e = "vec4(1.0, mul0((" + s1 + ").y, (" + s2 + ").y), (" + s1 + ").z, (" + s2 + ").w)"; break;
                    case 29:
                        emit("a0 = int(clamp(floor((" + s1 + ").w + 0.5), -256.0, 255.0));");
                        usesA0 = true;
                        e = "smax(" + s1 + ", " + s2 + ")";
                        break;
                    default:
                        fail(fmt("vector op %s not supported", vi.name));
                        e = "vec4(0.0)";
                    }
                    if (a.vectorClamp()) e = "clamp(" + e + ", 0.0, 1.0)";
                    emit("vres = " + e + ";");
                }

                // Scalar op -> sres, and ps
                uint32_t sop = a.scalarOp();
                // Every scalar op except retain_prev updates ps (the previous-scalar register that
                // *_prev ops read), even when nothing is written to a register
                bool scalarActive = sop != 50;
                if (scalarActive)
                {
                    std::string x, y; // a (W) and b (X)
                    if (si.operands == 1)
                    {
                        if (si.twoComponent)
                        {
                            emit("s_src = vec2(" + source(a, 3, 2) + ");");
                            x = "s_src.x"; y = "s_src.y";
                        }
                        else
                            x = "(" + source(a, 3, 1) + ")";
                    }
                    else if (si.operands == 2)
                    {
                        std::string neg = a.srcNegate(3) ? "-" : "";
                        std::string idx = fmt("%u", a.srcReg(3));
                        if (a.srcConstIsAddressed(3)) { idx += a.constAddressRegisterRelative() ? " + a0" : " + aL"; usesA0 |= a.constAddressRegisterRelative(); }
                        std::string c = fmt("%s.c[%s].%c", constBlock(), idx.c_str(), kComp[swizzledComponent(a.srcSwizzle(3), 3)]);
                        std::string r = fmt("r[%u].%c", a.scalarConstOpTempReg(), kComp[swizzledComponent(a.srcSwizzle(3), 0)]);
                        if (a.absConstants()) { c = "abs(" + c + ")"; r = "abs(" + r + ")"; }
                        x = "(" + neg + c + ")"; y = "(" + neg + r + ")";
                    }
                    std::string e;
                    switch (sop)
                    {
                    case 0: e = x + " + " + y; break;                       // adds
                    case 1: e = x + " + ps"; break;                         // adds_prev
                    case 2: e = "mul0(" + x + ", " + y + ")"; break;        // muls
                    case 3: e = "mul0(" + x + ", ps)"; break;               // muls_prev
                    case 4: e = "((ps == -3.402823466e38 || isinf(ps) || isnan(ps) || isinf(" + y + ") || isnan(" + y + ") || " + y +
                                " <= 0.0) ? -3.402823466e38 : mul0(" + x + ", ps))"; break; // muls_prev2
                    case 5: e = "smax(" + x + ", " + y + ")"; break;         // maxs
                    case 6: e = "smin(" + x + ", " + y + ")"; break;         // mins
                    case 7: e = "(" + x + " == 0.0 ? 1.0 : 0.0)"; break;    // seqs
                    case 8: e = "(" + x + " > 0.0 ? 1.0 : 0.0)"; break;     // sgts
                    case 9: e = "(" + x + " >= 0.0 ? 1.0 : 0.0)"; break;    // sges
                    case 10: e = "(" + x + " != 0.0 ? 1.0 : 0.0)"; break;   // snes
                    case 11: e = "fract(" + x + ")"; break;
                    case 12: e = "trunc(" + x + ")"; break;
                    case 13: e = "floor(" + x + ")"; break;
                    case 14: e = "exp2(" + x + ")"; break;
                    case 15: e = "logc(" + x + ")"; break;
                    case 16: e = "log2(" + x + ")"; break;
                    case 17: e = "rcpc(" + x + ")"; break;
                    case 18: e = "rcpf(" + x + ")"; break;
                    case 19: e = "(1.0 / " + x + ")"; break;
                    case 20: e = "rsqc(" + x + ")"; break;
                    case 21: e = "rsqf(" + x + ")"; break;
                    case 22: e = "inversesqrt(" + x + ")"; break;
                    case 23: emit("a0 = int(clamp(floor(" + x + " + 0.5), -256.0, 255.0));"); usesA0 = true; e = "smax(" + x + ", " + y + ")"; break;
                    case 24: emit("a0 = int(clamp(floor(" + x + "), -256.0, 255.0));"); usesA0 = true; e = "smax(" + x + ", " + y + ")"; break;
                    case 25: e = x + " - " + y; break;                      // subs
                    case 26: e = x + " - ps"; break;                        // subs_prev
                    case 27: emit("p = " + x + " == 0.0;"); e = "(p ? 0.0 : 1.0)"; break; // setp_eq
                    case 28: emit("p = " + x + " != 0.0;"); e = "(p ? 0.0 : 1.0)"; break; // setp_ne
                    case 29: emit("p = " + x + " > 0.0;"); e = "(p ? 0.0 : 1.0)"; break;  // setp_gt
                    case 30: emit("p = " + x + " >= 0.0;"); e = "(p ? 0.0 : 1.0)"; break; // setp_ge
                    // setp_inv, setp_pop, setp_rstr as Xenia's ucode.h describes them (x = src0.a)
                    case 31: emit("p = " + x + " == 1.0;"); e = "(p ? 0.0 : (" + x + " == 0.0 ? 1.0 : " + x + "))"; break; // setp_inv
                    case 32: emit("p = " + x + " - 1.0 <= 0.0;"); e = "(p ? 0.0 : " + x + " - 1.0)"; break;                 // setp_pop
                    case 33: emit("p = false;"); e = "3.402823466e38"; break;              // setp_clr
                    case 34: emit("p = " + x + " == 0.0;"); e = "(p ? 0.0 : " + x + ")"; break;                                // setp_rstr
                    case 35: case 36: case 37: case 38:
                    {
                        static const char* cmp[4] = { "==", ">", ">=", "!=" };
                        emit("if (" + x + " " + cmp[sop - 35] + " 0.0) discard;");
                        out.kills = true;
                        e = "0.0";
                        break;
                    }
                    case 39: emit("if (" + x + " == 1.0) discard;"); e = "0.0"; out.kills = true; break; // kills_one
                    case 40: e = "sqrt(" + x + ")"; break;
                    case 42: case 43: e = "mul0(" + x + ", " + y + ")"; break; // mulsc
                    case 44: case 45: e = x + " + " + y; break;               // addsc
                    case 46: case 47: e = x + " - " + y; break;               // subsc
                    case 48: e = "sin(" + x + ")"; break;
                    case 49: e = "cos(" + x + ")"; break;
                    default:
                        fail(fmt("scalar op %s not supported", si.name));
                        e = "0.0";
                    }
                    if (a.scalarClamp()) e = "clamp(" + e + ", 0.0, 1.0)";
                    emit("ps = " + e + ";");
                }

                // Write results
                if (exp)
                {
                    uint32_t reg = a.vectorDest();
                    if (reg == 32)
                    {
                        // A new eA closes the export in progress; remember the stream constant
                        emit("xeMemExportFlush();");
                        if (a.vectorOp() == 11 && !a.srcIsTemp(3)) out.memExportStreams[(a.srcReg(3) & 0xFF) / 32] |= 1u << (a.srcReg(3) % 32);
                    }
                    std::string t = exportTarget(reg);
                    for (uint32_t c = 0; c < 4; c++)
                    {
                        bool v = (vmask >> c) & 1, s = (smask >> c) & 1;
                        if (v && s) emit(fmt("%s.%c = 1.0;", t.c_str(), kComp[c]));
                        else if (v) emit(fmt("%s.%c = vres.%c;", t.c_str(), kComp[c], kComp[c]));
                        else if (s) emit(fmt("%s.%c = ps;", t.c_str(), kComp[c]));
                        else if (a.scalarDestRelative()) emit(fmt("%s.%c = 0.0;", t.c_str(), kComp[c]));
                    }
                    if (reg >= 33 && reg <= 37) emit(fmt("xe_eMWritten |= %uu;", 1u << (reg - 33)));
                }
                else
                {
                    if (((vmask & 1) && (a.vectorDest() == 0 || a.vectorDestRelative())) ||
                        ((smask & 1) && (a.scalarDest() == 0 || a.scalarDestRelative())))
                        r0xWritten = true;
                    std::string vd = a.vectorDestRelative() ? fmt("r[%u + aL]", a.vectorDest()) : fmt("r[%u]", a.vectorDest());
                    std::string sd = a.scalarDestRelative() ? fmt("r[%u + aL]", a.scalarDest()) : fmt("r[%u]", a.scalarDest());
                    for (uint32_t c = 0; c < 4; c++)
                        if ((vmask >> c) & 1) emit(fmt("%s.%c = vres.%c;", vd.c_str(), kComp[c], kComp[c]));
                    for (uint32_t c = 0; c < 4; c++)
                        if ((smask >> c) & 1) emit(fmt("%s.%c = ps;", sd.c_str(), kComp[c]));
                }
                if (a.isPredicated()) { indent--; emit("}"); }
            }

            // ---- Fetch ----
            void writeFetchResult(const Fetch& f, const std::string& value)
            {
                std::string d = f.dstRegRelative() ? fmt("r[%u + aL]", f.dstReg()) : fmt("r[%u]", f.dstReg());
                emit("fres = " + value + ";");
                if ((f.dstReg() == 0 || f.dstRegRelative()) && (f.dstSwizzle() & 7) != 7) r0xWritten = true;
                for (uint32_t c = 0; c < 4; c++)
                {
                    uint32_t s = (f.dstSwizzle() >> (3 * c)) & 7;
                    if (s == 7) continue;                      // keep
                    if (s < 4) emit(fmt("%s.%c = fres.%c;", d.c_str(), kComp[c], kComp[s]));
                    else emit(fmt("%s.%c = %s;", d.c_str(), kComp[c], s == 5 ? "1.0" : "0.0"));
                }
            }

            uint32_t vfetchLastSlot = 0;
            bool r0xWritten = false; // r0.x no longer holds the vertex index
            void vfetch(const Fetch& f)
            {
                if (type != ShaderType::Vertex) { fail("vertex fetch in a pixel shader"); return; }
                if (f.isPredicated()) { emit(fmt("if (%sp) {", f.predCondition() ? "" : "!")); indent++; }
                if (!f.vfIsMini())
                {
                    uint32_t slot = f.vfFetchSlot();
                    vfetchLastSlot = slot;
                    fetchConstantsUsed.insert(slot);
                    out.vertexFetchSlots[slot / 32] |= 1u << (slot % 32);
                    // Fetch constant slot -> 2 dwords in fetchc (6-dword texture slots hold 3 vertex slots)
                    uint32_t dw = slot * 2;
                    // Index: floor, or round to nearest when the instruction asks for it
                    emit(fmt("vf_base = fetchc[%u].%c + uint(int(floor(r[%u].%c%s))) * %uu;", dw / 4, kComp[dw % 4], f.srcReg(),
                             kComp[f.vfSrcSwizzle()], f.vfIsIndexRounded() ? " + 0.5" : "", f.vfStride()));
                    if (f.srcReg() != 0 || f.vfSrcSwizzle() != 0 || r0xWritten) out.vertexIndexDirect = false;
                    out.vertexStride[slot] = uint8_t(std::max<uint32_t>(out.vertexStride[slot], f.vfStride()));
                    emit(fmt("vf_endian = fetchc[%u].%c & 3u;", (dw + 1) / 4, kComp[(dw + 1) % 4]));
                }
                uint32_t format = f.vfFormat();
                int32_t off = f.vfOffset();
                {
                    uint32_t size = (format == 38) ? 4 : (format == 57) ? 3 : (format == 37 || format == 32 || format == 26) ? 2 : 1;
                    uint8_t& extent = out.vertexExtent[vfetchLastSlot];
                    extent = uint8_t(std::clamp<int32_t>(std::max<int32_t>(extent, off + int32_t(size)), 0, 255));
                }
                auto word = [&](int i) { return fmt("gpuSwap(mem[vf_base + %du], vf_endian)", off + i); };
                bool isSigned = f.vfFormatCompAll(), isInt = f.vfNumFormatAll();
                std::string v;
                auto fixed = [&](const char* unsignedExpr, const char* signedExpr, const char* scale) {
                    // normalized: divide by (2^n - 1) or (2^(n-1) - 1); integer: as is
                    std::string e = isSigned ? signedExpr : unsignedExpr;
                    if (!isInt) e = isSigned ? "max(" + e + " / " + scale + ", vec4(-1.0))" : e + " / " + scale;
                    return e;
                };
                switch (format)
                {
                case 36: v = "vec4(uintBitsToFloat(" + word(0) + "), 0.0, 0.0, 0.0)"; break;
                case 37: v = "vec4(uintBitsToFloat(" + word(0) + "), uintBitsToFloat(" + word(1) + "), 0.0, 0.0)"; break;
                case 57: v = "vec4(uintBitsToFloat(" + word(0) + "), uintBitsToFloat(" + word(1) + "), uintBitsToFloat(" + word(2) + "), 0.0)"; break;
                case 38: v = "vec4(uintBitsToFloat(" + word(0) + "), uintBitsToFloat(" + word(1) + "), uintBitsToFloat(" + word(2) +
                             "), uintBitsToFloat(" + word(3) + "))"; break;
                case 31: v = "vec4(unpackHalf2x16(" + word(0) + "), 0.0, 0.0)"; break;
                case 32: v = "vec4(unpackHalf2x16(" + word(0) + "), unpackHalf2x16(" + word(1) + "))"; break;
                case 6:
                    emit("vf_w = " + word(0) + ";");
                    v = fixed("vec4(uvec4(vf_w, vf_w >> 8, vf_w >> 16, vf_w >> 24) & 0xFFu)",
                              "vec4(ivec4(vf_w << 24, vf_w << 16, vf_w << 8, vf_w) >> 24)", "vec4(255.0, 255.0, 255.0, 255.0)");
                    if (isSigned && !isInt) v = "max(vec4(ivec4(vf_w << 24, vf_w << 16, vf_w << 8, vf_w) >> 24) / 127.0, vec4(-1.0))";
                    break;
                case 7:
                    emit("vf_w = " + word(0) + ";");
                    if (isSigned)
                        v = isInt ? "vec4(ivec4(vf_w << 22, vf_w << 12, vf_w << 2, vf_w) >> ivec4(22, 22, 22, 30))"
                                  : "max(vec4(ivec4(vf_w << 22, vf_w << 12, vf_w << 2, vf_w) >> ivec4(22, 22, 22, 30)) / vec4(511.0, 511.0, 511.0, 1.0), vec4(-1.0))";
                    else
                        v = isInt ? "vec4(uvec4(vf_w, vf_w >> 10, vf_w >> 20, vf_w >> 30) & uvec4(1023u, 1023u, 1023u, 3u))"
                                  : "vec4(uvec4(vf_w, vf_w >> 10, vf_w >> 20, vf_w >> 30) & uvec4(1023u, 1023u, 1023u, 3u)) / vec4(1023.0, 1023.0, 1023.0, 3.0)";
                    break;
                case 25:
                    emit("vf_w = " + word(0) + ";");
                    if (isSigned)
                        v = isInt ? "vec4(vec2(ivec2(vf_w << 16, vf_w) >> 16), 0.0, 0.0)"
                                  : "vec4(max(vec2(ivec2(vf_w << 16, vf_w) >> 16) / 32767.0, vec2(-1.0)), 0.0, 0.0)";
                    else
                        v = isInt ? "vec4(vec2(uvec2(vf_w, vf_w >> 16) & 0xFFFFu), 0.0, 0.0)" : "vec4(vec2(uvec2(vf_w, vf_w >> 16) & 0xFFFFu) / 65535.0, 0.0, 0.0)";
                    break;
                case 26:
                    emit("vf_w = " + word(0) + "; vf_w2 = " + word(1) + ";");
                    if (isSigned)
                        v = isInt ? "vec4(ivec4(vf_w << 16, vf_w, vf_w2 << 16, vf_w2) >> 16)"
                                  : "max(vec4(ivec4(vf_w << 16, vf_w, vf_w2 << 16, vf_w2) >> 16) / 32767.0, vec4(-1.0))";
                    else
                        v = isInt ? "vec4(uvec4(vf_w, vf_w >> 16, vf_w2, vf_w2 >> 16) & 0xFFFFu)"
                                  : "vec4(uvec4(vf_w, vf_w >> 16, vf_w2, vf_w2 >> 16) & 0xFFFFu) / 65535.0";
                    break;
                default:
                    fail(fmt("vertex format %s not supported", vertexFormatName(format)));
                    v = "vec4(0.0)";
                }
                if (f.vfExpAdjust()) v = fmt("(%s) * %.9g", v.c_str(), std::ldexp(1.0, f.vfExpAdjust()));
                writeFetchResult(f, v);
                if (f.isPredicated()) { indent--; emit("}"); }
            }

            void tfetch(const Fetch& f)
            {
                bool weights = f.op() == uint32_t(FetchOp::GetTextureWeights);
                if (f.op() != uint32_t(FetchOp::TextureFetch) && !weights) { fail(fmt("fetch op %s not supported", fetchName(f.op()))); return; }
                uint32_t slot = f.constIndex(), dim = f.tfDimension();
                if (out.textureSlots & (1u << slot) && out.textureDims[slot] != dim)
                    { fail(fmt("texture slot %u used with two dimensions", slot)); return; }
                out.textureSlots |= 1u << slot;
                out.textureDims[slot] = dim;
                if (f.isPredicated()) { emit(fmt("if (%sp) {", f.predCondition() ? "" : "!")); indent++; }
                std::string src = fmt("r[%u]%s", f.srcReg(), f.srcRegRelative() ? "" : "");
                if (f.srcRegRelative()) src = fmt("r[%u + aL]", f.srcReg());
                std::string coords;
                for (int c = 0; c < 3; c++) coords += kComp[(f.tfSrcSwizzle() >> (2 * c)) & 3];
                std::string t = fmt("tex%u", slot);
                std::string coord;
                switch (dim)
                {
                case 0: coord = src + "." + coords.substr(0, 1); break;
                case 1: coord = src + "." + coords.substr(0, 2); break;
                case 2: coord = src + "." + coords; break;
                default: coord = "cubeDir(" + src + "." + coords + ")"; break;
                }
                // Fetch offsets: half-texel steps, added in texel space (Xenia's spirv translator, which
                // also adds 1.5/1024 texel so point sampling between texels picks the console's texel)
                bool hasOffset = f.tfOffsetX() || f.tfOffsetY() || f.tfOffsetZ();
                if (hasOffset && dim == 3) fail("texture fetch offsets on cube maps not supported");
                if (weights)
                {
                    // getWeights: bilinear lerp factors, fract(coordinate in texels + offset - 0.5) per
                    // component (Xenia), the rest 0
                    if (dim == 3) { fail("getWeights on cube maps not supported"); return; }
                    const double eps = 1.5 / 1024.0;
                    int n = dim == 0 ? 1 : dim == 1 ? 2 : 3;
                    int32_t offs[3] = { f.tfOffsetX(), f.tfOffsetY(), f.tfOffsetZ() };
                    emit("fres = vec4(0.0);");
                    emit("cube_src.xyz = vec3(textureSize(" + t + ", 0)" + (dim == 0 ? ", 1.0, 1.0" : dim == 1 ? ", 1.0" : "") + ");");
                    for (int c = 0; c < n; c++)
                    {
                        std::string coordC = src + "." + coords[c];
                        if (!f.tfCoordDenormalized()) coordC += fmt(" * cube_src.%c", kComp[c]);
                        emit(fmt("fres.%c = fract(%s + %.9g);", kComp[c], coordC.c_str(), offs[c] * 0.5 + eps - 0.5));
                    }
                    writeFetchResult(f, "fres");
                    if (f.isPredicated()) { indent--; emit("}"); }
                    return;
                }
                const char* vecType = dim == 0 ? "float" : dim == 1 ? "vec2" : "vec3";
                std::string size = std::string(vecType) + "(textureSize(" + t + ", 0))";
                std::string offset;
                if (hasOffset)
                {
                    const double eps = 1.5 / 1024.0;
                    double o[3] = { f.tfOffsetX() * 0.5 + eps, f.tfOffsetY() * 0.5 + eps, f.tfOffsetZ() * 0.5 + eps };
                    offset = dim == 0 ? fmt("%.9g", o[0]) : dim == 1 ? fmt("vec2(%.9g, %.9g)", o[0], o[1])
                                                                     : fmt("vec3(%.9g, %.9g, %.9g)", o[0], o[1], o[2]);
                }
                if (dim != 3)
                {
                    if (f.tfCoordDenormalized())
                        coord = "(" + coord + (hasOffset ? " + " + offset : "") + ") / " + size;
                    else if (hasOffset)
                        coord = "(" + coord + " + " + offset + " / " + size + ")";
                }
                std::string sample;
                bool lod = type == ShaderType::Vertex;
                if (lod) sample = "textureLod(" + t + ", " + coord + ", 0.0)";
                else if (f.tfLodBias()) sample = fmt("texture(%s, %s, %.4f)", t.c_str(), coord.c_str(), f.tfLodBias() / 16.0);
                else sample = "texture(" + t + ", " + coord + ")";
                writeFetchResult(f, sample);
                if (f.isPredicated()) { indent--; emit("}"); }
            }

            void exec(const Cf& cf)
            {
                for (uint32_t k = 0; k < cf.execCount(); k++)
                {
                    uint32_t slot = cf.execAddress() + k;
                    if (size_t(slot) * 3 + 3 > w.size()) { fail("exec slot out of range"); return; }
                    const uint32_t* d = &w[slot * 3];
                    if ((cf.execSequence() >> (2 * k)) & 1)
                    {
                        Fetch f{ { d[0], d[1], d[2] } };
                        if (f.op() == uint32_t(FetchOp::VertexFetch)) vfetch(f); else tfetch(f);
                    }
                    else
                        alu(Alu{ { d[0], d[1], d[2] } });
                }
            }

            // One control flow instruction other than a jump: exec blocks (conditional on a bool
            // constant or the predicate), allocs and no-ops. False if it is not supported.
            bool controlFlow(const Cf& cf)
            {
                switch (cf.op())
                {
                case CfOp::Alloc:
                    emit("xeMemExportFlush();"); // an alloc ends the memory export in progress
                    return true;
                case CfOp::Nop: case CfOp::MarkVsFetchDone:
                    return true;
                case CfOp::Exec: case CfOp::ExecEnd:
                    exec(cf);
                    return true;
                case CfOp::CondExec: case CfOp::CondExecEnd:
                {
                    uint32_t b = cf.condBoolAddress();
                    emit(fmt("if (((boolc[%u].%c >> %uu) & 1u) == %uu) {", b / 128, kComp[(b % 128) / 32], b % 32, cf.condition() ? 1 : 0));
                    indent++; exec(cf); indent--; emit("}");
                    return true;
                }
                case CfOp::CondExecPred: case CfOp::CondExecPredEnd: case CfOp::CondExecPredClean: case CfOp::CondExecPredCleanEnd:
                    emit(fmt("if (%sp) {", cf.condition() ? "" : "!"));
                    indent++; exec(cf); indent--; emit("}");
                    return true;
                default:
                    return false;
                }
            }

            void translate()
            {
                // The control flow instructions: two per three words, up to where the exec code starts
                std::vector<Cf> cfs;
                size_t cfEnd = w.size();
                for (size_t i = 0; i + 3 <= w.size() && i < cfEnd; i += 3)
                {
                    Cf pair[2];
                    unpackCf(&w[i], pair);
                    for (const Cf& cf : pair)
                    {
                        if (isExec(cf.op()) && cf.execCount() && cf.execAddress() * 3 < cfEnd) cfEnd = cf.execAddress() * 3;
                        cfs.push_back(cf);
                    }
                }
                bool jumps = std::any_of(cfs.begin(), cfs.end(), [](const Cf& cf) { return cf.op() == CfOp::CondJmp; });

                if (!jumps)
                {
                    // Straight-line exec blocks until the first one that ends the shader
                    bool ended = false;
                    for (const Cf& cf : cfs)
                    {
                        if (!controlFlow(cf)) fail(fmt("control flow %s not supported yet", cfName(cf.op())));
                        if ((ended = endsShader(cf.op()))) break;
                    }
                    if (!ended) fail("shader has no exec-end");
                    return;
                }

                // With jumps (jmp/cjmp): a state machine over the control flow instructions, as
                // Xenia's translators do - xe_pc is the index of the next instruction, an
                // instruction that ends the shader sets it to -1 so the code after the loop (the
                // color and depth writes) still runs.
                bool anyEnd = false;
                emit("int xe_pc = 0;");
                emit("while (xe_pc >= 0) {");
                indent++;
                emit("switch (xe_pc) {");
                for (size_t k = 0; k < cfs.size(); k++)
                {
                    const Cf& cf = cfs[k];
                    emit(fmt("case %u: {", uint32_t(k)));
                    indent++;
                    if (cf.op() == CfOp::CondJmp)
                    {
                        // Target: an absolute control flow index. Unconditional, on the predicate, or on
                        // a bool constant, compared with the instruction's condition bit.
                        uint32_t target = cf.target();
                        if (target >= cfs.size()) fail(fmt("jump to control flow %u past the end (%zu)", target, cfs.size()));
                        if (cf.isUnconditional())
                            emit(fmt("xe_pc = %u; break;", target));
                        else if (cf.isPredicated())
                            emit(fmt("xe_pc = (p == %s) ? %u : %u; break;", cf.condition() ? "true" : "false", target, uint32_t(k + 1)));
                        else
                        {
                            uint32_t b = cf.condBoolAddress();
                            emit(fmt("xe_pc = (((boolc[%u].%c >> %uu) & 1u) == %uu) ? %u : %u; break;", b / 128, kComp[(b % 128) / 32], b % 32,
                                     cf.condition() ? 1 : 0, target, uint32_t(k + 1)));
                        }
                    }
                    else
                    {
                        if (!controlFlow(cf)) fail(fmt("control flow %s not supported yet", cfName(cf.op())));
                        bool end = endsShader(cf.op());
                        anyEnd |= end;
                        emit(end ? std::string("xe_pc = -1; break;") : fmt("xe_pc = %u; break;", uint32_t(k + 1)));
                    }
                    indent--;
                    emit("}");
                }
                emit("default: xe_pc = -1; break;");
                emit("}");
                indent--;
                emit("}");
                if (!anyEnd) fail("shader has no exec-end");
            }

            std::string assemble()
            {
                std::string s = "#version 450\n// Generated by backbreaker-recomp's Xenos shader translator\n";
                // Which color slots the render pass this shader is used in has an image for: writes
                // to the outputs it does not are left out, they would be undefined values (the
                // renderer compiles a variant per mask it needs).
                s += "#ifndef BB_COLOR_MASK\n#define BB_COLOR_MASK 15\n#endif\n";
                // Which interpolators a pixel shader reads at the centroid of the pixel's covered samples
                // rather than at its center (the renderer compiles a variant per mask; issue #166)
                if (type != ShaderType::Vertex) s += "#ifndef BB_CENTROID\n#define BB_CENTROID 0\n#endif\n";
                s += kPrelude;
                s += "layout(set = 0, binding = 0, std430) readonly buffer GuestMemory { uint mem[]; };\n";
                s += "layout(set = 0, binding = 1, std140) uniform VsConstants { vec4 c[256]; } vsc;\n";
                s += "layout(set = 0, binding = 2, std140) uniform PsConstants { vec4 c[256]; } psc;\n";
                s += "layout(set = 0, binding = 3, std140) uniform OtherConstants { uvec4 boolc[2]; uvec4 loopc[8]; uvec4 fetchc[48]; };\n";
                // System constants: guest -> host viewport transform, vertex index offset, alpha test
                s += "layout(push_constant) uniform Push { vec4 ndcScale; vec4 ndcOffset; int vertexIndexOffset; uint flags; uint alphaFunc; float alphaRef; } pc;\n";
                s += out.memExport ? kMemExport : "void xeMemExportFlush() {}\n";
                static const char* samplers[4] = { "sampler1D", "sampler2D", "sampler3D", "samplerCube" };
                for (uint32_t i = 0; i < 32; i++)
                    if (out.textureSlots & (1u << i))
                        s += fmt("layout(set = 1, binding = %u) uniform %s tex%u;\n", i, samplers[out.textureDims[i]], i);
                if (type == ShaderType::Vertex)
                {
                    s += "layout(location = 0) out XeInterpolators { vec4 v[16]; } xe_out;\n";
                    // Multipass rendering (decals and markings drawn on the field with a less-equal
                    // depth test) needs the same position from the same inputs in every shader; without
                    // this the compiler may contract the math differently per shader (issue #54)
                    s += "invariant gl_Position;\n";
                }
                else
                {
                    s += "layout(location = 0) in XeInterpolators { vec4 v[16]; } xe_in;\n";
                    for (uint32_t i = 0; i < 4; i++)
                        if (out.colorOutputs & (1u << i)) s += fmt("#if (BB_COLOR_MASK & %u)\nlayout(location = %u) out vec4 frag_col%u;\n#endif\n", 1u << i, i, i);
                }
                s += "\nvoid main() {\n";
                s += "  vec4 r[64];\n  for (int i = 0; i < 64; i++) r[i] = vec4(0.0);\n";
                s += "  vec4 vres = vec4(0.0), fres = vec4(0.0), cube_src = vec4(0.0);\n  vec2 s_src = vec2(0.0);\n  float ps = 0.0;\n  bool p = false;\n  int a0 = 0, aL = 0;\n";
                if (type == ShaderType::Vertex)
                {
                    s += "  uint vf_base = 0u, vf_endian = 0u, vf_w = 0u, vf_w2 = 0u;\n";
                    s += "  vec4 o_pos = vec4(0.0), o_pts = vec4(0.0);\n  vec4 o_int[16];\n  for (int i = 0; i < 16; i++) o_int[i] = vec4(0.0);\n";
                    s += "  r[0].x = float(gl_VertexIndex + pc.vertexIndexOffset); // vertex index, with VGT_INDX_OFFSET\n";
                }
                else
                {
                    s += "  vec4 o_col[4] = vec4[4](vec4(0.0), vec4(0.0), vec4(0.0), vec4(0.0));\n  vec4 o_depth = vec4(gl_FragCoord.z);\n";
                    // With MSAA, the game picks per interpolator whether an edge pixel interpolates at its
                    // center or at the centroid of its covered samples (SQ_CONTEXT_MISC sc_sample_cntl,
                    // SQ_INTERPOLATOR_CNTL sampling_pattern). At the center, a pixel only partly covered
                    // reads the interpolator extrapolated outside the primitive: on the field's grass
                    // blades squeezed thin around the controlled player, a clip-space W the shader
                    // divides by went to 0 and left a row of black pixels (issue #166). The interface
                    // stays one array in every stage; the ones in BB_CENTROID are read at the centroid.
                    for (uint32_t i = 0; i < 16; i++)
                        s += fmt("#if (BB_CENTROID & 0x%X)\n  r[%u] = interpolateAtCentroid(xe_in.v[%u]);\n#else\n  r[%u] = xe_in.v[%u];\n#endif\n",
                                 1u << i, i, i, i, i);
                }
                s += "  vec4 o_unused = vec4(0.0);\n";
                s += body;
                s += "  xeMemExportFlush();\n";
                if (type == ShaderType::Vertex)
                {
                    // Undo the guest position formats (PA_CL_VTE_CNTL), then guest viewport -> host NDC
                    s += "  vec4 pos = o_pos;\n";
                    s += "  if ((pc.flags & 4u) == 0u) pos.w = 1.0 / pos.w; // W given as 1/W\n";
                    s += "  if ((pc.flags & 1u) != 0u) pos.xy *= pos.w;     // XY given divided by W\n";
                    s += "  if ((pc.flags & 2u) != 0u) pos.z *= pos.w;      // Z given divided by W\n";
                    s += "  pos.xyz = pos.xyz * pc.ndcScale.xyz + pc.ndcOffset.xyz * pos.w;\n";
                    s += "  gl_Position = pos;\n  gl_PointSize = 1.0;\n";
                    s += "  for (int i = 0; i < 16; i++) xe_out.v[i] = o_int[i];\n";
                }
                else
                {
                    // Alpha test (RB_COLORCONTROL), compare functions as VkCompareOp; 7 = always
                    if (out.colorOutputs & 1)
                        s += "  if (pc.alphaFunc != 7u) {\n    float a = o_col[0].w, ref = pc.alphaRef;\n"
                             "    bool pass = pc.alphaFunc == 1u ? a < ref : pc.alphaFunc == 2u ? a == ref : pc.alphaFunc == 3u ? a <= ref :\n"
                             "                pc.alphaFunc == 4u ? a > ref : pc.alphaFunc == 5u ? a != ref : pc.alphaFunc == 6u ? a >= ref : false;\n"
                             "    if (!pass) discard;\n  }\n";
                    for (uint32_t i = 0; i < 4; i++)
                        if (out.colorOutputs & (1u << i)) s += fmt("#if (BB_COLOR_MASK & %u)\n  frag_col%u = o_col[%u];\n#endif\n", 1u << i, i, i);
                    if (out.writesDepth) s += "  gl_FragDepth = o_depth.x;\n";
                }
                s += "}\n";
                return s;
            }
        };
    }

    PositionFetch analyzePositionFetch(const std::vector<uint32_t>& w)
    {
        // Straight-line code only (exec / alloc / nop, no predication), tracking for each temp
        // register whether it holds a vertex fetch result, and through which component swizzle
        struct Reg { bool fetched = false; uint32_t slot = 0, stride = 0, format = 0; int32_t offset = 0; uint8_t comp[4] = { 6, 6, 6, 6 }; };
        Reg regs[64];
        PositionFetch none, pos;
        bool r0xWritten = false, haveFull = false, indexDirect = false, exported = false;
        uint32_t lastSlot = 0, lastStride = 0;
        auto components = [](uint32_t format) { return format == 37 ? 2u : format == 57 ? 3u : format == 38 ? 4u : 0u; };
        auto clobber = [&](uint32_t reg, uint32_t mask) {
            Reg& r = regs[reg & 63];
            for (uint32_t c = 0; c < 4; c++)
                if ((mask >> c) & 1) r.comp[c] = 6;
            if (mask & 3) r.fetched = false;
            if ((reg & 63) == 0 && (mask & 1)) r0xWritten = true;
        };
        for (size_t i = 0;; i++)
        {
            if ((i / 2) * 3 + 3 > w.size()) return none;
            Cf pair[2];
            unpackCf(&w[(i / 2) * 3], pair);
            const Cf& cf = pair[i & 1];
            CfOp op = cf.op();
            if (op == CfOp::Nop || op == CfOp::Alloc || op == CfOp::MarkVsFetchDone) continue;
            if (op != CfOp::Exec && op != CfOp::ExecEnd) return none; // conditions, loops, calls, jumps
            for (uint32_t k = 0; k < cf.execCount(); k++)
            {
                uint32_t slot = cf.execAddress() + k;
                if (size_t(slot) * 3 + 3 > w.size()) return none;
                const uint32_t* d = &w[slot * 3];
                if ((cf.execSequence() >> (2 * k)) & 1)
                {
                    Fetch f{ { d[0], d[1], d[2] } };
                    if (f.isPredicated() || f.dstRegRelative()) return none;
                    uint32_t written = 0;
                    for (uint32_t c = 0; c < 4; c++)
                        if (((f.dstSwizzle() >> (3 * c)) & 7) != 7) written |= 1u << c;
                    if (f.op() != uint32_t(FetchOp::VertexFetch)) { clobber(f.dstReg(), written); continue; }
                    if (!f.vfIsMini())
                    {
                        haveFull = true;
                        lastSlot = f.vfFetchSlot();
                        lastStride = f.vfStride();
                        indexDirect = f.srcReg() == 0 && !f.srcRegRelative() && f.vfSrcSwizzle() == 0 && !r0xWritten;
                    }
                    if (!haveFull) return none;
                    uint32_t format = f.vfFormat(), n = components(format);
                    Reg r = regs[f.dstReg() & 63];
                    if ((written & 3) != 3 || !n || !indexDirect || f.vfExpAdjust()) { clobber(f.dstReg(), written); continue; }
                    r.fetched = true;
                    r.slot = lastSlot, r.stride = lastStride, r.format = format, r.offset = f.vfOffset();
                    for (uint32_t c = 0; c < 4; c++)
                    {
                        uint32_t sw = (f.dstSwizzle() >> (3 * c)) & 7;
                        if (sw == 7) r.comp[c] = 6; // kept from before: not this fetch
                        else r.comp[c] = uint8_t(sw < 4 ? (sw < n ? sw : 6) : sw == 4 ? 4 : sw == 5 ? 5 : 6);
                    }
                    if (r.comp[0] > 3 || r.comp[1] > 3) r.fetched = false;
                    regs[f.dstReg() & 63] = r;
                    if ((f.dstReg() & 63) == 0 && (written & 1)) r0xWritten = true;
                    continue;
                }
                Alu a{ { d[0], d[1], d[2] } };
                if (a.isPredicated() || a.vectorDestRelative() || a.scalarDestRelative()) return none;
                uint32_t vmask = a.vectorWriteMask(), smask = a.scalarWriteMask();
                // A move: max(r, r) of one temp register, unswizzled, no abs / negate / clamp
                bool move = a.vectorOp() == 2 && a.srcIsTemp(1) && a.srcIsTemp(2) && a.srcReg(1) == a.srcReg(2) && !(a.srcReg(1) & 0xC0) &&
                            a.srcSwizzle(1) == 0 && a.srcSwizzle(2) == 0 && !a.srcNegate(1) && !a.srcNegate(2) && !a.vectorClamp();
                const Reg src = move ? regs[a.srcReg(1) & 63] : Reg{};
                if (a.isExport())
                {
                    if (a.vectorDest() == 62 && vmask)
                    {
                        exported = true;
                        if ((vmask & 3) && !(move && src.fetched)) pos.valid = false;
                        else if (vmask & 3)
                        {
                            pos.valid = true;
                            pos.slot = src.slot, pos.stride = src.stride, pos.format = src.format, pos.offset = src.offset;
                            for (uint32_t c = 0; c < 4; c++)
                                if ((vmask >> c) & 1) pos.comp[c] = src.comp[c];
                        }
                        else if (vmask & 8) pos.comp[3] = move && src.fetched ? src.comp[3] : 6;
                    }
                    if (a.scalarDest() == 62 && smask)
                    {
                        if (smask & 3) pos.valid = false;
                        if (smask & 8) pos.comp[3] = 6;
                    }
                    continue;
                }
                if (vmask)
                {
                    uint32_t dst = a.vectorDest() & 63;
                    if (move && src.fetched && (vmask & 3) == 3)
                    {
                        Reg r = src;
                        for (uint32_t c = 0; c < 4; c++)
                            if (!((vmask >> c) & 1)) r.comp[c] = 6;
                        regs[dst] = r;
                        if (dst == 0) r0xWritten = true;
                    }
                    else
                        clobber(dst, vmask);
                }
                if (smask) clobber(a.scalarDest(), smask);
            }
            if (op == CfOp::ExecEnd) break;
        }
        return exported && pos.valid ? pos : none;
    }

    TranslatedShader translateToGlsl(const std::vector<uint32_t>& words, ShaderType type)
    {
        Translator t(words, type);
        t.translate();
        t.out.glsl = t.assemble();
        t.out.ok = !t.failed;
        if (type == ShaderType::Vertex) t.out.position = analyzePositionFetch(words);
        return t.out;
    }
}
