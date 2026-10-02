/**
 * @file TestNET100Handshake.cpp
 * @brief NET-100: hostile tests for the signed-ephemeral X25519 handshake (SecureHandshake.h).
 *
 * Every case drives the shipped ClientHandshake / RespondToClientHello code.
 * A handshake either yields two interoperating SecureChannels or fails closed
 * with a typed HandshakeError and no channel.
 */

#include "TestFramework.h"
#include "Engine/Networking/NetworkManager.h"
#include "Engine/Networking/SecureHandshake.h"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

using namespace Spark::Net;

namespace
{
    /// ConnectAccepted prefix: client id 7, server time 0.0f, version NETWORK_PROTOCOL_VERSION.
    constexpr std::array<uint8_t, CONNECT_ACCEPT_PREFIX_SIZE> kPrefix = {
        7,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        static_cast<uint8_t>(NETWORK_PROTOCOL_VERSION),
        static_cast<uint8_t>(NETWORK_PROTOCOL_VERSION >> 8)};

    struct Peers
    {
        std::unique_ptr<SecureChannel> client;
        std::unique_ptr<SecureChannel> server;
    };

    ServerIdentity MakeIdentity()
    {
        auto identity = GenerateServerIdentity();
        EXPECT_TRUE(identity.has_value());
        return identity.value_or(ServerIdentity{});
    }

    /// Full handshake; the result carries both channels on success.
    std::expected<Peers, HandshakeError> Handshake(const ServerIdentity& identity, const ServerPublicKey& pinned)
    {
        ClientHandshake client;
        auto hello = client.Begin(NETWORK_PROTOCOL_VERSION);
        if (!hello)
            return std::unexpected(hello.error());
        auto response = RespondToClientHello(*hello, kPrefix, identity);
        if (!response)
            return std::unexpected(response.error());
        auto channel = client.Finish(kPrefix, response->serverHello, pinned);
        if (!channel)
            return std::unexpected(channel.error());
        return Peers{std::move(*channel), std::move(response->channel)};
    }

    bool RoundTrip(SecureChannel& from, SecureChannel& to, std::string_view text)
    {
        const std::vector<uint8_t> payload(text.begin(), text.end());
        std::vector<uint8_t> packet;
        std::vector<uint8_t> out;
        return from.Seal(payload, packet) && to.Open(packet, out) == OpenResult::Ok && out == payload;
    }

    bool Contains(std::span<const uint8_t> haystack, std::span<const uint8_t> needle)
    {
        return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end()) != haystack.end();
    }

    int ErrorCode(HandshakeError error)
    {
        return static_cast<int>(error);
    }
} // namespace

TEST(Transport_Handshake_HappyPathChannelsInteroperate)
{
    const ServerIdentity identity = MakeIdentity();
    auto peers = Handshake(identity, identity.publicKey);
    ASSERT_TRUE(peers.has_value());
    ASSERT_TRUE(peers->client != nullptr && peers->server != nullptr);
    EXPECT_TRUE(RoundTrip(*peers->client, *peers->server, "client to server"));
    EXPECT_TRUE(RoundTrip(*peers->server, *peers->client, "server to client"));

    // A second session with the same server derives unrelated keys.
    auto other = Handshake(identity, identity.publicKey);
    ASSERT_TRUE(other.has_value());
    std::vector<uint8_t> packet;
    std::vector<uint8_t> out;
    const std::vector<uint8_t> payload{1, 2, 3};
    ASSERT_TRUE(peers->client->Seal(payload, packet));
    EXPECT_EQ(static_cast<int>(other->server->Open(packet, out)), static_cast<int>(OpenResult::AuthenticationFailed));
}

