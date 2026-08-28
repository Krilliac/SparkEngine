/**
 * @file TestNetworkSecurityPhaseHH.cpp
 * @brief Phase HH Theme 3D tests for Spark::Net::NetworkSecurity
 *
 * NetworkSecurity is a per-instance class (not a singleton) that
 * provides an isolated XOR transform plus an in-process token lifecycle.
 * Neither prototype provides encryption or connection authentication; these
 * tests only pin their legacy API behavior.
 */

#include "TestFramework.h"
#include "Engine/Networking/NetworkSecurity.h"

#include <cstring>
#include <vector>

TEST(NetworkSecurityPhaseHH_DefaultConstructsWithKey)
{
    Spark::Net::NetworkSecurity sec;
    const auto& key = sec.GetEncryptionKey();
    // Key should be non-zero (random-initialised).
    bool anyNonZero = false;
    for (auto b : key)
        if (b != 0)
            anyNonZero = true;
    EXPECT_TRUE(anyNonZero);
}

TEST(NetworkSecurityPhaseHH_XorTransformRoundTrip)
{
    Spark::Net::NetworkSecurity sec;
    const auto& key = sec.GetEncryptionKey();

    std::vector<uint8_t> plaintext = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04};
    auto obfuscated = Spark::Net::NetworkSecurity::Encrypt(plaintext, key);
    EXPECT_EQ(obfuscated.size(), plaintext.size());
    EXPECT_TRUE(std::memcmp(obfuscated.data(), plaintext.data(), plaintext.size()) != 0);

    auto restored = Spark::Net::NetworkSecurity::Decrypt(obfuscated, key);
    EXPECT_EQ(restored.size(), plaintext.size());
    EXPECT_EQ(std::memcmp(restored.data(), plaintext.data(), plaintext.size()), 0);
}

TEST(NetworkSecurityPhaseHH_InPlaceXorTransform)
{
    Spark::Net::NetworkSecurity::Key key{};
    Spark::Net::NetworkSecurity::GenerateKey(key);

    uint8_t data[16];
    for (int i = 0; i < 16; ++i)
        data[i] = static_cast<uint8_t>(i);
    uint8_t original[16];
    std::memcpy(original, data, 16);

    Spark::Net::NetworkSecurity::PacketEncrypt(data, 16, key);
    EXPECT_TRUE(std::memcmp(data, original, 16) != 0);

    Spark::Net::NetworkSecurity::PacketDecrypt(data, 16, key);
    EXPECT_EQ(std::memcmp(data, original, 16), 0);
}

TEST(NetworkSecurityPhaseHH_XorEmptyDataIsSafe)
{
    Spark::Net::NetworkSecurity::Key key{};
    Spark::Net::NetworkSecurity::GenerateKey(key);
    Spark::Net::NetworkSecurity::PacketEncrypt(nullptr, 0, key);
    Spark::Net::NetworkSecurity::PacketDecrypt(nullptr, 0, key);
    // No crash.
    EXPECT_TRUE(true);
}

TEST(NetworkSecurityPhaseHH_ConnectionTokenRoundTrip)
{
    Spark::Net::NetworkSecurity sec;
    auto token = sec.GenerateConnectionToken();
    EXPECT_TRUE(sec.ValidateConnectionToken(token));
    // Token is single-use; second validation must fail.
    EXPECT_FALSE(sec.ValidateConnectionToken(token));
}

TEST(NetworkSecurityPhaseHH_UnknownTokenFailsValidation)
{
    Spark::Net::NetworkSecurity sec;
    Spark::Net::NetworkSecurity::Token fake{};
    for (size_t i = 0; i < Spark::Net::CONNECTION_TOKEN_SIZE; ++i)
        fake[i] = static_cast<uint8_t>(0xAB);
    EXPECT_FALSE(sec.ValidateConnectionToken(fake));
}

TEST(NetworkSecurityPhaseHH_SetEncryptionKey)
{
    Spark::Net::NetworkSecurity sec;
    Spark::Net::NetworkSecurity::Key customKey{};
    for (size_t i = 0; i < Spark::Net::SECURITY_KEY_SIZE; ++i)
        customKey[i] = static_cast<uint8_t>(0xFF);

    sec.SetEncryptionKey(customKey);
    const auto& retrieved = sec.GetEncryptionKey();
    for (size_t i = 0; i < Spark::Net::SECURITY_KEY_SIZE; ++i)
    {
        EXPECT_EQ(retrieved[i], static_cast<uint8_t>(0xFF));
    }
}

TEST(NetworkSecurityPhaseHH_GenerateKeyProducesNonZero)
{
    Spark::Net::NetworkSecurity::Key key{};
    Spark::Net::NetworkSecurity::GenerateKey(key);
    bool anyNonZero = false;
    for (auto b : key)
        if (b != 0)
            anyNonZero = true;
    EXPECT_TRUE(anyNonZero);
}
