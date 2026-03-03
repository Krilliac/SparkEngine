#pragma once

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

// DEBUG_BREAK: trigger a debugger breakpoint.
//   - MSVC (all versions, including VS 2026): uses the intrinsic __debugbreak().
//   - Modern Clang/GCC (incl. Apple Clang, Intel LLVM, GCC 12+): uses the
//     portable __builtin_debugtrap() when available via __has_builtin.
//   - Fallback: raise(SIGTRAP) on POSIX systems.
#if defined(_MSC_VER)
#  include <intrin.h>
#  define DEBUG_BREAK() __debugbreak()
#elif defined(__has_builtin) && __has_builtin(__builtin_debugtrap)
   // Clang 3.8+, Apple Clang, Intel LLVM (ICPX), GCC 12+
#  define DEBUG_BREAK() __builtin_debugtrap()
#else
#  include <signal.h>
#  define DEBUG_BREAK() raise(SIGTRAP)
#endif

#ifdef _WIN32
#  include <windows.h>
#  include <dbghelp.h>
#  pragma comment(lib, "dbghelp.lib")
#endif

// Forward declarations
void TriggerCrashHandler(const char* assertMsg);

namespace Assert
{
    // Core failure implementation
    inline void Fail(const char* expr,
        const char* file,
        int         line,
        const char* fmt = nullptr, ...)
    {
        // Timestamp
        std::time_t t = std::time(nullptr);
        char timeBuf[64];
        std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));

        // Format user message if provided
        char userMsg[512] = {};
        if (fmt)
        {
            va_list args;
            va_start(args, fmt);
            vsnprintf(userMsg, sizeof(userMsg), fmt, args);
            va_end(args);
        }

        // Build the full assertion text
        char fullMsg[2048];
        std::snprintf(fullMsg, sizeof(fullMsg),
            "===== ASSERTION FAILED =====\n"
            "Time       : %s\n"
            "Expression : %s\n"
            "Location   : %s(%d)\n"
            "Message    : %s\n"
            "Thread ID  : 0x%08X\n",
            timeBuf,
            expr,
            file, line,
            userMsg[0] ? userMsg : "(none)",
#ifdef _WIN32
            static_cast<unsigned>(GetCurrentThreadId())
#else
            0u
#endif
        );

        // Print immediately
        std::fprintf(stderr, "%s", fullMsg);
        std::fflush(stderr);

        // Trigger optional crash handler
        TriggerCrashHandler(fullMsg);

        DEBUG_BREAK();
        std::abort();
    }

    // HRESULT-specific fail
    inline void FailHResult(const char* expr,
        const char* file,
        int         line,
        long        hr,
        const char* fmt = nullptr, ...)
    {
        char userMsg[512] = {};
        if (fmt)
        {
            va_list args;
            va_start(args, fmt);
            vsnprintf(userMsg, sizeof(userMsg), fmt, args);
            va_end(args);
        }

        char fullMsg[2048];
        std::snprintf(fullMsg, sizeof(fullMsg),
            "===== HRESULT FAILED =====\n"
            "Expression : %s returned 0x%08lX\n"
            "Location   : %s(%d)\n"
            "Message    : %s\n"
            "Thread ID  : 0x%08X\n",
            expr, hr,
            file, line,
            userMsg[0] ? userMsg : "(none)",
#ifdef _WIN32
            static_cast<unsigned>(GetCurrentThreadId())
#else
            0u
#endif
        );

        std::fprintf(stderr, "%s", fullMsg);
        std::fflush(stderr);

        TriggerCrashHandler(fullMsg);
        DEBUG_BREAK();
        std::abort();
    }
}

