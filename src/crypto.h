// crypto.h - self-contained SHA-256, HMAC-SHA256 and AES-128 (ECB).
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace mc {

// ---------------------------------------------------------------------------
// SHA-256
// ---------------------------------------------------------------------------
class Sha256 {
public:
    Sha256() { reset(); }

    void reset()
    {
        bitlen_ = 0;
        buflen_ = 0;
        state_[0] = 0x6a09e667u; state_[1] = 0xbb67ae85u;
        state_[2] = 0x3c6ef372u; state_[3] = 0xa54ff53au;
        state_[4] = 0x510e527fu; state_[5] = 0x9b05688cu;
        state_[6] = 0x1f83d9abu; state_[7] = 0x5be0cd19u;
    }

    void update(const uint8_t* data, size_t len)
    {
        bitlen_ += static_cast<uint64_t>(len) * 8u;
        while (len) {
            const size_t take = std::min(len, sizeof(buf_) - buflen_);
            std::memcpy(buf_ + buflen_, data, take);
            buflen_ += take;
            data += take;
            len -= take;
            if (buflen_ == sizeof(buf_)) {
                transform(buf_);
                buflen_ = 0;
            }
        }
    }

    void finalize(uint8_t out[32])
    {
        const uint64_t bits = bitlen_;
        const uint8_t pad = 0x80;
        update(&pad, 1);
        const uint8_t zero = 0;
        while (buflen_ != 56) update(&zero, 1);
        uint8_t lenbuf[8];
        for (int i = 0; i < 8; ++i) lenbuf[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
        update(lenbuf, 8);

        for (int i = 0; i < 8; ++i) {
            out[4 * i + 0] = static_cast<uint8_t>(state_[i] >> 24);
            out[4 * i + 1] = static_cast<uint8_t>(state_[i] >> 16);
            out[4 * i + 2] = static_cast<uint8_t>(state_[i] >> 8);
            out[4 * i + 3] = static_cast<uint8_t>(state_[i]);
        }
    }

private:
    static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    void transform(const uint8_t* block)
    {
        static const uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(block[4 * i]) << 24) |
                   (static_cast<uint32_t>(block[4 * i + 1]) << 16) |
                   (static_cast<uint32_t>(block[4 * i + 2]) << 8) |
                   static_cast<uint32_t>(block[4 * i + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const uint32_t ch = (e & f) ^ ((~e) & g);
            const uint32_t t1 = h + S1 + ch + K[i] + w[i];
            const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = S0 + maj;
            h = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
        state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
    }

    uint32_t state_[8]{};
    uint64_t bitlen_ = 0;
    uint8_t buf_[64]{};
    size_t buflen_ = 0;
};

inline std::array<uint8_t, 32> sha256(const uint8_t* data, size_t len)
{
    Sha256 h;
    h.update(data, len);
    std::array<uint8_t, 32> out{};
    h.finalize(out.data());
    return out;
}

inline void hmacSha256(const uint8_t* key, size_t keylen,
                       const uint8_t* msg, size_t msglen,
                       uint8_t out[32])
{
    uint8_t k[64] = {};
    if (keylen > sizeof(k)) {
        const auto h = sha256(key, keylen);
        std::memcpy(k, h.data(), h.size());
    } else {
        std::memcpy(k, key, keylen);
    }

    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) {
        ipad[i] = static_cast<uint8_t>(k[i] ^ 0x36);
        opad[i] = static_cast<uint8_t>(k[i] ^ 0x5c);
    }

    Sha256 inner;
    inner.update(ipad, sizeof(ipad));
    inner.update(msg, msglen);
    uint8_t innerHash[32];
    inner.finalize(innerHash);

    Sha256 outer;
    outer.update(opad, sizeof(opad));
    outer.update(innerHash, sizeof(innerHash));
    outer.finalize(out);
}

// ---------------------------------------------------------------------------
// AES-128 (ECB primitives; MeshCore uses AES-128 with zero padding)
// ---------------------------------------------------------------------------
class Aes128 {
public:
    void setKey(const uint8_t key[16])
    {
        std::memcpy(roundKey_, key, 16);
        static const uint8_t rcon[11] = {0x00, 0x01, 0x02, 0x04, 0x08,
                                         0x10, 0x20, 0x40, 0x80, 0x1b, 0x36};
        for (int i = 16; i < 176; i += 4) {
            uint8_t t[4] = {roundKey_[i - 4], roundKey_[i - 3],
                            roundKey_[i - 2], roundKey_[i - 1]};
            if (i % 16 == 0) {
                const uint8_t tmp = t[0];
                t[0] = sbox_[t[1]];
                t[1] = sbox_[t[2]];
                t[2] = sbox_[t[3]];
                t[3] = sbox_[tmp];
                t[0] ^= rcon[i / 16];
            }
            roundKey_[i + 0] = static_cast<uint8_t>(roundKey_[i - 16] ^ t[0]);
            roundKey_[i + 1] = static_cast<uint8_t>(roundKey_[i - 15] ^ t[1]);
            roundKey_[i + 2] = static_cast<uint8_t>(roundKey_[i - 14] ^ t[2]);
            roundKey_[i + 3] = static_cast<uint8_t>(roundKey_[i - 13] ^ t[3]);
        }
    }

    void encryptBlock(const uint8_t in[16], uint8_t out[16]) const
    {
        uint8_t s[16];
        std::memcpy(s, in, 16);
        addRoundKey(s, 0);
        for (int round = 1; round <= 9; ++round) {
            subBytes(s);
            shiftRows(s);
            mixColumns(s);
            addRoundKey(s, round);
        }
        subBytes(s);
        shiftRows(s);
        addRoundKey(s, 10);
        std::memcpy(out, s, 16);
    }

    void decryptBlock(const uint8_t in[16], uint8_t out[16]) const
    {
        uint8_t s[16];
        std::memcpy(s, in, 16);
        addRoundKey(s, 10);
        for (int round = 9; round >= 1; --round) {
            invShiftRows(s);
            invSubBytes(s);
            addRoundKey(s, round);
            invMixColumns(s);
        }
        invShiftRows(s);
        invSubBytes(s);
        addRoundKey(s, 0);
        std::memcpy(out, s, 16);
    }

private:
    static uint8_t xtime(uint8_t x)
    {
        return static_cast<uint8_t>((x << 1) ^ ((x >> 7) ? 0x1b : 0x00));
    }

    static uint8_t gmul(uint8_t a, uint8_t b)
    {
        uint8_t p = 0;
        for (int i = 0; i < 8; ++i) {
            if (b & 1) p ^= a;
            const uint8_t hi = static_cast<uint8_t>(a & 0x80);
            a = static_cast<uint8_t>(a << 1);
            if (hi) a ^= 0x1b;
            b >>= 1;
        }
        return p;
    }

    void addRoundKey(uint8_t s[16], int round) const
    {
        const uint8_t* rk = roundKey_ + 16 * round;
        for (int i = 0; i < 16; ++i) s[i] ^= rk[i];
    }

    static void subBytes(uint8_t s[16])
    {
        for (int i = 0; i < 16; ++i) s[i] = sbox_[s[i]];
    }

    static void invSubBytes(uint8_t s[16])
    {
        for (int i = 0; i < 16; ++i) s[i] = rsbox_[s[i]];
    }

    static void shiftRows(uint8_t s[16])
    {
        for (int r = 1; r < 4; ++r) {
            uint8_t tmp[4] = {s[r], s[r + 4], s[r + 8], s[r + 12]};
            for (int c = 0; c < 4; ++c) s[r + 4 * c] = tmp[(c + r) & 3];
        }
    }

    static void invShiftRows(uint8_t s[16])
    {
        for (int r = 1; r < 4; ++r) {
            uint8_t tmp[4] = {s[r], s[r + 4], s[r + 8], s[r + 12]};
            for (int c = 0; c < 4; ++c) s[r + 4 * c] = tmp[(c - r + 4) & 3];
        }
    }

    static void mixColumns(uint8_t s[16])
    {
        for (int c = 0; c < 4; ++c) {
            const uint8_t a0 = s[4 * c], a1 = s[4 * c + 1], a2 = s[4 * c + 2], a3 = s[4 * c + 3];
            s[4 * c + 0] = static_cast<uint8_t>(xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3);
            s[4 * c + 1] = static_cast<uint8_t>(a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3);
            s[4 * c + 2] = static_cast<uint8_t>(a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3));
            s[4 * c + 3] = static_cast<uint8_t>((xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3));
        }
    }

    static void invMixColumns(uint8_t s[16])
    {
        for (int c = 0; c < 4; ++c) {
            const uint8_t a0 = s[4 * c], a1 = s[4 * c + 1], a2 = s[4 * c + 2], a3 = s[4 * c + 3];
            s[4 * c + 0] = static_cast<uint8_t>(gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9));
            s[4 * c + 1] = static_cast<uint8_t>(gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13));
            s[4 * c + 2] = static_cast<uint8_t>(gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11));
            s[4 * c + 3] = static_cast<uint8_t>(gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14));
        }
    }

    static const uint8_t sbox_[256];
    static const uint8_t rsbox_[256];
    uint8_t roundKey_[176]{};
};

inline const uint8_t Aes128::sbox_[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16};

inline const uint8_t Aes128::rsbox_[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d};

inline bool cryptoSelfTest()
{
    static const uint8_t key[16] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
                                    0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f};
    static const uint8_t pt[16] = {0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
                                   0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff};
    static const uint8_t ct[16] = {0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,
                                   0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a};
    Aes128 aes;
    aes.setKey(key);
    uint8_t buf[16], back[16];
    aes.encryptBlock(pt, buf);
    if (std::memcmp(buf, ct, 16) != 0) return false;
    aes.decryptBlock(ct, back);
    if (std::memcmp(back, pt, 16) != 0) return false;

    // SHA-256("abc")
    static const uint8_t abc[3] = {'a', 'b', 'c'};
    static const uint8_t want[32] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
    const auto h = sha256(abc, sizeof(abc));
    if (std::memcmp(h.data(), want, 32) != 0) return false;
    return true;
}

} // namespace mc
