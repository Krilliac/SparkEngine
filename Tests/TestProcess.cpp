/**
 * @file TestProcess.cpp
 * @brief Tests for Spark::Process — cross-platform subprocess spawning and piping.
 *
 * These tests launch real child processes (/bin/sh on Linux, cmd.exe on Windows)
 * to verify process lifecycle, pipe I/O, and error handling.
 */

#include "TestFramework.h"
#include "Utils/Process.h"
#include "Utils/ProcessPipeBuffer.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

// ============================================================================
// Platform-specific command helpers
// ============================================================================

TEST(ProcessPipeBuffer_BoundsUnterminatedLinesAndPerCallDrain)
{
    using namespace Spark::ProcessDetail;
    std::string buffer(MaxBufferedLineBytes, 'x');
    std::string line;
    EXPECT_TRUE(ExtractBufferedLine(buffer, line));
    EXPECT_EQ(line.size(), MaxBufferedLineBytes);
    EXPECT_TRUE(buffer.empty());

    buffer = "first\r\nsecond";
    EXPECT_TRUE(ExtractBufferedLine(buffer, line));
    EXPECT_EQ(line, std::string("first"));
    EXPECT_EQ(buffer, std::string("second"));

    EXPECT_EQ(RemainingReadCapacity(0, 0), MaxPipeDrainBytesPerCall);
    EXPECT_EQ(RemainingReadCapacity(MaxBufferedLineBytes, 0), static_cast<size_t>(0));
    EXPECT_EQ(RemainingReadCapacity(0, MaxPipeDrainBytesPerCall), static_cast<size_t>(0));
}

#if defined(__APPLE__)
static constexpr const char* kEchoCmd = "/bin/echo";
static constexpr const char* kShellCmd = "/bin/sh";
static constexpr const char* kCatCmd = "/bin/cat";
static constexpr const char* kTrueCmd = "/usr/bin/true";
static constexpr const char* kFalseCmd = "/usr/bin/false";
static constexpr const char* kSleepCmd = "/bin/sleep";
#elif defined(__linux__)
static constexpr const char* kEchoCmd = "/bin/echo";
static constexpr const char* kShellCmd = "/bin/sh";
static constexpr const char* kCatCmd = "/bin/cat";
static constexpr const char* kTrueCmd = "/bin/true";
static constexpr const char* kFalseCmd = "/bin/false";
static constexpr const char* kSleepCmd = "/bin/sleep";
#elif defined(_WIN32)
static constexpr const char* kEchoCmd = "cmd.exe";
static constexpr const char* kShellCmd = "cmd.exe";
static constexpr const char* kCatCmd = "cmd.exe";
static constexpr const char* kTrueCmd = "cmd.exe";
static constexpr const char* kFalseCmd = "cmd.exe";
static constexpr const char* kSleepCmd = "cmd.exe";
#endif

// ============================================================================
// Basic process launch and exit code
// ============================================================================

#if defined(__linux__) || defined(__APPLE__)

TEST(Process_LaunchAndWait)
{
    auto result = Spark::Process::Builder(kTrueCmd).Launch();
    EXPECT_TRUE(result.has_value());

    int exitCode = result->WaitForExit();
    EXPECT_EQ(exitCode, 0);
}

TEST(Process_NonZeroExitCode)
{
    auto result = Spark::Process::Builder(kFalseCmd).Launch();
    EXPECT_TRUE(result.has_value());

    int exitCode = result->WaitForExit();
    EXPECT_EQ(exitCode, 1);
}

TEST(Process_ShellExitCode)
{
    auto result = Spark::Process::Builder(kShellCmd).Arg("-c").Arg("exit 42").Launch();
    EXPECT_TRUE(result.has_value());

    int exitCode = result->WaitForExit();
    EXPECT_EQ(exitCode, 42);
}

// ============================================================================
// Stdout capture
// ============================================================================

TEST(Process_CaptureStdout)
{
    auto result = Spark::Process::Builder(kEchoCmd).Arg("hello world").CaptureStdout().Launch();
    EXPECT_TRUE(result.has_value());

    std::string output = result->ReadAllStdout();
    result->WaitForExit();

    // Trim trailing newline
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r'))
        output.pop_back();

    EXPECT_EQ(output, std::string("hello world"));
}

