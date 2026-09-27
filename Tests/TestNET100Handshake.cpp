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

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

using namespace Spark::Net;

namespace
{
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
        auto response = RespondToClientHello(*hello, identity);
        if (!response)
            return std::unexpected(response.error());
        auto channel = client.Finish(response->serverHello, pinned);
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
    auto response = RespondToClientHello(*hello, impostor);
    ASSERT_TRUE(response.has_value());
    std::copy(real.publicKey.begin(), real.publicKey.end(), response->serverHello.begin() + 1);
    auto channel = client.Finish(response->serverHello, real.publicKey);
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
        auto response = RespondToClientHello(*hello, identity);
        ASSERT_TRUE(response.has_value());
        response->serverHello[index] ^= 0x01;
        if (client.Finish(response->serverHello, identity.publicKey).has_value())
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
        auto response = RespondToClientHello(std::span(extendedClient).first(length), identity);
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
        auto response = RespondToClientHello(*hello, identity);
        ASSERT_TRUE(response.has_value());
        std::vector<uint8_t> serverHello(response->serverHello.begin(), response->serverHello.end());
        serverHello.resize(length, 0);
        auto channel = client.Finish(serverHello, identity.publicKey);
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
        auto response = RespondToClientHello(*hello, identity);
        ASSERT_FALSE(response.has_value());
        EXPECT_EQ(ErrorCode(response.error()), ErrorCode(HandshakeError::UnsupportedVersion));
    }

    // A rewritten suite in the ClientHello is refused by the server...
    {
        ClientHandshake client;
        auto hello = client.Begin(NETWORK_PROTOCOL_VERSION);
        ASSERT_TRUE(hello.has_value());
        (*hello)[6] = 0;
        auto response = RespondToClientHello(*hello, identity);
        ASSERT_FALSE(response.has_value());
        EXPECT_EQ(ErrorCode(response.error()), ErrorCode(HandshakeError::UnsupportedSuite));
    }

    // ...and in the ServerHello by the client.
    {
        ClientHandshake client;
        auto hello = client.Begin(NETWORK_PROTOCOL_VERSION);
        ASSERT_TRUE(hello.has_value());
        auto response = RespondToClientHello(*hello, identity);
        ASSERT_TRUE(response.has_value());
        response->serverHello[0] = 2;
        auto channel = client.Finish(response->serverHello, identity.publicKey);
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
        auto response = RespondToClientHello(altered, identity);
        ASSERT_TRUE(response.has_value());
        auto channel = client.Finish(response->serverHello, identity.publicKey);
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
    auto recorded = RespondToClientHello(*firstHello, identity);
    ASSERT_TRUE(recorded.has_value());
    ASSERT_TRUE(first.Finish(recorded->serverHello, identity.publicKey).has_value());

    // The recorded ServerHello is signed over the first client's ephemeral key.
    ClientHandshake second;
    ASSERT_TRUE(second.Begin(NETWORK_PROTOCOL_VERSION).has_value());
    auto replayed = second.Finish(recorded->serverHello, identity.publicKey);
    ASSERT_FALSE(replayed.has_value());
    EXPECT_EQ(ErrorCode(replayed.error()), ErrorCode(HandshakeError::BadSignature));

    // A finished (or failed) handshake cannot be reused.
    auto again = first.Finish(recorded->serverHello, identity.publicKey);
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
        auto response = RespondToClientHello(weak, identity);
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
        auto magic = RespondToClientHello(badMagic, identity);
        ASSERT_FALSE(magic.has_value());
        EXPECT_EQ(ErrorCode(magic.error()), ErrorCode(HandshakeError::Malformed));

        auto none = RespondToClientHello(empty, identity);
        ASSERT_FALSE(none.has_value());
        EXPECT_EQ(ErrorCode(none.error()), ErrorCode(HandshakeError::Malformed));

        auto noise = RespondToClientHello(garbage, identity);
        ASSERT_FALSE(noise.has_value());
        EXPECT_EQ(ErrorCode(noise.error()), ErrorCode(HandshakeError::Malformed));
    }

    // Finish before Begin is refused rather than using an unset ephemeral key.
    ClientHandshake unstarted;
    auto channel = unstarted.Finish(std::vector<uint8_t>(SERVER_HELLO_SIZE, 0), identity.publicKey);
    ASSERT_FALSE(channel.has_value());
    EXPECT_EQ(ErrorCode(channel.error()), ErrorCode(HandshakeError::InvalidState));
}

TEST(Transport_Handshake_HellosCarryNoSecretMaterial)
{
    const ServerIdentity identity = MakeIdentity();
    ClientHandshake client;
    auto hello = client.Begin(NETWORK_PROTOCOL_VERSION);
    ASSERT_TRUE(hello.has_value());
    auto response = RespondToClientHello(*hello, identity);
    ASSERT_TRUE(response.has_value());

    // Ed25519 secret keys are seed || public key; neither half of the seed may leak.
    const std::span<const uint8_t> seed(identity.secretKey.data(), 32);
    for (size_t offset = 0; offset + 8 <= seed.size(); offset += 8)
    {
        EXPECT_FALSE(Contains(response->serverHello, seed.subspan(offset, 8)));
        EXPECT_FALSE(Contains(*hello, seed.subspan(offset, 8)));
    }

    // The hellos are public; the channels they produce are not derivable from them.
    auto channel = client.Finish(response->serverHello, identity.publicKey);
    ASSERT_TRUE(channel.has_value());
    const std::vector<uint8_t> secretText{'h', 'u', 'n', 't', 'e', 'r', '2', '!'};
    std::vector<uint8_t> packet;
    ASSERT_TRUE((*channel)->Seal(secretText, packet));
    EXPECT_FALSE(Contains(packet, secretText));
    EXPECT_FALSE(Contains(response->serverHello, secretText));
}
