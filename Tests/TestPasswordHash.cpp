#include "TestFramework.h"
#include "Utils/PasswordHash.h"

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
    EXPECT_FALSE(Spark::PasswordHash::Verify(
        "secret", "pbkdf2-sha256$999999999$00112233445566778899aabbccddeeff$"
                  "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"));
    EXPECT_TRUE(Spark::PasswordHash::Create(std::string(1025, 'x')).empty());
}

TEST(SparkPasswordHash_MatchesPublishedPBKDF2Sha256Construction)
{
    // Independently generated with Python's hashlib.pbkdf2_hmac using the
    // password "password", the 16-byte salt below, 600,000 rounds, and dkLen=32.
    const std::string knownAnswer =
        "pbkdf2-sha256$600000$00112233445566778899aabbccddeeff$"
        "8cb706e2cabf91c72c10ab9524294fa38f247d34f3f93842bcb05b8aaa66d334";

    EXPECT_TRUE(Spark::PasswordHash::Verify("password", knownAnswer));
    EXPECT_FALSE(Spark::PasswordHash::Verify("Password", knownAnswer));
}
