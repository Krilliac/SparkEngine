#include "TestFramework.h"
#include "Utils/PasswordHash.h"

#include <array>

TEST(SparkPasswordHash_CreatesSelfDescribingUniqueHashes)
{
    const std::string first = Spark::PasswordHash::Create("correct horse battery staple");
    const std::string second = Spark::PasswordHash::Create("correct horse battery staple");
    EXPECT_TRUE(first.rfind("pbkdf2-sha256$600000$", 0) == 0);
    EXPECT_TRUE(second.rfind("pbkdf2-sha256$600000$", 0) == 0);
    EXPECT_NE(first, second);
    EXPECT_TRUE(Spark::PasswordHash::Verify("correct horse battery staple", first));
    EXPECT_FALSE(Spark::PasswordHash::Verify("wrong password", first));
}

TEST(SparkPasswordHash_RejectsLegacyMalformedAndUnboundedWorkFactors)
{
    EXPECT_FALSE(Spark::PasswordHash::Verify("secret", "1234abcd"));
    EXPECT_FALSE(Spark::PasswordHash::Verify("secret", "pbkdf2-sha256$not-a-number$00$00"));
    EXPECT_FALSE(Spark::PasswordHash::Verify("secret",
                                             "pbkdf2-sha256$999999999$00112233445566778899aabbccddeeff$"
                                             "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"));
    EXPECT_TRUE(Spark::PasswordHash::Create(std::string(1025, 'x')).empty());
}

TEST(SparkPasswordHash_MatchesPublishedPBKDF2Sha256Construction)
{
    // Independently generated with Python's hashlib.pbkdf2_hmac using the
    // password "password", the 16-byte salt below, 600,000 rounds, and dkLen=32.
    const std::string knownAnswer = "pbkdf2-sha256$600000$00112233445566778899aabbccddeeff$"
                                    "8cb706e2cabf91c72c10ab9524294fa38f247d34f3f93842bcb05b8aaa66d334";

    EXPECT_TRUE(Spark::PasswordHash::Verify("password", knownAnswer));
    EXPECT_FALSE(Spark::PasswordHash::Verify("Password", knownAnswer));
}

TEST(SparkPasswordHash_PreservesPreMigrationHmacBytes)
{
    // Captured from PasswordHash.cpp at 4dd5aaf79 before the libsodium migration.
    std::array<uint8_t, 80> key{};
    for (size_t i = 0; i < key.size(); ++i)
    {
        key[i] = static_cast<uint8_t>(i);
    }
    const std::array<uint8_t, 5> data{0x00, 0x41, 0x80, 0xff, 0x42};
    const auto digest = Spark::PasswordHash::ComputeHmacSha256(key, data);
    static constexpr char digits[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(digest.size() * 2);
    for (uint8_t value : digest)
    {
        hex.push_back(digits[value >> 4]);
        hex.push_back(digits[value & 0x0f]);
    }
    EXPECT_EQ(hex, "24f3f58a1deaec238e54cde4e26c670c50037b52d339425ae6911b283127b2bc");
}
