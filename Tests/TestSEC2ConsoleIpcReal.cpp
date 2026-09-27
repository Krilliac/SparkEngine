/**
 * @file TestSEC2ConsoleIpcReal.cpp
 * @brief SEC2 console-IPC security regressions, against the real code paths.
 *
 * Every test here failed (or hung past its own deadline) before its fix:
 * - Win32 launches inherited every inheritable handle in the process, so an
 *   unrelated pipe's write end stayed open inside the child.
 * - The SparkConsole mirror thread wrote to the child's stdin with an
 *   unbounded blocking write that Shutdown() then joined.
 * - Lines typed into the external console reached only a private five-command
 *   registry, never SimpleConsole.
 * - SparkConsole's history kept credential arguments verbatim.
 * - On POSIX, SparkConsole ignored engine-pipe EOF and never exited.
 * - SparkGameFPS let "nan" through as a time scale / coordinate.
 * - The Shipping gate removed only god/noclip.
 *
 * Registered as the SEC2ConsoleIpcReal CTest with a pinned count.
 */

#include "TestFramework.h"

#include "Console/FPSConsolePolicy.h"
#include "Utils/ConsoleProcessManager.h"
#include "Utils/Process.h"
#include "Utils/SparkConsole.h"

#include "../SparkConsole/src/ConsoleHistoryPolicy.h"

#include <chrono>
#include <cmath>
#include <future>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include "Utils/ProcessWin32HandleList.h"

#include <windows.h>
#endif

namespace
{
    using namespace std::chrono_literals;

    /// Launch a child that holds its captured stdin open but never reads it.
    std::expected<Spark::Process, std::string> LaunchNonReadingChild()
    {
#ifdef _WIN32
        // ping never touches stdin; cmd /c does not read it either.
        return Spark::Process::Builder("cmd.exe")
            .Arg("/c")
            .Arg("ping")
            .Arg("-n")
            .Arg("30")
            .Arg("127.0.0.1")
            .CaptureStdin()
            .CaptureStdout()
            .NoWindow()
            .Launch();
#else
        return Spark::Process::Builder("/bin/sleep").Arg("30").CaptureStdin().Launch();
#endif
    }

    /// Restores SimpleConsole to the state the test found it in.
    struct SimpleConsoleGuard final
    {
        Spark::SimpleConsole& console;
        bool restoreUninitialized;
        std::vector<std::string> registered;

        explicit SimpleConsoleGuard(Spark::SimpleConsole& target)
            : console(target), restoreUninitialized(!target.IsInitialized())
        {
        }

        ~SimpleConsoleGuard()
        {
            for (const std::string& name : registered)
                console.UnregisterCommand(name);
            if (restoreUninitialized)
                console.Shutdown();
        }
    };
} // namespace

// =============================================================================
// Finding 3 - Win32 children inherit only their own standard handles
// =============================================================================

#ifdef _WIN32

TEST(SEC2Console_Win32ChildInheritsOnlyItsOwnPipeEnds)
{
    // Stand-in for another launcher's pipe (the build pipeline, a git capture):
    // its write end is inheritable while this launch happens.
    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = static_cast<DWORD>(sizeof(inheritable));
    inheritable.bInheritHandle = TRUE;
    HANDLE foreignRead = nullptr;
    HANDLE foreignWrite = nullptr;
    ASSERT_TRUE(CreatePipe(&foreignRead, &foreignWrite, &inheritable, 0) != FALSE);
    SetHandleInformation(foreignRead, HANDLE_FLAG_INHERIT, 0);

    // cmd.exe waits on its captured stdin, so it stays alive while we look.
    auto child = Spark::Process::Builder("cmd.exe").CaptureStdin().CaptureStdout().NoWindow().Launch();
    ASSERT_TRUE(child.has_value());

    // With the only writer closed here, the pipe is broken — unless the child
    // received a copy of the write end, which keeps it open (and a reader of it
    // would never see EOF until the unrelated child exits).
    CloseHandle(foreignWrite);
    DWORD available = 0;
    const BOOL stillOpen = PeekNamedPipe(foreignRead, nullptr, 0, nullptr, &available, nullptr);
    const DWORD peekError = stillOpen ? ERROR_SUCCESS : GetLastError();

    child->CloseStdin();
    if (!child->WaitForExit(5000ms))
        child->Kill();
    CloseHandle(foreignRead);

    EXPECT_FALSE(stillOpen != FALSE);
    EXPECT_EQ(peekError, static_cast<DWORD>(ERROR_BROKEN_PIPE));
}

