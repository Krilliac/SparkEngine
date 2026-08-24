/**
 * @file TFCrypto.cpp
 * @brief SHA-256 / HMAC-SHA256 / PBKDF2-HMAC-SHA256 implementation.
 *
 * SHA-256 core follows the standard FIPS 180-4 compression function (the same
 * public-domain construction shipped in countless small C SHA-256 libraries);
 * HMAC follows RFC 2104; PBKDF2 follows RFC 8018 5.2. See TFCrypto.h for the
 * known-answer tests that pin these down.
 */
#include "Account/TFCrypto.h"

#include <algorithm>
#include <atomic>
#include <cstring>

namespace Terrafront::Crypto
{

    namespace
    {

        void SecureErase(void* data, size_t size) noexcept
        {
            auto* bytes = static_cast<volatile uint8_t*>(data);
            while (bytes && size > 0)
            {
                *bytes++ = 0;
                --size;
            }
            std::atomic_signal_fence(std::memory_order_seq_cst);
        }

        class EraseOnExit
        {
          public:
            EraseOnExit(void* data, size_t size) noexcept : m_data(data), m_size(size) {}
            ~EraseOnExit() { SecureErase(m_data, m_size); }

            EraseOnExit(const EraseOnExit&) = delete;
            EraseOnExit& operator=(const EraseOnExit&) = delete;

          private:
            void* m_data;
            size_t m_size;
        };

        class EraseVectorOnFailure
        {
          public:
            explicit EraseVectorOnFailure(std::vector<uint8_t>& value) noexcept : m_value(value) {}
            ~EraseVectorOnFailure()
            {
                if (m_armed && !m_value.empty())
                    SecureErase(m_value.data(), m_value.size());
            }

            EraseVectorOnFailure(const EraseVectorOnFailure&) = delete;
            EraseVectorOnFailure& operator=(const EraseVectorOnFailure&) = delete;

            void Release() noexcept { m_armed = false; }

          private:
            std::vector<uint8_t>& m_value;
            bool m_armed = true;
        };

