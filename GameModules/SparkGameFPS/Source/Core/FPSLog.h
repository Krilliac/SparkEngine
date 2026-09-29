/**
 * @file FPSLog.h
 * @brief SparkGameFPS logging and in-game console output through the public SDK.
 *
 * SparkGameModule::OnLoad binds its IEngineContext with Spark::ModuleLog::Bind,
 * and every FPS source logs through these macros instead of the engine-private
 * Utils/LogMacros.h:
 * - FPS_LOG_INFO/WARN/ERROR/DEBUG(format, args...) write the host engine log
 *   through ILogger (std::format syntax). FPS_LOG_DEBUG compiles out in NDEBUG
 *   builds, as the engine's debug logging does.
 * - FPS_LOG_EVERY_SECONDS(Level, intervalSeconds, format, args...) logs at most once
 *   per @p intervalSeconds from one call site; Level is Info, Warn, Error or Debug.
 * - FPS_CONSOLE(message, type) prints a line to the host's in-game console
 *   through IConsole::Print; type is its severity tag ("INFO", "SUCCESS",
 *   "WARNING", "ERROR", "OPERATION").
 * - FPS_CONSOLE_RATE_LIMITED(maxPerWindow, windowSeconds, message, type) prints
 *   at most @p maxPerWindow lines per @p windowSeconds from one call site; the
 *   message is only built when the line is printed.
 *
 * The names are distinct from the engine macros on purpose: a source that still
 * reaches Utils/LogMacros.h through another engine header cannot silently bind
 * to the engine's DLL-local console and logger.
 *
 * Nothing is logged before OnLoad binds the context or after OnUnload unbinds it.
 * Thread affinity: as Spark::ModuleLog (any thread). The rate limiters keep
 * per-call-site statics and are meant for the game thread.
 */

#pragma once

#include <Spark/ModuleLog.h>

#include <chrono>

#define FPS_LOG_INFO(...) ::Spark::ModuleLog::Info(__VA_ARGS__)
#define FPS_LOG_WARN(...) ::Spark::ModuleLog::Warn(__VA_ARGS__)
#define FPS_LOG_ERROR(...) ::Spark::ModuleLog::Error(__VA_ARGS__)
#ifdef NDEBUG
#define FPS_LOG_DEBUG(...) ((void)0)
#else
#define FPS_LOG_DEBUG(...) ::Spark::ModuleLog::Debug(__VA_ARGS__)
#endif

#define FPS_LOG_EVERY_SECONDS(level, intervalSeconds, ...)                                                             \
    do                                                                                                                 \
    {                                                                                                                  \
        static std::chrono::steady_clock::time_point fpsLogLastTime{};                                                 \
        const auto fpsLogNow = std::chrono::steady_clock::now();                                                       \
        if (fpsLogLastTime == std::chrono::steady_clock::time_point{} ||                                               \
            fpsLogNow - fpsLogLastTime >= std::chrono::seconds(intervalSeconds))                                       \
        {                                                                                                              \
            fpsLogLastTime = fpsLogNow;                                                                                \
            ::Spark::ModuleLog::level(__VA_ARGS__);                                                                    \
        }                                                                                                              \
    } while (0)

#define FPS_CONSOLE(message, type) ::Spark::ModuleLog::Print((message), (type))

#define FPS_CONSOLE_RATE_LIMITED(maxPerWindow, windowSeconds, message, type)                                           \
    do                                                                                                                 \
    {                                                                                                                  \
        static auto fpsConsoleWindowStart = std::chrono::steady_clock::now();                                          \
        static int fpsConsoleWindowCount = 0;                                                                          \
        const auto fpsConsoleNow = std::chrono::steady_clock::now();                                                   \
        const bool fpsConsoleWindowElapsed =                                                                           \
            fpsConsoleNow - fpsConsoleWindowStart >= std::chrono::seconds(windowSeconds);                              \
        if (fpsConsoleWindowElapsed || fpsConsoleWindowCount < (maxPerWindow))                                         \
        {                                                                                                              \
            FPS_CONSOLE(message, type);                                                                                \
            if (fpsConsoleWindowElapsed)                                                                               \
            {                                                                                                          \
                fpsConsoleWindowStart = fpsConsoleNow;                                                                 \
                fpsConsoleWindowCount = 0;                                                                             \
            }                                                                                                          \
            else                                                                                                       \
            {                                                                                                          \
                ++fpsConsoleWindowCount;                                                                               \
            }                                                                                                          \
        }                                                                                                              \
    } while (0)
