/**
 * @file TestFramework.h
 * @brief Lightweight test framework for SparkEngine
 *
 * Simple test framework without external dependencies.
 * Include this header in all test files, and include TestMain.cpp
 * in the build to get the main runner.
 *
 * Registration uses an intrusive linked list with const char* and function
 * pointers — zero heap allocations at static-init time.
 */

#pragma once

#include <iostream>
#include <string>
#include <vector>
#include <functional>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <exception>

// ============================================================================
// Minimal Test Framework
// ============================================================================

struct TestCase
{
    const char* name;
    const char* file;
    int line;
    void (*func)();
    TestCase* next = nullptr;
};

// Intrusive linked-list head — O(1) registration, no heap allocation
inline TestCase*& GetTestListHead()
{
    static TestCase* head = nullptr;
    return head;
}

// Build a flat vector from the linked list on first call (once, from main).
// The linked list is LIFO, so we reverse to get registration order.
inline std::vector<TestCase*>& GetTestRegistry()
{
    static std::vector<TestCase*> tests;
    static bool built = false;
    if (!built)
    {
        built = true;
        for (TestCase* p = GetTestListHead(); p; p = p->next)
            tests.push_back(p);
        std::reverse(tests.begin(), tests.end());
    }
    return tests;
}

struct TestRegistrar
{
    TestCase entry;

    TestRegistrar(const char* name, const char* file, int line, void (*func)())
    {
        entry.name = name;
        entry.file = file;
        entry.line = line;
        entry.func = func;
        entry.next = GetTestListHead();
        GetTestListHead() = &entry;
    }
};

extern int g_assertionsPassed;
extern int g_assertionsFailed;
extern int g_testsWarned;
extern std::string g_currentTest;

// Thrown by the fatal ASSERT_* macros to abort the current test only. The test
// runner in TestMain.cpp catches this per-test (around test->func()), so a
// failed precondition fails one test in isolation instead of letting execution
// fall through into an unsafe dereference and crash the whole process.
struct TestAbort
{
};

#define TEST(name)                                                                                                     \
    void test_##name();                                                                                                \
    static TestRegistrar registrar_##name(#name, __FILE__, __LINE__, test_##name);                                     \
    void test_##name()

// Fixture-based test. FixtureClass must define SetUp() and TearDown() methods.
// Test names appear as "FixtureClass.testName" in output.
#define TEST_F(FixtureClass, testName)                                                                                 \
    struct TestFixture_##FixtureClass##_##testName : public FixtureClass                                               \
    {                                                                                                                  \
        void Run();                                                                                                    \
    };                                                                                                                 \
    static void testfn_fixture_##FixtureClass##_##testName()                                                           \
    {                                                                                                                  \
        TestFixture_##FixtureClass##_##testName fixture;                                                               \
        try                                                                                                            \
        {                                                                                                              \
            fixture.SetUp();                                                                                           \
            fixture.Run();                                                                                             \
        }                                                                                                              \
        catch (...)                                                                                                    \
        {                                                                                                              \
            fixture.TearDown();                                                                                        \
            throw;                                                                                                     \
        }                                                                                                              \
        fixture.TearDown();                                                                                            \
    }                                                                                                                  \
    static TestRegistrar registrar_fixture_##FixtureClass##_##testName(                                                \
        #FixtureClass "." #testName, __FILE__, __LINE__, testfn_fixture_##FixtureClass##_##testName);                  \
    void TestFixture_##FixtureClass##_##testName::Run()

#define EXPECT_TRUE(expr)                                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        if (expr)                                                                                                      \
        {                                                                                                              \
            g_assertionsPassed++;                                                                                      \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            g_assertionsFailed++;                                                                                      \
            std::cerr << "  FAIL: " << #expr << " was false (" << __FILE__ << ":" << __LINE__ << ")\n";                \
        }                                                                                                              \
    } while (0)

#define EXPECT_FALSE(expr) EXPECT_TRUE(!(expr))

#define EXPECT_EQ(a, b)                                                                                                \
    do                                                                                                                 \
    {                                                                                                                  \
        auto _a = (a);                                                                                                 \
        auto _b = (b);                                                                                                 \
        if (_a == _b)                                                                                                  \
        {                                                                                                              \
            g_assertionsPassed++;                                                                                      \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            g_assertionsFailed++;                                                                                      \
            std::cerr << "  FAIL: " << #a << " == " << #b << " (" << _a << " != " << _b << ") at " << __FILE__ << ":"  \
                      << __LINE__ << "\n";                                                                             \
        }                                                                                                              \
    } while (0)

