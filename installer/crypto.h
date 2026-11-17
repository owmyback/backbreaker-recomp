// The two pieces of cryptography the installer needs (#138), so it depends on no crypto library:
// AES-128 decryption (FIPS-197) for the executable's encryption (an XEX2 decrypts its key with ECB
// and its payload with CBC), and SHA-256 (FIPS 180-4) to recognise the player's executable.
// tests/installer_test.cpp checks both against the standards' own test vectors.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace installer
{
    class Aes128Decryptor
    {
    public:
        explicit Aes128Decryptor(const uint8_t key[16]);

        void decryptBlock(const uint8_t in[16], uint8_t out[16]) const;
        // CBC over whole 16-byte blocks, in place. iv is advanced, so a long payload can be
        // decrypted in pieces.
        void decryptCbc(uint8_t* data, size_t size, uint8_t iv[16]) const;

    private:
        uint8_t roundKeys_[11][16];
    };

    class Sha256
    {
    public:
        Sha256();
        void update(const void* data, size_t size);
        std::array<uint8_t, 32> finish();

    private:
        void block(const uint8_t* p);
        uint32_t h_[8];
        uint8_t buffer_[64];
        size_t buffered_ = 0;
        uint64_t length_ = 0;
    };

    std::string toHex(const uint8_t* data, size_t size);
    bool fromHex(const std::string& hex, uint8_t* out, size_t size);
}
