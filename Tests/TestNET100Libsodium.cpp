/**
 * @file TestNET100Libsodium.cpp
 * @brief NET-100: the vendored libsodium initializes and computes the RFC 8439 AEAD vector.
 *
 * Proves the spark_sodium build (cmake/SparkLibsodium.cmake) is a working
 * libsodium, independent of Spark's own wrappers: EnsureSodium() succeeds and
 * libsodium's IETF ChaCha20-Poly1305 reproduces RFC 8439 section 2.8.2.
 */

#include "TestFramework.h"
#include "Engine/Networking/NetworkEncryption.h"

#include <sodium.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    std::vector<uint8_t> HexBytes(std::string_view hex)
    {
        std::vector<uint8_t> out;
        for (size_t i = 0; i + 1 < hex.size(); i += 2)
            out.push_back(static_cast<uint8_t>(std::stoul(std::string(hex.substr(i, 2)), nullptr, 16)));
        return out;
    }
} // namespace

TEST(Transport_Libsodium_InitAndRfc8439Vector)
{
    ASSERT_TRUE(Spark::Net::EnsureSodium());
    ASSERT_TRUE(Spark::Net::EnsureSodium()); // idempotent
    EXPECT_EQ(sodium_init(), 1);             // already initialized by EnsureSodium()

    // RFC 8439 section 2.8.2.
    const auto key = HexBytes("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f");
    const auto nonce = HexBytes("070000004041424344454647");
    const auto aad = HexBytes("50515253c0c1c2c3c4c5c6c7");
    const std::string_view plaintext = "Ladies and Gentlemen of the class of '99: If I could offer you only one "
                                       "tip for the future, sunscreen would be it.";
    const auto expected =
        HexBytes("d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e8ca9671282fafb"
                 "69da92728b1a71de0a9e060b2905d6a5b67ecd3b3692ddbd7f2d778b8c9803aee328091b58fab324e4fa"
                 "d675945585808b4831d7bc3ff4def08e4b7a9de576d26586cec64b6116"
                 "1ae10b594f09e26a7e902ecbd0600691");

    std::vector<uint8_t> sealed(plaintext.size() + crypto_aead_chacha20poly1305_ietf_ABYTES);
    unsigned long long sealedLen = 0;
    ASSERT_EQ(crypto_aead_chacha20poly1305_ietf_encrypt(
                  sealed.data(), &sealedLen, reinterpret_cast<const unsigned char*>(plaintext.data()), plaintext.size(),
                  aad.data(), aad.size(), nullptr, nonce.data(), key.data()),
              0);
    EXPECT_EQ(sealedLen, static_cast<unsigned long long>(expected.size()));
    EXPECT_TRUE(sealed == expected);

    std::vector<uint8_t> opened(plaintext.size());
    unsigned long long openedLen = 0;
    ASSERT_EQ(crypto_aead_chacha20poly1305_ietf_decrypt(opened.data(), &openedLen, nullptr, sealed.data(),
                                                        sealed.size(), aad.data(), aad.size(), nonce.data(),
                                                        key.data()),
              0);
    EXPECT_TRUE(std::string_view(reinterpret_cast<const char*>(opened.data()), openedLen) == plaintext);

    sealed[0] ^= 0x01;
    EXPECT_EQ(crypto_aead_chacha20poly1305_ietf_decrypt(opened.data(), &openedLen, nullptr, sealed.data(),
                                                        sealed.size(), aad.data(), aad.size(), nonce.data(),
                                                        key.data()),
              -1);
}

// BLD-100 / OD-04: the stable-v1 x86-64 floor is SSE4.2. cmake/SparkLibsodium.cmake
// compiles libsodium without its AVX, AVX2, AVX-512 and AES-NI/PCLMUL variants on
// every toolchain, so the CPUID dispatch must never report them, even on a host
// that has them. On MSVC private/common.h enables those variants unconditionally;
// a regression there makes this fail on any AVX2 host (every local and hosted
// runner), not only on the below-floor CPUs it would crash.
TEST(CpuFloor_Libsodium_AboveFloorVariantsExcluded)
{
    ASSERT_TRUE(Spark::Net::EnsureSodium());
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    EXPECT_EQ(sodium_runtime_has_avx(), 0);
    EXPECT_EQ(sodium_runtime_has_avx2(), 0);
    EXPECT_EQ(sodium_runtime_has_avx512f(), 0);
    EXPECT_EQ(sodium_runtime_has_aesni(), 0);
    EXPECT_EQ(sodium_runtime_has_pclmul(), 0);
    EXPECT_EQ(sodium_runtime_has_rdrand(), 0);
    EXPECT_EQ(crypto_aead_aes256gcm_is_available(), 0);
    // The floor-level paths stay selected on any x86-64 CPU.
    EXPECT_EQ(sodium_runtime_has_sse2(), 1);
#endif
}