// Simple assert: no message
#if defined(_DEBUG) || defined(DEBUG)
#  define ASSERT(expr) \
     ((expr) ? (void)0 : Assert::Fail(#expr, __FILE__, __LINE__))
#else
#  define ASSERT(expr) ((void)0)
#endif

// Assert with formatted message
#if defined(_DEBUG) || defined(DEBUG)
#  define ASSERT_MSG(expr, fmt, ...) \
     ((expr) ? (void)0 : Assert::Fail(#expr, __FILE__, __LINE__, fmt, ##__VA_ARGS__))
#else
#  define ASSERT_MSG(expr, fmt, ...) ((void)0)
#endif

// Always-on asserts
#if !defined(DISABLE_ALWAYS_ASSERTS)
#  define ASSERT_ALWAYS(expr) \
     ((expr) ? (void)0 : Assert::Fail(#expr, __FILE__, __LINE__))
#  define ASSERT_ALWAYS_MSG(expr, fmt, ...) \
     ((expr) ? (void)0 : Assert::Fail(#expr, __FILE__, __LINE__, fmt, ##__VA_ARGS__))
#else
#  define ASSERT_ALWAYS(expr)       ((void)0)
#  define ASSERT_ALWAYS_MSG(expr, fmt, ...) ((void)0)
#endif

// HRESULT-specific asserts
#if defined(_DEBUG) || defined(DEBUG)
#  define ASSERT_HR(hrExpr)                                \
     do { long _hr = static_cast<long>(hrExpr);             \
          if (FAILED(_hr))                                  \
             Assert::FailHResult(#hrExpr, __FILE__, __LINE__, _hr); \
     } while(0)

#  define ASSERT_HR_MSG(hrExpr, fmt, ...)                   \
     do { long _hr = static_cast<long>(hrExpr);             \
          if (FAILED(_hr))                                  \
             Assert::FailHResult(#hrExpr, __FILE__, __LINE__, _hr, fmt, ##__VA_ARGS__); \
     } while(0)
#else
#  define ASSERT_HR(hrExpr)       ((void)(hrExpr))
#  define ASSERT_HR_MSG(hrExpr, fmt, ...) ((void)(hrExpr))
#endif

// Null-pointer assertion
#define ASSERT_NOT_NULL(ptr)   ASSERT_MSG((ptr) != nullptr, "Pointer " #ptr " must not be null")
#define ASSERT_NOT_NULL_ALWAYS(ptr) ASSERT_ALWAYS_MSG((ptr) != nullptr, "Pointer " #ptr " must not be null")

// Range assertion
#define ASSERT_IN_RANGE(v, min, max) \
    ASSERT_MSG((v) >= (min) && (v) <= (max), #v " out of range [" #min "," #max "]")

// VERIFY: like ASSERT but active in ALL builds (debug and release)
// Use for conditions that should never fail even in production.
#define VERIFY(expr) \
    ((expr) ? (void)0 : Assert::Fail(#expr, __FILE__, __LINE__))

#define VERIFY_MSG(expr, fmt, ...) \
    ((expr) ? (void)0 : Assert::Fail(#expr, __FILE__, __LINE__, fmt, ##__VA_ARGS__))

// VERIFY_HR: HRESULT check active in ALL builds
#define VERIFY_HR(hrExpr) \
    do { long _hr = static_cast<long>(hrExpr); \
         if (FAILED(_hr)) \
            Assert::FailHResult(#hrExpr, __FILE__, __LINE__, _hr); \
    } while(0)

// Bounds assertion with value info (shows what index was vs valid range)
#define ASSERT_BOUNDS(index, size) \
    ASSERT_MSG(static_cast<long long>(index) >= 0 && static_cast<size_t>(index) < (size), \
               #index " = %lld out of bounds [0, %llu)", \
               static_cast<long long>(index), static_cast<unsigned long long>(size))

// Enum range assertion
#define ASSERT_ENUM_RANGE(val, maxVal) \
    ASSERT_MSG(static_cast<int>(val) >= 0 && static_cast<int>(val) < static_cast<int>(maxVal), \
               #val " enum value %d out of valid range [0, %d)", \
               static_cast<int>(val), static_cast<int>(maxVal))