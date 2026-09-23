/**
 * @file TestNET100TransportReal.cpp
 * @brief NET-100: production-linked hostile tests for the ChaCha20-Poly1305 SecureChannel.
 *
 * Every test calls the shipped Spark::Net code in NetworkEncryption.cpp (no
 * mirrored reimplementation). The known-answer vectors pin the primitive to
 * RFC 8439; the remaining tests prove each channel property fails closed.
 */

#include "TestFramework.h"
#include "Engine/Networking/NetworkEncryption.h"

#include <algorithm>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <vector>

using namespace Spark::Net;

namespace
{
    std::vector<uint8_t> FromHex(std::string_view hex)
    {
        std::vector<uint8_t> out;
        for (size_t i = 0; i + 1 < hex.size(); i += 2)
            out.push_back(static_cast<uint8_t>(std::stoul(std::string(hex.substr(i, 2)), nullptr, 16)));
        return out;
    }

    template <size_t N> std::array<uint8_t, N> ArrayFromHex(std::string_view hex)
    {
        std::array<uint8_t, N> out{};
        const auto bytes = FromHex(hex);
        std::copy_n(bytes.begin(), N, out.begin());
        return out;
    }

    std::vector<uint8_t> Bytes(std::string_view text)
    {
        return {text.begin(), text.end()};
    }

    SessionKey TestSecret(uint8_t seed)
    {
        SessionKey key{};
        for (size_t i = 0; i < key.size(); ++i)
            key[i] = static_cast<uint8_t>(seed + i * 31);
        return key;
    }

    bool Contains(const std::vector<uint8_t>& haystack, const std::vector<uint8_t>& needle)
    {
        return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end()) != haystack.end();
    }

    constexpr std::string_view kSunscreen = "Ladies and Gentlemen of the class of '99: If I could offer you only one "
                                            "tip for the future, sunscreen would be it.";
} // namespace

// ============================================================================
// Known-answer vectors (the primitive is RFC 8439, not a homemade cipher)
// ============================================================================

TEST(Transport_KnownAnswer_Rfc8439AeadVector)
{
    // RFC 8439 section 2.8.2.
    const auto key = ArrayFromHex<32>("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f");
    const auto nonce = ArrayFromHex<12>("070000004041424344454647");
    const auto aad = FromHex("50515253c0c1c2c3c4c5c6c7");
    const auto expected =
        FromHex("d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e8ca9671282fafb"
                "69da92728b1a71de0a9e060b2905d6a5b67ecd3b3692ddbd7f2d778b8c9803aee328091b58fab324e4fa"
                "d675945585808b4831d7bc3ff4def08e4b7a9de576d26586cec64b6116"
                "1ae10b594f09e26a7e902ecbd0600691");

    const auto sealed = ChaCha20Poly1305Seal(key, nonce, aad, Bytes(kSunscreen));
    EXPECT_EQ(sealed.size(), expected.size());
    EXPECT_TRUE(sealed == expected);

    std::vector<uint8_t> opened;
    EXPECT_TRUE(ChaCha20Poly1305Open(key, nonce, aad, sealed, opened));
    EXPECT_TRUE(opened == Bytes(kSunscreen));
}

TEST(Transport_KnownAnswer_MultiBlockEmptyAadCrossCheckedWithOpenSSL)
{
    // 300-byte payload (5 ChaCha20 blocks, partial last block), empty AAD.
    // Expected output generated independently with OpenSSL 3.0 `enc -chacha20` and `mac POLY1305`.
    const auto key = ArrayFromHex<32>("1c9240a5eb55d38af333888604f6b5f0473917c1402b80099dca5cbc207075c0");
    const auto nonce = ArrayFromHex<12>("000000000102030405060708");
    std::vector<uint8_t> plaintext(300);
    for (size_t i = 0; i < plaintext.size(); ++i)
        plaintext[i] = static_cast<uint8_t>(i * 7 + 3);

    const auto expected =
        FromHex("2ec4e36818ce52b476f659f6aacc55f14c88b855dfb80f4b2e9abaa563d7340bc1f27ec0fc3a2293df9ca6117d3302dc"
                "838cae3d301811a4300482acfb8e011e9f8b26beddcd1b5407eacc3bd335d3c807e226422eaf0bbd27ab895408d53caa"
                "51593df6244402da7817b4d2b9bf6650057ec8732a3e5791c084287457d37ffd54e88ece8552c0937020bbb27354242b"
                "43f3b08c62561f13a7d1e880954d7710a54e31caa7cd43a1b5d18be1726ce5c3acb3af9871e4c18f76244873b3153254"
                "2d9e054a69f2788842ba0332c3b58f831121e0fc820ce8d4fb3dff130073076907ec49954a152fe0d562f8e12087879c"
                "8c23cb8ef603cb7072c0c68dfbf3b6b2d7c23edf3322cdc43df67f3ee8a08c6e74b8856c5845cfe51f02240fdc24e013"
                "559ba586ca1fc4588e907de0"
                "e21db46609fa5e352afbf43ad1a89b99");

    const auto sealed = ChaCha20Poly1305Seal(key, nonce, {}, plaintext);
    EXPECT_TRUE(sealed == expected);
}