        constexpr uint32_t kK[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

        inline uint32_t RotR(uint32_t x, uint32_t n)
        {
            return (x >> n) | (x << (32 - n));
        }

        // Streaming SHA-256 state -- shared by the free Sha256() functions and HMAC
        // (HMAC needs two independent SHA-256 computations, so it uses this directly
        // rather than round-tripping through Sha256Digest-returning wrappers).
        struct Sha256State
        {
            uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                             0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
            uint8_t buf[64] = {};
            size_t bufLen = 0;
            uint64_t totalLen = 0; // total input bytes seen (for the length suffix)

            void Transform(const uint8_t block[64])
            {
                uint32_t w[64];
                const EraseOnExit clearSchedule(w, sizeof(w));
                for (int i = 0; i < 16; ++i)
                {
                    w[i] = (uint32_t(block[i * 4]) << 24) | (uint32_t(block[i * 4 + 1]) << 16) |
                           (uint32_t(block[i * 4 + 2]) << 8) | uint32_t(block[i * 4 + 3]);
                }
                for (int i = 16; i < 64; ++i)
                {
                    uint32_t s0 = RotR(w[i - 15], 7) ^ RotR(w[i - 15], 18) ^ (w[i - 15] >> 3);
                    uint32_t s1 = RotR(w[i - 2], 17) ^ RotR(w[i - 2], 19) ^ (w[i - 2] >> 10);
                    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
                }

                uint32_t working[8] = {h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7]};
                const EraseOnExit clearWorking(working, sizeof(working));
                auto& [a, b, c, d, e, f, g, hh] = working;
                for (int i = 0; i < 64; ++i)
                {
                    uint32_t S1 = RotR(e, 6) ^ RotR(e, 11) ^ RotR(e, 25);
                    uint32_t ch = (e & f) ^ (~e & g);
                    uint32_t t1 = hh + S1 + ch + kK[i] + w[i];
                    uint32_t S0 = RotR(a, 2) ^ RotR(a, 13) ^ RotR(a, 22);
                    uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
                    uint32_t t2 = S0 + maj;
                    hh = g;
                    g = f;
                    f = e;
                    e = d + t1;
                    d = c;
                    c = b;
                    b = a;
                    a = t1 + t2;
                }
                h[0] += a;
                h[1] += b;
                h[2] += c;
                h[3] += d;
                h[4] += e;
                h[5] += f;
                h[6] += g;
                h[7] += hh;
            }

            void Update(const uint8_t* data, size_t len)
            {
                totalLen += len;
                while (len > 0)
                {
                    size_t n = std::min(len, size_t(64) - bufLen);
                    std::memcpy(buf + bufLen, data, n);
                    bufLen += n;
                    data += n;
                    len -= n;
                    if (bufLen == 64)
                    {
                        Transform(buf);
                        bufLen = 0;
                    }
                }
            }

            Sha256Digest Finalize()
            {
                const uint64_t bitLen = totalLen * 8; // capture before padding mutates totalLen further

                const uint8_t pad = 0x80;
                Update(&pad, 1);

                static const uint8_t zeros[64] = {};
                if (bufLen <= 56)
                {
                    Update(zeros, 56 - bufLen);
                }
                else
                {
                    Update(zeros, 64 - bufLen); // completes and transforms the current block
                    Update(zeros, 56);          // pad the next block up to the length-suffix slot
                }

                uint8_t lenBytes[8];
                for (int i = 0; i < 8; ++i)
                    lenBytes[i] = static_cast<uint8_t>(bitLen >> (56 - 8 * i));
                Update(lenBytes, 8);

                Sha256Digest out{};
                for (int i = 0; i < 8; ++i)
                {
                    out[i * 4 + 0] = static_cast<uint8_t>(h[i] >> 24);
                    out[i * 4 + 1] = static_cast<uint8_t>(h[i] >> 16);
                    out[i * 4 + 2] = static_cast<uint8_t>(h[i] >> 8);
                    out[i * 4 + 3] = static_cast<uint8_t>(h[i]);
                }
                return out;
            }
        };

        int HexNibble(char c)
        {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            return -1;
        }

    } // namespace

    Sha256Digest Sha256(const uint8_t* data, size_t len)
    {
        Sha256State st;
        const EraseOnExit clearState(&st, sizeof(st));
        st.Update(data, len);
        return st.Finalize();
    }

    Sha256Digest Sha256(const std::string& data)
    {
        return Sha256(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    }

    Sha256Digest HmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t dataLen)
    {
        constexpr size_t kBlockSize = 64;
        uint8_t keyBlock[kBlockSize] = {};
        const EraseOnExit clearKeyBlock(keyBlock, sizeof(keyBlock));

        if (keyLen > kBlockSize)
        {
            Sha256Digest hashedKey = Sha256(key, keyLen);
            const EraseOnExit clearHashedKey(hashedKey.data(), hashedKey.size());
            std::memcpy(keyBlock, hashedKey.data(), hashedKey.size());
        }
        else if (keyLen > 0)
        {
            std::memcpy(keyBlock, key, keyLen);
        }

        uint8_t ipad[kBlockSize], opad[kBlockSize];
        const EraseOnExit clearIpad(ipad, sizeof(ipad));
        const EraseOnExit clearOpad(opad, sizeof(opad));
        for (size_t i = 0; i < kBlockSize; ++i)
        {
            ipad[i] = static_cast<uint8_t>(keyBlock[i] ^ 0x36);
            opad[i] = static_cast<uint8_t>(keyBlock[i] ^ 0x5c);
        }

        Sha256State inner;
        const EraseOnExit clearInner(&inner, sizeof(inner));
        inner.Update(ipad, kBlockSize);
        inner.Update(data, dataLen);
        Sha256Digest innerHash = inner.Finalize();
        const EraseOnExit clearInnerHash(innerHash.data(), innerHash.size());

        Sha256State outer;
        const EraseOnExit clearOuter(&outer, sizeof(outer));
        outer.Update(opad, kBlockSize);
        outer.Update(innerHash.data(), innerHash.size());
        return outer.Finalize();
    }

