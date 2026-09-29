/**
 * @file TestSessionGateProtocol.cpp
 * @brief MOD-320 canonical session-gate wire codec contract tests.
 */
#include "TestFramework.h"

#include "GameModules/SparkGameMMO/Source/Session/MMOSessionGateProtocol.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <vector>

using namespace MMO::SessionGateWire;

namespace
{
    template <size_t N> void SetText(std::array<char, N>& output, const char* text)
    {
        std::fill(output.begin(), output.end(), '\0');
        std::memcpy(output.data(), text, std::min(std::strlen(text), N - 1U));
    }

    Packet LoginPacket()
    {
        Packet packet{};
        packet.operation = Operation::Login;
        packet.requestId = 17;
        SetText(packet.username, "alice");
        SetText(packet.password, "correct horse");
        return packet;
    }
} // namespace

TEST(SessionGateProtocol_RoundTripsEveryOperation)
{
    const std::array<Operation, 7> operations = {Operation::Register,   Operation::Login, Operation::CreateCharacter,
                                                 Operation::EnterWorld, Operation::Move,  Operation::Interact,
                                                 Operation::State};
    for (const Operation operation : operations)
    {
        Packet expected{};
        expected.operation = operation;
        expected.requestId = 42;
        expected.accountId = 1001;
        expected.characterId = 2002;
        expected.targetId = 3003;
        expected.areaId = 4004;
        expected.interactionCount = 5;
        expected.x = operation == Operation::Move ? 0.75F : 12.5F;
        expected.y = 23.5F;
        expected.z = operation == Operation::Move ? -0.25F : 34.5F;
        expected.health = 99.0F;
        expected.race = 2;
        expected.classId = 7;
        SetText(expected.username, "alice");
        SetText(expected.password, "secret");
        SetText(expected.name, "Aster");
        if (operation == Operation::CreateCharacter)
        {
            expected.accountId = 1001;
        }

        const std::vector<uint8_t> bytes = Encode(expected);
        ASSERT_FALSE(bytes.empty());
        ASSERT_TRUE(bytes.size() <= MaxPacketBytes);
        Packet actual{};
        ASSERT_TRUE(Decode(bytes, actual));
        EXPECT_EQ(static_cast<uint8_t>(actual.operation), static_cast<uint8_t>(expected.operation));
        EXPECT_EQ(actual.requestId, expected.requestId);
        EXPECT_EQ(actual.accountId, expected.accountId);
        EXPECT_EQ(actual.characterId, expected.characterId);
        EXPECT_EQ(actual.targetId, expected.targetId);
        EXPECT_EQ(actual.areaId, expected.areaId);
        EXPECT_EQ(actual.interactionCount, expected.interactionCount);
        EXPECT_EQ(actual.race, expected.race);
        EXPECT_EQ(actual.classId, expected.classId);
        EXPECT_NEAR(actual.x, expected.x, 0.0001F);
        EXPECT_NEAR(actual.y, expected.y, 0.0001F);
        EXPECT_NEAR(actual.z, expected.z, 0.0001F);
        EXPECT_NEAR(actual.health, expected.health, 0.0001F);
    }
}

TEST(SessionGateProtocol_ResponseUsesAuthoritativeSnapshot)
{
    Packet expected{};
    expected.operation = Operation::Move;
    expected.status = Status::Ok;
    expected.response = true;
    expected.requestId = 19;
    expected.accountId = 10;
    expected.characterId = 20;
    expected.targetId = 30;
    expected.areaId = 40;
    expected.x = 1.0F;
    expected.y = 2.0F;
    expected.z = 3.0F;
    expected.health = 88.0F;
    expected.interactionCount = 9;

    const auto bytes = Encode(expected);
    Packet actual{};
    ASSERT_TRUE(Decode(bytes, actual));
    EXPECT_TRUE(actual.response);
    EXPECT_EQ(static_cast<uint8_t>(actual.status), static_cast<uint8_t>(Status::Ok));
    EXPECT_EQ(actual.accountId, 10U);
    EXPECT_EQ(actual.characterId, 20U);
    EXPECT_EQ(actual.targetId, 30U);
    EXPECT_EQ(actual.areaId, 40U);
    EXPECT_EQ(actual.interactionCount, 9U);
    EXPECT_NEAR(actual.x, 1.0F, 0.0001F);
    EXPECT_NEAR(actual.y, 2.0F, 0.0001F);
    EXPECT_NEAR(actual.z, 3.0F, 0.0001F);
    EXPECT_NEAR(actual.health, 88.0F, 0.0001F);
}

TEST(SessionGateProtocol_RejectsMalformedEnvelopeAndExactSizeViolations)
{
    const auto valid = Encode(LoginPacket());
    ASSERT_FALSE(valid.empty());
    Packet decoded{};
    EXPECT_FALSE(Decode({}, decoded));

    auto wrongMagic = valid;
    wrongMagic[0] ^= 1U;
    EXPECT_FALSE(Decode(wrongMagic, decoded));
    auto wrongVersion = valid;
    wrongVersion[2] = 2;
    EXPECT_FALSE(Decode(wrongVersion, decoded));
    auto unknownOperation = valid;
    unknownOperation[3] = 99;
    EXPECT_FALSE(Decode(unknownOperation, decoded));
    auto unknownFlags = valid;
    unknownFlags[5] = 2;
    EXPECT_FALSE(Decode(unknownFlags, decoded));

    auto truncated = valid;
    truncated.pop_back();
    EXPECT_FALSE(Decode(truncated, decoded));
    auto trailing = valid;
    trailing.push_back(0);
    EXPECT_FALSE(Decode(trailing, decoded));

    std::vector<uint8_t> oversized(MaxPacketBytes + 1U, 0);
    EXPECT_FALSE(Decode(oversized, decoded));
}

TEST(SessionGateProtocol_RejectsInvalidFieldsAndClearsOutput)
{
    Packet invalid = LoginPacket();
    invalid.x = std::numeric_limits<float>::quiet_NaN();
    EXPECT_TRUE(Encode(invalid).empty());

    invalid = LoginPacket();
    invalid.password[0] = static_cast<char>(0x01);
    EXPECT_TRUE(Encode(invalid).empty());

    invalid = LoginPacket();
    invalid.password[128] = 'x';
    EXPECT_TRUE(Encode(invalid).empty());

    const auto valid = Encode(LoginPacket());
    auto malformed = valid;
    malformed[10] = 129;
    Packet output{};
    output.accountId = 1234;
    output.username[0] = 'x';
    EXPECT_FALSE(Decode(malformed, output));
    EXPECT_EQ(output.accountId, 0U);
    EXPECT_EQ(output.username[0], '\0');
}
