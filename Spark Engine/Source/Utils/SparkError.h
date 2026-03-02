/**
 * @file SparkError.h
 * @brief Comprehensive error handling, bounds checking, and verbose diagnostics for Spark Engine
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides:
 *  - SPARK_CHECK / SPARK_CHECK_MSG   -- soft checks that log and continue
 *  - SPARK_VERIFY / SPARK_VERIFY_MSG -- checks that stay in Release builds
 *  - SPARK_BOUNDS_CHECK              -- index/range validation
 *  - SPARK_HR_CHECK                  -- HRESULT checking with verbose logging
 *  - SPARK_CATCH_ALL                 -- structured exception/catch wrapper
 *  - SPARK_SCOPED_CONTEXT            -- scoped breadcrumb for crash reports
 *  - Severity-tagged log helpers     -- SPARK_LOG_TRACE .. SPARK_LOG_FATAL
 */

#pragma once

#include "Assert.h"      // existing ASSERT / ASSERT_MSG
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <string>
#include <sstream>
#include <mutex>

#ifdef _WIN32
#  include <windows.h>
#endif

// ---------------------------------------------------------------------------
// Internal helpers (not part of the public API)
// ---------------------------------------------------------------------------
namespace SparkError
{
    // Severity levels matching the editor logger but usable engine-wide
    enum class Severity
    {
        Trace = 0,
        Debug = 1,
        Info  = 2,
        Warn  = 3,
        Error = 4,
        Fatal = 5
    };

    inline const char* SeverityToString(Severity s)
    {
        switch (s)
        {
            case Severity::Trace: return "TRACE";
            case Severity::Debug: return "DEBUG";
            case Severity::Info:  return "INFO ";
            case Severity::Warn:  return "WARN ";
            case Severity::Error: return "ERROR";
            case Severity::Fatal: return "FATAL";
            default:              return "?????";
        }
    }

    // Thread-safe stderr + OutputDebugString logger
    inline void LogMessage(Severity severity,
                           const char* category,
                           const char* file,
                           int         line,
                           const char* func,
                           const char* fmt, ...)
    {
        static std::mutex s_logMutex;

        char userMsg[1024] = {};
        if (fmt)
        {
            va_list args;
            va_start(args, fmt);
            vsnprintf(userMsg, sizeof(userMsg), fmt, args);
            va_end(args);
        }

        // Shorten file path to just filename for readability
        const char* shortFile = file;
        if (file)
        {
            const char* slash = strrchr(file, '\\');
            if (!slash) slash = strrchr(file, '/');
            if (slash) shortFile = slash + 1;
        }

        char fullMsg[2048];
        snprintf(fullMsg, sizeof(fullMsg),
            "[%s][%-8s] %s  (%s:%d in %s)\n",
            SeverityToString(severity),
            category ? category : "Engine",
            userMsg,
            shortFile ? shortFile : "?",
            line,
            func ? func : "?");

        {
            std::lock_guard<std::mutex> lock(s_logMutex);
            fprintf(stderr, "%s", fullMsg);
            fflush(stderr);
#ifdef _WIN32
            OutputDebugStringA(fullMsg);
#endif
        }
    }

    // Soft-check handler: logs error but does NOT abort
    inline bool CheckFailed(const char* expr,
                            const char* file,
                            int         line,
                            const char* func,
                            const char* msg = nullptr)
    {
        if (msg && msg[0])
        {
            LogMessage(Severity::Error, "Check", file, line, func,
                       "CHECK FAILED: %s -- %s", expr, msg);
        }
        else
        {
            LogMessage(Severity::Error, "Check", file, line, func,
                       "CHECK FAILED: %s", expr);
        }
        return false; // always returns false for use in conditionals
    }

