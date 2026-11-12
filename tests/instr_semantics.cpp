// Checks the code patterns XenonRecomp now emits for the instructions added for Backbreaker.
// Guest vectors are big-endian; XenonRecomp keeps them fully byte-reversed in host registers.
// Each test builds guest-order inputs, runs the exact expression the recompiler emits,
// and compares against the PowerPC-defined result in guest order.
#include <ppc_config.h>
#include <ppc_context.h>
#include <cstdio>
#include <cstring>
#include <climits>
#include <cmath>

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// guest big-endian bytes <-> host register (reversed)
static void fromGuest(PPCVRegister& r, const uint8_t g[16]) { for (int i = 0; i < 16; i++) r.u8[i] = g[15 - i]; }
static void toGuest(const PPCVRegister& r, uint8_t g[16]) { for (int i = 0; i < 16; i++) g[i] = r.u8[15 - i]; }
static void setW(PPCVRegister& r, const int32_t w[4]) { uint8_t g[16]; for (int i = 0; i < 4; i++) { uint32_t x = w[i]; g[4*i] = x >> 24; g[4*i+1] = x >> 16; g[4*i+2] = x >> 8; g[4*i+3] = x; } fromGuest(r, g); }
static void setH(PPCVRegister& r, const uint16_t h[8]) { uint8_t g[16]; for (int i = 0; i < 8; i++) { g[2*i] = h[i] >> 8; g[2*i+1] = h[i]; } fromGuest(r, g); }
static void setF(PPCVRegister& r, const float f[4]) { int32_t w[4]; memcpy(w, f, 16); setW(r, w); }
static void getH(const PPCVRegister& r, uint16_t h[8]) { uint8_t g[16]; toGuest(r, g); for (int i = 0; i < 8; i++) h[i] = (g[2*i] << 8) | g[2*i+1]; }
static void getW(const PPCVRegister& r, uint32_t w[4]) { uint8_t g[16]; toGuest(r, g); for (int i = 0; i < 4; i++) w[i] = (g[4*i] << 24) | (g[4*i+1] << 16) | (g[4*i+2] << 8) | g[4*i+3]; }

int main()
{
    PPCVRegister v0{}, v1{}, v2{}, vTemp{};
    PPCRegister temp{};

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

    // vpkuhus128: unsigned halfword -> unsigned byte, saturating (0x8000+ must give 0xFF, not 0)
    { uint16_t a[8] = {0, 255, 256, 65535, 0x8000, 7, 1, 2}, b[8] = {3, 4, 5, 6, 300, 0, 0xFF, 0x7FFF}; setH(v1, a); setH(v2, b);
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

    // vslh / vsrah lane-wise with per-lane counts
    { uint16_t a[8] = {1, 0x8000, 0xFFF0, 3, 4, 5, 6, 7}, b[8] = {4, 1, 2, 17, 0, 15, 3, 8}; setH(v1, a); setH(v2, b);
      for (size_t i = 0; i < 8; i++) v0.s16[i] = v1.s16[i] >> (v2.u16[i] & 0xF);
      uint16_t h[8]; getH(v0, h);
      for (int i = 0; i < 8; i++) { uint16_t e = (uint16_t)((int16_t)a[i] >> (b[i] & 0xF)); CHECK(h[i] == e, "vsrah[%d] = %04x, want %04x", i, h[i], e); } }

    // Scalar carry ops
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

    { PPCRegister r3{}, r4{}, r5{}; PPCXERRegister xer{}; r4.u64 = 0xFFFFFFFF; r5.u64 = 2;
      xer.ca = r4.u32 + r5.u32 < r4.u32; r3.u64 = r4.u64 + r5.u64;
      CHECK(r3.u32 == 1 && xer.ca == 1, "addc carry: rd %x ca %d", r3.u32, xer.ca); }

    printf("%s (%d failures)\n", failures ? "FAILED" : "all instruction checks passed", failures);
    return failures != 0;
}