    Sha256Digest HmacSha256(const std::string& key, const std::string& data)
    {
        return HmacSha256(reinterpret_cast<const uint8_t*>(key.data()), key.size(),
                          reinterpret_cast<const uint8_t*>(data.data()), data.size());
    }

    std::vector<uint8_t> Pbkdf2HmacSha256(const std::string& password, const std::vector<uint8_t>& salt,
                                          uint32_t iterations, size_t dkLen)
    {
        constexpr size_t kHLen = 32;
        std::vector<uint8_t> dk;
        EraseVectorOnFailure clearDerivedKeyOnFailure(dk);
        dk.reserve(dkLen);

        const uint8_t* pw = reinterpret_cast<const uint8_t*>(password.data());
        const size_t pwLen = password.size();
        const uint32_t blockCount = static_cast<uint32_t>((dkLen + kHLen - 1) / kHLen);

        for (uint32_t i = 1; i <= blockCount; ++i)
        {
            std::vector<uint8_t> saltIdx = salt;
            saltIdx.push_back(static_cast<uint8_t>(i >> 24));
            saltIdx.push_back(static_cast<uint8_t>(i >> 16));
            saltIdx.push_back(static_cast<uint8_t>(i >> 8));
            saltIdx.push_back(static_cast<uint8_t>(i));

            Sha256Digest u = HmacSha256(pw, pwLen, saltIdx.data(), saltIdx.size());
            Sha256Digest t = u;
            const EraseOnExit clearU(u.data(), u.size());
            const EraseOnExit clearT(t.data(), t.size());
            for (uint32_t round = 1; round < iterations; ++round)
            {
                Sha256Digest nextU = HmacSha256(pw, pwLen, u.data(), u.size());
                const EraseOnExit clearNextU(nextU.data(), nextU.size());
                u = nextU;
                for (size_t k = 0; k < t.size(); ++k)
                    t[k] ^= u[k];
            }

            const size_t take = std::min(kHLen, dkLen - dk.size());
            dk.insert(dk.end(), t.begin(), t.begin() + static_cast<std::ptrdiff_t>(take));
        }
        clearDerivedKeyOnFailure.Release();
        return dk;
    }

    std::string ToHex(const uint8_t* data, size_t len)
    {
        static const char kDigits[] = "0123456789abcdef";
        std::string out;
        out.resize(len * 2);
        for (size_t i = 0; i < len; ++i)
        {
            out[i * 2] = kDigits[(data[i] >> 4) & 0xF];
            out[i * 2 + 1] = kDigits[data[i] & 0xF];
        }
        return out;
    }

    std::string ToHex(const std::vector<uint8_t>& data)
    {
        return ToHex(data.data(), data.size());
    }

    std::vector<uint8_t> FromHex(const std::string& hex)
    {
        std::vector<uint8_t> out;
        if (hex.size() % 2 != 0)
            return out; // malformed (odd length) -> empty
        out.reserve(hex.size() / 2);
        for (size_t i = 0; i < hex.size(); i += 2)
        {
            const int hi = HexNibble(hex[i]);
            const int lo = HexNibble(hex[i + 1]);
            if (hi < 0 || lo < 0)
                return {}; // malformed (non-hex char) -> empty
            out.push_back(static_cast<uint8_t>((hi << 4) | lo));
        }
        return out;
    }

    bool ConstantTimeEquals(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
    {
        if (a.size() != b.size())
            return false;
        uint8_t diff = 0;
        for (size_t i = 0; i < a.size(); ++i)
            diff |= static_cast<uint8_t>(a[i] ^ b[i]);
        return diff == 0;
    }

    bool ConstantTimeEquals(const std::string& a, const std::string& b)
    {
        if (a.size() != b.size())
            return false;
        uint8_t diff = 0;
        for (size_t i = 0; i < a.size(); ++i)
            diff |= static_cast<uint8_t>(a[i]) ^ static_cast<uint8_t>(b[i]);
        return diff == 0;
    }

} // namespace Terrafront::Crypto
