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

namespace Spark::FPSLog
{
    /**
     * @brief Stateful fixed-window admission gate for console messages.
     *
     * The first admitted call starts a window; a call at or after the window
     * duration starts the next window and is admitted as its first message.
     * A non-positive quota admits nothing. The class is intentionally not
     * thread-safe: FPS console call sites are game-thread state.
     */
    class RateLimiter final
    {
      public:
        using Clock = std::chrono::steady_clock;
        using TimePoint = Clock::time_point;

        [[nodiscard]] bool Allow(int maxPerWindow, std::chrono::seconds window, TimePoint now)
        {
            if (maxPerWindow <= 0)
            {
                return false;
            }

            if (!m_initialized || now - m_windowStart >= window)
            {
                m_initialized = true;
                m_windowStart = now;
                m_count = 0;
            }

            if (m_count >= maxPerWindow)
            {
                return false;
            }

            ++m_count;
            return true;
        }

      private:
        TimePoint m_windowStart{};
        int m_count = 0;
        bool m_initialized = false;
    };
} // namespace Spark::FPSLog

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
        static ::Spark::FPSLog::RateLimiter fpsConsoleRateLimiter;                                                     \
        if (fpsConsoleRateLimiter.Allow((maxPerWindow), std::chrono::seconds(windowSeconds),                           \
                                        ::Spark::FPSLog::RateLimiter::Clock::now()))                                   \
        {                                                                                                              \
            FPS_CONSOLE(message, type);                                                                                \
        }                                                                                                              \
    } while (0)
