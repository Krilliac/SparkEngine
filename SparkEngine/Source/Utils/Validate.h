/**
 * @file Validate.h
 * @brief Project-wide validation, defensive programming, and diagnostic logging utilities
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a unified validation framework combining:
 *
 * ## Defensive Programming (fail-safe with logging)
 * - SPARK_VALIDATE / SPARK_VALIDATE_MSG        — soft checks that log + return on failure
 * - SPARK_VALIDATE_NOT_NULL                     — null pointer guard with early return
 * - SPARK_VALIDATE_RANGE                        — numeric range guard
 * - SPARK_VALIDATE_ENUM                         — enum range guard
 * - SPARK_VALIDATE_NOT_EMPTY                    — string/container non-empty guard
 *
 * ## Offensive Programming (fail-fast with abort)
 * - SPARK_REQUIRE / SPARK_REQUIRE_MSG           — precondition checks (abort on failure)
 * - SPARK_REQUIRE_NOT_NULL                      — non-null precondition
 * - SPARK_ENSURE / SPARK_ENSURE_MSG             — postcondition checks (abort on failure)
 * - SPARK_UNREACHABLE                           — marks impossible code paths
 *
 * ## Diagnostic Logging Utilities
 * - SPARK_TRACE_ENTER / SPARK_TRACE_EXIT        — function entry/exit tracing
 * - SPARK_TRACE_SCOPE                           — RAII scope tracing
 * - SPARK_TRACE_VALUE                           — log named variable value
 * - SPARK_DIAGNOSTIC_CONTEXT                    — scoped breadcrumb for crash reports
 *
 * ## Contract Logging (structured validation reports)
 * - SPARK_CONTRACT_LOG_PRE                      — log precondition check result
 * - SPARK_CONTRACT_LOG_POST                     — log postcondition check result
 * - SPARK_CONTRACT_LOG_INVARIANT                — log invariant check result
 *
 * All macros integrate with the unified Logger system (LogMacros.h) and the
 * Assert system (Assert.h). Defensive macros log errors and continue; offensive
 * macros log fatal errors and abort via ASSERT_ALWAYS.
 *
 * @see Assert.h, LogMacros.h, SparkError.h, Result.h
 */

#pragma once

#include "Assert.h"
#include "LogMacros.h"
#include <string>
#include <type_traits>

// ============================================================================
// Internal helpers
// ============================================================================

namespace Spark::Validation
{

    /**
     * @brief Log a validation failure through the unified logger
     *
     * @param level     Severity level for the log message
     * @param category  Log category (subsystem that triggered the validation)
     * @param kind      Validation kind label ("PRECONDITION", "POSTCONDITION", etc.)
     * @param expr      The stringified expression that was checked
     * @param file      Source file (__FILE__)
     * @param line      Source line (__LINE__)
     * @param func      Function name (__FUNCTION__)
     * @param msg       Optional human-readable message (may be nullptr)
     */
    inline void LogValidationFailure(Spark::LogLevel level, Spark::LogCategory category, const char* kind,
                                     const char* expr, const char* file, int line, const char* func,
                                     const char* msg = nullptr)
    {
        auto& logger = Spark::Logger::Get();
        if (!logger.IsInitialized())
        {
            // Fallback to stderr before logger is ready
            if (msg && msg[0])
            {
                std::fprintf(stderr, "[%s] VALIDATION FAILED: %s — %s  (%s:%d in %s)\n", kind, expr, msg, file, line,
                             func ? func : "?");
            }
            else
            {
                std::fprintf(stderr, "[%s] VALIDATION FAILED: %s  (%s:%d in %s)\n", kind, expr, file, line,
                             func ? func : "?");
            }
            auto earlyTrace = Spark::StackTrace::Capture(2);
            std::string earlyTraceStr = earlyTrace.ToString("    ");
            if (!earlyTraceStr.empty())
            {
                std::fprintf(stderr, "Stack Trace:\n%s", earlyTraceStr.c_str());
            }
            std::fflush(stderr);
            return;
        }

        char buf[1024];
        if (msg && msg[0])
        {
            std::snprintf(buf, sizeof(buf), "[%s] VALIDATION FAILED: %s — %s", kind, expr, msg);
        }
        else
        {
            std::snprintf(buf, sizeof(buf), "[%s] VALIDATION FAILED: %s", kind, expr);
        }
        logger.Log(level, category, file, line, func, std::string(buf));

        // Append stack trace for diagnostic context
        auto stackTrace = Spark::StackTrace::Capture(2);
        std::string traceStr = stackTrace.ToString("    ");
        if (!traceStr.empty())
        {
            logger.Log(level, category, file, line, func, std::string("Stack Trace:\n") + traceStr);
        }
    }

