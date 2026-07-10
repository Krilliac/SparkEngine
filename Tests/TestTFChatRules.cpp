/**
 * @file TestTFChatRules.cpp
 * @brief Standalone validation of TERRAFRONT chat normalization and routing.
 */
#include "TestFramework.h"

#include "Net/TFChatRules.h"

#include <array>
#include <cstring>

using namespace Terrafront;

static_assert(sizeof(TF_ChatMsg) == 128);
static_assert(sizeof(TF_ChatMsg{}.text) == 122);

TEST(TFChat_NormalizeTrimsAndCollapsesWhitespace)
{
    const char input[] = "  rally\t\n  at   the crown  ";
    char output[122]{};
    EXPECT_TRUE(NormalizeChatText(input, sizeof(input), output, sizeof(output)));
    EXPECT_TRUE(std::strcmp(output, "rally at the crown") == 0);
}

TEST(TFChat_NormalizeBoundsAndTerminates)
{
    char input[256];
    std::memset(input, 'x', sizeof(input));
    char output[122];
    std::memset(output, 'z', sizeof(output));
    EXPECT_TRUE(NormalizeChatText(input, sizeof(input), output, sizeof(output)));
    EXPECT_EQ(std::strlen(output), size_t{121});
    EXPECT_EQ(output[121], '\0');
}

TEST(TFChat_NormalizeDoesNotSplitUtf8AtWireBoundary)
{
    const char input[] = "A\xE2\x82\xAC" "B"; // A, euro sign, B

    std::array<char, 5> exact{};
    EXPECT_TRUE(NormalizeChatText(input, sizeof(input) - 1, exact.data(), exact.size()));
    EXPECT_EQ(std::strlen(exact.data()), size_t{4});
    EXPECT_TRUE(std::memcmp(exact.data(), "A\xE2\x82\xAC", 4) == 0);

    std::array<char, 4> truncated{};
    EXPECT_TRUE(NormalizeChatText(input, sizeof(input) - 1,
                                  truncated.data(), truncated.size()));
    EXPECT_TRUE(std::strcmp(truncated.data(), "A") == 0);
}

TEST(TFChat_NormalizeRejectsMalformedUtf8)
{
    const char truncated[] = {'o', 'k', ' ', static_cast<char>(0xE2),
                              static_cast<char>(0x82)};
    char output[122];
    std::memset(output, 'z', sizeof(output));
    EXPECT_FALSE(NormalizeChatText(truncated, sizeof(truncated), output, sizeof(output)));
    EXPECT_EQ(output[0], '\0');

    const char overlong[] = {static_cast<char>(0xC0), static_cast<char>(0xAF)};
    EXPECT_FALSE(NormalizeChatText(overlong, sizeof(overlong), output, sizeof(output)));
    EXPECT_EQ(output[0], '\0');

    const char surrogate[] = {static_cast<char>(0xED), static_cast<char>(0xA0),
                              static_cast<char>(0x80)};
    EXPECT_FALSE(NormalizeChatText(surrogate, sizeof(surrogate), output, sizeof(output)));
    EXPECT_EQ(output[0], '\0');
}

TEST(TFChat_NormalizeWireFieldKeepsWholeFinalCodePoint)
{
    char input[123]{};
    std::memset(input, 'x', 118);
    input[118] = static_cast<char>(0xF0);
    input[119] = static_cast<char>(0x9F);
    input[120] = static_cast<char>(0x98);
    input[121] = static_cast<char>(0x80);

    TF_ChatMsg msg{};
    EXPECT_TRUE(NormalizeChatText(input, 122, msg.text, sizeof(msg.text)));
    EXPECT_EQ(std::strlen(msg.text), size_t{118});
    EXPECT_EQ(msg.text[118], '\0');
}

TEST(TFChat_NormalizeRejectsWhitespaceOnly)
{
    const char input[] = " \r\n\t ";
    char output[122]{};
    EXPECT_FALSE(NormalizeChatText(input, sizeof(input), output, sizeof(output)));
    EXPECT_EQ(output[0], '\0');
}

TEST(TFChat_NormalizeHandlesTinyAndEmptyBuffers)
{
    char oneByte[1]{'z'};
    EXPECT_FALSE(NormalizeChatText("x", 1, oneByte, sizeof(oneByte)));
    EXPECT_EQ(oneByte[0], '\0');

    char output[4]{'z', 'z', 'z', 'z'};
    EXPECT_FALSE(NormalizeChatText("", 0, output, sizeof(output)));
    EXPECT_EQ(output[0], '\0');
    EXPECT_FALSE(NormalizeChatText(nullptr, 0, output, sizeof(output)));
    EXPECT_FALSE(NormalizeChatText("x", 1, nullptr, 0));
}

TEST(TFChat_ChannelValidationPinsWireRange)
{
    EXPECT_TRUE(IsValidChatChannel(static_cast<uint8_t>(ChatChannel::Region)));
    EXPECT_TRUE(IsValidChatChannel(static_cast<uint8_t>(ChatChannel::Yell)));
    EXPECT_FALSE(IsValidChatChannel(4));
    EXPECT_FALSE(IsValidChatChannel(255));
}

TEST(TFChat_SenderAlwaysReceivesAcceptedMessage)
{
    TFChatRouteView view{};
    view.isSender = true;
    EXPECT_TRUE(ShouldReceiveChat(ChatChannel::Region, view));
    EXPECT_TRUE(ShouldReceiveChat(ChatChannel::Faction, view));
    EXPECT_TRUE(ShouldReceiveChat(ChatChannel::Squad, view));
    EXPECT_TRUE(ShouldReceiveChat(ChatChannel::Yell, view));
    EXPECT_FALSE(ShouldReceiveChat(static_cast<ChatChannel>(255), view));
}

TEST(TFChat_ChannelScopesAreIndependent)
{
    TFChatRouteView view{};
    view.sameRegion = true;
    EXPECT_TRUE(ShouldReceiveChat(ChatChannel::Region, view));
    EXPECT_FALSE(ShouldReceiveChat(ChatChannel::Faction, view));
    EXPECT_FALSE(ShouldReceiveChat(ChatChannel::Squad, view));

    view = {};
    view.sameFaction = true;
    EXPECT_TRUE(ShouldReceiveChat(ChatChannel::Faction, view));

    view = {};
    view.sameSquad = true;
    EXPECT_TRUE(ShouldReceiveChat(ChatChannel::Squad, view));
}

TEST(TFChat_YellRequiresPawnsWithinInclusiveRange)
{
    TFChatRouteView view{};
    view.bothHavePawns = true;
    view.distanceSq = kTFYellRangeM * kTFYellRangeM;
    EXPECT_TRUE(ShouldReceiveChat(ChatChannel::Yell, view));
    view.distanceSq += 0.01f;
    EXPECT_FALSE(ShouldReceiveChat(ChatChannel::Yell, view));
    view.distanceSq = 0.0f;
    view.bothHavePawns = false;
    EXPECT_FALSE(ShouldReceiveChat(ChatChannel::Yell, view));
    view.bothHavePawns = true;
    view.distanceSq = -0.01f;
    EXPECT_FALSE(ShouldReceiveChat(ChatChannel::Yell, view));
}
