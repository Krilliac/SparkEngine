#include "TestFramework.h"
#include "Utils/SecureRandom.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

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

TEST(SecureRandom_CreatesPrivateFilesWithoutOverwriting)
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / Spark::SecureRandom::HexToken(12);
    const std::filesystem::path file = root / "gateway.key";
    std::string error;
    EXPECT_TRUE(Spark::SecureRandom::CreatePrivateFile(file, "first-secret\n", &error));

    std::ifstream input(file, std::ios::binary);
    const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    EXPECT_EQ(contents, std::string("first-secret\n"));

    error.clear();
    EXPECT_FALSE(Spark::SecureRandom::CreatePrivateFile(file, "replacement\n", &error));
    EXPECT_FALSE(error.empty());

    std::ifstream unchanged(file, std::ios::binary);
    const std::string unchangedContents((std::istreambuf_iterator<char>(unchanged)), std::istreambuf_iterator<char>());
    EXPECT_EQ(unchangedContents, std::string("first-secret\n"));
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}
