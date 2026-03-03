/**
 * @file TestFramework.h
 * @brief Lightweight test framework for SparkEngine
 *
 * Simple test framework without external dependencies.
 * Include this header in all test files, and include TestMain.cpp
 * in the build to get the main runner.
 */

#pragma once

#include <iostream>
#include <string>
#include <vector>
#include <functional>
#include <cstdlib>
#include <cmath>

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

inline std::vector<TestCase>& GetTestRegistry()
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

extern int g_assertionsPassed;
extern int g_assertionsFailed;
extern std::string g_currentTest;

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

#define EXPECT_GE(a, b) \
    do { \
        auto _a = (a); auto _b = (b); \
        if (_a >= _b) { g_assertionsPassed++; } \
        else { g_assertionsFailed++; \
            std::cerr << "  FAIL: " << #a << " >= " << #b << " (" << _a << " < " << _b << ") at " << __FILE__ << ":" << __LINE__ << "\n"; } \
    } while(0)

#define EXPECT_LE(a, b) \
    do { \
        auto _a = (a); auto _b = (b); \
        if (_a <= _b) { g_assertionsPassed++; } \
        else { g_assertionsFailed++; \
            std::cerr << "  FAIL: " << #a << " <= " << #b << " (" << _a << " > " << _b << ") at " << __FILE__ << ":" << __LINE__ << "\n"; } \
    } while(0)

#define EXPECT_THROW(expr, exception_type) \
    do { \
        bool caught = false; \
        try { expr; } \
        catch (const exception_type&) { caught = true; } \
        catch (...) {} \
        if (caught) { g_assertionsPassed++; } \
        else { g_assertionsFailed++; \
            std::cerr << "  FAIL: Expected " << #exception_type << " from " << #expr << " at " << __FILE__ << ":" << __LINE__ << "\n"; } \
    } while(0)

#define EXPECT_NO_THROW(expr) \
    do { \
        bool threw = false; \
        try { expr; } \
        catch (...) { threw = true; } \
        if (!threw) { g_assertionsPassed++; } \
        else { g_assertionsFailed++; \
            std::cerr << "  FAIL: Unexpected exception from " << #expr << " at " << __FILE__ << ":" << __LINE__ << "\n"; } \
    } while(0)