    /**
     * @brief Log a validation success (trace level) for contract auditing
     */
    inline void LogValidationSuccess(Spark::LogCategory category, const char* kind, const char* expr, const char* file,
                                     int line, const char* func)
    {
        auto& logger = Spark::Logger::Get();
        if (!logger.IsInitialized() || !logger.ShouldLog(Spark::LogLevel::Trace, category))
            return;

        char buf[512];
        std::snprintf(buf, sizeof(buf), "[%s] OK: %s", kind, expr);
        logger.Log(Spark::LogLevel::Trace, category, file, line, func, std::string(buf));
    }

    /**
     * @brief RAII scope tracer that logs function/scope entry and exit
     */
    class ScopeTracer
    {
      public:
        ScopeTracer(Spark::LogCategory category, const char* scopeName, const char* file, int line)
            : m_category(category), m_scopeName(scopeName), m_file(file), m_line(line)
        {
            SPARK_LOG_TRACE(category, ">> ENTER: %s", scopeName);
        }

        ~ScopeTracer() { SPARK_LOG_TRACE(m_category, "<< EXIT:  %s", m_scopeName); }

        ScopeTracer(const ScopeTracer&) = delete;
        ScopeTracer& operator=(const ScopeTracer&) = delete;

      private:
        Spark::LogCategory m_category;
        const char* m_scopeName;
        const char* m_file;
        int m_line;
    };

} // namespace Spark::Validation

// ============================================================================
// Defensive Programming — Soft checks (log + return, never abort)
// ============================================================================

/**
 * @def SPARK_VALIDATE(category, expr)
 * @brief Soft validation: logs an error and returns (void) if expr is false
 *
 * Use for recoverable conditions at system boundaries. The expression is always
 * evaluated. On failure, logs at Error level and returns from the calling function.
 */
#define SPARK_VALIDATE(category, expr)                                                                                 \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(expr))                                                                                                   \
        {                                                                                                              \
            Spark::Validation::LogValidationFailure(Spark::LogLevel::Error, category, "VALIDATE", #expr, __FILE__,     \
                                                    __LINE__, __FUNCTION__);                                           \
            return;                                                                                                    \
        }                                                                                                              \
    } while (0)

/**
 * @def SPARK_VALIDATE_MSG(category, expr, msg)
 * @brief Soft validation with a human-readable message
 */
#define SPARK_VALIDATE_MSG(category, expr, msg)                                                                        \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(expr))                                                                                                   \
        {                                                                                                              \
            Spark::Validation::LogValidationFailure(Spark::LogLevel::Error, category, "VALIDATE", #expr, __FILE__,     \
                                                    __LINE__, __FUNCTION__, msg);                                      \
            return;                                                                                                    \
        }                                                                                                              \
    } while (0)

/**
 * @def SPARK_VALIDATE_RET(category, expr, retval)
 * @brief Soft validation that returns a specific value on failure
 */
#define SPARK_VALIDATE_RET(category, expr, retval)                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(expr))                                                                                                   \
        {                                                                                                              \
            Spark::Validation::LogValidationFailure(Spark::LogLevel::Error, category, "VALIDATE", #expr, __FILE__,     \
                                                    __LINE__, __FUNCTION__);                                           \
            return (retval);                                                                                           \
        }                                                                                                              \
    } while (0)

/**
 * @def SPARK_VALIDATE_NOT_NULL(category, ptr)
 * @brief Null-pointer guard: logs error and returns (void) if ptr is null
 */
