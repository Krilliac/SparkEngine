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
 *   --help                 Show usage information
 *
 * Environment variables (still supported):
 *   SPARK_TEST_LIMIT=N     Stop after N tests
 *   SPARK_TEST_FILE=name   Filter tests by source file
 *   SPARK_TEST_NAME=name   Filter tests by test name
 */

#include "TestFramework.h"
#include "../SparkEngine/Source/Utils/Logger.h"

#include <fstream>
#include <sstream>

// Global test state (defined here, declared extern in TestFramework.h)
int g_assertionsPassed = 0;
int g_assertionsFailed = 0;
std::string g_currentTest;

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

    // Error output — always written to both console and file
    void PrintError(const std::string& msg)
    {
        std::cerr << msg;
        if (hasFile)
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

static void PrintUsage(const char* argv0)
{
    std::cout << "Usage: " << argv0 << " [options]\n"
              << "\n"
              << "Options:\n"
              << "  --output-file <path>   Write test output to a file\n"
              << "  --errors-only          Only write failed tests to the output file\n"
              << "  --help                 Show this help message\n"
              << "\n"
              << "Environment variables:\n"
              << "  SPARK_TEST_LIMIT=N     Stop after N tests\n"
              << "  SPARK_TEST_FILE=name   Filter tests by source file\n"
              << "  SPARK_TEST_NAME=name   Filter tests by test name\n";
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main(int argc, char** argv)
{
    // Parse CLI arguments
    std::string outputFilePath;
    bool errorsOnly = false;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--output-file" && i + 1 < argc)
        {
            outputFilePath = argv[++i];
        }
        else if (arg == "--errors-only")
        {
            errorsOnly = true;
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

    auto& tests = GetTestRegistry();
    int passed = 0;
    int failed = 0;

    out.PrintSummary("=== SparkEngine Test Suite ===\n");
    out.PrintSummary("Running " + std::to_string(tests.size()) + " tests...\n\n");

    // Allow limiting test count for bisection debugging (SPARK_TEST_LIMIT=N)
    // Allow filtering to a specific file (SPARK_TEST_FILE=filename)
    int testLimit = static_cast<int>(tests.size());
    if (const char* limitEnv = std::getenv("SPARK_TEST_LIMIT"))
        testLimit = std::min(testLimit, std::atoi(limitEnv));
    const char* fileFilter = std::getenv("SPARK_TEST_FILE");
    const char* nameFilter = std::getenv("SPARK_TEST_NAME");

    // Capture stderr from assertion macros when writing to file
    std::streambuf* origCerrBuf = nullptr;
    std::ostringstream cerrCapture;
    if (out.hasFile)
    {
        origCerrBuf = std::cerr.rdbuf();
        std::cerr.rdbuf(cerrCapture.rdbuf());
    }

    int testIndex = 0;
    int ranCount = 0;
    for (auto& test : tests)
    {
        if (ranCount >= testLimit)
            break;
        ++testIndex;
        if (fileFilter && test.file.find(fileFilter) == std::string::npos)
            continue;
        if (nameFilter && test.name.find(nameFilter) == std::string::npos)
            continue;
        ++ranCount;
        g_currentTest = test.name;
        int prevFailed = g_assertionsFailed;

        // Clear the stderr capture buffer for this test
        if (out.hasFile)
        {
            cerrCapture.str("");
            cerrCapture.clear();
        }

        out.Print("[ RUN    ] " + test.name + "\n");

        try
        {
            test.func();
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

        bool testFailed = (g_assertionsFailed != prevFailed);

        // Flush any captured stderr (assertion failure details) to both original stderr and file
        if (out.hasFile)
        {
            std::string captured = cerrCapture.str();
            if (!captured.empty())
            {
                origCerrBuf->sputn(captured.c_str(), static_cast<std::streamsize>(captured.size()));
                out.file << captured;
            }
        }

        if (!testFailed)
        {
            out.Print("[   OK   ] " + test.name + "\n");
            passed++;
        }
        else
        {
            out.Print("[ FAILED ] " + test.name + "\n", true);
            failed++;
        }
    }

    // Restore original stderr
    if (origCerrBuf)
        std::cerr.rdbuf(origCerrBuf);

    out.PrintSummary("\n=== Results ===\n");
    out.PrintSummary("Tests:      " + std::to_string(passed) + " passed, " + std::to_string(failed) + " failed, " +
                     std::to_string(tests.size()) + " total\n");
    out.PrintSummary("Assertions: " + std::to_string(g_assertionsPassed) + " passed, " +
                     std::to_string(g_assertionsFailed) + " failed\n");

    if (out.hasFile)
    {
        out.PrintSummary("\nOutput written to: " + outputFilePath + "\n");
    }

    return (failed > 0) ? EXIT_FAILURE : EXIT_SUCCESS;
}