TEST(Transport_Handshake_WrongPinnedKeyFails)
{
    const ServerIdentity real = MakeIdentity();
    const ServerIdentity impostor = MakeIdentity();

    // The client pinned `real`; `impostor` answers.
    auto peers = Handshake(impostor, real.publicKey);
    ASSERT_FALSE(peers.has_value());
    EXPECT_EQ(ErrorCode(peers.error()), ErrorCode(HandshakeError::ServerIdentityMismatch));

    // An impostor that claims the pinned key in its hello still cannot sign for it.
    ClientHandshake client;
    auto hello = client.Begin(NETWORK_PROTOCOL_VERSION);
    ASSERT_TRUE(hello.has_value());
    auto response = RespondToClientHello(*hello, kPrefix, impostor);
    ASSERT_TRUE(response.has_value());
    std::copy(real.publicKey.begin(), real.publicKey.end(), response->serverHello.begin() + 1);
    auto channel = client.Finish(kPrefix, response->serverHello, real.publicKey);
    ASSERT_FALSE(channel.has_value());
    EXPECT_EQ(ErrorCode(channel.error()), ErrorCode(HandshakeError::BadSignature));
}

TEST(Transport_Handshake_EveryServerHelloByteFlipFails)
{
    const ServerIdentity identity = MakeIdentity();
    int accepted = 0;
    for (size_t index = 0; index < SERVER_HELLO_SIZE; ++index)
    {
        ClientHandshake client;
        auto hello = client.Begin(NETWORK_PROTOCOL_VERSION);
        ASSERT_TRUE(hello.has_value());
        auto response = RespondToClientHello(*hello, kPrefix, identity);
        ASSERT_TRUE(response.has_value());
        response->serverHello[index] ^= 0x01;
        if (client.Finish(kPrefix, response->serverHello, identity.publicKey).has_value())
            ++accepted;
    }
    EXPECT_EQ(accepted, 0);
}

TEST(Transport_Handshake_TruncatedOrExtendedHellosFail)
{
    const ServerIdentity identity = MakeIdentity();

    ClientHandshake probe;
    auto clientHello = probe.Begin(NETWORK_PROTOCOL_VERSION);
    ASSERT_TRUE(clientHello.has_value());
    std::vector<uint8_t> extendedClient(clientHello->begin(), clientHello->end());
    extendedClient.push_back(0);
    for (size_t length = 0; length <= extendedClient.size(); ++length)
    {
        if (length == CLIENT_HELLO_SIZE)
            continue;
        auto response = RespondToClientHello(std::span(extendedClient).first(length), kPrefix, identity);
        ASSERT_FALSE(response.has_value());
        EXPECT_EQ(ErrorCode(response.error()), ErrorCode(HandshakeError::Malformed));
    }

    for (size_t length = 0; length <= SERVER_HELLO_SIZE + 1; ++length)
    {
        if (length == SERVER_HELLO_SIZE)
            continue;
        ClientHandshake client;
        auto hello = client.Begin(NETWORK_PROTOCOL_VERSION);
        ASSERT_TRUE(hello.has_value());
        auto response = RespondToClientHello(*hello, kPrefix, identity);
        ASSERT_TRUE(response.has_value());
        std::vector<uint8_t> serverHello(response->serverHello.begin(), response->serverHello.end());
        serverHello.resize(length, 0);
        auto channel = client.Finish(kPrefix, serverHello, identity.publicKey);
        ASSERT_FALSE(channel.has_value());
        EXPECT_EQ(ErrorCode(channel.error()), ErrorCode(HandshakeError::Malformed));
    }
}

