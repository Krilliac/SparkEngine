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

TEST(WindowsCommandLine_FlagDoesNotMatchInsideAnotherArgumentsValue)
{
    // The Windows entry point parsed -headless/-dedicated/-threads with
    // std::wstring::find, so a module path containing the flag text switched
    // the process into headless mode (or resized the worker pool).
    const wchar_t* headlessLookalike = L"-game \"D:\\builds\\fps-headless\\Game.dll\" -test-frames 2";
    EXPECT_FALSE(Spark::Platform::HasWindowsCommandLineOption(headlessLookalike, L"-headless"));
    EXPECT_FALSE(Spark::Platform::HasWindowsCommandLineOption(headlessLookalike, L"-dedicated"));
    EXPECT_TRUE(Spark::Platform::HasWindowsCommandLineOption(L"-headless -test-frames 2", L"-headless"));

    EXPECT_FALSE(Spark::Platform::HasWindowsCommandLineOption(L"-threadsafe 4", L"-threads"));
    EXPECT_FALSE(Spark::Platform::FindWindowsCommandLineNumber<long long>(L"-threadsafe 4", L"-threads").has_value());
}

TEST(WindowsCommandLine_ParsesWholeNumericTokensOnly)
{
    const auto frames = Spark::Platform::FindWindowsCommandLineNumber<long long>(L"-test-frames 240", L"-test-frames");
    ASSERT_TRUE(frames.has_value());
    EXPECT_EQ(*frames, 240LL);

    const auto seconds = Spark::Platform::FindWindowsCommandLineNumber<double>(L"-test-seconds 2.5", L"-test-seconds");
    ASSERT_TRUE(seconds.has_value());
    EXPECT_NEAR(*seconds, 2.5, 1e-9);

    // Trailing garbage and non-numeric values are rejected rather than silently
    // read as a partial number (the old `std::stoi(substr(pos))` accepted both).
    EXPECT_FALSE(
        Spark::Platform::FindWindowsCommandLineNumber<long long>(L"-test-frames 12x", L"-test-frames").has_value());
    EXPECT_FALSE(Spark::Platform::FindWindowsCommandLineNumber<long long>(L"-threads abc", L"-threads").has_value());
    EXPECT_FALSE(
        Spark::Platform::FindWindowsCommandLineNumber<long long>(L"-test-frames", L"-test-frames").has_value());
}

TEST(WindowsCommandLine_ParsesWindowSizeComponentsAsWholeTokens)
{
    const auto size = Spark::Platform::FindWindowsCommandLineArgument(L"-window-size 1280x720", L"-window-size");
    ASSERT_TRUE(size.has_value());
    const auto separator = size->find(L'x');
    ASSERT_TRUE(separator != std::wstring::npos);
    const auto width = Spark::Platform::ParseWholeWideNumber<long long>(size->substr(0, separator));
    const auto height = Spark::Platform::ParseWholeWideNumber<long long>(size->substr(separator + 1));
    ASSERT_TRUE(width.has_value());
    ASSERT_TRUE(height.has_value());
    EXPECT_EQ(*width, 1280LL);
    EXPECT_EQ(*height, 720LL);

    EXPECT_FALSE(Spark::Platform::ParseWholeWideNumber<long long>(std::wstring(L"12 8")).has_value());
}

#endif // SPARK_PLATFORM_WINDOWS
