/**
 * @file PasswordHash.cpp
 * @brief Self-contained SHA-256, HMAC-SHA256, and PBKDF2 password hashing.
 */
#include "PasswordHash.h"
#include "SecureRandom.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <limits>
#include <string_view>
#include <vector>

namespace Spark::PasswordHash
{
    namespace
    {
        using Digest = std::array<uint8_t, 32>;
        constexpr uint32_t kIterations = 600000;
        constexpr uint32_t kMinimumIterations = 600000;
        constexpr uint32_t kMaximumIterations = 1000000;
        constexpr size_t kSaltBytes = 16;
        constexpr size_t kDerivedBytes = 32;
        constexpr size_t kMaximumPasswordBytes = 1024;
        constexpr size_t kMaximumEncodedBytes = 256;
        constexpr std::string_view kScheme = "pbkdf2-sha256";

        constexpr uint32_t kRoundConstants[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
            0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
            0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
            0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
            0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
            0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
            0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
            0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
            0xc67178f2};

        constexpr uint32_t RotateRight(uint32_t value, uint32_t count)
        {
            return (value >> count) | (value << (32 - count));
        }

        struct Sha256State
        {
            uint32_t hash[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
            uint8_t buffer[64] = {};
            size_t bufferLength = 0;
            uint64_t totalLength = 0;

            void Transform(const uint8_t block[64])
            {
                uint32_t words[64];
                for (size_t i = 0; i < 16; ++i)
                {
                    words[i] = (uint32_t(block[i * 4]) << 24) | (uint32_t(block[i * 4 + 1]) << 16) |
                               (uint32_t(block[i * 4 + 2]) << 8) | uint32_t(block[i * 4 + 3]);
                }
                for (size_t i = 16; i < 64; ++i)
                {
                    const uint32_t s0 = RotateRight(words[i - 15], 7) ^ RotateRight(words[i - 15], 18) ^
                                        (words[i - 15] >> 3);
                    const uint32_t s1 = RotateRight(words[i - 2], 17) ^ RotateRight(words[i - 2], 19) ^
                                        (words[i - 2] >> 10);
                    words[i] = words[i - 16] + s0 + words[i - 7] + s1;
                }

                uint32_t a = hash[0], b = hash[1], c = hash[2], d = hash[3];
                uint32_t e = hash[4], f = hash[5], g = hash[6], h = hash[7];
                for (size_t i = 0; i < 64; ++i)
                {
                    const uint32_t sum1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
                    const uint32_t choice = (e & f) ^ (~e & g);
                    const uint32_t temp1 = h + sum1 + choice + kRoundConstants[i] + words[i];
                    const uint32_t sum0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
                    const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
                    const uint32_t temp2 = sum0 + majority;
                    h = g;
                    g = f;
                    f = e;
                    e = d + temp1;
                    d = c;
                    c = b;
                    b = a;
                    a = temp1 + temp2;
                }
                hash[0] += a;
                hash[1] += b;
                hash[2] += c;
                hash[3] += d;
                hash[4] += e;
                hash[5] += f;
                hash[6] += g;
                hash[7] += h;
            }

            void Update(const uint8_t* data, size_t length)
            {
                totalLength += length;
                while (length > 0)
                {
                    const size_t count = std::min(length, sizeof(buffer) - bufferLength);
                    std::memcpy(buffer + bufferLength, data, count);
                    bufferLength += count;
                    data += count;
                    length -= count;
                    if (bufferLength == sizeof(buffer))
                    {
                        Transform(buffer);
                        bufferLength = 0;
                    }
                }
            }

            Digest Finalize()
            {
                const uint64_t bitLength = totalLength * 8;
                const uint8_t marker = 0x80;
                static constexpr uint8_t zeros[64] = {};
                Update(&marker, 1);
                if (bufferLength <= 56)
                    Update(zeros, 56 - bufferLength);
                else
                {
                    Update(zeros, 64 - bufferLength);
                    Update(zeros, 56);
                }
                uint8_t encodedLength[8];
                for (size_t i = 0; i < 8; ++i)
                    encodedLength[i] = static_cast<uint8_t>(bitLength >> (56 - 8 * i));
                Update(encodedLength, sizeof(encodedLength));

                Digest output{};
                for (size_t i = 0; i < 8; ++i)
                {
                    output[i * 4] = static_cast<uint8_t>(hash[i] >> 24);
                    output[i * 4 + 1] = static_cast<uint8_t>(hash[i] >> 16);
                    output[i * 4 + 2] = static_cast<uint8_t>(hash[i] >> 8);
                    output[i * 4 + 3] = static_cast<uint8_t>(hash[i]);
                }
                return output;
            }
        };

        Digest Sha256(const uint8_t* data, size_t length)
        {
            Sha256State state;
            state.Update(data, length);
            return state.Finalize();
        }

        struct HmacSha256Key
        {
            std::array<uint8_t, 64> innerPad{};
            std::array<uint8_t, 64> outerPad{};
        };

        HmacSha256Key PrepareHmacSha256Key(const uint8_t* key, size_t keyLength)
        {
            HmacSha256Key prepared;
            std::array<uint8_t, 64> keyBlock{};
            if (keyLength > keyBlock.size())
            {
                const Digest hashedKey = Sha256(key, keyLength);
                std::memcpy(keyBlock.data(), hashedKey.data(), hashedKey.size());
            }
            else if (keyLength > 0)
                std::memcpy(keyBlock.data(), key, keyLength);

            for (size_t i = 0; i < keyBlock.size(); ++i)
            {
                prepared.innerPad[i] = static_cast<uint8_t>(keyBlock[i] ^ 0x36);
                prepared.outerPad[i] = static_cast<uint8_t>(keyBlock[i] ^ 0x5c);
            }
            return prepared;
        }