TEST(Transport_Handshake_VersionAndSuiteDowngradeFails)
{
    const ServerIdentity identity = MakeIdentity();

    // Other protocol versions are refused before any key material is produced.
    for (const uint16_t version : {uint16_t{0}, static_cast<uint16_t>(NETWORK_PROTOCOL_VERSION + 1), uint16_t{0xFFFF}})
    {
        ClientHandshake client;
        auto hello = client.Begin(version);
        ASSERT_TRUE(hello.has_value());
        auto response = RespondToClientHello(*hello, kPrefix, identity);
        ASSERT_FALSE(response.has_value());
        EXPECT_EQ(ErrorCode(response.error()), ErrorCode(HandshakeError::UnsupportedVersion));
    }

    // A rewritten suite in the ClientHello is refused by the server...
    {
        ClientHandshake client;
        auto hello = client.Begin(NETWORK_PROTOCOL_VERSION);
        ASSERT_TRUE(hello.has_value());
        (*hello)[6] = 0;
        auto response = RespondToClientHello(*hello, kPrefix, identity);
        ASSERT_FALSE(response.has_value());
        EXPECT_EQ(ErrorCode(response.error()), ErrorCode(HandshakeError::UnsupportedSuite));
    }

    // ...and in the ServerHello by the client.
    {
        ClientHandshake client;
        auto hello = client.Begin(NETWORK_PROTOCOL_VERSION);
        ASSERT_TRUE(hello.has_value());
        auto response = RespondToClientHello(*hello, kPrefix, identity);
        ASSERT_TRUE(response.has_value());
        response->serverHello[0] = 2;
        auto channel = client.Finish(kPrefix, response->serverHello, identity.publicKey);
        ASSERT_FALSE(channel.has_value());
        EXPECT_EQ(ErrorCode(channel.error()), ErrorCode(HandshakeError::UnsupportedSuite));
    }

    // A man in the middle who alters any signed ClientHello byte the server
    // accepts (here the nonce) breaks the transcript signature for the client.
    {
        ClientHandshake client;
        auto hello = client.Begin(NETWORK_PROTOCOL_VERSION);
        ASSERT_TRUE(hello.has_value());
        ClientHello altered = *hello;
        altered[CLIENT_HELLO_SIZE - 1] ^= 0x80;
        auto response = RespondToClientHello(altered, kPrefix, identity);
        ASSERT_TRUE(response.has_value());
        auto channel = client.Finish(kPrefix, response->serverHello, identity.publicKey);
        ASSERT_FALSE(channel.has_value());
        EXPECT_EQ(ErrorCode(channel.error()), ErrorCode(HandshakeError::BadSignature));
    }
}

TEST(Transport_Handshake_ReplayedServerHelloAcrossSessionsFails)
{
    const ServerIdentity identity = MakeIdentity();

    ClientHandshake first;
    auto firstHello = first.Begin(NETWORK_PROTOCOL_VERSION);
    ASSERT_TRUE(firstHello.has_value());
    auto recorded = RespondToClientHello(*firstHello, kPrefix, identity);
    ASSERT_TRUE(recorded.has_value());
    ASSERT_TRUE(first.Finish(kPrefix, recorded->serverHello, identity.publicKey).has_value());

    // The recorded ServerHello is signed over the first client's ephemeral key.
    ClientHandshake second;
    ASSERT_TRUE(second.Begin(NETWORK_PROTOCOL_VERSION).has_value());
    auto replayed = second.Finish(kPrefix, recorded->serverHello, identity.publicKey);
    ASSERT_FALSE(replayed.has_value());
    EXPECT_EQ(ErrorCode(replayed.error()), ErrorCode(HandshakeError::BadSignature));

    // A finished (or failed) handshake cannot be reused.
    auto again = first.Finish(kPrefix, recorded->serverHello, identity.publicKey);
    ASSERT_FALSE(again.has_value());
    EXPECT_EQ(ErrorCode(again.error()), ErrorCode(HandshakeError::InvalidState));
    auto restart = first.Begin(NETWORK_PROTOCOL_VERSION);
    ASSERT_FALSE(restart.has_value());
    EXPECT_EQ(ErrorCode(restart.error()), ErrorCode(HandshakeError::InvalidState));
}

