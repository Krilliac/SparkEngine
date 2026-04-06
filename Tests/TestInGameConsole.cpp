// TestInGameConsole.cpp - Tests for Spark::InGameConsole
#include "TestFramework.h"
#include "Utils/InGameConsole.h"

// ============================================================================
// Initialization
// ============================================================================

TEST(InGameConsole_Initialize)
{
    auto& console = Spark::InGameConsole::GetInstance();
    console.Initialize();
    EXPECT_FALSE(console.IsOpen());
    // Built-in commands: help, clear, echo, quit, history
    EXPECT_TRUE(console.GetCommandCount() >= 5);
    // Init prints a welcome message
    EXPECT_FALSE(console.GetOutputLines().empty());
    console.Shutdown();
}

// ============================================================================
// Command registration and execution
// ============================================================================

TEST(InGameConsole_RegisterCommand)
{
    auto& console = Spark::InGameConsole::GetInstance();
    console.Initialize();

    console.RegisterCommand("god", "Toggle god mode",
                            [](const std::vector<std::string>&) { return std::string("God mode toggled"); });

    auto names = console.GetCommandNames();
    bool found = false;
    for (const auto& n : names)
    {
        if (n == "god")
            found = true;
    }
    EXPECT_TRUE(found);

    console.Shutdown();
}

TEST(InGameConsole_Execute)
{
    auto& console = Spark::InGameConsole::GetInstance();
    console.Initialize();

    console.RegisterCommand("ping", "Ping test", [](const std::vector<std::string>&) { return std::string("pong"); });

    std::string result = console.Execute("ping");
    EXPECT_EQ(std::string("pong"), result);

    console.Shutdown();
}

TEST(InGameConsole_ExecuteWithArgs)
{
    auto& console = Spark::InGameConsole::GetInstance();
    console.Initialize();

    console.RegisterCommand("add", "Add numbers",
                            [](const std::vector<std::string>& args)
                            {
                                int sum = 0;
                                for (const auto& a : args)
                                    sum += std::stoi(a);
                                return std::to_string(sum);
                            });

    std::string result = console.Execute("add 10 20 30");
    EXPECT_EQ(std::string("60"), result);

    console.Shutdown();
}

TEST(InGameConsole_UnknownCommand)
{
    auto& console = Spark::InGameConsole::GetInstance();
    console.Initialize();

    std::string result = console.Execute("nonexistent_cmd");
    EXPECT_STR_CONTAINS(result, "Unknown command");

    console.Shutdown();
}

// ============================================================================
// Toggle and state
// ============================================================================

TEST(InGameConsole_ToggleOpenClose)
{
    auto& console = Spark::InGameConsole::GetInstance();
    console.Initialize();

    EXPECT_FALSE(console.IsOpen());
    bool consumed = console.OnKeyPress('`');
    EXPECT_TRUE(consumed);
    EXPECT_TRUE(console.IsOpen());

    consumed = console.OnKeyPress('`');
    EXPECT_TRUE(consumed);
    EXPECT_FALSE(console.IsOpen());

    // Non-toggle key should not be consumed
    consumed = console.OnKeyPress('a');
    EXPECT_FALSE(consumed);

    console.Shutdown();
}

// ============================================================================
// Command history
// ============================================================================

TEST(InGameConsole_CommandHistory)
{
    auto& console = Spark::InGameConsole::GetInstance();
    console.Initialize();
    console.OnKeyPress('`'); // Open

    // Type and execute "echo hello"
    for (char c : std::string("echo hello"))
        console.OnCharInput(c);
    console.OnCharInput('\n');

    // Type and execute "echo world"
    for (char c : std::string("echo world"))
        console.OnCharInput(c);
    console.OnCharInput('\n');

    // Navigate history up — index 0 is most recent ("echo world")
    console.NavigateHistory(1);
    EXPECT_EQ(std::string("echo world"), console.GetInputBuffer());

    // Navigate further up — index 1 is "echo hello"
    console.NavigateHistory(1);
    EXPECT_EQ(std::string("echo hello"), console.GetInputBuffer());

    console.Shutdown();
}

// ============================================================================
// Auto-complete
// ============================================================================

TEST(InGameConsole_AutoComplete)
{
    auto& console = Spark::InGameConsole::GetInstance();
    console.Initialize();
    console.OnKeyPress('`');

    // Type "hel" then tab — should complete to "help "
    for (char c : std::string("hel"))
        console.OnCharInput(c);
    console.OnCharInput('\t');

    EXPECT_EQ(std::string("help "), console.GetInputBuffer());

    console.Shutdown();
}

// ============================================================================
// Output
// ============================================================================

TEST(InGameConsole_PrintLevels)
{
    auto& console = Spark::InGameConsole::GetInstance();
    console.Initialize();
    console.ClearOutput();

    console.Print("info msg");
    console.PrintWarning("warn msg");
    console.PrintError("error msg");
    console.PrintSuccess("success msg");

    const auto& lines = console.GetOutputLines();
    EXPECT_EQ(static_cast<size_t>(4), lines.size());
    EXPECT_TRUE(lines[0].level == Spark::ConsoleLogLevel::Info);
    EXPECT_TRUE(lines[1].level == Spark::ConsoleLogLevel::Warning);
    EXPECT_TRUE(lines[2].level == Spark::ConsoleLogLevel::Error);
    EXPECT_TRUE(lines[3].level == Spark::ConsoleLogLevel::Success);

    console.Shutdown();
}

TEST(InGameConsole_ClearOutput)
{
    auto& console = Spark::InGameConsole::GetInstance();
    console.Initialize();
    console.Print("test line");
    EXPECT_FALSE(console.GetOutputLines().empty());

    console.ClearOutput();
    EXPECT_TRUE(console.GetOutputLines().empty());

    console.Shutdown();
}

TEST(InGameConsole_UnregisterCommand)
{
    auto& console = Spark::InGameConsole::GetInstance();
    console.Initialize();

    console.RegisterCommand("temp", "Temporary", [](const std::vector<std::string>&) { return std::string("temp"); });
    size_t countBefore = console.GetCommandCount();
    console.UnregisterCommand("temp");
    EXPECT_EQ(countBefore - 1, console.GetCommandCount());

    std::string result = console.Execute("temp");
    EXPECT_STR_CONTAINS(result, "Unknown command");

    console.Shutdown();
}

TEST(InGameConsole_EchoBuiltIn)
{
    auto& console = Spark::InGameConsole::GetInstance();
    console.Initialize();

    std::string result = console.Execute("echo hello world");
    EXPECT_STR_CONTAINS(result, "hello");
    EXPECT_STR_CONTAINS(result, "world");

    console.Shutdown();
}
