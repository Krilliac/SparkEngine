/**
 * @file SparkEngineCameraInternal.h
 * @brief Shared console-logging helpers for the SparkEngineCamera implementation files
 *
 * File-local logging macros and the wide-to-narrow string converter used by both
 * SparkEngineCamera.cpp and SparkEngineCameraConsoleOps.cpp.
 * Split from SparkEngineCamera.cpp for maintainability. Include only from those
 * implementation files — this is not a public header.
 */

#pragma once
#include "../Core/Platform.h"
#include "../Utils/SparkConsole.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif // SPARK_PLATFORM_WINDOWS
#include <chrono>
#include <string>

namespace
{
    /// Convert a wide string to a narrow (UTF-8) std::string for the console sink.
    /// `std::string(w.begin(), w.end())` truncates any code unit above 0xFF to a
    /// single byte (mojibake for non-ASCII); use the platform converter instead.
    std::string WideToNarrow(const std::wstring& w)
    {
#ifdef SPARK_PLATFORM_WINDOWS
        if (w.empty())
            return {};
        int len = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        if (len <= 0)
            return {};
        std::string out(static_cast<size_t>(len), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), len, nullptr, nullptr);
        return out;
#else
        return std::string(w.begin(), w.end());
#endif
    }
} // namespace

// **FIXED: Corrected logging macros to handle string conversion properly**
#undef LOG_TO_CONSOLE_RATE_LIMITED
#undef LOG_TO_CONSOLE
#undef LOG_TO_CONSOLE_IMMEDIATE
#define LOG_TO_CONSOLE_RATE_LIMITED(msg, type)                                                                         \
    do                                                                                                                 \
    {                                                                                                                  \
        static auto lastLogTime = std::chrono::steady_clock::now();                                                    \
        static int logCounter = 0;                                                                                     \
        auto now = std::chrono::steady_clock::now();                                                                   \
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastLogTime).count();                    \
        if (elapsed >= 10 || logCounter < 1)                                                                           \
        {                                                                                                              \
            std::wstring wmsg = msg;                                                                                   \
            std::wstring wtype = type;                                                                                 \
            std::string smsg = WideToNarrow(wmsg);                                                                     \
            std::string stype = WideToNarrow(wtype);                                                                   \
            Spark::SimpleConsole::GetInstance().Log(smsg, stype);                                                      \
            if (elapsed >= 10)                                                                                         \
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

#define LOG_TO_CONSOLE(msg, type) LOG_TO_CONSOLE_RATE_LIMITED(msg, type)
#define LOG_TO_CONSOLE_IMMEDIATE(msg, type)                                                                            \
    do                                                                                                                 \
    {                                                                                                                  \
        std::wstring wmsg = msg;                                                                                       \
        std::wstring wtype = type;                                                                                     \
        std::string smsg = WideToNarrow(wmsg);                                                                         \
        std::string stype = WideToNarrow(wtype);                                                                       \
        Spark::SimpleConsole::GetInstance().Log(smsg, stype);                                                          \
    } while (0)