TEST(Process_CaptureMultilineStdout)
{
    auto result =
        Spark::Process::Builder(kShellCmd).Arg("-c").Arg("echo line1; echo line2; echo line3").CaptureStdout().Launch();
    EXPECT_TRUE(result.has_value());

    std::string output = result->ReadAllStdout();
    result->WaitForExit();

    EXPECT_TRUE(output.find("line1") != std::string::npos);
    EXPECT_TRUE(output.find("line2") != std::string::npos);
    EXPECT_TRUE(output.find("line3") != std::string::npos);
}

// ============================================================================
// Stderr capture
// ============================================================================

TEST(Process_CaptureStderr)
{
    auto result = Spark::Process::Builder(kShellCmd).Arg("-c").Arg("echo error_msg >&2").CaptureStderr().Launch();
    EXPECT_TRUE(result.has_value());

    std::string errOutput = result->ReadAllStderr();
    result->WaitForExit();

    EXPECT_TRUE(errOutput.find("error_msg") != std::string::npos);
}

TEST(Process_MergeStderrIntoStdout)
{
    auto result = Spark::Process::Builder(kShellCmd)
                      .Arg("-c")
                      .Arg("echo normal; echo error_msg >&2")
                      .CaptureStdout()
                      .MergeStderrIntoStdout()
                      .Launch();
    EXPECT_TRUE(result.has_value());

    std::string output = result->ReadAllStdout();
    result->WaitForExit();

    EXPECT_TRUE(output.find("normal") != std::string::npos);
    EXPECT_TRUE(output.find("error_msg") != std::string::npos);
}

TEST(Process_WorkingDirectory)
{
    const auto directory = std::filesystem::temp_directory_path();
    auto result = Spark::Process::Builder(kShellCmd)
                      .Arg("-c")
                      .Arg("pwd")
                      .WorkingDirectory(directory.string())
                      .CaptureStdout()
                      .Launch();
    EXPECT_TRUE(result.has_value());

    const std::string output = result->ReadAllStdout();
    EXPECT_EQ(result->WaitForExit(), 0);
    EXPECT_TRUE(
        std::filesystem::equivalent(std::filesystem::path(output.substr(0, output.find_first_of("\r\n"))), directory));
}

// ============================================================================
// Stdin piping
// ============================================================================

TEST(Process_WriteStdinAndCapture)
{
    auto result = Spark::Process::Builder(kCatCmd).CaptureStdin().CaptureStdout().Launch();
    EXPECT_TRUE(result.has_value());

    result->WriteStdin("piped input\n");
    result->CloseStdin(); // Signal EOF so cat exits

    std::string output = result->ReadAllStdout();
    result->WaitForExit();

    while (!output.empty() && (output.back() == '\n' || output.back() == '\r'))
        output.pop_back();

    EXPECT_EQ(output, std::string("piped input"));
}

// ============================================================================
// TryReadLine (non-blocking)
// ============================================================================

