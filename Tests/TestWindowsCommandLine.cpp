/**
 * @file TestWindowsCommandLine.cpp
 * @brief Regression tests for quoted Windows engine startup arguments.
 */

#include "TestFramework.h"

#ifdef SPARK_PLATFORM_WINDOWS

#include "Core/WindowsCommandLine.h"

TEST(WindowsCommandLine_ParsesQuotedPathWithSpaces)
{
    const auto value = Spark::Platform::FindWindowsCommandLineUtf8Argument(
        L"-game \"D:\\Projects With Spaces\\Playable Game.dll\" -test-frames 1", L"-game");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, std::string("D:\\Projects With Spaces\\Playable Game.dll"));
}

TEST(WindowsCommandLine_PreservesUnicodeAsUtf8)
{
    const auto value = Spark::Platform::FindWindowsCommandLineUtf8Argument(
        L"-exec \"D:\\Caf\u00e9 Project\\smoke commands.cfg\"", L"-exec");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, std::string("D:\\Caf\xC3\xA9 Project\\smoke commands.cfg"));
}

TEST(WindowsCommandLine_ParsesExplicitUnicodeManifest)
{
    const auto value = Spark::Platform::FindWindowsCommandLineUtf8Argument(
        L"-manifest \"D:\\Caf\u00e9 Project\\spark.modules.json\" -game Game.dll", L"-manifest");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, std::string("D:\\Caf\xC3\xA9 Project\\spark.modules.json"));
}

TEST(WindowsCommandLine_DoesNotTreatOptionPrefixAsMatch)
{
    EXPECT_FALSE(Spark::Platform::FindWindowsCommandLineArgument(L"-gamepad foo.dll", L"-game").has_value());
    EXPECT_FALSE(Spark::Platform::FindWindowsCommandLineArgument(L"-game", L"-game").has_value());
    EXPECT_FALSE(Spark::Platform::HasWindowsCommandLineOption(L"-gamepad foo.dll", L"-game"));
    EXPECT_TRUE(Spark::Platform::HasWindowsCommandLineOption(L"-game", L"-game"));
}

TEST(WindowsCommandLine_PreservesExplicitEmptyArgument)
{
    const auto value = Spark::Platform::FindWindowsCommandLineArgument(L"-exec \"\" -test-frames 1", L"-exec");
    ASSERT_TRUE(value.has_value());
    EXPECT_TRUE(value->empty());
}

#endif // SPARK_PLATFORM_WINDOWS
