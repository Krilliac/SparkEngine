/**
 * @file TestMain.cpp
 * @brief Lightweight test runner for SparkEngine
 *
 * Simple test framework without external dependencies.
 * Each test file registers tests via the TEST() macro from TestFramework.h.
 *
 * CLI options:
 *   --output-file <path>   Write all test output to a file
 *   --errors-only          Only include failed tests and error details in output file
 *   --junit-xml <path>     Write JUnit XML report (for CI integration)
 *   --shuffle [seed]       Randomize test execution order (seed for reproducibility)
 *   --retries N            Retry failed tests up to N times (for flaky test mitigation)
 *   --warn-is-error        Treat known-flaky test warnings as hard failures
 *   --list-warnings        List all registered flaky test patterns and exit
 *   --help                 Show usage information
 *
 * Environment variables (still supported):
 *   SPARK_TEST_LIMIT=N     Stop after N tests
 *   SPARK_TEST_FILE=name   Filter tests by source file
 *   SPARK_TEST_NAME=name   Filter tests by test name
 *   SPARK_TEST_EXCLUDE=pat Exclude tests whose name contains pat (comma-separated)
 */

#include "TestFramework.h"
#include "TestWarnings.h"
#include "Utils/Logger.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <numeric>
#include <random>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#else
#include <csignal>
#include <cstdio>
#include <unistd.h>
#if __has_include(<execinfo.h>)
#include <execinfo.h>
#define SPARK_TEST_HAS_BACKTRACE 1
#else
#define SPARK_TEST_HAS_BACKTRACE 0
#endif
#endif

// Global test state (defined here, declared extern in TestFramework.h)
int g_assertionsPassed = 0;
int g_assertionsFailed = 0;
int g_testsWarned = 0;
std::string g_currentTest;

// ============================================================================
// Per-test result storage (for timing, JUnit XML, and slowest-test summary)
// ============================================================================

struct TestResult
{
    std::string name;
    double durationMs = 0.0;
    int assertions = 0;
    bool passed = true;
    bool warned = false;
    std::string warningReason;
    std::string failureOutput;
};

// ============================================================================
// Signal/exception handling — catch crashes so they don't silently kill the suite
// ============================================================================