    // Bounds-check handler: logs and returns false
    inline bool BoundsCheckFailed(const char* indexExpr,
                                  long long   index,
                                  long long   size,
                                  const char* file,
                                  int         line,
                                  const char* func)
    {
        LogMessage(Severity::Error, "Bounds", file, line, func,
                   "BOUNDS CHECK FAILED: %s = %lld, valid range [0, %lld)",
                   indexExpr, index, size);
        return false;
    }

    // HRESULT handler: logs HRESULT details
    inline bool HResultFailed(const char* expr,
                              long        hr,
                              const char* file,
                              int         line,
                              const char* func,
                              const char* msg = nullptr)
    {
#ifdef _WIN32
        // Attempt to get the system error message for this HRESULT
        char sysMsg[256] = {};
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, static_cast<DWORD>(hr),
                       MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                       sysMsg, sizeof(sysMsg), nullptr);
        // Strip trailing newlines
        for (int i = (int)strlen(sysMsg) - 1; i >= 0 && (sysMsg[i] == '\n' || sysMsg[i] == '\r'); --i)
            sysMsg[i] = '\0';
#else
        const char* sysMsg = "";
#endif
        if (msg && msg[0])
        {
            LogMessage(Severity::Error, "HRESULT", file, line, func,
                       "HR FAILED: %s returned 0x%08lX (%s) -- %s",
                       expr, hr, sysMsg, msg);
        }
        else
        {
            LogMessage(Severity::Error, "HRESULT", file, line, func,
                       "HR FAILED: %s returned 0x%08lX (%s)",
                       expr, hr, sysMsg);
        }
        return false;
    }

    // Scoped breadcrumb for crash context
    class ScopedContext
    {
    public:
        ScopedContext(const char* context, const char* file, int line)
            : m_context(context), m_file(file), m_line(line)
        {
            LogMessage(Severity::Trace, "Context", file, line, nullptr,
                       ">> ENTER: %s", context);
        }
        ~ScopedContext()
        {
            LogMessage(Severity::Trace, "Context", m_file, m_line, nullptr,
                       "<< EXIT:  %s", m_context);
        }
    private:
        const char* m_context;
        const char* m_file;
        int m_line;
    };

    // Exception code to human-readable name (Windows SEH)
#ifdef _WIN32
    inline const char* ExceptionCodeToString(DWORD code)
    {
        switch (code)
        {
            case EXCEPTION_ACCESS_VIOLATION:         return "ACCESS_VIOLATION";
            case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "ARRAY_BOUNDS_EXCEEDED";
            case EXCEPTION_BREAKPOINT:               return "BREAKPOINT";
            case EXCEPTION_DATATYPE_MISALIGNMENT:    return "DATATYPE_MISALIGNMENT";
            case EXCEPTION_FLT_DENORMAL_OPERAND:     return "FLT_DENORMAL_OPERAND";
            case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "FLT_DIVIDE_BY_ZERO";
            case EXCEPTION_FLT_INEXACT_RESULT:       return "FLT_INEXACT_RESULT";
            case EXCEPTION_FLT_INVALID_OPERATION:    return "FLT_INVALID_OPERATION";
            case EXCEPTION_FLT_OVERFLOW:             return "FLT_OVERFLOW";
            case EXCEPTION_FLT_STACK_CHECK:          return "FLT_STACK_CHECK";
            case EXCEPTION_FLT_UNDERFLOW:            return "FLT_UNDERFLOW";
            case EXCEPTION_ILLEGAL_INSTRUCTION:      return "ILLEGAL_INSTRUCTION";
            case EXCEPTION_IN_PAGE_ERROR:            return "IN_PAGE_ERROR";
            case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "INT_DIVIDE_BY_ZERO";
            case EXCEPTION_INT_OVERFLOW:             return "INT_OVERFLOW";
            case EXCEPTION_INVALID_DISPOSITION:      return "INVALID_DISPOSITION";
            case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "NONCONTINUABLE_EXCEPTION";
            case EXCEPTION_PRIV_INSTRUCTION:         return "PRIV_INSTRUCTION";
            case EXCEPTION_SINGLE_STEP:              return "SINGLE_STEP";
            case EXCEPTION_STACK_OVERFLOW:           return "STACK_OVERFLOW";
            case STATUS_FATAL_APP_EXIT:              return "FATAL_APP_EXIT (Assert)";
            default:                                 return "UNKNOWN_EXCEPTION";
        }
    }