// ============================================================================
// Tamper / wrong key / wrong direction / AAD substitution
// ============================================================================

TEST(Transport_Tamper_EveryBitFlipIsRejected)
{
    SecureChannel client(TestSecret(1), ChannelRole::Client);
    SecureChannel server(TestSecret(1), ChannelRole::Server);

    std::vector<uint8_t> packet;
    EXPECT_TRUE(client.Seal(Bytes("move 12 34"), packet));

    // Flip every bit of the header, ciphertext and tag in turn. Only the sequence bytes may
    // legitimately produce a different rejection reason; none may be accepted.
    int accepted = 0;
    for (size_t byte = 0; byte < packet.size(); ++byte)
    {
        for (int bit = 0; bit < 8; ++bit)
        {
            auto tampered = packet;
            tampered[byte] ^= static_cast<uint8_t>(1u << bit);
            std::vector<uint8_t> out;
            if (server.Open(tampered, out) == OpenResult::Ok)
                ++accepted;
            EXPECT_TRUE(out.empty());
        }
    }
    EXPECT_EQ(accepted, 0);

    // The untouched original still authenticates afterwards: forgeries did not poison the window.
    std::vector<uint8_t> out;
    EXPECT_EQ(static_cast<int>(server.Open(packet, out)), static_cast<int>(OpenResult::Ok));
    EXPECT_TRUE(out == Bytes("move 12 34"));
}

TEST(Transport_Tamper_WrongKeyAndReflectionFail)
{
    SecureChannel client(TestSecret(1), ChannelRole::Client);
    SecureChannel attackerServer(TestSecret(2), ChannelRole::Server);
    SecureChannel otherClient(TestSecret(1), ChannelRole::Client);

    std::vector<uint8_t> packet;
    EXPECT_TRUE(client.Seal(Bytes("hello"), packet));

    std::vector<uint8_t> out;
    EXPECT_EQ(static_cast<int>(attackerServer.Open(packet, out)), static_cast<int>(OpenResult::AuthenticationFailed));
    // Reflection: a client-to-server packet must not authenticate in the server-to-client direction.
    EXPECT_EQ(static_cast<int>(otherClient.Open(packet, out)), static_cast<int>(OpenResult::AuthenticationFailed));
}

TEST(Transport_Tamper_AssociatedDataSubstitutionFails)
{
    SecureChannel client(TestSecret(3), ChannelRole::Client);
    SecureChannel server(TestSecret(3), ChannelRole::Server);

    const std::vector<uint8_t> chatType{0x10, 0x00};
    const std::vector<uint8_t> adminType{0x7f, 0x00};
    std::vector<uint8_t> packet;
    EXPECT_TRUE(client.Seal(Bytes("gg"), packet, chatType));

    std::vector<uint8_t> out;
    EXPECT_EQ(static_cast<int>(server.Open(packet, out, adminType)),
              static_cast<int>(OpenResult::AuthenticationFailed));
    EXPECT_EQ(static_cast<int>(server.Open(packet, out, chatType)), static_cast<int>(OpenResult::Ok));
}

// ============================================================================
// Replay, nonce reuse, reorder
// ============================================================================

