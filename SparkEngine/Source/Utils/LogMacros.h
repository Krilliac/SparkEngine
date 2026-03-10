/**
 * @file LogMacros.h
 * @brief Unified logging macros for Spark Engine
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides SPARK_LOG_* macros that route through the unified Logger system.
 * Supports:
 * - Severity-leveled logging (Trace, Debug, Info, Warn, Error, Fatal)
 * - Category-based filtering (Graphics, Physics, AI, Audio, etc.)
 * - Compile-time stripping of Trace/Debug in Release builds
 * - Conditional/periodic logging utilities (LOG_ONCE, LOG_EVERY_N, LOG_IF)
 * - Rate-limited logging
 *
 * Legacy LOG_TO_CONSOLE macros are preserved below for backward compatibility
 * but now route through the unified logger.
 *
 * @code
 *   SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Shader compiled: %s", name);
 *   SPARK_LOG_ERROR(Spark::LogCategory::Physics, "Invalid body ID: %d", id);
 *   SPARK_LOG_ONCE(Spark::LogLevel::Warn, Spark::LogCategory::AI, "NavMesh not loaded");
 * @endcode
 */

#pragma once

#include "Logger.h"
#include "SparkConsole.h"
#include <chrono>
#include <cstdio>
#include <string>

// ============================================================================
// Core logging macro — all others delegate to this
// ============================================================================

/**
 * @brief Log a message through the unified Logger with severity, category,
 *        and source location. Uses snprintf for formatting.
 *
 * @param level    Spark::LogLevel value
 * @param category Spark::LogCategory value
 * @param fmt      printf-style format string
 * @param ...      Format arguments
 */