#ifdef _WIN32
// Walk the faulting thread's stack via DbgHelp and print a symbolicated
// backtrace to stdout + stderr. Resolves up to 64 frames. Called from
// the top-level exception filter so test crashes leave a real trail
// instead of just a bare exception name.
static void PrintWindowsStackTrace(CONTEXT* ctx)
{
    HANDLE process = GetCurrentProcess();
    HANDLE thread = GetCurrentThread();

    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(process, nullptr, TRUE);

    STACKFRAME64 frame = {};
    DWORD machine = 0;
#if defined(_M_X64) || defined(__x86_64__)
    machine = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset = ctx->Rip;
    frame.AddrFrame.Offset = ctx->Rbp;
    frame.AddrStack.Offset = ctx->Rsp;
#elif defined(_M_IX86) || defined(__i386__)
    machine = IMAGE_FILE_MACHINE_I386;
    frame.AddrPC.Offset = ctx->Eip;
    frame.AddrFrame.Offset = ctx->Ebp;
    frame.AddrStack.Offset = ctx->Esp;
#elif defined(_M_ARM64) || defined(__aarch64__)
    machine = IMAGE_FILE_MACHINE_ARM64;
    frame.AddrPC.Offset = ctx->Pc;
    frame.AddrFrame.Offset = ctx->Fp;
    frame.AddrStack.Offset = ctx->Sp;
#else
    machine = IMAGE_FILE_MACHINE_UNKNOWN;
#endif
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Mode = AddrModeFlat;

    std::cout << "[ CRASH  ] Stack trace:\n";

    constexpr int MAX_FRAMES = 64;
    for (int i = 0; i < MAX_FRAMES; ++i)
    {
        if (!StackWalk64(machine, process, thread, &frame, ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64,
                         nullptr))
            break;
        if (frame.AddrPC.Offset == 0)
            break;

        char symBuf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(symBuf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 displacement = 0;
        const char* name = "?";
        if (SymFromAddr(process, frame.AddrPC.Offset, &displacement, sym))
            name = sym->Name;

        IMAGEHLP_LINE64 line = {};
        line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
        DWORD lineDisp = 0;
        bool haveLine = SymGetLineFromAddr64(process, frame.AddrPC.Offset, &lineDisp, &line) != 0;

        char out[512];
        if (haveLine)
        {
            std::snprintf(out, sizeof(out), "  #%-2d 0x%016llx  %s  (%s:%lu)\n", i,
                          static_cast<unsigned long long>(frame.AddrPC.Offset), name, line.FileName,
                          static_cast<unsigned long>(line.LineNumber));
        }
        else
        {
            std::snprintf(out, sizeof(out), "  #%-2d 0x%016llx  %s\n", i,
                          static_cast<unsigned long long>(frame.AddrPC.Offset), name);
        }
        std::cout << out;
        std::cerr << out;
    }
    std::cout.flush();
    std::cerr.flush();

    SymCleanup(process);
}

static LONG WINAPI CrashExceptionFilter(EXCEPTION_POINTERS* exInfo)
{
    const char* excName = "UNKNOWN";
    if (exInfo && exInfo->ExceptionRecord)
    {
        switch (exInfo->ExceptionRecord->ExceptionCode)
        {
        case EXCEPTION_ACCESS_VIOLATION:
            excName = "ACCESS_VIOLATION";
            break;
        case EXCEPTION_STACK_OVERFLOW:
            excName = "STACK_OVERFLOW";
            break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
            excName = "DIVIDE_BY_ZERO";
            break;
        case EXCEPTION_ILLEGAL_INSTRUCTION:
            excName = "ILLEGAL_INSTRUCTION";
            break;
        default:
            excName = "EXCEPTION";
            break;
        }
    }

    // Write crash info to stderr (OutputDebugString is not async-safe either)
    HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
    if (hErr != INVALID_HANDLE_VALUE)
    {
        DWORD written;
        WriteFile(hErr, "\n[ CRASH  ] ", 12, &written, nullptr);
        WriteFile(hErr, g_currentTest.c_str(), static_cast<DWORD>(g_currentTest.size()), &written, nullptr);
        WriteFile(hErr, " (", 2, &written, nullptr);
        WriteFile(hErr, excName, static_cast<DWORD>(strlen(excName)), &written, nullptr);
        WriteFile(hErr, ")\n", 2, &written, nullptr);
    }

    // Print to stdout as well so test output captures it
    std::cout << "\n[ CRASH  ] " << g_currentTest << " (" << excName << ")\n";
    std::cout.flush();

    // Walk the faulting thread's stack and print a symbolicated trace.
    // This runs in the exception filter, so DbgHelp calls are allowed
    // (they are not async-signal-safe, but Windows SEH is not signal-
    // based). On stack overflow DbgHelp may itself recurse; we guard
    // by skipping the walk on STACK_OVERFLOW.
    if (exInfo && exInfo->ContextRecord && exInfo->ExceptionRecord->ExceptionCode != EXCEPTION_STACK_OVERFLOW)
    {
        PrintWindowsStackTrace(exInfo->ContextRecord);
    }
    else
    {
        std::cout << "[ CRASH  ] (stack trace skipped — unavailable or stack overflow)\n";
    }

    ExitProcess(1);
}

static void InstallCrashHandlers()
{
    SetUnhandledExceptionFilter(CrashExceptionFilter);
}
#else
static void CrashSignalHandler(int sig)
{
    const char* sigName = "UNKNOWN";
    switch (sig)
    {
    case SIGSEGV:
        sigName = "SIGSEGV";
        break;
    case SIGABRT:
        sigName = "SIGABRT";
        break;
    case SIGFPE:
        sigName = "SIGFPE";
        break;
    case SIGBUS:
        sigName = "SIGBUS";
        break;
    case SIGILL:
        sigName = "SIGILL";
        break;
    }

    // Use write() — async-signal-safe, unlike std::cerr.
    const char* prefix = "\n[ CRASH  ] ";
    [[maybe_unused]] auto r1 = write(STDERR_FILENO, prefix, 12);
    [[maybe_unused]] auto r2 = write(STDERR_FILENO, g_currentTest.c_str(), g_currentTest.size());
    const char* mid = " (signal: ";
    [[maybe_unused]] auto r3 = write(STDERR_FILENO, mid, 10);
    [[maybe_unused]] auto r4 = write(STDERR_FILENO, sigName, strlen(sigName));
    const char* suffix = ")\n";
    [[maybe_unused]] auto r5 = write(STDERR_FILENO, suffix, 2);

#if SPARK_TEST_HAS_BACKTRACE
    // backtrace() + backtrace_symbols_fd() are async-signal-safe on glibc
    // (backtrace_symbols() is NOT — that one calls malloc). We use the
    // _fd variant that writes directly to a file descriptor.
    const char* btHeader = "[ CRASH  ] Stack trace:\n";
    [[maybe_unused]] auto r6 = write(STDERR_FILENO, btHeader, strlen(btHeader));
    void* frames[64];
    int n = backtrace(frames, 64);
    backtrace_symbols_fd(frames, n, STDERR_FILENO);
    // Mirror to stdout so the test output capture also includes it.
    [[maybe_unused]] auto r7 = write(STDOUT_FILENO, prefix, 12);
    [[maybe_unused]] auto r8 = write(STDOUT_FILENO, g_currentTest.c_str(), g_currentTest.size());
    [[maybe_unused]] auto r9 = write(STDOUT_FILENO, mid, 10);
    [[maybe_unused]] auto rA = write(STDOUT_FILENO, sigName, strlen(sigName));
    [[maybe_unused]] auto rB = write(STDOUT_FILENO, suffix, 2);
    [[maybe_unused]] auto rC = write(STDOUT_FILENO, btHeader, strlen(btHeader));
    backtrace_symbols_fd(frames, n, STDOUT_FILENO);
#else
    const char* unavail = "[ CRASH  ] (stack trace unavailable — backtrace() not present)\n";
    [[maybe_unused]] auto r6 = write(STDERR_FILENO, unavail, strlen(unavail));
#endif

    _exit(128 + sig);
}

static void InstallCrashHandlers()
{
    signal(SIGSEGV, CrashSignalHandler);
    signal(SIGABRT, CrashSignalHandler);
    signal(SIGFPE, CrashSignalHandler);
    signal(SIGBUS, CrashSignalHandler);
    signal(SIGILL, CrashSignalHandler);
}
#endif

// ============================================================================
// Output helper — writes to console and optionally to a file
// ============================================================================

struct TestOutput
{
    std::ofstream file;
    bool errorsOnly = false;
    bool hasFile = false;

    void Open(const std::string& path)
    {
        file.open(path, std::ios::out | std::ios::trunc);
        hasFile = file.is_open();
    }

    // Always write to console; write to file unless errors-only mode filters it
    void Print(const std::string& msg, bool isError = false)
    {
        std::cout << msg;
        if (hasFile && (!errorsOnly || isError))
            file << msg;
    }

    // Summary lines — always written to file regardless of errors-only mode
    void PrintSummary(const std::string& msg)
    {
        std::cout << msg;
        if (hasFile)
            file << msg;
    }
};

// ============================================================================
// JUnit XML output
// ============================================================================

static std::string XmlEscape(const std::string& s)
{
    std::string result;
    result.reserve(s.size());
    for (char c : s)
    {
        switch (c)
        {
        case '&':
            result += "&amp;";
            break;
        case '<':
            result += "&lt;";
            break;
        case '>':
            result += "&gt;";
            break;
        case '"':
            result += "&quot;";
            break;
        case '\'':
            result += "&apos;";
            break;
        default:
            result += c;
        }
    }
    return result;
}

static void WriteJUnitXml(const std::string& path, const std::vector<TestResult>& results, double totalTimeMs)
{
    std::ofstream xml(path, std::ios::out | std::ios::trunc);
    if (!xml.is_open())
    {
        std::cerr << "Warning: Could not open JUnit XML file: " << path << "\n";
        return;
    }

    int totalTests = static_cast<int>(results.size());
    int failures = 0;
    int warnings = 0;
    for (const auto& r : results)
    {
        if (r.warned)
            ++warnings;
        else if (!r.passed)
            ++failures;
    }

    xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    xml << "<testsuites tests=\"" << totalTests << "\" failures=\"" << failures << "\" skipped=\"" << warnings
        << "\" time=\"" << (totalTimeMs / 1000.0) << "\">\n";
    xml << "  <testsuite name=\"SparkEngine\" tests=\"" << totalTests << "\" failures=\"" << failures << "\" skipped=\""
        << warnings << "\" time=\"" << (totalTimeMs / 1000.0) << "\">\n";

    for (const auto& r : results)
    {
        xml << "    <testcase name=\"" << XmlEscape(r.name) << "\" time=\"" << (r.durationMs / 1000.0) << "\"";
        if (r.passed && !r.warned)
        {
            xml << "/>\n";
        }
        else if (r.warned)
        {
            xml << ">\n";
            xml << "      <skipped message=\"Known flaky: " << XmlEscape(r.warningReason) << "\">"
                << XmlEscape(r.failureOutput) << "</skipped>\n";
            xml << "    </testcase>\n";
        }
        else
        {
            xml << ">\n";
            xml << "      <failure message=\"Test failed\">" << XmlEscape(r.failureOutput) << "</failure>\n";
            xml << "    </testcase>\n";
        }
    }

    xml << "  </testsuite>\n";
    xml << "</testsuites>\n";
}

// ============================================================================
// Helpers
// ============================================================================

static std::string FormatDuration(double ms)
{
    if (ms < 1.0)
        return std::to_string(static_cast<int>(ms * 1000.0)) + "us";
    if (ms < 1000.0)
    {
        std::ostringstream oss;
        oss << std::fixed;
        oss.precision(1);
        oss << ms << "ms";
        return oss.str();
    }
    std::ostringstream oss;
    oss << std::fixed;
    oss.precision(2);
    oss << (ms / 1000.0) << "s";
    return oss.str();
}

static void PrintUsage(const char* argv0)
{
    std::cout << "Usage: " << argv0 << " [options]\n"
              << "\n"
              << "Options:\n"
              << "  --output-file <path>   Write test output to a file\n"
              << "  --errors-only          Only write failed tests to the output file\n"
              << "  --junit-xml <path>     Write JUnit XML report for CI integration\n"
              << "  --shuffle [seed]       Randomize test order (optional integer seed)\n"
              << "  --retries <N>          Retry failed tests up to N times (default: 0)\n"
              << "  --warn-is-error        Treat known-flaky test warnings as hard failures\n"
              << "  --list-warnings        List all registered flaky test patterns and exit\n"
              << "  --help                 Show this help message\n"
              << "\n"
              << "Environment variables:\n"
              << "  SPARK_TEST_LIMIT=N     Stop after N tests\n"
              << "  SPARK_TEST_FILE=name   Filter tests by source file\n"
              << "  SPARK_TEST_NAME=name   Filter tests by test name\n"
              << "  SPARK_TEST_EXCLUDE=pat Exclude tests whose name contains pat (comma-separated)\n";
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main(int argc, char** argv)
{
    // The test executable opens real UDP listeners. Confining them to
    // loopback prevents a fresh Windows machine from showing a firewall
    // permission dialog while preserving all same-host wire tests.
#ifdef _WIN32
    if (_putenv_s("SPARK_NETWORK_BIND_MODE", "loopback") != 0)
#else
    if (setenv("SPARK_NETWORK_BIND_MODE", "loopback", 1) != 0)
#endif
    {
        std::cerr << "Error: Could not enforce loopback-only network tests\n";
        return EXIT_FAILURE;
    }

    InstallCrashHandlers();

    // Parse CLI arguments
    std::string outputFilePath;
    std::string junitXmlPath;
    bool errorsOnly = false;
    bool shuffle = false;
    unsigned int shuffleSeed = 0;
    int maxRetries = 0;
    bool warnIsError = false;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--output-file" && i + 1 < argc)
        {
            outputFilePath = argv[++i];
        }
        else if (arg == "--junit-xml" && i + 1 < argc)
        {
            junitXmlPath = argv[++i];
        }
        else if (arg == "--errors-only")
        {
            errorsOnly = true;
        }
        else if (arg == "--shuffle")
        {
            shuffle = true;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                shuffleSeed = static_cast<unsigned int>(std::stoul(argv[++i]));
            else
                shuffleSeed = std::random_device{}();
        }
        else if (arg == "--retries" && i + 1 < argc)
        {
            maxRetries = std::max(0, std::atoi(argv[++i]));
        }
        else if (arg == "--warn-is-error")
        {
            warnIsError = true;
        }
        else if (arg == "--list-warnings")
        {
            std::cout << "Registered flaky test warning patterns (" << g_testWarningPatternCount << "):\n\n";
            for (size_t j = 0; j < g_testWarningPatternCount; ++j)
            {
                std::cout << "  " << g_testWarningPatterns[j].pattern << "\n"
                          << "    Reason: " << g_testWarningPatterns[j].reason << "\n\n";
            }
            return EXIT_SUCCESS;
        }
        else if (arg == "--help")
        {
            PrintUsage(argv[0]);
            return EXIT_SUCCESS;
        }
    }

    TestOutput out;
    out.errorsOnly = errorsOnly;
    if (!outputFilePath.empty())
    {
        out.Open(outputFilePath);
        if (!out.hasFile)
        {
            std::cerr << "Error: Could not open output file: " << outputFilePath << "\n";
            return EXIT_FAILURE;
        }
    }

    // Initialize the Logger with a stderr sink so SPARK_LOG_* output is visible
    auto& logger = Spark::Logger::Get();
    logger.Initialize(false);
    logger.AddSink(std::make_unique<Spark::StderrSink>());
    logger.SetGlobalLevel(Spark::LogLevel::Debug);

    // Disable auto stack traces during tests — many tests deliberately trigger
    // errors (refused connections, invalid params, etc.) and the multi-line
    // stack traces clutter output and can confuse CI error parsers.
    // Individual tests that verify stack trace capture enable it explicitly.
    logger.SetStackTraceLevel(Spark::LogLevel::Off);

    auto& tests = GetTestRegistry();
    int passed = 0;
    int failed = 0;
    int warned = 0;

    // Shuffle test order if requested
    if (shuffle)
    {
        std::mt19937 rng(shuffleSeed);
        std::shuffle(tests.begin(), tests.end(), rng);
    }

    out.PrintSummary("=== SparkEngine Test Suite ===\n");
    out.PrintSummary("Running " + std::to_string(tests.size()) + " tests...\n");
    if (shuffle)
        out.PrintSummary("Shuffle seed: " + std::to_string(shuffleSeed) + "\n");
    if (maxRetries > 0)
        out.PrintSummary("Retry policy: up to " + std::to_string(maxRetries) + " retries for failed tests\n");
    if (g_testWarningPatternCount > 0 && !warnIsError)
        out.PrintSummary("Warning patterns: " + std::to_string(g_testWarningPatternCount) +
                         " known flaky test(s) will warn instead of fail\n");
    out.PrintSummary("\n");

    // Allow limiting test count for bisection debugging (SPARK_TEST_LIMIT=N)
    // Allow filtering to a specific file (SPARK_TEST_FILE=filename)
    int testLimit = static_cast<int>(tests.size());
    if (const char* limitEnv = std::getenv("SPARK_TEST_LIMIT"))
        testLimit = std::min(testLimit, std::atoi(limitEnv));
    const char* fileFilter = std::getenv("SPARK_TEST_FILE");
    const char* nameFilter = std::getenv("SPARK_TEST_NAME");

    // Parse comma-separated exclude patterns
    std::vector<std::string> excludePatterns;
    if (const char* excludeEnv = std::getenv("SPARK_TEST_EXCLUDE"))
    {
        std::string pat;
        std::istringstream stream(excludeEnv);
        while (std::getline(stream, pat, ','))
        {
            if (!pat.empty())
                excludePatterns.push_back(pat);
        }
    }

    // Capture stderr from assertion macros when writing to file
    std::streambuf* origCerrBuf = nullptr;
    std::ostringstream cerrCapture;
    if (out.hasFile)
    {
        origCerrBuf = std::cerr.rdbuf();
        std::cerr.rdbuf(cerrCapture.rdbuf());
    }

    std::vector<TestResult> results;
    results.reserve(tests.size());

    auto suiteStart = std::chrono::steady_clock::now();

    int ranCount = 0;
    for (auto* test : tests)
    {
        if (ranCount >= testLimit)
            break;
        if (fileFilter && !std::strstr(test->file, fileFilter))
            continue;
        if (nameFilter && !std::strstr(test->name, nameFilter))
            continue;
        bool excluded = false;
        for (const auto& pat : excludePatterns)
        {
            if (std::strstr(test->name, pat.c_str()))
            {
                excluded = true;
                break;
            }
        }
        if (excluded)
            continue;
        ++ranCount;
        g_currentTest = test->name;
        int prevFailed = g_assertionsFailed;
        int prevPassed = g_assertionsPassed;

        // Clear the stderr capture buffer for this test
        if (out.hasFile)
        {
            cerrCapture.str("");
            cerrCapture.clear();
        }

        out.Print("[ RUN    ] " + g_currentTest + "\n");

        auto testStart = std::chrono::steady_clock::now();

        try
        {
            test->func();
        }
        catch (const TestAbort&)
        {
            // A fatal ASSERT_* aborted this test. The failure was already
            // recorded and its reason printed by the macro; nothing more to
            // do — the abort intentionally isolates the failure to this test.
        }
        catch (const std::exception& e)
        {
            g_assertionsFailed++;
            std::string msg = "  EXCEPTION: " + std::string(e.what()) + "\n";
            // Write directly since cerr is captured
            if (origCerrBuf)
                origCerrBuf->sputn(msg.c_str(), static_cast<std::streamsize>(msg.size()));
            else
                std::cerr << msg;
            if (out.hasFile)
                out.file << msg;
        }
        catch (...)
        {
            g_assertionsFailed++;
            std::string msg = "  EXCEPTION: Unknown exception\n";
            if (origCerrBuf)
                origCerrBuf->sputn(msg.c_str(), static_cast<std::streamsize>(msg.size()));
            else
                std::cerr << msg;
            if (out.hasFile)
                out.file << msg;
        }

        auto testEnd = std::chrono::steady_clock::now();
        double durationMs = std::chrono::duration<double, std::milli>(testEnd - testStart).count();
        int testAssertions = (g_assertionsPassed - prevPassed) + (g_assertionsFailed - prevFailed);
        bool testFailed = (g_assertionsFailed != prevFailed);

        // Flush any captured stderr (assertion failure details) to both original stderr and file
        std::string capturedErrors;
        if (out.hasFile)
        {
            capturedErrors = cerrCapture.str();
            if (!capturedErrors.empty())
            {
                origCerrBuf->sputn(capturedErrors.c_str(), static_cast<std::streamsize>(capturedErrors.size()));
                out.file << capturedErrors;
            }
        }

        // Build status line with timing and assertion count
        std::string duration = FormatDuration(durationMs);
        std::string suffix = " (" + duration + ", " + std::to_string(testAssertions) + " assertions)";

        // Check if a failing test matches a known-flaky warning pattern
        const char* warnReason = testFailed ? GetTestWarningReason(test->name) : nullptr;
        bool isWarning = warnReason != nullptr && !warnIsError;

        if (!testFailed)
        {
            out.Print("[   OK   ] " + g_currentTest + suffix + "\n");
            passed++;
        }
        else if (isWarning)
        {
            out.Print("[ WARN   ] " + g_currentTest + suffix + "\n");
            out.Print("           Known flaky: " + std::string(warnReason) + "\n");
            // Emit GitHub Actions warning annotation (visible in CI summary)
            std::cout << "::warning title=Flaky test: " << g_currentTest << "::" << g_currentTest
                      << " failed but is a known flaky test (" << warnReason << ")\n";
            warned++;
            g_testsWarned++;
            // Roll back the assertion failure count — warning tests don't count as failures
            g_assertionsFailed = prevFailed;
        }
        else
        {
            out.Print("[ FAILED ] " + g_currentTest + suffix + "\n", true);
            failed++;
        }

        TestResult result;
        result.name = test->name;
        result.durationMs = durationMs;
        result.assertions = testAssertions;
        result.passed = !testFailed || isWarning;
        result.warned = isWarning;
        result.warningReason = warnReason ? warnReason : "";
        result.failureOutput = capturedErrors;
        results.push_back(std::move(result));
    }

    // Retry failed tests if --retries was specified
    int retriedCount = 0;
    int retriedPassed = 0;
    if (maxRetries > 0 && failed > 0)
    {
        // Collect indices of failed tests
        std::vector<size_t> failedIndices;
        for (size_t i = 0; i < results.size(); ++i)
        {
            if (!results[i].passed)
                failedIndices.push_back(i);
        }

        out.Print("\n=== Retrying " + std::to_string(failedIndices.size()) + " failed test(s), up to " +
                      std::to_string(maxRetries) + " retries each ===\n",
                  false);

        for (size_t idx : failedIndices)
        {
            const std::string& testName = results[idx].name;

            // Find the matching test entry
            TestCase* matchedTest = nullptr;
            for (auto* t : tests)
            {
                if (testName == t->name)
                {
                    matchedTest = t;
                    break;
                }
            }
            if (!matchedTest)
                continue;

            bool retryPassed = false;
            for (int attempt = 1; attempt <= maxRetries; ++attempt)
            {
                ++retriedCount;
                out.Print("[ RETRY  ] " + testName + " (attempt " + std::to_string(attempt) + "/" +
                              std::to_string(maxRetries) + ")\n",
                          false);

                int prevFailed = g_assertionsFailed;
                int prevPassed = g_assertionsPassed;

                // Attribute this attempt to the test being retried so the crash
                // handler and any log output name the right test (retries assume
                // the test is idempotent — it re-runs with no shared-state reset).
                g_currentTest = testName;

                try
                {
                    matchedTest->func();
                }
                catch (const TestAbort&)
                {
                    // Fatal ASSERT_* already recorded the failure for this attempt.
                }
                catch (const std::exception&)
                {
                    g_assertionsFailed++;
                }
                catch (...)
                {
                    g_assertionsFailed++;
                }

                if (g_assertionsFailed == prevFailed)
                {
                    retryPassed = true;
                    int retryAssertions = (g_assertionsPassed - prevPassed);
                    out.Print("[   OK   ] " + testName + " (passed on retry " + std::to_string(attempt) + ", " +
                                  std::to_string(retryAssertions) + " assertions)\n",
                              false);
                    break;
                }
            }

            if (retryPassed)
            {
                ++retriedPassed;
                --failed;
                ++passed;
                results[idx].passed = true;
            }
        }
    }

    auto suiteEnd = std::chrono::steady_clock::now();
    double totalMs = std::chrono::duration<double, std::milli>(suiteEnd - suiteStart).count();

    // Restore original stderr
    if (origCerrBuf)
        std::cerr.rdbuf(origCerrBuf);

    out.PrintSummary("\n=== Results ===\n");
    out.PrintSummary("Tests:      " + std::to_string(passed) + " passed, " + std::to_string(failed) + " failed");
    if (warned > 0)
        out.PrintSummary(", " + std::to_string(warned) + " warned");
    out.PrintSummary(", " + std::to_string(passed + failed + warned) + " total\n");
    out.PrintSummary("Assertions: " + std::to_string(g_assertionsPassed) + " passed, " +
                     std::to_string(g_assertionsFailed) + " failed\n");
    out.PrintSummary("Duration:   " + FormatDuration(totalMs) + "\n");
    if (warned > 0)
    {
        out.PrintSummary("Warnings:   " + std::to_string(warned) +
                         " known flaky test(s) failed (not counted as errors)\n");
    }
    if (retriedPassed > 0)
    {
        out.PrintSummary("Retries:    " + std::to_string(retriedPassed) + " flaky test(s) passed on retry (" +
                         std::to_string(retriedCount) + " total retry attempts)\n");
    }

    // Print slowest tests
    if (results.size() > 1)
    {
        std::vector<size_t> indices(results.size());
        std::iota(indices.begin(), indices.end(), 0);
        std::sort(indices.begin(), indices.end(),
                  [&](size_t a, size_t b) { return results[a].durationMs > results[b].durationMs; });

        size_t count = std::min<size_t>(10, results.size());
        out.PrintSummary("\n=== Slowest " + std::to_string(count) + " Tests ===\n");
        for (size_t i = 0; i < count; ++i)
        {
            const auto& r = results[indices[i]];
            out.PrintSummary("  " + FormatDuration(r.durationMs) + "  " + r.name + "\n");
        }
    }

    if (out.hasFile)
        out.PrintSummary("\nOutput written to: " + outputFilePath + "\n");

    // Write JUnit XML if requested
    if (!junitXmlPath.empty())
    {
        WriteJUnitXml(junitXmlPath, results, totalMs);
        out.PrintSummary("JUnit XML written to: " + junitXmlPath + "\n");
    }

    return (failed > 0) ? EXIT_FAILURE : EXIT_SUCCESS;
}