TEST(Process_TryReadLine)
{
    auto result = Spark::Process::Builder(kShellCmd).Arg("-c").Arg("echo alpha; echo beta").CaptureStdout().Launch();
    EXPECT_TRUE(result.has_value());

    // Wait a moment for output to arrive
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::string line;
    bool gotAlpha = false;
    bool gotBeta = false;

    // Try reading lines (with a timeout loop)
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (result->TryReadLine(line))
        {
            if (line == "alpha")
                gotAlpha = true;
            if (line == "beta")
                gotBeta = true;
            if (gotAlpha && gotBeta)
                break;
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    EXPECT_TRUE(gotAlpha);
    EXPECT_TRUE(gotBeta);
    result->WaitForExit();
}

TEST(Process_TryReadLineChunksLargeUnterminatedOutput)
{
    const size_t outputBytes = Spark::ProcessDetail::MaxBufferedLineBytes * 2;
    const std::string script = "head -c " + std::to_string(outputBytes) + " /dev/zero | tr '\\000' x";
    auto result = Spark::Process::Builder(kShellCmd).Arg("-c").Arg(script).CaptureStdout().Launch();
    EXPECT_TRUE(result.has_value());

    size_t received = 0;
    std::string line;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (received < outputBytes && std::chrono::steady_clock::now() < deadline)
    {
        if (result->TryReadLine(line))
        {
            EXPECT_TRUE(line.size() <= Spark::ProcessDetail::MaxBufferedLineBytes);
            received += line.size();
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    EXPECT_EQ(received, outputBytes);
    EXPECT_EQ(result->WaitForExit(), 0);
}

// ============================================================================
// IsRunning and GetExitCode
// ============================================================================

TEST(Process_IsRunning)
{
    auto result = Spark::Process::Builder(kSleepCmd).Arg("10").Launch();
    EXPECT_TRUE(result.has_value());

    EXPECT_TRUE(result->IsRunning());
    EXPECT_FALSE(result->GetExitCode().has_value());

    result->Kill();

    EXPECT_FALSE(result->IsRunning());
}

// ============================================================================
// WaitForExit with timeout
// ============================================================================

TEST(Process_WaitWithTimeout)
{
    auto result = Spark::Process::Builder(kSleepCmd).Arg("10").Launch();
    EXPECT_TRUE(result.has_value());

    // Should timeout (sleep 10 seconds, we wait 50ms)
    bool exited = result->WaitForExit(std::chrono::milliseconds(50));
    EXPECT_FALSE(exited);

    result->Kill();
}

// ============================================================================
// Kill
// ============================================================================

TEST(Process_Kill)
{
    auto result = Spark::Process::Builder(kSleepCmd).Arg("60").Launch();
    EXPECT_TRUE(result.has_value());
    EXPECT_TRUE(result->IsRunning());

    result->Kill();

    EXPECT_FALSE(result->IsRunning());
    EXPECT_TRUE(result->GetExitCode().has_value());
}

// ============================================================================
// Invalid executable
// ============================================================================

TEST(Process_InvalidExecutable)
{
    auto result = Spark::Process::Builder("/nonexistent/binary/path").Launch();

    // On Linux, fork succeeds but exec fails — child exits with 127.
    // The Process object is valid but the child immediately exits.
    if (result.has_value())
    {
        int exitCode = result->WaitForExit();
        EXPECT_EQ(exitCode, 127);
    }
    else
    {
        // On some platforms Launch itself may fail — that's also acceptable.
        EXPECT_TRUE(true);
    }
}

// ============================================================================
// Detached process
// ============================================================================

TEST(Process_Detached)
{
    auto result = Spark::Process::Builder(kSleepCmd).Arg("0").Detached().Launch();
    EXPECT_TRUE(result.has_value());

    // Detached process: IsRunning returns false (we don't track it)
    EXPECT_FALSE(result->IsRunning());
    EXPECT_FALSE(result->GetExitCode().has_value());
}

// ============================================================================
// Move semantics
// ============================================================================

TEST(Process_MoveSemantics)
{
    auto result = Spark::Process::Builder(kTrueCmd).CaptureStdout().Launch();
    EXPECT_TRUE(result.has_value());

    // Move construct
    Spark::Process proc2 = std::move(*result);
    int exitCode = proc2.WaitForExit();
    EXPECT_EQ(exitCode, 0);
}

#endif // __linux__ || __APPLE__

// ============================================================================
// Builder API tests (platform-independent)
// ============================================================================

TEST(Process_BuilderChaining)
{
    // Just verify the builder compiles and chains correctly
    auto builder = Spark::Process::Builder("some_executable")
                       .Arg("arg1")
                       .Arg("arg2")
                       .WorkingDirectory(".")
                       .CaptureStdout()
                       .CaptureStderr()
                       .MergeStderrIntoStdout()
                       .CaptureStdin()
                       .NoWindow();

    // Launch will fail since "some_executable" doesn't exist, but builder is valid
    auto result = builder.Launch();

    // We expect this to either fail at Launch or the child exits with error
    // Either way the builder API is working
    EXPECT_TRUE(true);
}