#define EXPECT_NE(a, b)                                                                                                \
    do                                                                                                                 \
    {                                                                                                                  \
        auto _a = (a);                                                                                                 \
        auto _b = (b);                                                                                                 \
        if (_a != _b)                                                                                                  \
        {                                                                                                              \
            g_assertionsPassed++;                                                                                      \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            g_assertionsFailed++;                                                                                      \
            std::cerr << "  FAIL: " << #a << " != " << #b << " (both " << _a << ") at " << __FILE__ << ":" << __LINE__ \
                      << "\n";                                                                                         \
        }                                                                                                              \
    } while (0)

// ============================================================================
// Fatal assertions — on failure they record the failure AND throw TestAbort,
// which stops the current test immediately (the runner catches it per-test).
// Use these instead of EXPECT_* wherever a false condition would make the very
// next line unsafe (e.g. ASSERT_TRUE(p != nullptr) before dereferencing p).
// ============================================================================

#define ASSERT_TRUE(expr)                                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        if (expr)                                                                                                      \
        {                                                                                                              \
            g_assertionsPassed++;                                                                                      \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            g_assertionsFailed++;                                                                                      \
            std::cerr << "  FATAL: " << #expr << " was false (" << __FILE__ << ":" << __LINE__ << ")\n";               \
            throw TestAbort{};                                                                                         \
        }                                                                                                              \
    } while (0)

#define ASSERT_FALSE(expr) ASSERT_TRUE(!(expr))

#define ASSERT_EQ(a, b)                                                                                                \
    do                                                                                                                 \
    {                                                                                                                  \
        auto _a = (a);                                                                                                 \
        auto _b = (b);                                                                                                 \
        if (_a == _b)                                                                                                  \
        {                                                                                                              \
            g_assertionsPassed++;                                                                                      \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            g_assertionsFailed++;                                                                                      \
            std::cerr << "  FATAL: " << #a << " == " << #b << " (" << _a << " != " << _b << ") at " << __FILE__ << ":" \
                      << __LINE__ << "\n";                                                                             \
            throw TestAbort{};                                                                                         \
        }                                                                                                              \
    } while (0)

#define ASSERT_NE(a, b)                                                                                                \
    do                                                                                                                 \
    {                                                                                                                  \
        auto _a = (a);                                                                                                 \
        auto _b = (b);                                                                                                 \
        if (_a != _b)                                                                                                  \
        {                                                                                                              \
            g_assertionsPassed++;                                                                                      \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            g_assertionsFailed++;                                                                                      \
            std::cerr << "  FATAL: " << #a << " != " << #b << " (both " << _a << ") at " << __FILE__ << ":"            \
                      << __LINE__ << "\n";                                                                             \
            throw TestAbort{};                                                                                         \
        }                                                                                                              \
    } while (0)

#define EXPECT_NEAR(a, b, tolerance)                                                                                   \
    do                                                                                                                 \
    {                                                                                                                  \
        auto _a = (a);                                                                                                 \
        auto _b = (b);                                                                                                 \
        auto _t = (tolerance);                                                                                         \
        if (std::abs(_a - _b) <= _t)                                                                                   \
        {                                                                                                              \
            g_assertionsPassed++;                                                                                      \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            g_assertionsFailed++;                                                                                      \
            std::cerr << "  FAIL: |" << #a << " - " << #b << "| <= " << _t << " (" << std::abs(_a - _b) << ") at "     \
                      << __FILE__ << ":" << __LINE__ << "\n";                                                          \
        }                                                                                                              \
    } while (0)

#define EXPECT_GT(a, b)                                                                                                \
    do                                                                                                                 \
    {                                                                                                                  \
        auto _a = (a);                                                                                                 \
        auto _b = (b);                                                                                                 \
        if (_a > _b)                                                                                                   \
        {                                                                                                              \
            g_assertionsPassed++;                                                                                      \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            g_assertionsFailed++;                                                                                      \
            std::cerr << "  FAIL: " << #a << " > " << #b << " (" << _a << " <= " << _b << ") at " << __FILE__ << ":"   \
                      << __LINE__ << "\n";                                                                             \
        }                                                                                                              \
    } while (0)

#define EXPECT_LT(a, b)                                                                                                \
    do                                                                                                                 \
    {                                                                                                                  \
        auto _a = (a);                                                                                                 \
        auto _b = (b);                                                                                                 \
        if (_a < _b)                                                                                                   \
        {                                                                                                              \
            g_assertionsPassed++;                                                                                      \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            g_assertionsFailed++;                                                                                      \
            std::cerr << "  FAIL: " << #a << " < " << #b << " (" << _a << " >= " << _b << ") at " << __FILE__ << ":"   \
                      << __LINE__ << "\n";                                                                             \
        }                                                                                                              \
    } while (0)

