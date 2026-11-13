// Xenos -> GLSL shader translator. Instruction semantics follow Xenia's
// spirv_shader_translator*.cc and shader_translator.cc (BSD 3-clause, see
// docs/licenses/xenia-LICENSE.txt); see shader_translator.h for the resource layout.
#include "shader_translator.h"
#include "ucode.h"

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
                    case 24: case 25: case 26: case 27:
                    {
                        static const char* cmp[4] = { "equal", "greaterThan", "greaterThanEqual", "notEqual" };
                        emit(std::string("if (any(") + cmp[vop - 24] + "(" + s1 + ", " + s2 + "))) discard;");
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
                bool scalarActive = sop != 50 && (smask || exp || (sop >= 27 && sop <= 39) || sop == 23 || sop == 24);
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
                    case 33: emit("p = false;"); e = "3.402823466e38"; break;              // setp_clr
                    case 35: case 36: case 37: case 38:
                    {
                        static const char* cmp[4] = { "==", ">", ">=", "!=" };
                        emit("if (" + x + " " + cmp[sop - 35] + " 0.0) discard;");
                        e = "0.0";
                        break;
                    }
                    case 39: emit("if (" + x + " == 1.0) discard;"); e = "0.0"; break;     // kills_one
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
                    std::string t = exportTarget(a.vectorDest());
                    for (uint32_t c = 0; c < 4; c++)
                    {
                        bool v = (vmask >> c) & 1, s = (smask >> c) & 1;
                        if (v && s) emit(fmt("%s.%c = 1.0;", t.c_str(), kComp[c]));
                        else if (v) emit(fmt("%s.%c = vres.%c;", t.c_str(), kComp[c], kComp[c]));
                        else if (s) emit(fmt("%s.%c = ps;", t.c_str(), kComp[c]));
                        else if (a.scalarDestRelative()) emit(fmt("%s.%c = 0.0;", t.c_str(), kComp[c]));
                    }
                }
                else
                {
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
                for (uint32_t c = 0; c < 4; c++)
                {
                    uint32_t s = (f.dstSwizzle() >> (3 * c)) & 7;
                    if (s == 7) continue;                      // keep
                    if (s < 4) emit(fmt("%s.%c = fres.%c;", d.c_str(), kComp[c], kComp[s]));
                    else emit(fmt("%s.%c = %s;", d.c_str(), kComp[c], s == 5 ? "1.0" : "0.0"));
                }
            }

            uint32_t vfetchLastSlot = 0;
            void vfetch(const Fetch& f)
            {
                if (type != ShaderType::Vertex) { fail("vertex fetch in a pixel shader"); return; }
                if (f.isPredicated()) { emit(fmt("if (%sp) {", f.predCondition() ? "" : "!")); indent++; }
                if (!f.vfIsMini())
                {
                    uint32_t slot = f.vfFetchSlot();
                    vfetchLastSlot = slot;
                    fetchConstantsUsed.insert(slot);
                    // Fetch constant slot -> 2 dwords in fetchc (6-dword texture slots hold 3 vertex slots)
                    uint32_t dw = slot * 2;
                    // Index: floor, or round to nearest when the instruction asks for it
                    emit(fmt("vf_base = (fetchc[%u].%c >> 2) + uint(int(floor(r[%u].%c%s))) * %uu;", dw / 4, kComp[dw % 4], f.srcReg(),
                             kComp[f.vfSrcSwizzle()], f.vfIsIndexRounded() ? " + 0.5" : "", f.vfStride()));
                    emit(fmt("vf_endian = fetchc[%u].%c & 3u;", (dw + 1) / 4, kComp[(dw + 1) % 4]));
                }
                uint32_t format = f.vfFormat();
                int32_t off = f.vfOffset();
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
                if (f.op() != uint32_t(FetchOp::TextureFetch)) { fail(fmt("fetch op %s not supported", fetchName(f.op()))); return; }
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
                if (f.tfCoordDenormalized() && dim != 3)
                    coord = "(" + coord + ") / " + (dim == 0 ? "float" : dim == 1 ? "vec2" : "vec3") + "(textureSize(" + t + ", 0))";
                std::string sample;
                bool lod = type == ShaderType::Vertex;
                if (lod) sample = "textureLod(" + t + ", " + coord + ", 0.0)";
                else if (f.tfLodBias()) sample = fmt("texture(%s, %s, %.4f)", t.c_str(), coord.c_str(), f.tfLodBias() / 16.0);
                else sample = "texture(" + t + ", " + coord + ")";
                if (f.tfOffsetX() || f.tfOffsetY() || f.tfOffsetZ()) fail("texture fetch offsets not supported");
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

            void translate()
            {
                // Control flow: straight-line exec blocks (conditional on bool constants or the predicate)
                size_t cfEnd = w.size();
                bool ended = false;
                for (size_t i = 0; i + 3 <= w.size() && i < cfEnd && !ended; i += 3)
                {
                    Cf pair[2];
                    unpackCf(&w[i], pair);
                    for (const Cf& cf : pair)
                    {
                        if (ended) break;
                        CfOp op = cf.op();
                        if (isExec(op) && cf.execCount() && cf.execAddress() * 3 < cfEnd) cfEnd = cf.execAddress() * 3;
                        switch (op)
                        {
                        case CfOp::Nop: case CfOp::Alloc: case CfOp::MarkVsFetchDone:
                            break;
                        case CfOp::Exec: case CfOp::ExecEnd:
                            exec(cf);
                            break;
                        case CfOp::CondExec: case CfOp::CondExecEnd:
                        {
                            uint32_t b = cf.condBoolAddress();
                            emit(fmt("if (((boolc[%u].%c >> %uu) & 1u) == %uu) {", b / 128, kComp[(b % 128) / 32], b % 32, cf.condition() ? 1 : 0));
                            indent++; exec(cf); indent--; emit("}");
                            break;
                        }
                        case CfOp::CondExecPred: case CfOp::CondExecPredEnd: case CfOp::CondExecPredClean: case CfOp::CondExecPredCleanEnd:
                            emit(fmt("if (%sp) {", cf.condition() ? "" : "!"));
                            indent++; exec(cf); indent--; emit("}");
                            break;
                        default:
                            fail(fmt("control flow %s not supported yet", cfName(op)));
                        }
                        ended = endsShader(op);
                    }
                }
                if (!ended) fail("shader has no exec-end");
            }

            std::string assemble()
            {
                std::string s = "#version 450\n// Generated by backbreaker-recomp's Xenos shader translator\n";
                s += kPrelude;
                s += "layout(set = 0, binding = 0, std430) readonly buffer GuestMemory { uint mem[]; };\n";
                s += "layout(set = 0, binding = 1, std140) uniform VsConstants { vec4 c[256]; } vsc;\n";
                s += "layout(set = 0, binding = 2, std140) uniform PsConstants { vec4 c[256]; } psc;\n";
                s += "layout(set = 0, binding = 3, std140) uniform OtherConstants { uvec4 boolc[2]; uvec4 loopc[8]; uvec4 fetchc[48]; };\n";
                s += "layout(push_constant) uniform Push { int vertexIndexOffset; };\n";
                static const char* samplers[4] = { "sampler1D", "sampler2D", "sampler3D", "samplerCube" };
                for (uint32_t i = 0; i < 32; i++)
                    if (out.textureSlots & (1u << i))
                        s += fmt("layout(set = 1, binding = %u) uniform %s tex%u;\n", i, samplers[out.textureDims[i]], i);
                if (type == ShaderType::Vertex)
                    s += "layout(location = 0) out vec4 v_int[16];\n";
                else
                {
                    s += "layout(location = 0) in vec4 v_int[16];\n";
                    for (uint32_t i = 0; i < 4; i++)
                        if (out.colorOutputs & (1u << i)) s += fmt("layout(location = %u) out vec4 frag_col%u;\n", i, i);
                }
                s += "\nvoid main() {\n";
                s += "  vec4 r[64];\n  for (int i = 0; i < 64; i++) r[i] = vec4(0.0);\n";
                s += "  vec4 vres = vec4(0.0), fres = vec4(0.0), cube_src = vec4(0.0);\n  vec2 s_src = vec2(0.0);\n  float ps = 0.0;\n  bool p = false;\n  int a0 = 0, aL = 0;\n";
                if (type == ShaderType::Vertex)
                {
                    s += "  uint vf_base = 0u, vf_endian = 0u, vf_w = 0u, vf_w2 = 0u;\n";
                    s += "  vec4 o_pos = vec4(0.0), o_pts = vec4(0.0);\n  vec4 o_int[16];\n  for (int i = 0; i < 16; i++) o_int[i] = vec4(0.0);\n";
                    s += "  r[0].x = float(gl_VertexIndex + vertexIndexOffset); // vertex index, with VGT_INDX_OFFSET\n";
                }
                else
                {
                    s += "  vec4 o_col[4] = vec4[4](vec4(0.0), vec4(0.0), vec4(0.0), vec4(0.0));\n  vec4 o_depth = vec4(gl_FragCoord.z);\n";
                    s += "  for (int i = 0; i < 16; i++) r[i] = v_int[i];\n";
                }
                s += "  vec4 o_unused = vec4(0.0);\n";
                s += body;
                if (type == ShaderType::Vertex)
                    s += "  gl_Position = o_pos;\n  for (int i = 0; i < 16; i++) v_int[i] = o_int[i];\n";
                else
                {
                    for (uint32_t i = 0; i < 4; i++)
                        if (out.colorOutputs & (1u << i)) s += fmt("  frag_col%u = o_col[%u];\n", i, i);
                    if (out.writesDepth) s += "  gl_FragDepth = o_depth.x;\n";
                }
                s += "}\n";
                return s;
            }
        };
    }

    TranslatedShader translateToGlsl(const std::vector<uint32_t>& words, ShaderType type)
    {
        Translator t(words, type);
        t.translate();
        t.out.glsl = t.assemble();
        t.out.ok = !t.failed;
        return t.out;
    }
}