        Digest HmacSha256(const HmacSha256Key& key, const uint8_t* data, size_t dataLength)
        {
            Sha256State inner;
            inner.Update(key.innerPad.data(), key.innerPad.size());
            inner.Update(data, dataLength);
            const Digest innerHash = inner.Finalize();
            Sha256State outer;
            outer.Update(key.outerPad.data(), key.outerPad.size());
            outer.Update(innerHash.data(), innerHash.size());
            return outer.Finalize();
        }

        std::vector<uint8_t> Derive(const std::string& password, const std::vector<uint8_t>& salt,
                                    uint32_t iterations, size_t derivedLength)
        {
            constexpr size_t hashLength = 32;
            std::vector<uint8_t> derived;
            derived.reserve(derivedLength);
            const auto* key = reinterpret_cast<const uint8_t*>(password.data());
            // PBKDF2's HMAC key normalization must happen once, not once per
            // iteration. Re-hashing long passwords in every round creates a
            // password-length-amplified denial-of-service path.
            const HmacSha256Key preparedKey = PrepareHmacSha256Key(key, password.size());
            for (uint32_t block = 1; derived.size() < derivedLength; ++block)
            {
                std::vector<uint8_t> input = salt;
                input.push_back(static_cast<uint8_t>(block >> 24));
                input.push_back(static_cast<uint8_t>(block >> 16));
                input.push_back(static_cast<uint8_t>(block >> 8));
                input.push_back(static_cast<uint8_t>(block));
                Digest value = HmacSha256(preparedKey, input.data(), input.size());
                Digest accumulated = value;
                for (uint32_t round = 1; round < iterations; ++round)
                {
                    value = HmacSha256(preparedKey, value.data(), value.size());
                    for (size_t i = 0; i < accumulated.size(); ++i)
                        accumulated[i] ^= value[i];
                }
                const size_t count = std::min(hashLength, derivedLength - derived.size());
                derived.insert(derived.end(), accumulated.begin(), accumulated.begin() + count);
            }
            return derived;
        }

        std::string ToHex(const std::vector<uint8_t>& bytes)
        {
            static constexpr char digits[] = "0123456789abcdef";
            std::string output(bytes.size() * 2, '\0');
            for (size_t i = 0; i < bytes.size(); ++i)
            {
                output[i * 2] = digits[bytes[i] >> 4];
                output[i * 2 + 1] = digits[bytes[i] & 0x0f];
            }
            return output;
        }

        int HexNibble(char value)
        {
            if (value >= '0' && value <= '9')
                return value - '0';
            if (value >= 'a' && value <= 'f')
                return value - 'a' + 10;
            if (value >= 'A' && value <= 'F')
                return value - 'A' + 10;
            return -1;
        }

        bool FromHex(std::string_view text, std::vector<uint8_t>& bytes)
        {
            if (text.empty() || text.size() % 2 != 0)
                return false;
            bytes.clear();
            bytes.reserve(text.size() / 2);
            for (size_t i = 0; i < text.size(); i += 2)
            {
                const int high = HexNibble(text[i]);
                const int low = HexNibble(text[i + 1]);
                if (high < 0 || low < 0)
                    return false;
                bytes.push_back(static_cast<uint8_t>((high << 4) | low));
            }
            return true;
        }

        bool ConstantTimeEqual(const std::vector<uint8_t>& left, const std::vector<uint8_t>& right)
        {
            if (left.size() != right.size())
                return false;
            uint8_t difference = 0;
            for (size_t i = 0; i < left.size(); ++i)
                difference |= static_cast<uint8_t>(left[i] ^ right[i]);
            return difference == 0;
        }

        std::vector<std::string_view> Split(std::string_view encoded)
        {
            std::vector<std::string_view> parts;
            size_t start = 0;
            while (true)
            {
                const size_t separator = encoded.find('$', start);
                parts.push_back(encoded.substr(start, separator == std::string_view::npos ? separator : separator - start));
                if (separator == std::string_view::npos)
                    break;
                start = separator + 1;
            }
            return parts;
        }
    }

    std::string Create(const std::string& password)
    {
        if (password.size() > kMaximumPasswordBytes)
            return {};
        std::vector<uint8_t> salt(kSaltBytes);
        if (!SecureRandom::Fill(salt.data(), salt.size()))
            return {};
        const std::vector<uint8_t> derived = Derive(password, salt, kIterations, kDerivedBytes);
        return std::string(kScheme) + '$' + std::to_string(kIterations) + '$' + ToHex(salt) + '$' + ToHex(derived);
    }

    bool Verify(const std::string& password, const std::string& encodedHash)
    {
        if (password.size() > kMaximumPasswordBytes || encodedHash.size() > kMaximumEncodedBytes)
            return false;
        const std::vector<std::string_view> parts = Split(encodedHash);
        if (parts.size() != 4 || parts[0] != kScheme)
            return false;
        uint32_t iterations = 0;
        const auto parse = std::from_chars(parts[1].data(), parts[1].data() + parts[1].size(), iterations);
        if (parse.ec != std::errc{} || parse.ptr != parts[1].data() + parts[1].size() ||
            iterations < kMinimumIterations || iterations > kMaximumIterations)
            return false;
        std::vector<uint8_t> salt;
        std::vector<uint8_t> expected;
        if (!FromHex(parts[2], salt) || salt.size() != kSaltBytes || !FromHex(parts[3], expected) ||
            expected.size() != kDerivedBytes)
            return false;
        const std::vector<uint8_t> actual = Derive(password, salt, iterations, expected.size());
        return ConstantTimeEqual(actual, expected);
    }
}