TEST(SEC2Console_Win32HandleListSkipsNonInheritableAndDuplicates)
{
    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    ASSERT_TRUE(CreatePipe(&readEnd, &writeEnd, nullptr, 0) != FALSE);

    Spark::ProcessDetail::InheritedHandleList list;
    // Not inheritable: listing it would make CreateProcess fail outright.
    list.AddIfInheritable(readEnd);
    list.AddIfInheritable(nullptr);
    list.AddIfInheritable(INVALID_HANDLE_VALUE);
    EXPECT_TRUE(list.Empty());

    // AddInheritable marks the handle and lists it once, however often it is named
    // (merged stdout/stderr share one handle; a duplicate fails the attribute).
    EXPECT_TRUE(list.AddInheritable(writeEnd));
    list.AddIfInheritable(writeEnd);
    EXPECT_EQ(list.Handles().size(), static_cast<size_t>(1));
    DWORD flags = 0;
    EXPECT_TRUE(GetHandleInformation(writeEnd, &flags) != FALSE);
    EXPECT_TRUE((flags & HANDLE_FLAG_INHERIT) != 0);
    EXPECT_EQ(list.Build(), static_cast<DWORD>(ERROR_SUCCESS));
    EXPECT_TRUE(list.Attributes() != nullptr);

    EXPECT_FALSE(list.AddInheritable(nullptr));

    CloseHandle(readEnd);
    CloseHandle(writeEnd);
}

#endif // _WIN32

// =============================================================================
// Finding 5 - a child that stops reading stdin cannot wedge the writer
// =============================================================================

TEST(SEC2Console_WriteStdinForReturnsWhenChildStopsReading)
{
    auto child = LaunchNonReadingChild();
    ASSERT_TRUE(child.has_value());

    // Far more than any pipe buffer: an unbounded write blocks until the child
    // reads, which this child never does.
    const std::string payload(4u * 1024u * 1024u, 'x');
    const auto started = std::chrono::steady_clock::now();
    auto pending = std::async(std::launch::async, [&] { return child->WriteStdinFor(payload, 200ms); });
    const bool returned = pending.wait_for(10s) == std::future_status::ready;
    if (!returned)
        child->Kill(); // Breaks the pipe so the stuck write (and the future) can finish.
    const size_t written = pending.get();
    const auto elapsed = std::chrono::steady_clock::now() - started;

    child->Kill();
    EXPECT_TRUE(returned);
    EXPECT_LT(written, payload.size());
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 5000LL);
}

TEST(SEC2Console_WriteStdinForDeliversEverythingToADrainingChild)
{
#ifdef _WIN32
    auto child = Spark::Process::Builder("cmd.exe").CaptureStdin().CaptureStdout().NoWindow().Launch();
    ASSERT_TRUE(child.has_value());
    const std::string command = "exit 7\r\n";
    EXPECT_EQ(child->WriteStdinFor(command, 2000ms), command.size());
    // Blocking mode is restored afterwards: the plain write path still works.
    child->WriteStdin("\r\n");
    ASSERT_TRUE(child->WaitForExit(5000ms));
    EXPECT_EQ(child->GetExitCode().value_or(-1), 7);
#else
    auto child = Spark::Process::Builder("/bin/cat").CaptureStdin().CaptureStdout().Launch();
    ASSERT_TRUE(child.has_value());
    EXPECT_EQ(child->WriteStdinFor("hello\n", 2000ms), static_cast<size_t>(6));
    child->WriteStdin("world\n");
    child->CloseStdin();
    EXPECT_EQ(child->ReadAllStdout(), std::string("hello\nworld\n"));
    EXPECT_EQ(child->WaitForExit(), 0);
#endif
}

