// Checks the code patterns XenonRecomp now emits for the instructions added for Backbreaker.
// Guest vectors are big-endian; XenonRecomp keeps them fully byte-reversed in host registers.
// Each test builds guest-order inputs, runs the exact expression the recompiler emits,
// and compares against the PowerPC-defined result in guest order.
// Hand-written expectations are cross-checked against a PowerPC reference run
// (powerpc-linux-gnu-gcc -O1 -maltivec, run under qemu-ppc -cpu 7450).
//
// Mutation self-check: run with --mutate and the vpkuhus128 clamp is deliberately dropped
// (the bug this test class exists to catch), so the suite must fail. A normal run re-execs
// itself with --mutate and requires exit status 1, which proves that the vpkuhus128 CHECK
// still catches a wrong value. The child is re-executed through /proc/self/exe, and the
// status is checked exactly, so a child that never runs cannot make the self-check pass.
#include <ppc_config.h>
#include <ppc_context.h>
#include <cstdio>
#include <cstring>
#include <climits>
#include <cmath>
#include <unistd.h>
#include <sys/wait.h>
#include <cerrno>

static int failures = 0;
static bool mutating = false;   // set when the deliberately broken variant is running
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("%s%s:%d ", mutating ? "MUTANT FAIL (expected) " : "FAIL ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// guest big-endian bytes <-> host register (reversed)
static void fromGuest(PPCVRegister& r, const uint8_t g[16]) { for (int i = 0; i < 16; i++) r.u8[i] = g[15 - i]; }
static void toGuest(const PPCVRegister& r, uint8_t g[16]) { for (int i = 0; i < 16; i++) g[i] = r.u8[15 - i]; }
static void setW(PPCVRegister& r, const int32_t w[4]) { uint8_t g[16]; for (int i = 0; i < 4; i++) { uint32_t x = w[i]; g[4*i] = x >> 24; g[4*i+1] = x >> 16; g[4*i+2] = x >> 8; g[4*i+3] = x; } fromGuest(r, g); }
static void setH(PPCVRegister& r, const uint16_t h[8]) { uint8_t g[16]; for (int i = 0; i < 8; i++) { g[2*i] = h[i] >> 8; g[2*i+1] = h[i]; } fromGuest(r, g); }
static void setB(PPCVRegister& r, const int8_t b[16]) { uint8_t g[16]; for (int i = 0; i < 16; i++) g[i] = (uint8_t)b[i]; fromGuest(r, g); }
static void setF(PPCVRegister& r, const float f[4]) { int32_t w[4]; memcpy(w, f, 16); setW(r, w); }
static void getB(const PPCVRegister& r, int8_t b[16]) { uint8_t g[16]; toGuest(r, g); for (int i = 0; i < 16; i++) b[i] = (int8_t)g[i]; }
static void getH(const PPCVRegister& r, uint16_t h[8]) { uint8_t g[16]; toGuest(r, g); for (int i = 0; i < 8; i++) h[i] = (g[2*i] << 8) | g[2*i+1]; }
static void getW(const PPCVRegister& r, uint32_t w[4]) { uint8_t g[16]; toGuest(r, g); for (int i = 0; i < 4; i++) w[i] = (g[4*i] << 24) | (g[4*i+1] << 16) | (g[4*i+2] << 8) | g[4*i+3]; }

// Guest memory for the load/store checks. `base` is the symbol the PPC_LOAD_* /
// PPC_STORE_* macros in ppc_context.h index into; every untouched byte holds
// 0x11 + offset so a wrong effective address is visible in the result.
static uint8_t gmem[256] __attribute__((aligned(16)));
static uint8_t* base = gmem;
static void be16(uint32_t off, uint16_t v) { gmem[off] = v >> 8; gmem[off+1] = v; }
static void be32(uint32_t off, uint32_t v) { gmem[off] = v >> 24; gmem[off+1] = v >> 16; gmem[off+2] = v >> 8; gmem[off+3] = v; }
static void be64(uint32_t off, uint64_t v) { for (int i = 0; i < 8; i++) gmem[off+i] = (uint8_t)(v >> (56 - 8*i)); }
static void memInit()
{
    for (int i = 0; i < (int)sizeof(gmem); i++) gmem[i] = (uint8_t)(0x11 + i);
    be16(0x00, 0x1234);                                  // lhzu/lhbrx/lhzux source
    be16(0x02, 0x5678);                                  // lbzux source
    be32(0x04, 0x12345678);                              // lwzux source (also lhzux high half)
    be64(0x08, 0x1122334455667788ull);                   // ldux/stdux/stfdu source pattern
    be32(0x10, 0x3FC00000);                              // 1.5f for lfsu/lfsux
    be64(0x18, 0x400921FB54442D18ull);                   // pi bits for lfdu/lfdux
    be16(0x20, 0xFEDC);                                  // -708 for lhau sign extension
}

// Record-form compares set cr6: LT = "all elements true", EQ = "none true", GT/SO cleared.
static void checkCR(const char* name, const PPCCRRegister& cr, int wantLt, int wantEq)
{
    CHECK(cr.lt == wantLt && cr.gt == 0 && cr.eq == wantEq && cr.so == 0,
          "%s record: lt %d gt %d eq %d so %d, want lt %d gt 0 eq %d so 0",
          name, cr.lt, cr.gt, cr.eq, cr.so, wantLt, wantEq);
}

static int runChecks(bool mutate)
{
    mutating = mutate;
    PPCVRegister v0{}, v1{}, v2{}, vTemp{};
    PPCRegister temp{};

    // ---- Vector packs / shifts / selects ----

    // vpkswss vD, vA, vB: D = sat16(A0..A3), sat16(B0..B3)
    { int32_t a[4] = {1, 70000, -70000, 5}, b[4] = {6, 7, 8, -9}; setW(v1, a); setW(v2, b);
      simde_mm_store_si128((simde__m128i*)v0.s16, simde_mm_packs_epi32(simde_mm_load_si128((simde__m128i*)v2.s32), simde_mm_load_si128((simde__m128i*)v1.s32)));
      uint16_t h[8]; getH(v0, h); int16_t e[8] = {1, 32767, -32768, 5, 6, 7, 8, -9};
      for (int i = 0; i < 8; i++) CHECK((int16_t)h[i] == e[i], "vpkswss[%d] = %d, want %d", i, (int16_t)h[i], e[i]); }

    // vpkswus128: unsigned saturate of signed words
    { int32_t a[4] = {-5, 70000, 100, 65535}, b[4] = {0, 1, -1, 2}; setW(v1, a); setW(v2, b);
      simde_mm_store_si128((simde__m128i*)v0.u16, simde_mm_packus_epi32(simde_mm_load_si128((simde__m128i*)v2.s32), simde_mm_load_si128((simde__m128i*)v1.s32)));
      uint16_t h[8]; getH(v0, h); uint16_t e[8] = {0, 65535, 100, 65535, 0, 1, 0, 2};
      for (int i = 0; i < 8; i++) CHECK(h[i] == e[i], "vpkswus[%d] = %u, want %u", i, h[i], e[i]); }

    // vpkuhus128: unsigned halfword -> unsigned byte, saturating (0x8000+ must give 0xFF, not 0).
    // In --mutate mode the clamps are dropped (the pre-fix expression), so this test must fail.
    { uint16_t a[8] = {0, 255, 256, 65535, 0x8000, 7, 1, 2}, b[8] = {3, 4, 5, 6, 300, 0, 0xFF, 0x7FFF}; setH(v1, a); setH(v2, b);
      if (mutate)
        simde_mm_store_si128((simde__m128i*)v0.u8, simde_mm_packus_epi16(simde_mm_load_si128((simde__m128i*)v2.u16), simde_mm_load_si128((simde__m128i*)v1.u16)));
      else
        simde_mm_store_si128((simde__m128i*)v0.u8, simde_mm_packus_epi16(simde_mm_min_epu16(simde_mm_load_si128((simde__m128i*)v2.u16), simde_mm_set1_epi16(0xFF)), simde_mm_min_epu16(simde_mm_load_si128((simde__m128i*)v1.u16), simde_mm_set1_epi16(0xFF))));
      uint8_t g[16]; toGuest(v0, g); uint8_t e[16] = {0, 255, 255, 255, 255, 7, 1, 2, 3, 4, 5, 6, 255, 0, 255, 255};
      for (int i = 0; i < 16; i++) CHECK(g[i] == e[i], "vpkuhus[%d] = %u, want %u", i, g[i], e[i]); }

    // vpkshss: signed halfword -> signed byte
    { uint16_t a[8] = {1, 200, (uint16_t)-200, 3, 4, 5, 6, 7}, b[8] = {8, 9, 10, 11, 12, 13, 14, (uint16_t)-1}; setH(v1, a); setH(v2, b);
      simde_mm_store_si128((simde__m128i*)v0.s8, simde_mm_packs_epi16(simde_mm_load_si128((simde__m128i*)v2.s16), simde_mm_load_si128((simde__m128i*)v1.s16)));
      uint8_t g[16]; toGuest(v0, g); int8_t e[16] = {1, 127, -128, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, -1};
      for (int i = 0; i < 16; i++) CHECK((int8_t)g[i] == e[i], "vpkshss[%d] = %d, want %d", i, (int8_t)g[i], e[i]); }

    // vsro vD, vA, vB: shift right by (vB byte 15 >> 3) octets, zeros enter at byte 0
    { uint8_t a[16], b[16] = {}; for (int i = 0; i < 16; i++) a[i] = i + 1; b[15] = 3 << 3; fromGuest(v1, a); fromGuest(v2, b);
      temp.u32 = (v2.u8[0] >> 3) & 0xF;
      for (int i = 0; i < 16; i++) vTemp.u8[i] = (i + temp.u32 < 16) ? v1.u8[i + temp.u32] : 0;
      v0 = vTemp;
      uint8_t g[16]; toGuest(v0, g);
      for (int i = 0; i < 16; i++) { uint8_t e = i < 3 ? 0 : a[i - 3]; CHECK(g[i] == e, "vsro[%d] = %u, want %u", i, g[i], e); } }

    // vsel128 vD, vA, vB, vD: D = (A & ~D) | (B & D)
    { uint8_t m[16], a[16], b[16]; for (int i = 0; i < 16; i++) { m[i] = (i & 1) ? 0xFF : 0x0F; a[i] = 0xA0 + i; b[i] = 0x50 + i; }
      fromGuest(v0, m); fromGuest(v1, a); fromGuest(v2, b);
      simde_mm_store_si128((simde__m128i*)v0.u8, simde_mm_or_si128(simde_mm_andnot_si128(simde_mm_load_si128((simde__m128i*)v0.u8), simde_mm_load_si128((simde__m128i*)v1.u8)), simde_mm_and_si128(simde_mm_load_si128((simde__m128i*)v0.u8), simde_mm_load_si128((simde__m128i*)v2.u8))));
      uint8_t g[16]; toGuest(v0, g);
      for (int i = 0; i < 16; i++) { uint8_t e = (a[i] & ~m[i]) | (b[i] & m[i]); CHECK(g[i] == e, "vsel128[%d] = %02x, want %02x", i, g[i], e); } }

    // vsubuws: unsigned saturating subtract
    { int32_t a[4] = {10, 5, (int32_t)0xFFFFFFFF, 0}, b[4] = {3, 9, 1, 0}; setW(v1, a); setW(v2, b);
      simde_mm_store_si128((simde__m128i*)v0.u32, simde_mm_sub_epi32(simde_mm_load_si128((simde__m128i*)v1.u32), simde_mm_min_epu32(simde_mm_load_si128((simde__m128i*)v1.u32), simde_mm_load_si128((simde__m128i*)v2.u32))));
      uint32_t w[4]; getW(v0, w); uint32_t e[4] = {7, 0, 0xFFFFFFFE, 0};
      for (int i = 0; i < 4; i++) CHECK(w[i] == e[i], "vsubuws[%d] = %u, want %u", i, w[i], e[i]); }

    // vcmpgtuw: unsigned compare
    { int32_t a[4] = {(int32_t)0x80000000, 1, 5, 0}, b[4] = {1, (int32_t)0x80000000, 5, 0}; setW(v1, a); setW(v2, b);
      simde_mm_store_si128((simde__m128i*)v0.u8, simde_mm_cmpgt_epi32(simde_mm_xor_si128(simde_mm_load_si128((simde__m128i*)v1.u32), simde_mm_set1_epi32(INT_MIN)), simde_mm_xor_si128(simde_mm_load_si128((simde__m128i*)v2.u32), simde_mm_set1_epi32(INT_MIN))));
      uint32_t w[4]; getW(v0, w); uint32_t e[4] = {0xFFFFFFFF, 0, 0, 0};
      for (int i = 0; i < 4; i++) CHECK(w[i] == e[i], "vcmpgtuw[%d] = %08x, want %08x", i, w[i], e[i]); }

    // vctuxs vD, vB, 1 (scale x2): saturating float -> u32, NaN -> 0
    { float f[4] = {-1.0f, 1.75f, 5e9f, NAN}; setF(v1, f);
      for (size_t i = 0; i < 4; i++) { temp.f64 = double(v1.f32[i]) * 2;
        vTemp.u32[i] = !(temp.f64 > 0.0) ? 0u : temp.f64 >= 4294967295.0 ? 0xFFFFFFFFu : uint32_t(temp.f64); }
      v0 = vTemp;
      uint32_t w[4]; getW(v0, w); uint32_t e[4] = {0, 3, 0xFFFFFFFF, 0};
      for (int i = 0; i < 4; i++) CHECK(w[i] == e[i], "vctuxs[%d] = %u, want %u", i, w[i], e[i]); }

    // vsrah lane-wise with per-lane counts (arithmetic). The ISA uses only the low 4 bits of each
    // count element, so a count of 17 shifts by 1; a raw SSE shift by >= 16 would give 0. The same
    // masking is checked with non-zero inputs in the vslh/vsrh blocks below.
    { uint16_t a[8] = {1, 0x8000, 0xFFF0, 3, 4, 5, 6, 7}, b[8] = {4, 1, 2, 17, 0, 15, 3, 8}; setH(v1, a); setH(v2, b);
      for (size_t i = 0; i < 8; i++) v0.s16[i] = v1.s16[i] >> (v2.u16[i] & 0xF);
      uint16_t h[8]; getH(v0, h);
      uint16_t e[8] = {0x0000, 0xC000, 0xFFFC, 0x0001, 0x0004, 0x0000, 0x0000, 0x0000};  // lane 3: 3 >> (17 & 15) = 1, not 0
      for (int i = 0; i < 8; i++) CHECK(h[i] == e[i], "vsrah[%d] = %04x, want %04x", i, h[i], e[i]); }

    // ---- Lane-wise VMX integer ----

    // vslh: logical left shift each halfword by its own count, overflow discarded. Only the low 4
    // bits of the count element are used (architected), so counts 16..31 shift by count & 15.
    // Lanes 1, 4 and 7 have counts >= 16 with a non-zero input: a raw SSE shift by 16 or more gives
    // 0 for those lanes, so they would fail if the mask were dropped.
    { uint16_t a[8] = {1, 3, 0xFFF0, 1, 4, 5, 6, 7}, b[8] = {4, 17, 2, 13, 20, 15, 3, 19}; setH(v1, a); setH(v2, b);
      for (size_t i = 0; i < 8; i++) v0.u16[i] = v1.u16[i] << (v2.u16[i] & 0xF);
      uint16_t h[8]; getH(v0, h); uint16_t e[8] = {0x0010, 0x0006, 0xFFC0, 0x2000, 0x0040, 0x8000, 0x0030, 0x0038};
      for (int i = 0; i < 8; i++) CHECK(h[i] == e[i], "vslh[%d] = %04x, want %04x", i, h[i], e[i]); }

    // vsrh: logical right shift each halfword by its own count (low 4 bits, same as vslh).
    // Lanes 5 and 7 use counts 24 and 17 with non-zero inputs: 0x7FFF >> 8 and 0x8000 >> 1, which
    // a raw SSE shift would turn into 0.
    { uint16_t a[8] = {0x8000, 0xFFF0, 0x0102, 0x00FF, 1, 0x7FFF, 5, 0x8000}, b[8] = {1, 2, 3, 4, 15, 24, 1, 17}; setH(v1, a); setH(v2, b);
      for (size_t i = 0; i < 8; i++) v0.u16[i] = v1.u16[i] >> (v2.u16[i] & 0xF);
      uint16_t h[8]; getH(v0, h); uint16_t e[8] = {0x4000, 0x3FFC, 0x0020, 0x000F, 0, 0x007F, 2, 0x4000};
      for (int i = 0; i < 8; i++) CHECK(h[i] == e[i], "vsrh[%d] = %04x, want %04x", i, h[i], e[i]); }

    // vsrab: arithmetic right shift each byte by its own count, sign extended. Only the low 3 bits
    // of the count element are used (architected). Lane 15 keeps the count-8 case (-8 >> (8 & 7) =
    // -8); lane 12 uses count 11, so 100 >> (11 & 7) = 12 -- a value that depends on the mask
    // instead of on the input being zero.
    { int8_t a[16] = {-1, -128, 127, -4, 0, 3, -100, 8, -64, 1, -2, 5, 100, -1, 127, -8};
      uint8_t b[16] = {1, 2, 0, 3, 7, 1, 5, 0, 1, 2, 3, 4, 11, 6, 7, 8};
      setB(v1, a); fromGuest(v2, b);
      for (size_t i = 0; i < 16; i++) v0.s8[i] = v1.s8[i] >> (v2.u8[i] & 0x7);
      int8_t r[16]; getB(v0, r); int8_t e[16] = {-1, -32, 127, -1, 0, 1, -4, 8, -32, 0, -1, 0, 12, -1, 0, -8};
      for (int i = 0; i < 16; i++) CHECK(r[i] == e[i], "vsrab[%d] = %d, want %d", i, r[i], e[i]); }

    // vrlh: rotate left each halfword by its own count (low 4 bits).
    // Lane 5 rotates by 17 & 15 = 1; a count taken literally would not rotate at all.
    { uint16_t a[8] = {0x8001, 0x1234, 0x00FF, 0x8000, 1, 0x0F0F, 0x7FFF, 0xFFFF}, b[8] = {1, 4, 8, 15, 0, 17, 3, 1}; setH(v1, a); setH(v2, b);
      for (size_t i = 0; i < 8; i++) v0.u16[i] = __builtin_rotateleft16(v1.u16[i], v2.u16[i] & 0xF);
      uint16_t h[8]; getH(v0, h); uint16_t e[8] = {0x0003, 0x2341, 0xFF00, 0x4000, 1, 0x1E1E, 0xFFFB, 0xFFFF};  // 0x0F0F rotated left 1
      for (int i = 0; i < 8; i++) CHECK(h[i] == e[i], "vrlh[%d] = %04x, want %04x", i, h[i], e[i]); }

    // vaddsbs: signed byte add, saturating to [-128, 127]
    { int8_t a[16] = {-10, -128, 50, 100, -5, 0, -128, 127, -128, 1, -100, 60, -1, 120, -120, 0};
      int8_t b[16] = {-5, -100, -60, -40, 1, -3, 5, -1, -100, 20, -128, -100, -128, -5, 120, 127};
      setB(v1, a); setB(v2, b);
      simde_mm_store_si128((simde__m128i*)v0.s8, simde_mm_adds_epi8(simde_mm_load_si128((simde__m128i*)v1.s8), simde_mm_load_si128((simde__m128i*)v2.s8)));
      int8_t r[16]; getB(v0, r); int8_t e[16] = {-15, -128, -10, 60, -4, -3, -123, 126, -128, 21, -128, -40, -128, 115, 0, 127};  // 1 + 20 does not saturate
      for (int i = 0; i < 16; i++) CHECK(r[i] == e[i], "vaddsbs[%d] = %d, want %d", i, r[i], e[i]); }

    // vaddsws: signed word add, saturating to [INT_MIN, INT_MAX]
    { int32_t a[4] = {100, 0x7FFFFFFF, INT_MIN, 2000000000}, b[4] = {200, 1, 5, -2000000000}; setW(v1, a); setW(v2, b);
      for (size_t i = 0; i < 4; i++) { int64_t t = int64_t(v1.s32[i]) + int64_t(v2.s32[i]);
        v0.s32[i] = t > INT_MAX ? INT_MAX : t < INT_MIN ? INT_MIN : t; }
      uint32_t w[4]; getW(v0, w); uint32_t e[4] = {300, 2147483647u, 0x80000005u, 0};  // INT_MIN + 5 is not saturated
      for (int i = 0; i < 4; i++) CHECK(w[i] == e[i], "vaddsws[%d] = %d, want %d", i, (int32_t)w[i], (int32_t)e[i]); }

    // vsubshs: signed halfword subtract, saturating to [-32768, 32767]
    { uint16_t a[8] = {1000, 32767, (uint16_t)-32768, 0, 500, (uint16_t)-100, 20000, 12345}; setH(v1, a);
      uint16_t b[8] = {1000, (uint16_t)-1, 1, 32767, (uint16_t)-20000, 100, 500, 12345}; setH(v2, b);
      simde_mm_store_si128((simde__m128i*)v0.s16, simde_mm_subs_epi16(simde_mm_load_si128((simde__m128i*)v1.s16), simde_mm_load_si128((simde__m128i*)v2.s16)));
      uint16_t h[8]; getH(v0, h); int16_t e[8] = {0, 32767, -32768, -32767, 20500, -200, 19500, 0};
      for (int i = 0; i < 8; i++) CHECK((int16_t)h[i] == e[i], "vsubshs[%d] = %d, want %d", i, (int16_t)h[i], e[i]); }

    // vsububm: unsigned byte subtract, wrapping modulo 256
    { uint8_t a[16] = {10, 0, 5, 200, 0, 255, 1, 100, 64, 0, 250, 3, 128, 255, 0, 7};
      uint8_t b[16] = {3, 1, 200, 100, 5, 255, 2, 99, 64, 1, 250, 200, 128, 0, 255, 8};
      fromGuest(v1, a); fromGuest(v2, b);
      simde_mm_store_si128((simde__m128i*)v0.u8, simde_mm_sub_epi8(simde_mm_load_si128((simde__m128i*)v1.u8), simde_mm_load_si128((simde__m128i*)v2.u8)));
      int8_t r[16]; getB(v0, r); uint8_t e[16] = {7, 255, 61, 100, 251, 0, 255, 1, 0, 255, 0, 59, 0, 255, 1, 255};
      for (int i = 0; i < 16; i++) CHECK((uint8_t)r[i] == e[i], "vsububm[%d] = %u, want %u", i, (uint8_t)r[i], e[i]); }

    // vavguh: unsigned halfword average, (a + b + 1) >> 1
    { uint16_t a[8] = {0, 15, 16, 0xFFFF, 0x1234, 5, 0x8000, 100}, b[8] = {0, 16, 0, 0xFFFF, 0, 5, 0x8000, 300}; setH(v1, a); setH(v2, b);
      simde_mm_store_si128((simde__m128i*)v0.u16, simde_mm_avg_epu16(simde_mm_load_si128((simde__m128i*)v1.u16), simde_mm_load_si128((simde__m128i*)v2.u16)));
      uint16_t h[8]; getH(v0, h); uint16_t e[8] = {0, 16, 8, 0xFFFF, 2330, 5, 32768, 200};
      for (int i = 0; i < 8; i++) CHECK(h[i] == e[i], "vavguh[%d] = %u, want %u", i, h[i], e[i]); }

    // vmaxsh: signed halfword max
    { uint16_t a[8] = {(uint16_t)-5, 32767, (uint16_t)-32768, 100, (uint16_t)-100, 0, 32767, 1},
              b[8] = {5, 32766, (uint16_t)-32767, 99, (uint16_t)-101, 1, (uint16_t)-1, 0};
      setH(v1, a); setH(v2, b);
      simde_mm_store_si128((simde__m128i*)v0.s16, simde_mm_max_epi16(simde_mm_load_si128((simde__m128i*)v1.s16), simde_mm_load_si128((simde__m128i*)v2.s16)));
      uint16_t h[8]; getH(v0, h); int16_t e[8] = {5, 32767, -32767, 100, -100, 1, 32767, 1};
      for (int i = 0; i < 8; i++) CHECK((int16_t)h[i] == e[i], "vmaxsh[%d] = %d, want %d", i, (int16_t)h[i], e[i]); }

    // vminsh: signed halfword min
    { uint16_t a[8] = {(uint16_t)-5, 32767, (uint16_t)-32768, 100, (uint16_t)-100, 0, 32767, 1},
              b[8] = {5, 32766, (uint16_t)-32767, 99, (uint16_t)-101, 1, (uint16_t)-1, 0};
      setH(v1, a); setH(v2, b);
      simde_mm_store_si128((simde__m128i*)v0.s16, simde_mm_min_epi16(simde_mm_load_si128((simde__m128i*)v1.s16), simde_mm_load_si128((simde__m128i*)v2.s16)));
      uint16_t h[8]; getH(v0, h); int16_t e[8] = {-5, 32766, -32768, 99, -101, 0, -1, 0};
      for (int i = 0; i < 8; i++) CHECK((int16_t)h[i] == e[i], "vminsh[%d] = %d, want %d", i, (int16_t)h[i], e[i]); }

    // vspltish: broadcast a sign-extended 6-bit immediate into every halfword
    // (recompiler emits short(int(0x...)) on the already sign-extended decode)
    { simde_mm_store_si128((simde__m128i*)v0.u16, simde_mm_set1_epi16(short(int(0x5))));
      uint16_t h[8]; getH(v0, h);
      for (int i = 0; i < 8; i++) CHECK((int16_t)h[i] == 5, "vspltish(5)[%d] = %d, want 5", i, (int16_t)h[i]);
      simde_mm_store_si128((simde__m128i*)v0.u16, simde_mm_set1_epi16(short(int(0xFFFFFFE1))));
      getH(v0, h);
      for (int i = 0; i < 8; i++) CHECK((int16_t)h[i] == -31, "vspltish(-31)[%d] = %d, want -31", i, (int16_t)h[i]); }

    // vnor128: NOR of the two vectors, all 128 bits
    { uint8_t a[16] = {0x00, 0x0F, 0xF0, 0xFF, 0x0F, 0xF0, 0xFF, 0x00, 0x0F, 0xFF, 0xF0, 0x00, 0xF0, 0x00, 0xFF, 0x0F};
      uint8_t b[16] = {0xF0, 0x0F, 0x0F, 0xFF, 0xFF, 0x0F, 0x0F, 0xF0, 0x00, 0xF0, 0xFF, 0x0F, 0x0F, 0xF0, 0x00, 0xFF};
      fromGuest(v1, a); fromGuest(v2, b);
      simde_mm_store_si128((simde__m128i*)v0.u8, simde_mm_xor_si128(simde_mm_or_si128(simde_mm_load_si128((simde__m128i*)v1.u8), simde_mm_load_si128((simde__m128i*)v2.u8)), simde_mm_set1_epi32(-1)));
      uint8_t g[16]; toGuest(v0, g);
      for (int i = 0; i < 16; i++) { uint8_t e = ~(a[i] | b[i]); CHECK(g[i] == e, "vnor128[%d] = %02x, want %02x", i, g[i], e); } }

    // vcmpequh: halfword equality (no signedness); record form sets cr6 LT=all, EQ=none
    { uint16_t a[8] = {1, 2, 2, 3, 4, 4, 5, 6}, b[8] = {1, 3, 4, 3, 8, 4, 9, 6}; setH(v1, a); setH(v2, b);
      simde_mm_store_si128((simde__m128i*)v0.u8, simde_mm_cmpeq_epi16(simde_mm_load_si128((simde__m128i*)v1.u16), simde_mm_load_si128((simde__m128i*)v2.u16)));
      uint16_t h[8]; getH(v0, h); uint16_t e[8] = {0xFFFF, 0, 0, 0xFFFF, 0, 0xFFFF, 0, 0xFFFF};
      for (int i = 0; i < 8; i++) CHECK(h[i] == e[i], "vcmpequh[%d] = %04x, want %04x", i, h[i], e[i]);
      PPCCRRegister cr6{}; cr6.setFromMask(simde_mm_load_si128((simde__m128i*)v0.u8), 0xFFFF);
      checkCR("vcmpequh.", cr6, 0, 0);
      uint16_t c[8] = {7, 7, 7, 7, 7, 7, 7, 7}; setH(v1, c); setH(v2, (const uint16_t*)v1.u16);
      simde_mm_store_si128((simde__m128i*)v0.u8, simde_mm_cmpeq_epi16(simde_mm_load_si128((simde__m128i*)v1.u16), simde_mm_load_si128((simde__m128i*)v2.u16)));
      cr6 = PPCCRRegister{}; cr6.setFromMask(simde_mm_load_si128((simde__m128i*)v0.u8), 0xFFFF);
      checkCR("vcmpequh.(all)", cr6, 1, 0);
      uint16_t c2[8] = {1, 2, 3, 4, 5, 6, 7, 8}, d2[8] = {9, 10, 11, 12, 13, 14, 15, 16}; setH(v1, c2); setH(v2, d2);
      simde_mm_store_si128((simde__m128i*)v0.u8, simde_mm_cmpeq_epi16(simde_mm_load_si128((simde__m128i*)v1.u16), simde_mm_load_si128((simde__m128i*)v2.u16)));
      cr6 = PPCCRRegister{}; cr6.setFromMask(simde_mm_load_si128((simde__m128i*)v0.u8), 0xFFFF);
      checkCR("vcmpequh.(none)", cr6, 0, 1); }

    // vcmpgtsh: signed halfword greater-than; record form sets cr6 LT=all, EQ=none
    { uint16_t a[8] = {(uint16_t)-1, 0, 5, 32767, (uint16_t)-32768, 100, 0, 5}, b[8] = {0, 1, 5, 32766, (uint16_t)-32768, 99, 1, 5}; setH(v1, a); setH(v2, b);
      simde_mm_store_si128((simde__m128i*)v0.u8, simde_mm_cmpgt_epi16(simde_mm_load_si128((simde__m128i*)v1.s16), simde_mm_load_si128((simde__m128i*)v2.s16)));
      uint16_t h[8]; getH(v0, h); uint16_t e[8] = {0, 0, 0, 0xFFFF, 0, 0xFFFF, 0, 0};
      for (int i = 0; i < 8; i++) CHECK(h[i] == e[i], "vcmpgtsh[%d] = %04x, want %04x", i, h[i], e[i]);
      PPCCRRegister cr6{}; cr6.setFromMask(simde_mm_load_si128((simde__m128i*)v0.u8), 0xFFFF);
      checkCR("vcmpgtsh.", cr6, 0, 0);
      uint16_t c[8] = {10, 10, 10, 10, 10, 10, 10, 10}, d[8] = {0, 0, 0, 0, 0, 0, 0, 0}; setH(v1, c); setH(v2, d);
      simde_mm_store_si128((simde__m128i*)v0.u8, simde_mm_cmpgt_epi16(simde_mm_load_si128((simde__m128i*)v1.s16), simde_mm_load_si128((simde__m128i*)v2.s16)));
      cr6 = PPCCRRegister{}; cr6.setFromMask(simde_mm_load_si128((simde__m128i*)v0.u8), 0xFFFF);
      checkCR("vcmpgtsh.(all)", cr6, 1, 0);
      setH(v1, d); setH(v2, c);
      simde_mm_store_si128((simde__m128i*)v0.u8, simde_mm_cmpgt_epi16(simde_mm_load_si128((simde__m128i*)v1.s16), simde_mm_load_si128((simde__m128i*)v2.s16)));
      cr6 = PPCCRRegister{}; cr6.setFromMask(simde_mm_load_si128((simde__m128i*)v0.u8), 0xFFFF);
      checkCR("vcmpgtsh.(none)", cr6, 0, 1); }

    // vcmpgtsw: signed word greater-than; record form sets cr6 LT=all, EQ=none
    { int32_t a[4] = {-1, 0x7FFFFFFF, 5, INT_MIN}, b[4] = {-2, 5, 5, INT_MAX}; setW(v1, a); setW(v2, b);
      simde_mm_store_si128((simde__m128i*)v0.u8, simde_mm_cmpgt_epi32(simde_mm_load_si128((simde__m128i*)v1.s32), simde_mm_load_si128((simde__m128i*)v2.s32)));
      uint32_t w[4]; getW(v0, w); uint32_t e[4] = {0xFFFFFFFF, 0xFFFFFFFF, 0, 0};
      for (int i = 0; i < 4; i++) CHECK(w[i] == e[i], "vcmpgtsw[%d] = %08x, want %08x", i, w[i], e[i]);
      PPCCRRegister cr6{}; cr6.setFromMask(simde_mm_load_ps(v0.f32), 0xF);
      checkCR("vcmpgtsw.", cr6, 0, 0);
      int32_t c[4] = {1, 2, 3, 4}, d[4] = {0, -1, -5, INT_MIN}; setW(v1, c); setW(v2, d);
      simde_mm_store_si128((simde__m128i*)v0.u8, simde_mm_cmpgt_epi32(simde_mm_load_si128((simde__m128i*)v1.s32), simde_mm_load_si128((simde__m128i*)v2.s32)));
      cr6 = PPCCRRegister{}; cr6.setFromMask(simde_mm_load_ps(v0.f32), 0xF);
      checkCR("vcmpgtsw.(all)", cr6, 1, 0);
      setW(v1, d); setW(v2, c);
      simde_mm_store_si128((simde__m128i*)v0.u8, simde_mm_cmpgt_epi32(simde_mm_load_si128((simde__m128i*)v1.s32), simde_mm_load_si128((simde__m128i*)v2.s32)));
      cr6 = PPCCRRegister{}; cr6.setFromMask(simde_mm_load_ps(v0.f32), 0xF);
      checkCR("vcmpgtsw.(none)", cr6, 0, 1); }

    // ---- Scalar: loads/stores with base-register update ----
    // The D-form update instructions compute ea = SI + rB and write it back to rB;
    // the X-form compute ea = rRa + rRb and write it back to rRa.

    memInit();

    // lhzu r3, 4(r4): r3 = zext16(MEM16(r4+4)), r4 = r4+4
    { PPCRegister r3{}, r4{}; uint64_t ea;
      ea = int32_t(4) + r4.u32; r3.u64 = PPC_LOAD_U16(ea); r4.u32 = ea;
      CHECK(r3.u32 == 0x1234 && r3.u64 == 0x1234 && r4.u32 == 4, "lhzu: r3 %llx r4 %x, want 1234 / 4", (unsigned long long)r3.u64, r4.u32); }

    // lhau r3, 0x20(r4): sign-extends the halfword into the register
    { PPCRegister r3{}, r4{}; uint64_t ea;
      ea = int32_t(0x20) + r4.u32; r3.s64 = int16_t(PPC_LOAD_U16(ea)); r4.u32 = ea;
      CHECK(r3.u64 == 0xFFFFFFFFFFFFFEDCull && r4.u32 == 0x20, "lhau: r3 %llx r4 %x, want ffffffff ffffedc / 20", (unsigned long long)r3.u64, r4.u32); }

    // lbzux r3, r5, r4: r3 = MEM8(r5+r4), r5 = r5+r4
    { PPCRegister r3{}, r4{}, r5{}; r5.u64 = 2; uint64_t ea;
      ea = r5.u32 + r4.u32; r3.u64 = PPC_LOAD_U8(ea); r5.u32 = ea;
      CHECK(r3.u32 == 0x56 && r5.u32 == 2, "lbzux: r3 %x r5 %x, want 56 / 2", r3.u32, r5.u32); }

    // lhzux r3, r5, r4: r3 = zext16(MEM16(r5+r4)), r5 = r5+r4
    { PPCRegister r3{}, r4{}, r5{}; r4.u64 = 4; uint64_t ea;
      ea = r5.u32 + r4.u32; r3.u64 = PPC_LOAD_U16(ea); r5.u32 = ea;
      CHECK(r3.u32 == 0x1234 && r5.u32 == 4, "lhzux: r3 %x r5 %x, want 1234 / 4", r3.u32, r5.u32); }

    // lwzux r3, r5, r4: r3 = zext32(MEM32(r5+r4)), r5 = r5+r4
    { PPCRegister r3{}, r4{}, r5{}; r5.u64 = 4; uint64_t ea;
      ea = r5.u32 + r4.u32; r3.u64 = PPC_LOAD_U32(ea); r5.u32 = ea;
      CHECK(r3.u32 == 0x12345678 && r5.u32 == 4, "lwzux: r3 %x r5 %x, want 12345678 / 4", r3.u32, r5.u32); }

    // ldux r3, r5, r4: r3 = MEM64(r5+r4), r5 = r5+r4
    { PPCRegister r3{}, r4{}, r5{}; r5.u64 = 8; uint64_t ea;
      ea = r5.u32 + r4.u32; r3.u64 = PPC_LOAD_U64(ea); r5.u32 = ea;
      CHECK(r3.u64 == 0x1122334455667788ull && r5.u32 == 8, "ldux: r3 %llx r5 %x, want 1122...7788 / 8", (unsigned long long)r3.u64, r5.u32); }

    // lfsu rF3, 0x10(r4): single-precision load converted to double, r4 = r4+SI
    { PPCRegister rF3{}, r4{}; uint64_t ea;
      ea = int32_t(0x10) + r4.u32; temp.u32 = PPC_LOAD_U32(ea); rF3.f64 = double(temp.f32); r4.u32 = ea;
      CHECK(rF3.f64 == 1.5 && r4.u32 == 0x10, "lfsu: f3 %f r4 %x, want 1.5 / 10", rF3.f64, r4.u32); }

    // lfsux rF3, r5, r4: single-precision load converted to double, r5 = r5+r4
    { PPCRegister rF3{}, r4{}, r5{}; r5.u64 = 0x10; uint64_t ea;
      ea = r5.u32 + r4.u32; temp.u32 = PPC_LOAD_U32(ea); rF3.f64 = double(temp.f32); r5.u32 = ea;
      CHECK(rF3.f64 == 1.5 && r5.u32 == 0x10, "lfsux: f3 %f r5 %x, want 1.5 / 10", rF3.f64, r5.u32); }

    // lfdu rF3, 0x18(r4): double-precision load, r4 = r4+SI
    { PPCRegister rF3{}, r4{}; uint64_t ea;
      ea = int32_t(0x18) + r4.u32; rF3.u64 = PPC_LOAD_U64(ea); r4.u32 = ea;
      CHECK(rF3.u64 == 0x400921FB54442D18ull && r4.u32 == 0x18, "lfdu: f3 %llx r4 %x, want pi bits / 18", (unsigned long long)rF3.u64, r4.u32); }

    // lfdux rF3, r5, r4: double-precision load, r5 = r5+r4
    { PPCRegister rF3{}, r4{}, r5{}; r5.u64 = 0x18; uint64_t ea;
      ea = r5.u32 + r4.u32; rF3.u64 = PPC_LOAD_U64(ea); r5.u32 = ea;
      CHECK(rF3.u64 == 0x400921FB54442D18ull && r5.u32 == 0x18, "lfdux: f3 %llx r5 %x, want pi bits / 18", (unsigned long long)rF3.u64, r5.u32); }

    memInit();

    // sthu r3, 0x30(r4): stores the low 16 bits of r3 big-endian, r4 = r4+SI
    { PPCRegister r3{}, r4{}; r3.u64 = 0xABCD; uint64_t ea;
      ea = int32_t(0x30) + r4.u32; PPC_STORE_U16(ea, r3.u16); r4.u32 = ea;
      CHECK(gmem[0x30] == 0xAB && gmem[0x31] == 0xCD && r4.u32 == 0x30, "sthu: %02x %02x r4 %x, want ab cd / 30", gmem[0x30], gmem[0x31], r4.u32); }

    // stbux r3, r5, r4: stores the low 8 bits of r3, r5 = r5+r4
    { PPCRegister r3{}, r4{}, r5{}; r3.u64 = 0x99; r5.u64 = 0x32; uint64_t ea;
      ea = r5.u32 + r4.u32; PPC_STORE_U8(ea, r3.u8); r5.u32 = ea;
      CHECK(gmem[0x32] == 0x99 && r5.u32 == 0x32, "stbux: %02x r5 %x, want 99 / 32", gmem[0x32], r5.u32); }

    // sthux r3, r5, r4: stores the low 16 bits of r3 big-endian, r5 = r5+r4
    { PPCRegister r3{}, r4{}, r5{}; r3.u64 = 0x12345678; r5.u64 = 0x34; uint64_t ea;
      ea = r5.u32 + r4.u32; PPC_STORE_U16(ea, r3.u16); r5.u32 = ea;
      CHECK(gmem[0x34] == 0x56 && gmem[0x35] == 0x78 && r5.u32 == 0x34, "sthux: %02x %02x r5 %x, want 56 78 / 34", gmem[0x34], gmem[0x35], r5.u32); }

    // stdux r3, r5, r4: stores the low 64 bits of r3 big-endian, r5 = r5+r4
    { PPCRegister r3{}, r4{}, r5{}; r3.u64 = 0x1122334455667788ull; r5.u64 = 0x38; uint64_t ea;
      ea = r5.u32 + r4.u32; PPC_STORE_U64(ea, r3.u64); r5.u32 = ea;
      uint64_t back = 0; for (int i = 0; i < 8; i++) back |= uint64_t(gmem[0x38+i]) << (56 - 8*i);
      CHECK(back == 0x1122334455667788ull && r5.u32 == 0x38, "stdux: %llx r5 %x, want 1122...7788 / 38", (unsigned long long)back, r5.u32); }

    // stfsu rF3, 0x40(r4): rounds the double to single precision (nearest, even), r4 = r4+SI
    { PPCRegister rF3{}, r4{}; rF3.f64 = 1.0 / 3.0; uint64_t ea;
      ea = int32_t(0x40) + r4.u32; temp.f32 = float(rF3.f64); PPC_STORE_U32(ea, temp.u32); r4.u32 = ea;
      CHECK(temp.u32 == 0x3EAAAAAB && r4.u32 == 0x40, "stfsu: %08x r4 %x, want 3eaaaaab / 40", temp.u32, r4.u32); }

    // stfsux rF3, r5, r4: rounds the double to single precision, r5 = r5+r4
    { PPCRegister rF3{}, r4{}, r5{}; rF3.f64 = -2.25; r5.u64 = 0x44; uint64_t ea;
      ea = r5.u32 + r4.u32; temp.f32 = float(rF3.f64); PPC_STORE_U32(ea, temp.u32); r5.u32 = ea;
      CHECK(temp.u32 == 0xC0100000 && r5.u32 == 0x44, "stfsux: %08x r5 %x, want c0100000 / 44", temp.u32, r5.u32); }

    // stfdu rF3, 0x48(r4): stores the double big-endian, r4 = r4+SI
    { PPCRegister rF3{}, r4{}; rF3.u64 = 0x400921FB54442D18ull; uint64_t ea;
      ea = int32_t(0x48) + r4.u32; PPC_STORE_U64(ea, rF3.u64); r4.u32 = ea;
      CHECK(gmem[0x48] == 0x40 && gmem[0x49] == 0x09 && gmem[0x4F] == 0x18 && r4.u32 == 0x48, "stfdu: %02x %02x %02x r4 %x, want 40 09 18 / 48", gmem[0x48], gmem[0x49], gmem[0x4F], r4.u32); }

    // stfdux rF3, r5, r4: stores the double big-endian, r5 = r5+r4
    { PPCRegister rF3{}, r4{}, r5{}; rF3.u64 = 0x400921FB54442D18ull; r5.u64 = 0x50; uint64_t ea;
      ea = r5.u32 + r4.u32; PPC_STORE_U64(ea, rF3.u64); r5.u32 = ea;
      CHECK(gmem[0x50] == 0x40 && gmem[0x51] == 0x09 && gmem[0x57] == 0x18 && r5.u32 == 0x50, "stfdux: %02x %02x %02x r5 %x, want 40 09 18 / 50", gmem[0x50], gmem[0x51], gmem[0x57], r5.u32); }

    // lhbrx r3, 0(r4): 16-bit load with byte swap
    { PPCRegister r3{}, r4{};
      r3.u64 = __builtin_bswap16(PPC_LOAD_U16(r4.u32));
      CHECK(r3.u32 == 0x3412, "lhbrx: r3 %04x, want 3412", r3.u32); }

    // ---- Scalar: logic, CR and CTR branches ----

    // eqv r3, r4, r5: equivalence (XNOR) - a result bit is 1 where the operand bits are equal
    { PPCRegister r3{}, r4{}, r5{}; r4.u64 = 0x00FF00FF00FF00FFull; r5.u64 = 0xFFFF0000FFFF0000ull;
      r3.u64 = ~(r4.u64 ^ r5.u64);
      CHECK(r3.u64 == 0x00FFFF0000FFFF00ull, "eqv: %llx, want 00ffff0000ffff00", (unsigned long long)r3.u64);
      r4.u64 = r5.u64 = 0xA5A5A5A5A5A5A5A5ull; r3.u64 = ~(r4.u64 ^ r5.u64);
      CHECK(r3.u64 == ~0ull, "eqv(equal): %llx, want all ones", (unsigned long long)r3.u64); }

    // cror / crorc: single CR-bit logic, T = B|C and T = B|~C
    { PPCCRRegister cr0{}, cr1{}, cr2{};
      cr1.gt = 1; cr2.eq = 1;
      cr0.lt = cr1.gt | cr2.eq;        // cror: 1 | 1
      CHECK(cr0.lt == 1, "cror(1,1) = %d, want 1", cr0.lt);
      cr0.eq = cr1.gt | !cr2.eq;       // crorc: 1 | ~1
      CHECK(cr0.eq == 1, "crorc(1,~1) = %d, want 1", cr0.eq);
      cr2.eq = 0;
      cr0.so = cr1.lt | !cr2.eq;       // crorc: 0 | ~0
      CHECK(cr0.so == 1, "crorc(0,~0) = %d, want 1", cr0.so);
      cr0.un = cr1.lt | cr2.eq;        // cror: 0 | 0
      CHECK(cr0.un == 0, "cror(0,0) = %d, want 0", cr0.un); }

    // bdzf: decrement CTR, branch only when CTR reached 0 AND the CR bit is clear
    { PPCRegister ctr{}; PPCCRRegister cr0{};
      ctr.u64 = 1; cr0.lt = 1;
      --ctr.u64; bool taken = (ctr.u32 == 0) && !cr0.lt;
      CHECK(!taken && ctr.u32 == 0, "bdzf(ctr 1, cr set): taken %d ctr %x, want no / 0", taken, ctr.u32);
      ctr.u64 = 1; cr0.lt = 0;
      --ctr.u64; taken = (ctr.u32 == 0) && !cr0.lt;
      CHECK(taken, "bdzf(ctr 1, cr clear): taken %d, want yes", taken);
      ctr.u64 = 0; cr0.lt = 1;
      --ctr.u64; taken = (ctr.u32 == 0) && !cr0.lt;
      CHECK(!taken && ctr.u32 == 0xFFFFFFFF, "bdzf(ctr 0 wrap): taken %d ctr %x, want no / ffffffff", taken, ctr.u32); }

    // bdnzt: decrement CTR, branch only when CTR is nonzero AND the CR bit is set
    { PPCRegister ctr{}; PPCCRRegister cr0{};
      ctr.u64 = 1; cr0.lt = 1;
      --ctr.u64; bool taken = (ctr.u32 != 0) && cr0.lt;
      CHECK(!taken, "bdnzt(ctr 1, cr set): taken %d, want no", taken);
      ctr.u64 = 2; cr0.lt = 1;
      --ctr.u64; taken = (ctr.u32 != 0) && cr0.lt;
      CHECK(taken && ctr.u32 == 1, "bdnzt(ctr 2, cr set): taken %d ctr %x, want yes / 1", taken, ctr.u32);
      ctr.u64 = 2; cr0.lt = 0;
      --ctr.u64; taken = (ctr.u32 != 0) && cr0.lt;
      CHECK(!taken, "bdnzt(ctr 2, cr clear): taken %d, want no", taken); }

    // ---- Scalar carry ops ----

    struct { uint32_t ra; uint8_t ca; uint32_t rd; uint8_t ca_out; } addme[] = {
        {0, 0, 0xFFFFFFFF, 0}, {0, 1, 0, 1}, {5, 0, 4, 1}, {0xFFFFFFFF, 1, 0xFFFFFFFF, 1} };
    for (auto& t : addme) {
        PPCRegister r3{}, r4{}; PPCXERRegister xer{}; r4.u64 = t.ra; xer.ca = t.ca;
        temp.u8 = (r4.u32 != 0) | xer.ca; r3.u64 = r4.u64 + xer.ca - 1; xer.ca = temp.u8;
        CHECK(r3.u32 == t.rd && xer.ca == t.ca_out, "addme(%x,%d) = %x ca %d, want %x ca %d", t.ra, t.ca, r3.u32, xer.ca, t.rd, t.ca_out); }

    struct { uint32_t ra; uint8_t ca; uint32_t rd; uint8_t ca_out; } subfze[] = {
        {0, 1, 0, 1}, {0, 0, 0xFFFFFFFF, 0}, {1, 1, 0xFFFFFFFF, 0}, {5, 0, 0xFFFFFFFA, 0} };
    for (auto& t : subfze) {
        PPCRegister r3{}, r4{}; PPCXERRegister xer{}; r4.u64 = t.ra; xer.ca = t.ca;
        temp.u8 = (r4.u32 == 0) & xer.ca; r3.u64 = ~r4.u64 + xer.ca; xer.ca = temp.u8;
        CHECK(r3.u32 == t.rd && xer.ca == t.ca_out, "subfze(%x,%d) = %x ca %d, want %x ca %d", t.ra, t.ca, r3.u32, xer.ca, t.rd, t.ca_out); }

    // addc: unsigned add, XER.CA = carry out of bit 31
    { PPCRegister r3{}, r4{}, r5{}; PPCXERRegister xer{}; r4.u64 = 0xFFFFFFFF; r5.u64 = 2;
      xer.ca = r4.u32 + r5.u32 < r4.u32; r3.u64 = r4.u64 + r5.u64;
      CHECK(r3.u32 == 1 && xer.ca == 1, "addc carry: rd %x ca %d", r3.u32, xer.ca); }

    // A jump table's index (bctr with a switch table) is the register's low 32 bits: the console
    // computes the table entry's address in 32 bits, and the bounds check ahead of it is a cmplwi.
    // Backbreaker's uniform editor (#186, 0x827B8144 calling sub_827CEDD0): rlwinm leaves 0xFFFFFFFE with
    // the upper half clear, addi r3,r11,20 makes 0x1'0000'0012, and the callee's addi r11,r3,-1 and
    // cmplwi r11,19 pass. A switch on all 64 bits then matches no case, and the generated default
    // is __builtin_unreachable(): the game crashed there.
    { PPCRegister r3{}, r11{}; PPCCRRegister cr6{}; PPCXERRegister xer{};
      r11.u64 = 0xFFFFFFFE; r3.s64 = r11.s64 + 20; r11.s64 = r3.s64 + -1;
      cr6.compare<uint32_t>(r11.u32, 19, xer);
      int taken = -1;
      if (!cr6.gt) switch (r11.u32) { case 17: taken = 17; break; default: taken = -2; break; }
      CHECK(r11.u64 == 0x100000011ull && taken == 17, "switch index: r11 %llx took %d, want case 17 of 0x100000011",
            (unsigned long long)r11.u64, taken); }

    return failures;
}

int main(int argc, char** argv)
{
    const bool mutate = argc > 1 && !strcmp(argv[1], "--mutate");
    int failures = runChecks(mutate);

    if (!mutate)
    {
        // Mutation self-check: re-exec with the vpkuhus128 clamp dropped; the suite must then
        // exit with the suite's failure code (1). That proves the vpkuhus128 CHECK still catches
        // a wrong value.
        //
        // Two things are needed for this to be a real check:
        //   - /proc/self/exe, not argv[0]. execl does not search PATH, and argv[0] is only the
        //     name the shell resolved, so exec fails when the binary is started by bare name.
        //   - an exact status, not just "non-zero". An exec failure leaves the child exiting 127,
        //     which a non-zero test would read as "the mutant failed as expected" while the
        //     mutant never actually ran.
        pid_t pid = fork();
        if (pid < 0)
        {
            printf("FAIL mutation self-check: fork failed (%d)\n", errno);
            failures++;
        }
        else if (pid == 0)
        {
            execl("/proc/self/exe", "instr_semantics", "--mutate", (char*)nullptr);
            _exit(127);
        }
        else
        {
            int status = 0;
            waitpid(pid, &status, 0);
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 1)
            {
                printf("FAIL mutation self-check: mutant must exit 1, got %d (status %d)\n",
                       WIFEXITED(status) ? WEXITSTATUS(status) : -1, status);
                failures++;
            }
            else
                printf("mutation self-check: mutant failed the vpkuhus128 check as expected (exit 1)\n");
        }
    }

    printf("%s (%d failures)\n", failures ? (mutating ? "mutation variant FAILED (this is the self-check)" : "FAILED") : "all instruction checks passed", failures);
    return failures != 0;
}