TEST(Transport_Handshake_LowOrderClientPointRejected)
{
    const ServerIdentity identity = MakeIdentity();
    ClientHandshake client;
    auto hello = client.Begin(NETWORK_PROTOCOL_VERSION);
    ASSERT_TRUE(hello.has_value());

    // The all-zero point (and the order-2 point 1) force an all-zero shared secret.
    for (const uint8_t lowOrderFirstByte : {uint8_t{0}, uint8_t{1}})
    {
        ClientHello weak = *hello;
        std::fill(weak.begin() + 7, weak.begin() + 7 + HANDSHAKE_PUBLIC_KEY_SIZE, uint8_t{0});
        weak[7] = lowOrderFirstByte;
        auto response = RespondToClientHello(weak, kPrefix, identity);
        ASSERT_FALSE(response.has_value());
        EXPECT_EQ(ErrorCode(response.error()), ErrorCode(HandshakeError::WeakSharedSecret));
    }
}

TEST(Transport_Handshake_MalformedClientHelloRejectedDeterministically)
{
    const ServerIdentity identity = MakeIdentity();
    ClientHandshake client;
    auto hello = client.Begin(NETWORK_PROTOCOL_VERSION);
    ASSERT_TRUE(hello.has_value());

    ClientHello badMagic = *hello;
    badMagic[0] ^= 0xFF;
    const std::vector<uint8_t> empty;
    const std::vector<uint8_t> garbage(CLIENT_HELLO_SIZE, 0xA5);

    for (int attempt = 0; attempt < 3; ++attempt)
    {
        auto magic = RespondToClientHello(badMagic, kPrefix, identity);
        ASSERT_FALSE(magic.has_value());
        EXPECT_EQ(ErrorCode(magic.error()), ErrorCode(HandshakeError::Malformed));

        auto none = RespondToClientHello(empty, kPrefix, identity);
        ASSERT_FALSE(none.has_value());
        EXPECT_EQ(ErrorCode(none.error()), ErrorCode(HandshakeError::Malformed));

        auto noise = RespondToClientHello(garbage, kPrefix, identity);
        ASSERT_FALSE(noise.has_value());
        EXPECT_EQ(ErrorCode(noise.error()), ErrorCode(HandshakeError::Malformed));
    }

    // Finish before Begin is refused rather than using an unset ephemeral key.
    ClientHandshake unstarted;
    auto channel = unstarted.Finish(kPrefix, std::vector<uint8_t>(SERVER_HELLO_SIZE, 0), identity.publicKey);
    ASSERT_FALSE(channel.has_value());
    EXPECT_EQ(ErrorCode(channel.error()), ErrorCode(HandshakeError::InvalidState));
}

TEST(Transport_Handshake_HellosCarryNoSecretMaterial)
{
    const ServerIdentity identity = MakeIdentity();
    ClientHandshake client;
    auto hello = client.Begin(NETWORK_PROTOCOL_VERSION);
    ASSERT_TRUE(hello.has_value());
    auto response = RespondToClientHello(*hello, kPrefix, identity);
    ASSERT_TRUE(response.has_value());

    // Ed25519 secret keys are seed || public key; neither half of the seed may leak.
    const std::span<const uint8_t> seed(identity.secretKey.data(), 32);
    for (size_t offset = 0; offset + 8 <= seed.size(); offset += 8)
    {
        EXPECT_FALSE(Contains(response->serverHello, seed.subspan(offset, 8)));
        EXPECT_FALSE(Contains(*hello, seed.subspan(offset, 8)));
    }

    // The hellos are public; the channels they produce are not derivable from them.
    auto channel = client.Finish(kPrefix, response->serverHello, identity.publicKey);
    ASSERT_TRUE(channel.has_value());
    const std::vector<uint8_t> secretText{'h', 'u', 'n', 't', 'e', 'r', '2', '!'};
    std::vector<uint8_t> packet;
    ASSERT_TRUE((*channel)->Seal(secretText, packet));
    EXPECT_FALSE(Contains(packet, secretText));
    EXPECT_FALSE(Contains(response->serverHello, secretText));
}