// =============================================================================
// Finding 6 - external console lines reach the real engine command registry
// =============================================================================

TEST(SEC2Console_ExternalConsoleLinesReachSimpleConsoleCommands)
{
    auto& console = Spark::SimpleConsole::GetInstance();
    SimpleConsoleGuard guard(console);
    ASSERT_TRUE(console.Initialize());

    const std::string name = "sec2_external_probe";
    std::vector<std::string> received;
    bool ran = false;
    ASSERT_TRUE(console.RegisterCommand(
        name,
        [&](const std::vector<std::string>& args)
        {
            ran = true;
            received = args;
            return std::string("probe ran");
        },
        "SEC2 probe", "Test", name));
    guard.registered.push_back(name);

    // Exactly what ProcessCommands() does with a line read from the SparkConsole pipe.
    const std::string result = Spark::ConsoleProcessManager::GetInstance().DispatchConsoleCommand(name + " 9.81 extra");

    EXPECT_TRUE(ran);
    ASSERT_EQ(received.size(), static_cast<size_t>(2));
    EXPECT_EQ(received[0], std::string("9.81"));
    // SimpleConsole logs its own result (mirrored back to the window).
    EXPECT_TRUE(result.empty());
}

TEST(SEC2Console_ManagerOwnedCommandsStayWithTheManager)
{
    auto& console = Spark::SimpleConsole::GetInstance();
    SimpleConsoleGuard guard(console);
    ASSERT_TRUE(console.Initialize());

    // assert_mode belongs to ConsoleProcessManager; SimpleConsole has no such
    // command, so reaching it would report "Unknown command" and change nothing.
    const std::string usage = Spark::ConsoleProcessManager::GetInstance().DispatchConsoleCommand("assert_mode");
    EXPECT_STR_CONTAINS(usage, "Usage: assert_mode");
    EXPECT_TRUE(Spark::ConsoleProcessManager::GetInstance().DispatchConsoleCommand("   ").empty());
}

// =============================================================================
// Finding 9 - SparkConsole history never keeps engine-command arguments
// =============================================================================

TEST(SEC2Console_HistoryPolicyKeepsNoUntrustedArguments)
{
    using ConsoleHistoryPolicy::EntryFor;

    // Engine commands (credential or not, SparkConsole cannot tell) keep only the name.
    const std::string login = EntryFor("tf_login alice sekret123", /*argumentsTrusted*/ false);
    EXPECT_EQ(login, std::string("tf_login <arguments-redacted>"));
    EXPECT_TRUE(login.find("sekret123") == std::string::npos);
    EXPECT_EQ(EntryFor("mmo_login \"bob smith\" \"pass word\"", false), std::string("mmo_login <arguments-redacted>"));
    EXPECT_EQ(EntryFor("fps", false), std::string("fps"));
    EXPECT_TRUE(EntryFor("   ", false).empty());

    // Console-local commands keep their arguments for recall.
    EXPECT_EQ(EntryFor("echo hello world", true), std::string("echo hello world"));
}

// =============================================================================
// Findings 9 and 10 - the real SparkConsole binary (POSIX)
// =============================================================================

#if defined(SPARK_TEST_SPARK_CONSOLE_PATH) && !defined(_WIN32)

TEST(SEC2Console_PipeChildExitsWhenEngineClosesItsStdin)
{
    auto console = Spark::Process::Builder(SPARK_TEST_SPARK_CONSOLE_PATH)
                       .Arg("--engine-pipe")
                       .CaptureStdin()
                       .CaptureStdout()
                       .MergeStderrIntoStdout()
                       .Launch();
    ASSERT_TRUE(console.has_value());
    std::this_thread::sleep_for(300ms); // Let the reader thread reach its select() loop.

    // The engine's Shutdown() does exactly this and then waits 500 ms before
    // killing. A pipe child that ignores EOF idles until killed — and forever
    // when the engine crashed and nobody kills it.
    console->CloseStdin();
    const bool exitedOnItsOwn = console->WaitForExit(5000ms);
    if (!exitedOnItsOwn)
        console->Kill();
    EXPECT_TRUE(exitedOnItsOwn);
}

