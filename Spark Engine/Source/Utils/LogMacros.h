/**
 * @file LogMacros.h
 * @brief Centralized console logging macros for Spark Engine
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides LOG_TO_CONSOLE_IMMEDIATE and LOG_TO_CONSOLE_RATE_LIMITED macros
 * that route wide-string messages to the SimpleConsole singleton. These were
 * previously defined independently (with inconsistent rate limits) in
 * Game.cpp, GraphicsEngine.cpp, and AudioEngine.cpp.
 *
 * Usage:
 * @code
 *   LOG_TO_CONSOLE_IMMEDIATE(L"Engine started", L"INFO");
 *   LOG_TO_CONSOLE_RATE_LIMITED(L"Frame dropped", L"WARNING");
 *   LOG_TO_CONSOLE(L"Update tick", L"DEBUG");  // alias for rate-limited
 * @endcode
 */

#pragma once

#include "SparkConsole.h"
#include <chrono>
#include <string>

/**
 * @brief Log a message to the SimpleConsole immediately (no rate limiting).
 *
 * Use for one-shot events like initialization, shutdown, and errors.
 *
 * @param wmsg  Wide-string message (e.g. L"Player spawned")
 * @param wtype Wide-string log type (e.g. L"INFO", L"ERROR", L"SUCCESS")
 */
#define LOG_TO_CONSOLE_IMMEDIATE(wmsg, wtype) \
    do { \
        std::wstring wstrMsg = wmsg; \
        std::wstring wstrType = wtype; \
        std::string strMsg(wstrMsg.begin(), wstrMsg.end()); \
        std::string strType(wstrType.begin(), wstrType.end()); \
        Spark::SimpleConsole::GetInstance().Log(strMsg, strType); \
    } while(0)

/**
 * @brief Log a message with rate limiting (max 3 messages per 3-second window).
 *
 * Use for per-frame or high-frequency events to avoid flooding the console.
 * Each call site maintains its own independent rate-limit counter.
 *
 * @param wmsg  Wide-string message
 * @param wtype Wide-string log type
 */
#define LOG_TO_CONSOLE_RATE_LIMITED(wmsg, wtype) \
    do { \
        static auto lastLogTime = std::chrono::steady_clock::now(); \
        static int logCounter = 0; \
        auto now = std::chrono::steady_clock::now(); \
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastLogTime).count(); \
        if (elapsed >= 3 || logCounter < 3) { \
            LOG_TO_CONSOLE_IMMEDIATE(wmsg, wtype); \
            if (elapsed >= 3) { \
                lastLogTime = now; \
                logCounter = 0; \
            } else { \
                logCounter++; \
            } \
        } \
    } while(0)

/** @brief Convenience alias — defaults to rate-limited logging. */
#define LOG_TO_CONSOLE(wmsg, wtype) LOG_TO_CONSOLE_RATE_LIMITED(wmsg, wtype)