#endif

} // namespace SparkError

// ===========================================================================
//  Public macros
// ===========================================================================

// ---------------------------------------------------------------------------
//  Logging macros -- always compiled in, filtered at runtime if needed
// ---------------------------------------------------------------------------
#define SPARK_LOG_TRACE(cat, fmt, ...) \
    SparkError::LogMessage(SparkError::Severity::Trace, cat, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__)

#define SPARK_LOG_DEBUG(cat, fmt, ...) \
    SparkError::LogMessage(SparkError::Severity::Debug, cat, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__)

#define SPARK_LOG_INFO(cat, fmt, ...) \
    SparkError::LogMessage(SparkError::Severity::Info, cat, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__)

#define SPARK_LOG_WARN(cat, fmt, ...) \
    SparkError::LogMessage(SparkError::Severity::Warn, cat, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__)

#define SPARK_LOG_ERROR(cat, fmt, ...) \
    SparkError::LogMessage(SparkError::Severity::Error, cat, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__)

#define SPARK_LOG_FATAL(cat, fmt, ...) \
    SparkError::LogMessage(SparkError::Severity::Fatal, cat, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__)

// ---------------------------------------------------------------------------
//  Soft checks -- log and continue (return false / early-out)
//  Use these for recoverable situations where aborting is not desired.
// ---------------------------------------------------------------------------
#define SPARK_CHECK(expr) \
    ((expr) ? true : SparkError::CheckFailed(#expr, __FILE__, __LINE__, __FUNCTION__))

#define SPARK_CHECK_MSG(expr, msg) \
    ((expr) ? true : SparkError::CheckFailed(#expr, __FILE__, __LINE__, __FUNCTION__, msg))

// ---------------------------------------------------------------------------
//  VERIFY -- like ASSERT but stays active in Release builds
//  Use for conditions that must never fail even in production.
// ---------------------------------------------------------------------------
#define SPARK_VERIFY(expr) \
    do { if (!(expr)) { \
        SparkError::LogMessage(SparkError::Severity::Fatal, "Verify", __FILE__, __LINE__, __FUNCTION__, \
            "VERIFY FAILED: %s", #expr); \
        Assert::Fail(#expr, __FILE__, __LINE__); \
    } } while(0)

#define SPARK_VERIFY_MSG(expr, fmt, ...) \
    do { if (!(expr)) { \
        SparkError::LogMessage(SparkError::Severity::Fatal, "Verify", __FILE__, __LINE__, __FUNCTION__, \
            "VERIFY FAILED: %s -- " fmt, #expr, ##__VA_ARGS__); \
        Assert::Fail(#expr, __FILE__, __LINE__, fmt, ##__VA_ARGS__); \
    } } while(0)

// ---------------------------------------------------------------------------
//  Bounds checking -- validates index is in [0, size)
//  Evaluates to true if in-bounds, false (and logs) if out-of-bounds.
// ---------------------------------------------------------------------------
#define SPARK_BOUNDS_CHECK(index, size) \
    (((long long)(index) >= 0 && (long long)(index) < (long long)(size)) ? true \
        : SparkError::BoundsCheckFailed(#index, (long long)(index), (long long)(size), \
                                        __FILE__, __LINE__, __FUNCTION__))

// Bounds check with custom message
#define SPARK_BOUNDS_CHECK_MSG(index, size, msg) \
    do { if (!SPARK_BOUNDS_CHECK(index, size)) { \
        SPARK_LOG_ERROR("Bounds", "  Additional context: %s", msg); \
    } } while(0)

// ---------------------------------------------------------------------------
//  HRESULT checking -- logs the translated error on failure
//  Evaluates to true if SUCCEEDED, false (and logs) if FAILED.
// ---------------------------------------------------------------------------
#define SPARK_HR_CHECK(hrExpr) \
    ([&]() -> bool { \
        long _hr = static_cast<long>(hrExpr); \
        if (SUCCEEDED(_hr)) return true; \
        return SparkError::HResultFailed(#hrExpr, _hr, __FILE__, __LINE__, __FUNCTION__); \
    }())

#define SPARK_HR_CHECK_MSG(hrExpr, msg) \
    ([&]() -> bool { \
        long _hr = static_cast<long>(hrExpr); \
        if (SUCCEEDED(_hr)) return true; \
        return SparkError::HResultFailed(#hrExpr, _hr, __FILE__, __LINE__, __FUNCTION__, msg); \
    }())

// ---------------------------------------------------------------------------
//  Not-null pointer check -- soft check variant
// ---------------------------------------------------------------------------
#define SPARK_CHECK_NOT_NULL(ptr) \
    SPARK_CHECK_MSG((ptr) != nullptr, "Pointer '" #ptr "' must not be null")

// ---------------------------------------------------------------------------
//  Structured exception / catch wrapper
//  Use around code that may throw; logs the exception and continues.
// ---------------------------------------------------------------------------
#define SPARK_CATCH_ALL(category, block) \
    do { try { block; } \
    catch (const std::exception& _ex) { \
        SparkError::LogMessage(SparkError::Severity::Error, category, __FILE__, __LINE__, __FUNCTION__, \
            "EXCEPTION CAUGHT: %s", _ex.what()); \
    } catch (...) { \
        SparkError::LogMessage(SparkError::Severity::Error, category, __FILE__, __LINE__, __FUNCTION__, \
            "UNKNOWN EXCEPTION CAUGHT"); \
    } } while(0)

// Same but returns a value on exception
#define SPARK_CATCH_ALL_RET(category, retval, block) \
    [&]() { try { block; } \
    catch (const std::exception& _ex) { \
        SparkError::LogMessage(SparkError::Severity::Error, category, __FILE__, __LINE__, __FUNCTION__, \
            "EXCEPTION CAUGHT (returning default): %s", _ex.what()); \
        return retval; \
    } catch (...) { \
        SparkError::LogMessage(SparkError::Severity::Error, category, __FILE__, __LINE__, __FUNCTION__, \
            "UNKNOWN EXCEPTION CAUGHT (returning default)"); \
        return retval; \
    } }()

// ---------------------------------------------------------------------------
//  Scoped context breadcrumb -- appears in trace logs and crash reports
// ---------------------------------------------------------------------------
#define SPARK_SCOPED_CONTEXT(name) \
    SparkError::ScopedContext _sparkCtx_##__LINE__(name, __FILE__, __LINE__)

// ---------------------------------------------------------------------------
//  Convenience: check + early return patterns
// ---------------------------------------------------------------------------
#define SPARK_CHECK_RETURN(expr) \
    do { if (!SPARK_CHECK(expr)) return; } while(0)

#define SPARK_CHECK_RETURN_VAL(expr, retval) \
    do { if (!SPARK_CHECK(expr)) return (retval); } while(0)

#define SPARK_CHECK_NOT_NULL_RETURN(ptr) \
    do { if (!SPARK_CHECK_NOT_NULL(ptr)) return; } while(0)

#define SPARK_CHECK_NOT_NULL_RETURN_VAL(ptr, retval) \
    do { if (!SPARK_CHECK_NOT_NULL(ptr)) return (retval); } while(0)

#define SPARK_HR_CHECK_RETURN(hrExpr) \
    do { if (!SPARK_HR_CHECK(hrExpr)) return; } while(0)

#define SPARK_HR_CHECK_RETURN_VAL(hrExpr, retval) \
    do { if (!SPARK_HR_CHECK(hrExpr)) return (retval); } while(0)