TEST(SEC2Console_StandaloneHistoryNeverShowsCredentials)
{
    auto console = Spark::Process::Builder(SPARK_TEST_SPARK_CONSOLE_PATH)
                       .CaptureStdin()
                       .CaptureStdout()
                       .MergeStderrIntoStdout()
                       .Launch();
    ASSERT_TRUE(console.has_value());

    console->WriteStdin("tf_login alice sekret123\nhistory\nexit\n");
    console->CloseStdin();
    const std::string output = console->ReadAllStdout();
    const bool exited = console->WaitForExit(5000ms);
    if (!exited)
        console->Kill();

    EXPECT_TRUE(exited);
    EXPECT_STR_CONTAINS(output, "tf_login <arguments-redacted>");
    EXPECT_TRUE(output.find("sekret123") == std::string::npos);
}

#endif // SPARK_TEST_SPARK_CONSOLE_PATH && !_WIN32
// =============================================================================
// Finding 7 - Shipping drops every developer command, not just god/noclip
// =============================================================================

TEST(SEC2Console_ShippingPolicyDropsEveryDeveloperCommand)
{
    using SparkFPS::ConsolePolicy::ShouldRegister;

    // The commands the SEC2 review found registered in the Shipping build.
    for (const char* cheat :
         {"god", "noclip", "player_tp", "spawn", "game_timescale", "gamemode", "give", "quest_start", "quest_all",
          "destroy", "scene_save", "wave_skip", "wave_difficulty", "xp", "powerup"})
    {
        EXPECT_FALSE(ShouldRegister(cheat, /*developerCommandsEnabled*/ false));
        EXPECT_TRUE(ShouldRegister(cheat, /*developerCommandsEnabled*/ true));
    }

    // Player-facing and read-only commands stay available in Shipping.
    for (const char* kept : {"game_status", "quicksave", "quickload", "hud", "audio_volume", "level", "net_host",
                             "net_connect", "wave_status", "replay_play"})
    {
        EXPECT_TRUE(ShouldRegister(kept, /*developerCommandsEnabled*/ false));
    }

    // Non-Shipping test builds keep them; the flag follows the same macros as the module.
#if defined(SPARK_DEVCOMMANDS_IN_SHIPPING) || !defined(SPARK_BUILD_SHIPPING)
    EXPECT_TRUE(SparkFPS::ConsolePolicy::kDeveloperCommandsEnabled);
#else
    EXPECT_FALSE(SparkFPS::ConsolePolicy::kDeveloperCommandsEnabled);
#endif
}

// =============================================================================
// Finding 8 - console numbers must be finite and fully consumed
// =============================================================================

TEST(SEC2Console_FiniteFloatParserRejectsNanInfinityAndGarbage)
{
    using SparkFPS::ConsolePolicy::ParseFiniteFloat;

    for (const char* rejected : {"nan", "NaN", "-nan", "inf", "-inf", "infinity", "1e39", "-1e39", "", " 1", "1 ",
                                 "1.5abc", "0x10", "++1", "abc"})
    {
        EXPECT_FALSE(ParseFiniteFloat(rejected).has_value());
    }

    EXPECT_NEAR(ParseFiniteFloat("2.5").value_or(0.0f), 2.5f, 1e-6f);
    EXPECT_NEAR(ParseFiniteFloat("-3").value_or(0.0f), -3.0f, 1e-6f);
    EXPECT_NEAR(ParseFiniteFloat("+0.25").value_or(0.0f), 0.25f, 1e-6f);
    EXPECT_NEAR(ParseFiniteFloat("1e3").value_or(0.0f), 1000.0f, 1e-3f);
}