#define EXPECT_GE(a, b)                                                                                                \
    do                                                                                                                 \
    {                                                                                                                  \
        auto _a = (a);                                                                                                 \
        auto _b = (b);                                                                                                 \
        if (_a >= _b)                                                                                                  \
        {                                                                                                              \
            g_assertionsPassed++;                                                                                      \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            g_assertionsFailed++;                                                                                      \
            std::cerr << "  FAIL: " << #a << " >= " << #b << " (" << _a << " < " << _b << ") at " << __FILE__ << ":"   \
                      << __LINE__ << "\n";                                                                             \
        }                                                                                                              \
    } while (0)

#define EXPECT_LE(a, b)                                                                                                \
    do                                                                                                                 \
    {                                                                                                                  \
        auto _a = (a);                                                                                                 \
        auto _b = (b);                                                                                                 \
        if (_a <= _b)                                                                                                  \
        {                                                                                                              \
            g_assertionsPassed++;                                                                                      \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            g_assertionsFailed++;                                                                                      \
            std::cerr << "  FAIL: " << #a << " <= " << #b << " (" << _a << " > " << _b << ") at " << __FILE__ << ":"   \
                      << __LINE__ << "\n";                                                                             \
        }                                                                                                              \
    } while (0)

#define EXPECT_THROW(expr, exception_type)                                                                             \
    do                                                                                                                 \
    {                                                                                                                  \
        bool caught = false;                                                                                           \
        bool wrongType = false;                                                                                        \
        std::string wrongMsg;                                                                                          \
        try                                                                                                            \
        {                                                                                                              \
            expr;                                                                                                      \
        }                                                                                                              \
        catch (const exception_type&)                                                                                  \
        {                                                                                                              \
            caught = true;                                                                                             \
        }                                                                                                              \
        catch (const std::exception& _ex)                                                                              \
        {                                                                                                              \
            wrongType = true;                                                                                          \
            wrongMsg = _ex.what();                                                                                     \
        }                                                                                                              \
        catch (...)                                                                                                    \
        {                                                                                                              \
            wrongType = true;                                                                                          \
            wrongMsg = "(non-std exception)";                                                                          \
        }                                                                                                              \
        if (caught)                                                                                                    \
        {                                                                                                              \
            g_assertionsPassed++;                                                                                      \
        }                                                                                                              \
        else if (wrongType)                                                                                            \
        {                                                                                                              \
            g_assertionsFailed++;                                                                                      \
            std::cerr << "  FAIL: Expected " << #exception_type << " from " << #expr << " but got: " << wrongMsg       \
                      << " at " << __FILE__ << ":" << __LINE__ << "\n";                                                \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            g_assertionsFailed++;                                                                                      \
            std::cerr << "  FAIL: Expected " << #exception_type << " from " << #expr << " but nothing was thrown"      \
                      << " at " << __FILE__ << ":" << __LINE__ << "\n";                                                \
        }                                                                                                              \
    } while (0)

#define EXPECT_NO_THROW(expr)                                                                                          \
    do                                                                                                                 \
    {                                                                                                                  \
        bool threw = false;                                                                                            \
        try                                                                                                            \
        {                                                                                                              \
            expr;                                                                                                      \
        }                                                                                                              \
        catch (...)                                                                                                    \
        {                                                                                                              \
            threw = true;                                                                                              \
        }                                                                                                              \
        if (!threw)                                                                                                    \
        {                                                                                                              \
            g_assertionsPassed++;                                                                                      \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            g_assertionsFailed++;                                                                                      \
            std::cerr << "  FAIL: Unexpected exception from " << #expr << " at " << __FILE__ << ":" << __LINE__        \
                      << "\n";                                                                                         \
        }                                                                                                              \
    } while (0)

#define EXPECT_STR_CONTAINS(haystack, needle)                                                                          \
    do                                                                                                                 \
    {                                                                                                                  \
        std::string _h = (haystack);                                                                                   \
        std::string _n = (needle);                                                                                     \
        if (_h.find(_n) != std::string::npos)                                                                          \
        {                                                                                                              \
            g_assertionsPassed++;                                                                                      \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            g_assertionsFailed++;                                                                                      \
            std::string hTrunc = _h.size() > 200 ? _h.substr(0, 200) + "..." : _h;                                     \
            std::string nTrunc = _n.size() > 200 ? _n.substr(0, 200) + "..." : _n;                                     \
            std::cerr << "  FAIL: \"" << hTrunc << "\" does not contain \"" << nTrunc << "\" at " << __FILE__ << ":"   \
                      << __LINE__ << "\n";                                                                             \
        }                                                                                                              \
    } while (0)
