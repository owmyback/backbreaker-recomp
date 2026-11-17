#include "crypto.h"

#include <cstring>

namespace installer
{
    namespace
    {
        // GF(2^8) with the AES polynomial x^8 + x^4 + x^3 + x + 1
        uint8_t gmul(uint8_t a, uint8_t b)
        {
            uint8_t p = 0;
            while (b) {
                if (b & 1) p ^= a;
                a = uint8_t((a << 1) ^ ((a & 0x80) ? 0x1B : 0));
                b >>= 1;
            }
            return p;
        }

        // The S-box and the multiplication tables are computed, not typed in: S(x) is the affine
        // transform of x's multiplicative inverse (FIPS-197 5.1.1), so a typo can't hide in a table
        struct Tables
        {
            uint8_t sbox[256], inv[256], mul9[256], mul11[256], mul13[256], mul14[256];
            Tables()
            {
                for (int x = 0; x < 256; x++) {
                    uint8_t i = 0; // inverse: x^254
                    if (x) {
                        uint8_t r = 1, base = uint8_t(x);
                        for (int e = 254; e; e >>= 1) {
                            if (e & 1) r = gmul(r, base);
                            base = gmul(base, base);
                        }
                        i = r;
                    }
                    uint8_t s = i;
                    for (int k = 1; k <= 4; k++) s ^= uint8_t((i << k) | (i >> (8 - k)));
                    s ^= 0x63;
                    sbox[x] = s;
                    inv[s] = uint8_t(x);
                    mul9[x] = gmul(uint8_t(x), 9);
                    mul11[x] = gmul(uint8_t(x), 11);
                    mul13[x] = gmul(uint8_t(x), 13);
                    mul14[x] = gmul(uint8_t(x), 14);
                }
            }
        };

        const Tables& tables()
        {
            static const Tables t;
            return t;
        }
    }

    Aes128Decryptor::Aes128Decryptor(const uint8_t key[16])
    {
        // Key expansion (FIPS-197 5.2): 44 words, the round constant doubling in GF(2^8)
        const Tables& t = tables();
        uint8_t w[176];
        std::memcpy(w, key, 16);
        uint8_t rcon = 1;
        for (int i = 4; i < 44; i++) {
            uint8_t temp[4];
            std::memcpy(temp, w + 4 * (i - 1), 4);
            if (i % 4 == 0) {
                const uint8_t first = temp[0];
                temp[0] = uint8_t(t.sbox[temp[1]] ^ rcon);
                temp[1] = t.sbox[temp[2]];
                temp[2] = t.sbox[temp[3]];
                temp[3] = t.sbox[first];
                rcon = gmul(rcon, 2);
            }
            for (int k = 0; k < 4; k++) w[4 * i + k] = uint8_t(w[4 * (i - 4) + k] ^ temp[k]);
        }
        std::memcpy(roundKeys_, w, sizeof(roundKeys_));
    }

    void Aes128Decryptor::decryptBlock(const uint8_t in[16], uint8_t out[16]) const
    {
        // The inverse cipher (FIPS-197 5.3); the state is column-major, s[row + 4 * column]
        const Tables& t = tables();
        uint8_t s[16];
        for (int k = 0; k < 16; k++) s[k] = uint8_t(in[k] ^ roundKeys_[10][k]);
        for (int round = 9; round >= 0; round--) {
            uint8_t u[16];
            for (int c = 0; c < 4; c++) // InvShiftRows and InvSubBytes: row r moves right by r
                for (int r = 0; r < 4; r++) u[r + 4 * ((c + r) % 4)] = t.inv[s[r + 4 * c]];
            for (int k = 0; k < 16; k++) u[k] ^= roundKeys_[round][k];
            if (round == 0) {
                std::memcpy(out, u, 16);
                return;
            }
            for (int c = 0; c < 4; c++) { // InvMixColumns
                const uint8_t* a = u + 4 * c;
                s[4 * c + 0] = uint8_t(t.mul14[a[0]] ^ t.mul11[a[1]] ^ t.mul13[a[2]] ^ t.mul9[a[3]]);
                s[4 * c + 1] = uint8_t(t.mul9[a[0]] ^ t.mul14[a[1]] ^ t.mul11[a[2]] ^ t.mul13[a[3]]);
                s[4 * c + 2] = uint8_t(t.mul13[a[0]] ^ t.mul9[a[1]] ^ t.mul14[a[2]] ^ t.mul11[a[3]]);
                s[4 * c + 3] = uint8_t(t.mul11[a[0]] ^ t.mul13[a[1]] ^ t.mul9[a[2]] ^ t.mul14[a[3]]);
            }
        }
    }

    void Aes128Decryptor::decryptCbc(uint8_t* data, size_t size, uint8_t iv[16]) const
    {
        uint8_t plain[16], next[16];
        for (size_t at = 0; at + 16 <= size; at += 16) {
            std::memcpy(next, data + at, 16);
            decryptBlock(data + at, plain);
            for (int k = 0; k < 16; k++) data[at + k] = uint8_t(plain[k] ^ iv[k]);
            std::memcpy(iv, next, 16);
        }
    }

    namespace
    {
        constexpr uint32_t kSha256K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
        };

        uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    }

    Sha256::Sha256()
        : h_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}
    {
    }

    void Sha256::block(const uint8_t* p)
    {
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
        for (int i = 16; i < 64; i++) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
        for (int i = 0; i < 64; i++) {
            const uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + kSha256K[i] + w[i];
            const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
    }

    void Sha256::update(const void* data, size_t size)
    {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        length_ += size;
        while (size) {
            if (buffered_ == 0 && size >= 64) {
                block(p);
                p += 64;
                size -= 64;
                continue;
            }
            const size_t n = size < 64 - buffered_ ? size : 64 - buffered_;
            std::memcpy(buffer_ + buffered_, p, n);
            buffered_ += n;
            p += n;
            size -= n;
            if (buffered_ == 64) {
                block(buffer_);
                buffered_ = 0;
            }
        }
    }

    std::array<uint8_t, 32> Sha256::finish()
    {
        const uint64_t bits = length_ * 8;
        const uint8_t one = 0x80, zero = 0;
        update(&one, 1);
        while (buffered_ != 56) update(&zero, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; i++) len[i] = uint8_t(bits >> (56 - 8 * i));
        update(len, 8);
        std::array<uint8_t, 32> out;
        for (int i = 0; i < 8; i++)
            for (int k = 0; k < 4; k++) out[4 * i + k] = uint8_t(h_[i] >> (24 - 8 * k));
        return out;
    }

    std::string toHex(const uint8_t* data, size_t size)
    {
        static const char digits[] = "0123456789abcdef";
        std::string s;
        s.reserve(size * 2);
        for (size_t i = 0; i < size; i++) {
            s += digits[data[i] >> 4];
            s += digits[data[i] & 15];
        }
        return s;
    }

    bool fromHex(const std::string& hex, uint8_t* out, size_t size)
    {
        if (hex.size() != size * 2) return false;
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        for (size_t i = 0; i < size; i++) {
            const int hi = nibble(hex[2 * i]), lo = nibble(hex[2 * i + 1]);
            if (hi < 0 || lo < 0) return false;
            out[i] = uint8_t(hi << 4 | lo);
        }
        return true;
    }
}