#define SPARK_VALIDATE_NOT_NULL(category, ptr)                                                                         \
    do                                                                                                                 \
    {                                                                                                                  \
        if ((ptr) == nullptr)                                                                                          \
        {                                                                                                              \
            Spark::Validation::LogValidationFailure(Spark::LogLevel::Error, category, "NULL_CHECK",                    \
                                                    "'" #ptr "' must not be null", __FILE__, __LINE__, __FUNCTION__);  \
            return;                                                                                                    \
        }                                                                                                              \
    } while (0)

/**
 * @def SPARK_VALIDATE_NOT_NULL_RET(category, ptr, retval)
 * @brief Null-pointer guard that returns a specific value on failure
 */
#define SPARK_VALIDATE_NOT_NULL_RET(category, ptr, retval)                                                             \
    do                                                                                                                 \
    {                                                                                                                  \
        if ((ptr) == nullptr)                                                                                          \
        {                                                                                                              \
            Spark::Validation::LogValidationFailure(Spark::LogLevel::Error, category, "NULL_CHECK",                    \
                                                    "'" #ptr "' must not be null", __FILE__, __LINE__, __FUNCTION__);  \
            return (retval);                                                                                           \
        }                                                                                                              \
    } while (0)

/**
 * @def SPARK_VALIDATE_RANGE(category, val, minVal, maxVal)
 * @brief Range guard: logs error and returns if val is outside [minVal, maxVal]
 */
#define SPARK_VALIDATE_RANGE(category, val, minVal, maxVal)                                                            \
    do                                                                                                                 \
    {                                                                                                                  \
        if ((val) < (minVal) || (val) > (maxVal))                                                                      \
        {                                                                                                              \
            SPARK_LOG_ERROR(category, "RANGE VALIDATION FAILED: %s = %g, valid range [%g, %g]", #val,                  \
                            static_cast<double>(val), static_cast<double>(minVal), static_cast<double>(maxVal));       \
            return;                                                                                                    \
        }                                                                                                              \
    } while (0)

/**
 * @def SPARK_VALIDATE_ENUM(category, val, maxVal)
 * @brief Enum range guard: logs error and returns if enum val is outside [0, maxVal)
 */
#define SPARK_VALIDATE_ENUM(category, val, maxVal)                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        if (static_cast<int>(val) < 0 || static_cast<int>(val) >= static_cast<int>(maxVal))                            \
        {                                                                                                              \
            SPARK_LOG_ERROR(category, "ENUM VALIDATION FAILED: %s = %d, valid range [0, %d)", #val,                    \
                            static_cast<int>(val), static_cast<int>(maxVal));                                          \
            return;                                                                                                    \
        }                                                                                                              \
    } while (0)

/**
 * @def SPARK_VALIDATE_NOT_EMPTY(category, container)
 * @brief Non-empty guard for strings and containers: logs error and returns if empty
 */
#define SPARK_VALIDATE_NOT_EMPTY(category, container)                                                                  \
    do                                                                                                                 \
    {                                                                                                                  \
        if ((container).empty())                                                                                       \
        {                                                                                                              \
            Spark::Validation::LogValidationFailure(Spark::LogLevel::Error, category, "EMPTY_CHECK",                   \
                                                    "'" #container "' must not be empty", __FILE__, __LINE__,          \
                                                    __FUNCTION__);                                                     \
            return;                                                                                                    \
        }                                                                                                              \
    } while (0)

/**
 * @def SPARK_VALIDATE_POSITIVE(category, val)
 * @brief Positive-value guard: logs error and returns if val <= 0
 */
#define SPARK_VALIDATE_POSITIVE(category, val)                                                                         \
    do                                                                                                                 \
    {                                                                                                                  \
        if ((val) <= 0)                                                                                                \
        {                                                                                                              \
            SPARK_LOG_ERROR(category, "POSITIVE VALIDATION FAILED: %s = %g, must be > 0", #val,                        \
                            static_cast<double>(val));                                                                 \
            return;                                                                                                    \
        }                                                                                                              \
    } while (0)

// ============================================================================
// Offensive Programming — Hard checks (log + abort, never returns)
// ============================================================================

/**
 * @def SPARK_REQUIRE(category, expr)
 * @brief Precondition check: logs fatal error and aborts if expr is false
 *
 * Use for invariants that indicate a programming error if violated.
 * Active in all builds (debug and release).
 */
#define SPARK_REQUIRE(category, expr)                                                                                  \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(expr))                                                                                                   \
        {                                                                                                              \
            Spark::Validation::LogValidationFailure(Spark::LogLevel::Fatal, category, "PRECONDITION", #expr, __FILE__, \
                                                    __LINE__, __FUNCTION__);                                           \
            ASSERT_ALWAYS(expr);                                                                                       \
        }                                                                                                              \
    } while (0)