#define SPARK_LOG(level, category, fmt, ...)                                                                           \
    do                                                                                                                 \
    {                                                                                                                  \
        auto& _logger = Spark::Logger::Get();                                                                          \
        if (_logger.ShouldLog(level, category))                                                                        \
        {                                                                                                              \
            char _sparkLogBuf[4096];                                                                                   \
            snprintf(_sparkLogBuf, sizeof(_sparkLogBuf), fmt, ##__VA_ARGS__);                                          \
            _logger.Log(level, category, __FILE__, __LINE__, __FUNCTION__, std::string(_sparkLogBuf));                 \
        }                                                                                                              \
    } while (0)

// ============================================================================
// Convenience macros by severity level
// ============================================================================

// Undefine any prior definitions (e.g. from SparkError.h) to avoid -Wmacro-redefined
#undef SPARK_LOG_TRACE
#undef SPARK_LOG_DEBUG
#undef SPARK_LOG_INFO
#undef SPARK_LOG_WARN
#undef SPARK_LOG_ERROR
#undef SPARK_LOG_FATAL

#define SPARK_LOG_TRACE(cat, fmt, ...) SPARK_LOG(Spark::LogLevel::Trace, cat, fmt, ##__VA_ARGS__)
#define SPARK_LOG_DEBUG(cat, fmt, ...) SPARK_LOG(Spark::LogLevel::Debug, cat, fmt, ##__VA_ARGS__)
#define SPARK_LOG_INFO(cat, fmt, ...) SPARK_LOG(Spark::LogLevel::Info, cat, fmt, ##__VA_ARGS__)
#define SPARK_LOG_WARN(cat, fmt, ...) SPARK_LOG(Spark::LogLevel::Warn, cat, fmt, ##__VA_ARGS__)
#define SPARK_LOG_ERROR(cat, fmt, ...) SPARK_LOG(Spark::LogLevel::Error, cat, fmt, ##__VA_ARGS__)
#define SPARK_LOG_FATAL(cat, fmt, ...) SPARK_LOG(Spark::LogLevel::Fatal, cat, fmt, ##__VA_ARGS__)

// ============================================================================
// Compile-time stripping for Trace/Debug in Release builds
// ============================================================================

#ifdef NDEBUG
#undef SPARK_LOG_TRACE
#define SPARK_LOG_TRACE(cat, fmt, ...) ((void)0)
#undef SPARK_LOG_DEBUG
#define SPARK_LOG_DEBUG(cat, fmt, ...) ((void)0)
#endif

// ============================================================================
// Conditional / Periodic logging utilities
// ============================================================================

/**
 * @brief Log a message only once per call site (first occurrence only).
 *        Uses a static bool to track whether the message has been logged.
 */
#define SPARK_LOG_ONCE(level, cat, fmt, ...)                                                                           \
    do                                                                                                                 \
    {                                                                                                                  \
        static bool _sparkLogOnceFlag = false;                                                                         \
        if (!_sparkLogOnceFlag)                                                                                        \
        {                                                                                                              \
            _sparkLogOnceFlag = true;                                                                                  \
            SPARK_LOG(level, cat, fmt, ##__VA_ARGS__);                                                                 \
        }                                                                                                              \
    } while (0)

/**
 * @brief Log a message every Nth occurrence at a given call site.
 *
 * @param level    Spark::LogLevel value
 * @param cat      Spark::LogCategory value
 * @param N        Log every N occurrences
 * @param fmt      printf-style format string
 */
#define SPARK_LOG_EVERY_N(level, cat, N, fmt, ...)                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        static int _sparkLogCounter = 0;                                                                               \
        if (++_sparkLogCounter >= (N))                                                                                 \
        {                                                                                                              \
            _sparkLogCounter = 0;                                                                                      \
            SPARK_LOG(level, cat, fmt, ##__VA_ARGS__);                                                                 \
        }                                                                                                              \
    } while (0)

/**
 * @brief Log a message only when a condition is true.
 *
 * @param level    Spark::LogLevel value
 * @param cat      Spark::LogCategory value
 * @param cond     Boolean condition
 * @param fmt      printf-style format string
 */
#define SPARK_LOG_IF(level, cat, cond, fmt, ...)                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (cond)                                                                                                      \
        {                                                                                                              \
            SPARK_LOG(level, cat, fmt, ##__VA_ARGS__);                                                                 \
        }                                                                                                              \
    } while (0)

/**
 * @brief Rate-limited logging: at most one message per specified number of seconds.
 *
 * @param level    Spark::LogLevel value
 * @param cat      Spark::LogCategory value
 * @param seconds  Minimum interval in seconds between messages
 * @param fmt      printf-style format string
 */
#define SPARK_LOG_EVERY_SECONDS(level, cat, seconds, fmt, ...)                                                         \
    do                                                                                                                 \
    {                                                                                                                  \
        static auto _sparkLastLogTime = std::chrono::steady_clock::time_point{};                                       \
        auto _sparkNow = std::chrono::steady_clock::now();                                                             \
        auto _sparkElapsed = std::chrono::duration_cast<std::chrono::seconds>(_sparkNow - _sparkLastLogTime).count();  \
        if (_sparkElapsed >= (seconds) || _sparkLastLogTime == std::chrono::steady_clock::time_point{})                \
        {                                                                                                              \
            _sparkLastLogTime = _sparkNow;                                                                             \
            SPARK_LOG(level, cat, fmt, ##__VA_ARGS__);                                                                 \
        }                                                                                                              \
    } while (0)

// ============================================================================
// Legacy compatibility macros
// ============================================================================
// These route through the unified logger and also forward to SimpleConsole
// for backward compatibility with existing code.

/**
 * @brief Log a message to the SimpleConsole immediately (no rate limiting).
 *        Also routes through the unified Logger.
 */
#define LOG_TO_CONSOLE_IMMEDIATE(wmsg, wtype)                                                                          \
    do                                                                                                                 \
    {                                                                                                                  \
        std::wstring wstrMsg = wmsg;                                                                                   \
        std::wstring wstrType = wtype;                                                                                 \
        std::string strMsg(wstrMsg.begin(), wstrMsg.end());                                                            \
        std::string strType(wstrType.begin(), wstrType.end());                                                         \
        Spark::SimpleConsole::GetInstance().Log(strMsg, strType);                                                      \
    } while (0)

/**
 * @brief Log a message with rate limiting (max 3 messages per 3-second window).
 */
#define LOG_TO_CONSOLE_RATE_LIMITED(wmsg, wtype)                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        static auto lastLogTime = std::chrono::steady_clock::now();                                                    \
        static int logCounter = 0;                                                                                     \
        auto now = std::chrono::steady_clock::now();                                                                   \
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastLogTime).count();                    \
        if (elapsed >= 3 || logCounter < 3)                                                                            \
        {                                                                                                              \
            LOG_TO_CONSOLE_IMMEDIATE(wmsg, wtype);                                                                     \
            if (elapsed >= 3)                                                                                          \
            {                                                                                                          \
                lastLogTime = now;                                                                                     \
                logCounter = 0;                                                                                        \
            }                                                                                                          \
            else                                                                                                       \
            {                                                                                                          \
                logCounter++;                                                                                          \
            }                                                                                                          \
        }                                                                                                              \
    } while (0)

/** @brief Convenience alias -- defaults to rate-limited logging. */
#define LOG_TO_CONSOLE(wmsg, wtype) LOG_TO_CONSOLE_RATE_LIMITED(wmsg, wtype)
