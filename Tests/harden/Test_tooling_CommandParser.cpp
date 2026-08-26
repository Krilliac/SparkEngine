/**
 * @file Test_tooling_CommandParser.cpp
 * @brief Regression tests for SparkConsole's CommandParser.
 *
 * Covers the hardening fixes in the tooling bucket:
 * - Quote-aware tokenization is now wired into ConsoleApp::ExecuteCommand, so a
 *   quoted argument (echo "a b") must survive as a single token.
 * - Tokenize() classifies whitespace via std::isspace on unsigned char, so a
 *   high (>0x7F) byte must not invoke undefined behavior.
 * - Engine pipe records survive arbitrary byte-stream read boundaries and an
 *   oversized unterminated record cannot become a prompt/output flood.
 */

#include "TestFramework.h"

#include "../../SparkConsole/src/CommandParser.h"
#include "../../SparkConsole/src/PipeMessageFramer.h"

#include <string>
#include <vector>

TEST(CommandParser_TokenizePlainWords)
{
    std::vector<std::string> tokens = CommandParser::Tokenize("echo hello world");
    EXPECT_EQ(tokens.size(), static_cast<size_t>(3));
    EXPECT_EQ(tokens[0], "echo");
    EXPECT_EQ(tokens[1], "hello");
    EXPECT_EQ(tokens[2], "world");
}

TEST(CommandParser_TokenizeQuotedArgument)
{
    // The quoted region must collapse to one token with the inner space kept.
    std::vector<std::string> tokens = CommandParser::Tokenize("echo \"a b\"");
    EXPECT_EQ(tokens.size(), static_cast<size_t>(2));
    EXPECT_EQ(tokens[0], "echo");
    EXPECT_EQ(tokens[1], "a b");
}

TEST(CommandParser_TokenizeEmpty)
{
    EXPECT_EQ(CommandParser::Tokenize("").size(), static_cast<size_t>(0));
    EXPECT_EQ(CommandParser::Tokenize("   ").size(), static_cast<size_t>(0));
}

TEST(CommandParser_TokenizeHighByteNoCrash)
{
    // 0xE9 is a negative char on platforms with signed char; passing it straight
    // to std::isspace would be UB. The fix casts to unsigned char first.
    std::string commandLine = "ca" + std::string(1, static_cast<char>(0xE9)) + "fe";
    std::vector<std::string> tokens;
    EXPECT_NO_THROW(tokens = CommandParser::Tokenize(commandLine));
    EXPECT_EQ(tokens.size(), static_cast<size_t>(1));
}

TEST(CommandParser_ParseCommandLineBasic)
{
    std::string name;
    std::vector<std::string> args;
    bool result = CommandParser::ParseCommandLine("teleport 10 20 30", name, args);
    EXPECT_TRUE(result);
    EXPECT_EQ(name, "teleport");
    EXPECT_EQ(args.size(), static_cast<size_t>(3));
    EXPECT_EQ(args[0], "10");
    EXPECT_EQ(args[1], "20");
    EXPECT_EQ(args[2], "30");
}

TEST(CommandParser_ParseCommandLineEmpty)
{
    std::string name;
    std::vector<std::string> args;
    bool result = CommandParser::ParseCommandLine("", name, args);
    EXPECT_FALSE(result);
}

TEST(PipeMessageFramer_RetainsPartialRecordAcrossReads)
{
    PipeMessageFramer framer;
    EXPECT_TRUE(framer.Push("[INFO] split").empty());

    const auto lines = framer.Push(" record\n");
    ASSERT_EQ(lines.size(), static_cast<size_t>(1));
    EXPECT_EQ(lines[0], "[INFO] split record");
}

TEST(PipeMessageFramer_HandlesMultipleRecordsAndCRLF)
{
    PipeMessageFramer framer;
    const auto lines = framer.Push("first\r\nsecond\n\nthird");
    ASSERT_EQ(lines.size(), static_cast<size_t>(2));
    EXPECT_EQ(lines[0], "first");
    EXPECT_EQ(lines[1], "second");

    const auto tail = framer.Push(" record\n");
    ASSERT_EQ(tail.size(), static_cast<size_t>(1));
    EXPECT_EQ(tail[0], "third record");
}

TEST(PipeMessageFramer_OversizedSplitRecordProducesOneNotice)
{
    PipeMessageFramer framer;
    EXPECT_TRUE(framer.Push(std::string(PipeMessageFramer::kMaxLineBytes, 'x')).empty());
    EXPECT_TRUE(framer.Push("more data without a delimiter").empty());

    const auto lines = framer.Push(" and the end\nhealthy\n");
    ASSERT_EQ(lines.size(), static_cast<size_t>(2));
    EXPECT_EQ(lines[0], std::string(PipeMessageFramer::kOversizedLineNotice));
    EXPECT_EQ(lines[1], "healthy");
}