/**
 * @def SPARK_REQUIRE_MSG(category, expr, msg)
 * @brief Precondition check with a diagnostic message
 */
#define SPARK_REQUIRE_MSG(category, expr, msg)                                                                         \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(expr))                                                                                                   \
        {                                                                                                              \
            Spark::Validation::LogValidationFailure(Spark::LogLevel::Fatal, category, "PRECONDITION", #expr, __FILE__, \
                                                    __LINE__, __FUNCTION__, msg);                                      \
            ASSERT_ALWAYS_MSG(expr, "%s", msg);                                                                        \
        }                                                                                                              \
    } while (0)

/**
 * @def SPARK_REQUIRE_NOT_NULL(category, ptr)
 * @brief Non-null precondition: logs fatal error and aborts if ptr is null
 */
#define SPARK_REQUIRE_NOT_NULL(category, ptr)                                                                          \
    SPARK_REQUIRE_MSG(category, (ptr) != nullptr, "Pointer '" #ptr "' must not be null")

/**
 * @def SPARK_ENSURE(category, expr)
 * @brief Postcondition check: logs fatal error and aborts if expr is false
 */
#define SPARK_ENSURE(category, expr)                                                                                   \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(expr))                                                                                                   \
        {                                                                                                              \
            Spark::Validation::LogValidationFailure(Spark::LogLevel::Fatal, category, "POSTCONDITION", #expr,          \
                                                    __FILE__, __LINE__, __FUNCTION__);                                 \
            ASSERT_ALWAYS(expr);                                                                                       \
        }                                                                                                              \
    } while (0)

/**
 * @def SPARK_ENSURE_MSG(category, expr, msg)
 * @brief Postcondition check with a diagnostic message
 */
#define SPARK_ENSURE_MSG(category, expr, msg)                                                                          \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(expr))                                                                                                   \
        {                                                                                                              \
            Spark::Validation::LogValidationFailure(Spark::LogLevel::Fatal, category, "POSTCONDITION", #expr,          \
                                                    __FILE__, __LINE__, __FUNCTION__, msg);                            \
            ASSERT_ALWAYS_MSG(expr, "%s", msg);                                                                        \
        }                                                                                                              \
    } while (0)

/**
 * @def SPARK_UNREACHABLE(category, msg)
 * @brief Marks code paths that should never be reached — aborts if executed
 */
#define SPARK_UNREACHABLE(category, msg)                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        Spark::Validation::LogValidationFailure(Spark::LogLevel::Fatal, category, "UNREACHABLE",                       \
                                                "Reached unreachable code", __FILE__, __LINE__, __FUNCTION__, msg);    \
        ASSERT_ALWAYS_MSG(false, "UNREACHABLE: %s", msg);                                                              \
    } while (0)

// ============================================================================
// Diagnostic Logging — Function/scope tracing
// ============================================================================

/**
 * @def SPARK_TRACE_ENTER(category)
 * @brief Log function entry at Trace level (compiled out in Release)
 */
#ifndef NDEBUG
#define SPARK_TRACE_ENTER(category) SPARK_LOG_TRACE(category, ">> ENTER: %s", __FUNCTION__)
#else
#define SPARK_TRACE_ENTER(category) ((void)0)
#endif

/**
 * @def SPARK_TRACE_EXIT(category)
 * @brief Log function exit at Trace level (compiled out in Release)
 */
#ifndef NDEBUG
#define SPARK_TRACE_EXIT(category) SPARK_LOG_TRACE(category, "<< EXIT:  %s", __FUNCTION__)
#else
#define SPARK_TRACE_EXIT(category) ((void)0)
#endif

/**
 * @def SPARK_TRACE_SCOPE(category, name)
 * @brief RAII scope tracer: logs entry and exit of a named scope
 */
