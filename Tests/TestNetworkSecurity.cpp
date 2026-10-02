// TestNetworkSecurity.cpp - Connection-token registry and removal of the legacy XOR prototype (NET-100)

#include "TestFramework.h"
#include "Engine/Networking/NetworkIntegration.h"
#include "Engine/Networking/NetworkSecurity.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

using namespace Spark::Net;

// =============================================================================
// The repeating-key XOR "encryption" prototype must not come back
// =============================================================================

namespace
{
    // Any member named Encrypt/Decrypt counts, whatever its key type: `&T::Encrypt` names a single
    // (static or non-static) member, and the call forms catch overload sets that `&` cannot name.
    // The SparkNetworkSecurityCsprngContract regex over the header is the backstop for other shapes.
    template <typename T>
    concept HasEncryptMember =
        requires { &T::Encrypt; } || requires { &T::Decrypt; } ||
        requires(std::vector<uint8_t>& bytes) { T::Encrypt(bytes); } ||
        requires(std::vector<uint8_t>& bytes) { T::Encrypt(bytes, std::array<uint8_t, 32>{}); } ||
        requires(std::vector<uint8_t>& bytes) { T::Encrypt(bytes, std::vector<uint8_t>{}); };

    template <typename T>
    concept HasXorKeyAccessor = requires(const T& security) { security.GetEncryptionKey(); };

    template <typename T>
    concept HasXorToggle = requires(T& security) { security.SetEncryptionEnabled(true); };

    template <typename T>
    concept HasStackTransform = requires(const T& stack, const std::vector<uint8_t>& bytes) {
        stack.Encrypt(bytes);
        stack.Decrypt(bytes);
    };

    template <typename T>
    concept HasEncryptionFlag = requires(T& config) { config.enableEncryption; };

    // Shapes of the deleted prototype, declared only, so the detectors are proven non-vacuous.
    struct LegacyStaticXorShape
    {
        static std::vector<uint8_t> Encrypt(const std::vector<uint8_t>& data, const std::vector<uint8_t>& key);
    };
    struct LegacyOverloadedXorShape
    {
        static void Encrypt(std::vector<uint8_t>& data, const std::array<uint8_t, 32>& key);
        static void Encrypt(std::vector<uint8_t>& data, const std::vector<uint8_t>& key);
    };
    struct LegacyStackShape
    {
        std::vector<uint8_t> Encrypt(const std::vector<uint8_t>& data) const;
        std::vector<uint8_t> Decrypt(const std::vector<uint8_t>& data) const;
    };
    static_assert(HasEncryptMember<LegacyStaticXorShape>);
    static_assert(HasEncryptMember<LegacyOverloadedXorShape>);
    static_assert(HasStackTransform<LegacyStackShape>);
} // namespace

static_assert(!HasEncryptMember<NetworkSecurity>, "NetworkSecurity must not expose the XOR transform");
static_assert(!HasXorKeyAccessor<NetworkSecurity>, "NetworkSecurity must not hold XOR key material");
static_assert(!HasXorToggle<NetworkSecurity>, "NetworkSecurity must not expose the XOR toggle");
static_assert(!HasStackTransform<NetworkStack>, "NetworkStack must not expose XOR Encrypt/Decrypt");
static_assert(!HasEncryptionFlag<NetworkStackConfig>, "NetworkStackConfig must not offer an XOR opt-in flag");

TEST(NetSecurity_LegacyXorApiIsGone)
{
    // The static_asserts above are the real check; this records them in the test report.
    EXPECT_FALSE(HasEncryptMember<NetworkSecurity>);
    EXPECT_FALSE(HasXorKeyAccessor<NetworkSecurity>);
    EXPECT_FALSE(HasXorToggle<NetworkSecurity>);
    EXPECT_FALSE(HasStackTransform<NetworkStack>);
    EXPECT_FALSE(HasEncryptionFlag<NetworkStackConfig>);
}

// =============================================================================
// Connection tokens
//
// These cover the success path only. The CSPRNG-failure branch (return false, token
// zeroed, nothing recorded) is not exercised: SecureRandom has no fault-injection seam.
// =============================================================================

TEST(NetSecurity_GenerateTokenIsRandom)
{
    NetworkSecurity security;
    NetworkSecurity::Token first{};
    NetworkSecurity::Token second{};
    ASSERT_TRUE(security.GenerateConnectionToken(first));
    ASSERT_TRUE(security.GenerateConnectionToken(second));

    EXPECT_FALSE(std::all_of(first.begin(), first.end(), [](uint8_t b) { return b == 0; }));
    EXPECT_TRUE(first != second);
}

TEST(NetSecurity_TokenIsSingleUse)
{
    NetworkSecurity security;
    NetworkSecurity::Token token{};
    ASSERT_TRUE(security.GenerateConnectionToken(token));

    EXPECT_TRUE(security.ValidateConnectionToken(token));
    EXPECT_FALSE(security.ValidateConnectionToken(token));
}

TEST(NetSecurity_UnknownAndZeroTokensRejected)
{
    NetworkSecurity security;
    NetworkSecurity::Token issued{};
    ASSERT_TRUE(security.GenerateConnectionToken(issued));

    NetworkSecurity::Token forged{};
    forged.fill(0xAA);
    EXPECT_FALSE(security.ValidateConnectionToken(forged));

    NetworkSecurity::Token zero{};
    EXPECT_FALSE(security.ValidateConnectionToken(zero));

    // A one-bit change to an issued token is a different token.
    NetworkSecurity::Token flipped = issued;
    flipped[TOKEN_SIZE - 1] ^= 0x01;
    EXPECT_FALSE(security.ValidateConnectionToken(flipped));

    // Rejections must not consume the genuine pending token.
    EXPECT_TRUE(security.ValidateConnectionToken(issued));
}

TEST(NetSecurity_TokensAreScopedToTheIssuingRegistry)
{
    NetworkSecurity issuer;
    NetworkSecurity other;
    NetworkSecurity::Token token{};
    ASSERT_TRUE(issuer.GenerateConnectionToken(token));

    EXPECT_FALSE(other.ValidateConnectionToken(token));
    EXPECT_TRUE(issuer.ValidateConnectionToken(token));
}

TEST(NetSecurity_ManyPendingTokensEachValidateOnce)
{
    NetworkSecurity security;
    std::vector<NetworkSecurity::Token> tokens(32);
    for (auto& token : tokens)
    {
        ASSERT_TRUE(security.GenerateConnectionToken(token));
    }

    // Validate out of issue order.
    for (size_t i = tokens.size(); i-- > 0;)
    {
        EXPECT_TRUE(security.ValidateConnectionToken(tokens[i]));
    }
    for (const auto& token : tokens)
    {
        EXPECT_FALSE(security.ValidateConnectionToken(token));
    }
}
