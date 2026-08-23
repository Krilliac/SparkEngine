#include "TestFramework.h"
#include "Utils/SecureRandom.h"

#include <algorithm>
#include <cctype>

TEST(SecureRandom_ProducesBoundedHexTokens)
{
    const std::string first = Spark::SecureRandom::HexToken(16);
    const std::string second = Spark::SecureRandom::HexToken(16);
    EXPECT_EQ(first.size(), size_t{32});
    EXPECT_EQ(second.size(), size_t{32});
    EXPECT_NE(first, second);
    EXPECT_TRUE(std::all_of(first.begin(), first.end(), [](unsigned char c) { return std::isxdigit(c) != 0; }));
    EXPECT_TRUE(Spark::SecureRandom::HexToken(0).empty());
    EXPECT_TRUE(Spark::SecureRandom::HexToken(1025).empty());
}

TEST(SecureRandom_FillValidatesItsBuffer)
{
    unsigned char bytes[32] = {};
    EXPECT_TRUE(Spark::SecureRandom::Fill(bytes, sizeof(bytes)));
    EXPECT_FALSE(Spark::SecureRandom::Fill(nullptr, 1));
    EXPECT_TRUE(Spark::SecureRandom::Fill(nullptr, 0));
    EXPECT_TRUE(std::any_of(std::begin(bytes), std::end(bytes), [](unsigned char value) { return value != 0; }));
}