TEST(Transport_Handshake_AcceptPrefixIsSigned)
{
    const ServerIdentity identity = MakeIdentity();

    // Any change to the assigned client id, server time or echoed version breaks the signature:
    // an on-path attacker cannot hand the client another player's id.
    int accepted = 0;
    for (size_t index = 0; index < CONNECT_ACCEPT_PREFIX_SIZE; ++index)
    {
        ClientHandshake client;
        auto hello = client.Begin(NETWORK_PROTOCOL_VERSION);
        ASSERT_TRUE(hello.has_value());
        auto response = RespondToClientHello(*hello, kPrefix, identity);
        ASSERT_TRUE(response.has_value());
        auto rewritten = kPrefix;
        rewritten[index] ^= 0x01;
        auto channel = client.Finish(rewritten, response->serverHello, identity.publicKey);
        if (channel.has_value())
            ++accepted;
        else
            EXPECT_EQ(ErrorCode(channel.error()), ErrorCode(HandshakeError::BadSignature));
    }
    EXPECT_EQ(accepted, 0);

    // A prefix of the wrong length is malformed on both sides.
    const std::array<uint8_t, CONNECT_ACCEPT_PREFIX_SIZE - 1> shortPrefix{};
    ClientHandshake client;
    auto hello = client.Begin(NETWORK_PROTOCOL_VERSION);
    ASSERT_TRUE(hello.has_value());
    auto refused = RespondToClientHello(*hello, shortPrefix, identity);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(ErrorCode(refused.error()), ErrorCode(HandshakeError::Malformed));
    auto response = RespondToClientHello(*hello, kPrefix, identity);
    ASSERT_TRUE(response.has_value());
    auto channel = client.Finish(shortPrefix, response->serverHello, identity.publicKey);
    ASSERT_FALSE(channel.has_value());
    EXPECT_EQ(ErrorCode(channel.error()), ErrorCode(HandshakeError::Malformed));
}