TEST(Transport_Replay_DuplicatePacketRejected)
{
    SecureChannel client(TestSecret(4), ChannelRole::Client);
    SecureChannel server(TestSecret(4), ChannelRole::Server);

    std::vector<uint8_t> packet;
    EXPECT_TRUE(client.Seal(Bytes("fire"), packet));
    std::vector<uint8_t> out;
    EXPECT_EQ(static_cast<int>(server.Open(packet, out)), static_cast<int>(OpenResult::Ok));
    EXPECT_EQ(static_cast<int>(server.Open(packet, out)), static_cast<int>(OpenResult::Replayed));
    EXPECT_TRUE(out.empty());
}

TEST(Transport_Replay_NonceReuseByRestartedSenderRejected)
{
    // A sender that restarts with the same session secret (no new handshake) reuses
    // sequence 1, i.e. the same (key, nonce). The receiver must reject it even though it
    // authenticates, otherwise the two ciphertexts leak plaintext XOR (the old XOR flaw).
    SecureChannel server(TestSecret(5), ChannelRole::Server);
    std::vector<uint8_t> first;
    std::vector<uint8_t> second;
    {
        SecureChannel client(TestSecret(5), ChannelRole::Client);
        EXPECT_TRUE(client.Seal(Bytes("attack at dawn"), first));
    }
    {
        SecureChannel restartedClient(TestSecret(5), ChannelRole::Client);
        EXPECT_TRUE(restartedClient.Seal(Bytes("attack at dusk"), second));
    }

    std::vector<uint8_t> out;
    EXPECT_EQ(static_cast<int>(server.Open(first, out)), static_cast<int>(OpenResult::Ok));
    EXPECT_EQ(static_cast<int>(server.Open(second, out)), static_cast<int>(OpenResult::Replayed));
}

TEST(Transport_Replay_SenderNeverReusesANonce)
{
    SecureChannel client(TestSecret(6), ChannelRole::Client);
    std::vector<uint8_t> a;
    std::vector<uint8_t> b;
    EXPECT_TRUE(client.Seal(Bytes("same payload"), a));
    EXPECT_TRUE(client.Seal(Bytes("same payload"), b));
    // Header sequence advances and the ciphertext differs for identical plaintext.
    EXPECT_FALSE(std::equal(a.begin() + 2, a.begin() + SECURE_HEADER_SIZE, b.begin() + 2));
    EXPECT_FALSE(std::equal(a.begin() + SECURE_HEADER_SIZE, a.end(), b.begin() + SECURE_HEADER_SIZE));
}

TEST(Transport_Replay_ReorderInsideWindowOnceOutsideWindowRejected)
{
    SecureChannel client(TestSecret(7), ChannelRole::Client);
    SecureChannel server(TestSecret(7), ChannelRole::Server);

    std::vector<std::vector<uint8_t>> packets(ReplayProtection::WINDOW_SIZE + 10);
    for (auto& packet : packets)
        EXPECT_TRUE(client.Seal(Bytes("tick"), packet));

    std::vector<uint8_t> out;
    // Deliver 3, 1, 2: UDP reordering within the window is accepted exactly once each.
    EXPECT_EQ(static_cast<int>(server.Open(packets[2], out)), static_cast<int>(OpenResult::Ok));
    EXPECT_EQ(static_cast<int>(server.Open(packets[0], out)), static_cast<int>(OpenResult::Ok));
    EXPECT_EQ(static_cast<int>(server.Open(packets[1], out)), static_cast<int>(OpenResult::Ok));
    EXPECT_EQ(static_cast<int>(server.Open(packets[0], out)), static_cast<int>(OpenResult::Replayed));

    // Jump past the window: an undelivered but now-too-old packet is dropped.
    EXPECT_EQ(static_cast<int>(server.Open(packets.back(), out)), static_cast<int>(OpenResult::Ok));
    EXPECT_EQ(static_cast<int>(server.Open(packets[3], out)), static_cast<int>(OpenResult::Replayed));
}

// ============================================================================
// Downgrade / malformed / truncation / bounds
// ============================================================================

TEST(Transport_Downgrade_OtherVersionsFailClosed)
{
    SecureChannel client(TestSecret(8), ChannelRole::Client);
    SecureChannel server(TestSecret(8), ChannelRole::Server);
    std::vector<uint8_t> packet;
    EXPECT_TRUE(client.Seal(Bytes("login"), packet));

    for (int version : {0, 2, 0xff})
    {
        auto downgraded = packet;
        downgraded[0] = static_cast<uint8_t>(version);
        std::vector<uint8_t> out;
        EXPECT_EQ(static_cast<int>(server.Open(downgraded, out)), static_cast<int>(OpenResult::UnsupportedVersion));
    }

    // A bare plaintext frame (the pre-NET-100 wire) is never accepted.
    std::vector<uint8_t> out;
    auto plaintext = Bytes("plaintext frame that is long enough to pass length checks");
    EXPECT_FALSE(server.Open(plaintext, out) == OpenResult::Ok);
}

