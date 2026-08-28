// TestNetworkSecurity.cpp - Tests for legacy XOR transforms and prototype token lifecycle

#include "TestFramework.h"
#include "Engine/Networking/NetworkSecurity.h"
#include <algorithm>

using namespace Spark::Net;

// =============================================================================
// Key Generation
// =============================================================================

TEST(NetSecurity_GenerateKeyFillsArray)
{
    NetworkSecurity::Key key{};
    NetworkSecurity::GenerateKey(key);
    // Key should not be all zeros after generation
    bool allZero = std::all_of(key.begin(), key.end(), [](uint8_t b) { return b == 0; });
    EXPECT_FALSE(allZero);
}

TEST(NetSecurity_GenerateKeyProducesUniqueKeys)
{
    NetworkSecurity::Key key1{}, key2{};
    NetworkSecurity::GenerateKey(key1);
    NetworkSecurity::GenerateKey(key2);
    EXPECT_TRUE(key1 != key2);
}

// =============================================================================
// Legacy XOR transformation
// =============================================================================

TEST(NetSecurity_XorTransformRoundTrip)
{
    NetworkSecurity::Key key{};
    NetworkSecurity::GenerateKey(key);

    std::vector<uint8_t> original = {0x48, 0x65, 0x6C, 0x6C, 0x6F}; // "Hello"
    auto obfuscated = NetworkSecurity::Encrypt(original, key);
    EXPECT_FALSE(obfuscated.empty());
    EXPECT_FALSE(obfuscated == original);

    auto restored = NetworkSecurity::Decrypt(obfuscated, key);
    EXPECT_TRUE(restored == original);
}

TEST(NetSecurity_XorTransformInPlace)
{
    NetworkSecurity::Key key{};
    NetworkSecurity::GenerateKey(key);

    std::vector<uint8_t> data = {1, 2, 3, 4, 5, 6, 7, 8};
    std::vector<uint8_t> original = data;

    NetworkSecurity::PacketEncrypt(data.data(), data.size(), key);
    EXPECT_FALSE(data == original);

    NetworkSecurity::PacketDecrypt(data.data(), data.size(), key);
    EXPECT_TRUE(data == original);
}

TEST(NetSecurity_XorTransformEmptyData)
{
    NetworkSecurity::Key key{};
    NetworkSecurity::GenerateKey(key);

    std::vector<uint8_t> empty;
    auto obfuscated = NetworkSecurity::Encrypt(empty, key);
    EXPECT_TRUE(obfuscated.empty());
}

TEST(NetSecurity_XorTransformLargeData)
{
    NetworkSecurity::Key key{};
    NetworkSecurity::GenerateKey(key);

    std::vector<uint8_t> data(1024);
    for (size_t i = 0; i < data.size(); ++i)
        data[i] = static_cast<uint8_t>(i & 0xFF);

    auto obfuscated = NetworkSecurity::Encrypt(data, key);
    auto restored = NetworkSecurity::Decrypt(obfuscated, key);
    EXPECT_TRUE(restored == data);
}

// =============================================================================
// Connection Tokens
// =============================================================================

TEST(NetSecurity_GenerateToken)
{
    NetworkSecurity security;
    auto token = security.GenerateConnectionToken();
    bool allZero = std::all_of(token.begin(), token.end(), [](uint8_t b) { return b == 0; });
    EXPECT_FALSE(allZero);
}

TEST(NetSecurity_ValidateToken)
{
    NetworkSecurity security;
    auto token = security.GenerateConnectionToken();
    EXPECT_TRUE(security.ValidateConnectionToken(token));
}

TEST(NetSecurity_TokenConsumedOnValidation)
{
    NetworkSecurity security;
    auto token = security.GenerateConnectionToken();
    EXPECT_TRUE(security.ValidateConnectionToken(token));
    // Second validation should fail (token consumed)
    EXPECT_FALSE(security.ValidateConnectionToken(token));
}

TEST(NetSecurity_InvalidTokenRejected)
{
    NetworkSecurity security;
    NetworkSecurity::Token fakeToken{};
    fakeToken.fill(0xAA);
    EXPECT_FALSE(security.ValidateConnectionToken(fakeToken));
}

// =============================================================================
// Legacy prototype toggle
// =============================================================================

TEST(NetSecurity_LegacyPrototypeToggle)
{
    NetworkSecurity security;

    security.SetEncryptionEnabled(true);
    EXPECT_TRUE(security.IsEncryptionEnabled());

    security.SetEncryptionEnabled(false);
    EXPECT_FALSE(security.IsEncryptionEnabled());
}

// =============================================================================
// Key Management
// =============================================================================

TEST(NetSecurity_SetGetEncryptionKey)
{
    NetworkSecurity security;
    NetworkSecurity::Key customKey{};
    customKey.fill(0x42);
    security.SetEncryptionKey(customKey);
    EXPECT_TRUE(security.GetEncryptionKey() == customKey);
}
