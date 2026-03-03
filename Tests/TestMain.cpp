/**
 * @file TestMain.cpp
 * @brief Lightweight test runner for SparkEngine
 *
 * Simple test framework without external dependencies.
 * Each test file registers tests via the TEST() macro from TestFramework.h.
 */

#include "TestFramework.h"

// Global test state (defined here, declared extern in TestFramework.h)
int g_assertionsPassed = 0;
int g_assertionsFailed = 0;
std::string g_currentTest;

// ============================================================================
// Main Test Runner
// ============================================================================

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    auto& tests = GetTestRegistry();
    int passed = 0;
    int failed = 0;

    std::cout << "=== SparkEngine Test Suite ===\n";
    std::cout << "Running " << tests.size() << " tests...\n\n";

    for (auto& test : tests)
    {
        g_currentTest = test.name;
        int prevFailed = g_assertionsFailed;

        std::cout << "[ RUN    ] " << test.name << "\n";

        try
        {
            test.func();
        }
        catch (const std::exception& e)
        {
            g_assertionsFailed++;
            std::cerr << "  EXCEPTION: " << e.what() << "\n";
        }
        catch (...)
        {
            g_assertionsFailed++;
            std::cerr << "  EXCEPTION: Unknown exception\n";
        }

        if (g_assertionsFailed == prevFailed)
        {
            std::cout << "[   OK   ] " << test.name << "\n";
            passed++;
        }
        else
        {
            std::cout << "[ FAILED ] " << test.name << "\n";
            failed++;
        }
    }

    std::cout << "\n=== Results ===\n";
    std::cout << "Tests:      " << passed << " passed, " << failed << " failed, " << tests.size() << " total\n";
    std::cout << "Assertions: " << g_assertionsPassed << " passed, " << g_assertionsFailed << " failed\n";

    return (failed > 0) ? EXIT_FAILURE : EXIT_SUCCESS;
}
