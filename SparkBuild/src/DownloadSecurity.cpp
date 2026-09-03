#include "DownloadSecurity.h"

#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <vector>

namespace SparkBuild::DownloadSecurity
{
    namespace
    {
        constexpr uint32_t RotateRight(uint32_t value, uint32_t bits)
        {
            return (value >> bits) | (value << (32u - bits));
        }

        bool StartsWithCaseInsensitive(std::string_view value, std::string_view prefix)
        {
            if (value.size() < prefix.size())
                return false;
            for (size_t index = 0; index < prefix.size(); ++index)
            {
                const auto actual = static_cast<unsigned char>(value[index]);
                const auto expected = static_cast<unsigned char>(prefix[index]);
                if (std::tolower(actual) != std::tolower(expected))
                    return false;
            }
            return true;
        }

        class Sha256
        {
          public:
            void Update(const uint8_t* data, size_t size)
            {
                for (size_t index = 0; index < size; ++index)
                {
                    m_buffer[m_bufferSize++] = data[index];
                    if (m_bufferSize == m_buffer.size())
                    {
                        Transform(m_buffer.data());
                        m_bitLength += 512;
                        m_bufferSize = 0;
                    }
                }
            }

            std::string Finalize()
            {
                m_bitLength += static_cast<uint64_t>(m_bufferSize) * 8u;
                m_buffer[m_bufferSize++] = 0x80u;
                if (m_bufferSize > 56)
                {
                    while (m_bufferSize < 64)
                        m_buffer[m_bufferSize++] = 0;
                    Transform(m_buffer.data());
                    m_bufferSize = 0;
                }
                while (m_bufferSize < 56)
                    m_buffer[m_bufferSize++] = 0;
                for (size_t index = 0; index < 8; ++index)
                    m_buffer[63 - index] = static_cast<uint8_t>(m_bitLength >> (index * 8u));
                Transform(m_buffer.data());

                static constexpr char kHex[] = "0123456789abcdef";
                std::string result(64, '\0');
                for (size_t word = 0; word < m_state.size(); ++word)
                {
                    for (size_t byte = 0; byte < 4; ++byte)
                    {
                        const uint8_t value = static_cast<uint8_t>(m_state[word] >> ((3u - byte) * 8u));
                        const size_t offset = (word * 8u) + (byte * 2u);
                        result[offset] = kHex[value >> 4u];
                        result[offset + 1] = kHex[value & 0x0fu];
                    }
                }
                return result;
            }

          private:
            void Transform(const uint8_t* block)
            {
                static constexpr std::array<uint32_t, 64> kRoundConstants = {
                    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
                    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
                    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
                    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
                    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
                    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
                    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
                    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
                    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
                    0xc67178f2u,
                };

                std::array<uint32_t, 64> words{};
                for (size_t index = 0; index < 16; ++index)
                {
                    const size_t offset = index * 4;
                    words[index] = (static_cast<uint32_t>(block[offset]) << 24u) |
                                   (static_cast<uint32_t>(block[offset + 1]) << 16u) |
                                   (static_cast<uint32_t>(block[offset + 2]) << 8u) |
                                   static_cast<uint32_t>(block[offset + 3]);
                }
                for (size_t index = 16; index < words.size(); ++index)
                {
                    const uint32_t s0 = RotateRight(words[index - 15], 7) ^ RotateRight(words[index - 15], 18) ^
                                        (words[index - 15] >> 3u);
                    const uint32_t s1 = RotateRight(words[index - 2], 17) ^ RotateRight(words[index - 2], 19) ^
                                        (words[index - 2] >> 10u);
                    words[index] = words[index - 16] + s0 + words[index - 7] + s1;
                }

                uint32_t a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3];
                uint32_t e = m_state[4], f = m_state[5], g = m_state[6], h = m_state[7];
                for (size_t index = 0; index < words.size(); ++index)
                {
                    const uint32_t sum1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
                    const uint32_t choose = (e & f) ^ (~e & g);
                    const uint32_t temp1 = h + sum1 + choose + kRoundConstants[index] + words[index];
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
                m_state[0] += a;
                m_state[1] += b;
                m_state[2] += c;
                m_state[3] += d;
                m_state[4] += e;
                m_state[5] += f;
                m_state[6] += g;
                m_state[7] += h;
            }

            std::array<uint32_t, 8> m_state = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                               0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
            std::array<uint8_t, 64> m_buffer{};
            size_t m_bufferSize = 0;
            uint64_t m_bitLength = 0;
        };
    } // namespace

    std::string RedirectProtocolPolicy(std::string_view sourceUrl)
    {
        return StartsWithCaseInsensitive(sourceUrl, "https://") ? "=https" : "=http,https";
    }

    bool ComputeSha256(const std::filesystem::path& path, std::string& digest, std::string& error)
    {
        error.clear();
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            error = "failed to open downloaded archive for SHA-256 verification";
            return false;
        }

        Sha256 sha;
        std::vector<uint8_t> buffer(64 * 1024);
        while (file)
        {
            file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize count = file.gcount();
            if (count > 0)
                sha.Update(buffer.data(), static_cast<size_t>(count));
        }
        if (!file.eof())
        {
            error = "failed while reading downloaded archive for SHA-256 verification";
            return false;
        }
        digest = sha.Finalize();
        return true;
    }

    bool VerifySha256(const std::filesystem::path& path, std::string_view expectedDigest, std::string& error)
    {
        error.clear();
        if (expectedDigest.size() != 64)
        {
            error = "expected SHA-256 digest must contain exactly 64 hexadecimal characters";
            return false;
        }
        for (const char character : expectedDigest)
        {
            if (!std::isxdigit(static_cast<unsigned char>(character)))
            {
                error = "expected SHA-256 digest contains a non-hexadecimal character";
                return false;
            }
        }

        std::string actualDigest;
        if (!ComputeSha256(path, actualDigest, error))
            return false;

        unsigned int difference = 0;
        for (size_t index = 0; index < actualDigest.size(); ++index)
        {
            const auto expected = static_cast<unsigned char>(expectedDigest[index]);
            difference |= static_cast<unsigned int>(actualDigest[index] ^ std::tolower(expected));
        }
        if (difference != 0)
        {
            error = "downloaded archive SHA-256 mismatch";
            return false;
        }
        return true;
    }
} // namespace SparkBuild::DownloadSecurity
