/**
 * @file TestMain.cpp
 * @brief Lightweight test runner for SparkEngine
 *
 * Simple test framework without external dependencies.
 * Each test file registers tests via the TEST() macro.
 */

#include <iostream>
#include <string>
#include <vector>
#include <functional>
#include <cstdlib>

// ============================================================================
// Minimal Test Framework
// ============================================================================

struct TestCase
{
    std::string name;
    std::string file;
    int line;
    std::function<void()> func;
};

static std::vector<TestCase>& GetTestRegistry()
{
    static std::vector<TestCase> tests;
    return tests;
}

struct TestRegistrar
{
    TestRegistrar(const char* name, const char* file, int line, std::function<void()> func)
    {
        GetTestRegistry().push_back({name, file, line, std::move(func)});
    }
};

static int g_assertionsPassed = 0;
static int g_assertionsFailed = 0;
static std::string g_currentTest;

#define TEST(name) \
    void test_##name(); \
    static TestRegistrar registrar_##name(#name, __FILE__, __LINE__, test_##name); \
    void test_##name()

#define EXPECT_TRUE(expr) \
    do { \
        if (expr) { g_assertionsPassed++; } \
        else { g_assertionsFailed++; \
            std::cerr << "  FAIL: " << #expr << " was false (" << __FILE__ << ":" << __LINE__ << ")\n"; } \
    } while(0)

#define EXPECT_FALSE(expr) EXPECT_TRUE(!(expr))

#define EXPECT_EQ(a, b) \
    do { \
        auto _a = (a); auto _b = (b); \
        if (_a == _b) { g_assertionsPassed++; } \
        else { g_assertionsFailed++; \
            std::cerr << "  FAIL: " << #a << " == " << #b << " (" << _a << " != " << _b << ") at " << __FILE__ << ":" << __LINE__ << "\n"; } \
    } while(0)

#define EXPECT_NE(a, b) \
    do { \
        auto _a = (a); auto _b = (b); \
        if (_a != _b) { g_assertionsPassed++; } \
        else { g_assertionsFailed++; \
            std::cerr << "  FAIL: " << #a << " != " << #b << " (both " << _a << ") at " << __FILE__ << ":" << __LINE__ << "\n"; } \
    } while(0)

#define EXPECT_NEAR(a, b, tolerance) \
    do { \
        auto _a = (a); auto _b = (b); auto _t = (tolerance); \
        if (std::abs(_a - _b) <= _t) { g_assertionsPassed++; } \
        else { g_assertionsFailed++; \
            std::cerr << "  FAIL: |" << #a << " - " << #b << "| <= " << _t \
                      << " (" << std::abs(_a - _b) << ") at " << __FILE__ << ":" << __LINE__ << "\n"; } \
    } while(0)

#define EXPECT_GT(a, b) \
    do { \
        auto _a = (a); auto _b = (b); \
        if (_a > _b) { g_assertionsPassed++; } \
        else { g_assertionsFailed++; \
            std::cerr << "  FAIL: " << #a << " > " << #b << " (" << _a << " <= " << _b << ") at " << __FILE__ << ":" << __LINE__ << "\n"; } \
    } while(0)

#define EXPECT_LT(a, b) \
    do { \
        auto _a = (a); auto _b = (b); \
        if (_a < _b) { g_assertionsPassed++; } \
        else { g_assertionsFailed++; \
            std::cerr << "  FAIL: " << #a << " < " << #b << " (" << _a << " >= " << _b << ") at " << __FILE__ << ":" << __LINE__ << "\n"; } \
    } while(0)

// ============================================================================
// Main Test Runner
// ============================================================================

int main(int argc, char** argv)
{
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