#ifndef NDEBUG
#define SPARK_TRACE_SCOPE(category, name)                                                                              \
    Spark::Validation::ScopeTracer _sparkScopeTracer_##__LINE__(category, name, __FILE__, __LINE__)
#else
#define SPARK_TRACE_SCOPE(category, name) ((void)0)
#endif

/**
 * @def SPARK_TRACE_VALUE(category, name, val)
 * @brief Log a named value at Trace level for diagnostics
 */
#ifndef NDEBUG
#define SPARK_TRACE_VALUE(category, name, val) SPARK_LOG_TRACE(category, "  %s = %s", name, std::to_string(val).c_str())
#else
#define SPARK_TRACE_VALUE(category, name, val) ((void)0)
#endif

/**
 * @def SPARK_DIAGNOSTIC_CONTEXT(name)
 * @brief Scoped breadcrumb for crash reports (delegates to SparkError ScopedContext)
 */
#define SPARK_DIAGNOSTIC_CONTEXT(name) SPARK_SCOPED_CONTEXT(name)

// ============================================================================
// Contract Logging — Structured validation audit trail
// ============================================================================

/**
 * @def SPARK_CONTRACT_LOG_PRE(category, expr)
 * @brief Log a precondition check result (pass or fail) at appropriate level
 */
#define SPARK_CONTRACT_LOG_PRE(category, expr)                                                                         \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(expr))                                                                                                   \
        {                                                                                                              \
            Spark::Validation::LogValidationFailure(Spark::LogLevel::Warn, category, "PRE", #expr, __FILE__, __LINE__, \
                                                    __FUNCTION__);                                                     \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            Spark::Validation::LogValidationSuccess(category, "PRE", #expr, __FILE__, __LINE__, __FUNCTION__);         \
        }                                                                                                              \
    } while (0)

/**
 * @def SPARK_CONTRACT_LOG_POST(category, expr)
 * @brief Log a postcondition check result (pass or fail) at appropriate level
 */
#define SPARK_CONTRACT_LOG_POST(category, expr)                                                                        \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(expr))                                                                                                   \
        {                                                                                                              \
            Spark::Validation::LogValidationFailure(Spark::LogLevel::Warn, category, "POST", #expr, __FILE__,          \
                                                    __LINE__, __FUNCTION__);                                           \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            Spark::Validation::LogValidationSuccess(category, "POST", #expr, __FILE__, __LINE__, __FUNCTION__);        \
        }                                                                                                              \
    } while (0)

/**
 * @def SPARK_CONTRACT_LOG_INVARIANT(category, expr)
 * @brief Log an invariant check result (pass or fail) at appropriate level
 */
#define SPARK_CONTRACT_LOG_INVARIANT(category, expr)                                                                   \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(expr))                                                                                                   \
        {                                                                                                              \
            Spark::Validation::LogValidationFailure(Spark::LogLevel::Warn, category, "INVARIANT", #expr, __FILE__,     \
                                                    __LINE__, __FUNCTION__);                                           \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            Spark::Validation::LogValidationSuccess(category, "INVARIANT", #expr, __FILE__, __LINE__, __FUNCTION__);   \
        }                                                                                                              \
    } while (0)

// ============================================================================
// Warn-only validation — Log but never return or abort
// ============================================================================

/**
 * @def SPARK_WARN_IF(category, cond, msg)
 * @brief Log a warning if condition is true (does not return or abort)
 */
#define SPARK_WARN_IF(category, cond, msg)                                                                             \
    do                                                                                                                 \
    {                                                                                                                  \
        if (cond)                                                                                                      \
        {                                                                                                              \
            SPARK_LOG_WARN(category, "%s", msg);                                                                       \
        }                                                                                                              \
    } while (0)

/**
 * @def SPARK_WARN_IF_NULL(category, ptr)
 * @brief Log a warning if pointer is null (does not return or abort)
 */
#define SPARK_WARN_IF_NULL(category, ptr)                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        if ((ptr) == nullptr)                                                                                          \
        {                                                                                                              \
            SPARK_LOG_WARN(category, "Pointer '%s' is null (non-fatal)", #ptr);                                        \
        }                                                                                                              \
    } while (0)