TEST(Transport_Downgrade_TruncationAndBoundsFailClosed)
{
    SecureChannel client(TestSecret(9), ChannelRole::Client);
    SecureChannel server(TestSecret(9), ChannelRole::Server);
    std::vector<uint8_t> packet;
    EXPECT_TRUE(client.Seal(Bytes("inventory"), packet));

    for (size_t length = 0; length < packet.size(); ++length)
    {
        std::vector<uint8_t> truncated(packet.begin(), packet.begin() + length);
        std::vector<uint8_t> out;
        EXPECT_FALSE(server.Open(truncated, out) == OpenResult::Ok);
    }

    // Sequence 0 is never produced by a sender and is rejected before crypto.
    auto zeroSequence = packet;
    std::fill(zeroSequence.begin() + 2, zeroSequence.begin() + SECURE_HEADER_SIZE, uint8_t{0});
    std::vector<uint8_t> out;
    EXPECT_EQ(static_cast<int>(server.Open(zeroSequence, out)), static_cast<int>(OpenResult::Malformed));

    // Oversized input is refused on both sides.
    std::vector<uint8_t> huge(SECURE_MAX_PAYLOAD + 1, 0xab);
    std::vector<uint8_t> sealed;
    EXPECT_FALSE(client.Seal(huge, sealed));
    huge.resize(SECURE_PACKET_OVERHEAD + SECURE_MAX_PAYLOAD + 1);
    EXPECT_EQ(static_cast<int>(server.Open(huge, out)), static_cast<int>(OpenResult::Malformed));
}

// ============================================================================
// Key rotation
// ============================================================================

TEST(Transport_KeyRotation_RatchetsForwardAndRetiresOldEpoch)
{
    SecureChannel client(TestSecret(10), ChannelRole::Client);
    SecureChannel server(TestSecret(10), ChannelRole::Server);

    std::vector<uint8_t> oldEpochPacket;
    std::vector<uint8_t> oldEpochLate;
    EXPECT_TRUE(client.Seal(Bytes("before"), oldEpochPacket));
    EXPECT_TRUE(client.Seal(Bytes("late"), oldEpochLate));
    std::vector<uint8_t> out;
    EXPECT_EQ(static_cast<int>(server.Open(oldEpochPacket, out)), static_cast<int>(OpenResult::Ok));

    EXPECT_TRUE(client.RotateSendKey());
    EXPECT_EQ(static_cast<int>(client.GetSendEpoch()), 1);
    std::vector<uint8_t> newEpochPacket;
    EXPECT_TRUE(client.Seal(Bytes("after"), newEpochPacket));
    // Sequence restarts at 1 under the new key; epoch 1 key differs, so no nonce reuse.
    EXPECT_EQ(static_cast<int>(newEpochPacket[1]), 1);

    EXPECT_EQ(static_cast<int>(server.Open(newEpochPacket, out)), static_cast<int>(OpenResult::Ok));
    EXPECT_TRUE(out == Bytes("after"));
    EXPECT_EQ(static_cast<int>(server.GetReceiveEpoch()), 1);

    // Once the receiver ratchets, old-epoch traffic is retired.
    EXPECT_EQ(static_cast<int>(server.Open(oldEpochLate, out)), static_cast<int>(OpenResult::UnknownKeyEpoch));
}

TEST(Transport_KeyRotation_ForgedEpochCannotAdvanceReceiver)
{
    SecureChannel client(TestSecret(11), ChannelRole::Client);
    SecureChannel server(TestSecret(11), ChannelRole::Server);
    std::vector<uint8_t> packet;
    EXPECT_TRUE(client.Seal(Bytes("x"), packet));

    auto forged = packet;
    forged[1] = 1; // claim the next epoch without knowing its key
    std::vector<uint8_t> out;
    EXPECT_EQ(static_cast<int>(server.Open(forged, out)), static_cast<int>(OpenResult::AuthenticationFailed));
    EXPECT_EQ(static_cast<int>(server.GetReceiveEpoch()), 0);

    forged[1] = 2; // skipping an epoch is never accepted
    EXPECT_EQ(static_cast<int>(server.Open(forged, out)), static_cast<int>(OpenResult::UnknownKeyEpoch));
    EXPECT_EQ(static_cast<int>(server.Open(packet, out)), static_cast<int>(OpenResult::Ok));
}