TEST(Transport_Handshake_EverySmallOrderKeyRefusedBeforeCurveWork)
{
    // The small-order Curve25519 u-coordinates (RFC 7748 section 7 / libsodium's blocklist):
    // 0, 1, the two order-8 points, p-1, p, p+1. Each is also tried with bit 255 set.
    std::vector<std::array<uint8_t, 32>> points;
    std::array<uint8_t, 32> point{};
    points.push_back(point); // 0
    point[0] = 1;
    points.push_back(point); // 1
    points.push_back({0xe0, 0xeb, 0x7a, 0x7c, 0x3b, 0x41, 0xb8, 0xae, 0x16, 0x56, 0xe3, 0xfa, 0xf1, 0x9f, 0xc4, 0x6a,
                      0xda, 0x09, 0x8d, 0xeb, 0x9c, 0x32, 0xb1, 0xfd, 0x86, 0x62, 0x05, 0x16, 0x5f, 0x49, 0xb8, 0x00});
    points.push_back({0x5f, 0x9c, 0x95, 0xbc, 0xa3, 0x50, 0x8c, 0x24, 0xb1, 0xd0, 0xb1, 0x55, 0x9c, 0x83, 0xef, 0x5b,
                      0x04, 0x44, 0x5c, 0xc4, 0x58, 0x1c, 0x8e, 0x86, 0xd8, 0x22, 0x4e, 0xdd, 0xd0, 0x9f, 0x11, 0x57});
    for (const uint8_t low : {uint8_t{0xec}, uint8_t{0xed}, uint8_t{0xee}})
    {
        point.fill(0xff);
        point[0] = low;
        point[31] = 0x7f;
        points.push_back(point);
    }
    ASSERT_EQ(points.size(), static_cast<size_t>(7));

    const ServerIdentity identity = MakeIdentity();
    ClientHandshake probe;
    auto hello = probe.Begin(NETWORK_PROTOCOL_VERSION);
    ASSERT_TRUE(hello.has_value());
    const std::array<uint8_t, 32> scalar{9};
    for (auto candidate : points)
    {
        for (const bool highBit : {false, true})
        {
            candidate[31] = static_cast<uint8_t>(highBit ? (candidate[31] | 0x80) : (candidate[31] & 0x7f));
            EXPECT_TRUE(IsLowOrderX25519PublicKey(candidate));
            // Oracle: libsodium itself refuses every one of these points.
            std::array<uint8_t, 32> shared{};
            EXPECT_TRUE(crypto_scalarmult(shared.data(), scalar.data(), candidate.data()) != 0);

            ClientHello weak = *hello;
            std::copy(candidate.begin(), candidate.end(), weak.begin() + CLIENT_HELLO_EPHEMERAL_OFFSET);
            auto response = RespondToClientHello(weak, kPrefix, identity);
            ASSERT_FALSE(response.has_value());
            EXPECT_EQ(ErrorCode(response.error()), ErrorCode(HandshakeError::WeakSharedSecret));
        }
    }

    // Real ephemeral keys are never mistaken for small-order points.
    for (int i = 0; i < 64; ++i)
    {
        ClientHandshake client;
        auto fresh = client.Begin(NETWORK_PROTOCOL_VERSION);
        ASSERT_TRUE(fresh.has_value());
        EXPECT_FALSE(IsLowOrderX25519PublicKey(
            std::span<const uint8_t>(*fresh).subspan(CLIENT_HELLO_EPHEMERAL_OFFSET, HANDSHAKE_PUBLIC_KEY_SIZE)));
    }
    EXPECT_TRUE(IsLowOrderX25519PublicKey(std::span<const uint8_t>{})); // not a key at all
}
TEST(Transport_RateLimit_BucketPerSourceRefillAndCap)
{
    ConnectRateLimiter limiter;
    limiter.Configure(ConnectRateLimit{3.0f, 2.0f});
    constexpr uint32_t kAlice = 0x0A000001;
    constexpr uint32_t kBob = 0x0A000002;

    // Burst, then refusal; another source has its own bucket.
    for (int i = 0; i < 3; ++i)
        EXPECT_TRUE(limiter.Allow(kAlice, 0.0f));
    EXPECT_FALSE(limiter.Allow(kAlice, 0.0f));
    EXPECT_TRUE(limiter.Allow(kBob, 0.0f));

    // Refill is rate * elapsed, capped at the burst.
    EXPECT_FALSE(limiter.Allow(kAlice, 0.25f)); // 0.5 token
    EXPECT_TRUE(limiter.Allow(kAlice, 0.5f));   // 1.0 token
    EXPECT_FALSE(limiter.Allow(kAlice, 0.5f));
    for (int i = 0; i < 3; ++i)
        EXPECT_TRUE(limiter.Allow(kAlice, 100.0f));
    EXPECT_FALSE(limiter.Allow(kAlice, 100.0f));

    // Server time restarting at zero never strands a source.
    EXPECT_TRUE(limiter.Allow(kAlice, 0.0f));

    // A full table evicts only refilled sources; with none refilled, newcomers are refused.
    limiter.Configure(ConnectRateLimit{1.0f, 0.001f});
    for (uint32_t source = 0; source < ConnectRateLimiter::MAX_TRACKED_SOURCES; ++source)
        ASSERT_TRUE(limiter.Allow(source, 0.0f));
    EXPECT_EQ(limiter.TrackedSources(), ConnectRateLimiter::MAX_TRACKED_SOURCES);
    EXPECT_FALSE(limiter.Allow(0xFFFFFFF0u, 1.0f));   // nobody has refilled yet: fail closed
    EXPECT_TRUE(limiter.Allow(0xFFFFFFF0u, 2000.0f)); // everyone refilled: forgotten, room made
    EXPECT_TRUE(limiter.TrackedSources() <= 1u);

    // A non-positive burst refuses everything.
    limiter.Configure(ConnectRateLimit{0.0f, 100.0f});
    EXPECT_FALSE(limiter.Allow(kAlice, 5.0f));
}