TEST(Transport_KeyRotation_EpochSpaceExhaustionFailsClosed)
{
    SecureChannel client(TestSecret(12), ChannelRole::Client);
    int rotations = 0;
    while (client.RotateSendKey())
        ++rotations;
    EXPECT_EQ(rotations, 255);
    EXPECT_FALSE(client.RotateSendKey());
    EXPECT_EQ(static_cast<int>(client.GetSendEpoch()), 255);
}

// ============================================================================
// Fuzz / capture / key generation
// ============================================================================

TEST(Transport_FuzzPacket_RandomMutationsNeverAuthenticate)
{
    SecureChannel client(TestSecret(13), ChannelRole::Client);
    SecureChannel server(TestSecret(13), ChannelRole::Server);
    std::mt19937 rng(0x5eed); // deterministic mutation schedule, not key material

    int accepted = 0;
    for (int iteration = 0; iteration < 2000; ++iteration)
    {
        std::vector<uint8_t> packet;
        std::vector<uint8_t> payload(rng() % 96);
        for (auto& b : payload)
            b = static_cast<uint8_t>(rng());
        EXPECT_TRUE(client.Seal(payload, packet));

        switch (rng() % 4)
        {
        case 0: // random byte overwrite
            packet[rng() % packet.size()] ^= static_cast<uint8_t>(1 + rng() % 255);
            break;
        case 1: // truncate
            packet.resize(rng() % packet.size());
            break;
        case 2: // append garbage
            for (uint32_t i = 0, n = 1 + rng() % 32; i < n; ++i)
                packet.push_back(static_cast<uint8_t>(rng()));
            break;
        default: // fully random frame
            for (auto& b : packet)
                b = static_cast<uint8_t>(rng());
            break;
        }

        std::vector<uint8_t> out;
        if (server.Open(packet, out) == OpenResult::Ok)
            ++accepted;
    }
    EXPECT_EQ(accepted, 0);
}

TEST(Transport_Capture_CredentialBytesNeverAppearOnTheWire)
{
    SecureChannel client(TestSecret(14), ChannelRole::Client);
    SecureChannel server(TestSecret(14), ChannelRole::Server);

    // Same shape as TF_AuthRequest: 32-byte user + 64-byte password.
    std::vector<uint8_t> authRequest(96, 0);
    const std::string user = "pilot_one";
    const std::string pass = "correct horse battery staple";
    std::copy(user.begin(), user.end(), authRequest.begin());
    std::copy(pass.begin(), pass.end(), authRequest.begin() + 32);

    std::vector<uint8_t> packet;
    EXPECT_TRUE(client.Seal(authRequest, packet));
    EXPECT_FALSE(Contains(packet, Bytes(pass)));
    EXPECT_FALSE(Contains(packet, Bytes(user)));

    std::vector<uint8_t> out;
    EXPECT_EQ(static_cast<int>(server.Open(packet, out)), static_cast<int>(OpenResult::Ok));
    EXPECT_TRUE(out == authRequest);
}

TEST(Transport_KeyGeneration_CsprngKeysAndTokens)
{
    SessionKey a{};
    SessionKey b{};
    EXPECT_TRUE(GenerateSessionKey(a));
    EXPECT_TRUE(GenerateSessionKey(b));
    EXPECT_FALSE(a == b);
    EXPECT_FALSE(a == SessionKey{});

    ConnectionToken t1{};
    ConnectionToken t2{};
    EXPECT_TRUE(GenerateConnectionToken(t1));
    EXPECT_TRUE(GenerateConnectionToken(t2));
    EXPECT_TRUE(ValidateToken(t1, t1));
    EXPECT_FALSE(ValidateToken(t1, t2));

    const std::vector<uint8_t> shortSpan{1, 2, 3};
    const std::vector<uint8_t> longSpan{1, 2, 3, 4};
    EXPECT_FALSE(ConstantTimeEqual(shortSpan, longSpan));
}
