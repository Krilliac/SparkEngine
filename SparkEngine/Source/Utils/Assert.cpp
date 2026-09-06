/**
 * @file Assert.cpp
 * @brief Implementation of assertion failure handlers with optional suppression
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides the implementation of Assert::Fail() and Assert::FailHResult(),
 * moved out of the header to support shared mutable state for the error
 * suppression toggle.
 *
 * ## Error Suppression
 *
 * When suppression is enabled, assertion failures log full diagnostics but
 * return instead of aborting. This allows the engine to continue running
 * past invariant violations — useful during content iteration and testing.
 *
 * **WARNING**: Suppression is a developer-only tool. Continuing past a
 * violated invariant can cause cascading failures, undefined behavior, or
 * data corruption. It is OFF by default and must be explicitly enabled.
 *
 * @see Assert.h, CrashHandler.h
 */

#include "Assert.h"
#include "SparkConsole.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <format>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

// =============================================================================
// Suppression state
// =============================================================================

static std::atomic<bool> g_suppressFatalAsserts{false};
static std::atomic<uint32_t> g_suppressedAssertCount{0};
static std::atomic<bool> g_debugBreakOnSuppressed{true};

// Rate-limiting: avoid log spam when the same assertion fires every frame
static std::mutex g_rateLimitMutex;
static std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> g_lastLogTime;

static constexpr auto RATE_LIMIT_INTERVAL = std::chrono::seconds(5);

/// Simple hash combine for file:line site identification
static uint64_t HashSite(const char* file, int line)
{
    uint64_t h = 14695981039346656037ULL; // FNV-1a offset basis
    if (file)
    {
        for (const char* p = file; *p; ++p)
        {
            h ^= static_cast<uint64_t>(*p);
            h *= 1099511628211ULL; // FNV-1a prime
        }
    }
    h ^= static_cast<uint64_t>(line);
    h *= 1099511628211ULL;
    return h;
}

/// Returns true if this site should emit full diagnostics (rate-limited)
static bool ShouldLogSite(const char* file, int line)
{
    uint64_t site = HashSite(file, line);
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_rateLimitMutex);
    auto it = g_lastLogTime.find(site);
    if (it == g_lastLogTime.end() || (now - it->second) > RATE_LIMIT_INTERVAL)
    {
        g_lastLogTime[site] = now;
        return true;
    }
    return false;
}

// =============================================================================
// Assert::Fail
// =============================================================================

namespace Assert
{
    void Fail(const char* expr, const char* file, int line, const char* fmt, ...)
    {
        bool shouldLog = ShouldLogSite(file, line);

        // Declared out here so the fatal path below can put the same text into
        // the crash report it writes immediately before abort().
        char fullMsg[4096] = {};

        if (shouldLog)
        {
            // Timestamp. std::localtime returns a pointer to a shared static std::tm,
            // so it is a data race when the async logger thread formats timestamps
            // concurrently. Fill a stack std::tm via the reentrant variant instead
            // (mirrors CrashHandler.cpp).
            std::time_t t = std::time(nullptr);
            std::tm tmBuf{};
#ifdef _WIN32
            localtime_s(&tmBuf, &t);
#else
            localtime_r(&t, &tmBuf);
#endif
            char timeBuf[64];
            std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &tmBuf);

            // Format user message if provided
            char userMsg[512] = {};
            if (fmt)
            {
                va_list args;
                va_start(args, fmt);
                vsnprintf(userMsg, sizeof(userMsg), fmt, args);
                va_end(args);
            }

            // Capture stack trace
            auto stackTrace = Spark::StackTrace::Capture(2);
            std::string traceStr = stackTrace.ToString("    ");

            // Build the full assertion text
            std::snprintf(fullMsg, sizeof(fullMsg),
                          "===== ASSERTION FAILED =====\n"
                          "Time       : %s\n"
                          "Expression : %s\n"
                          "Location   : %s(%d)\n"
                          "Message    : %s\n"
                          "Thread ID  : 0x%08X\n"
                          "Stack Trace:\n%s",
                          timeBuf, expr, file, line, userMsg[0] ? userMsg : "(none)",
#ifdef _WIN32
                          static_cast<unsigned>(GetCurrentThreadId()),
#else
                          0u,
#endif
                          traceStr.c_str());

            std::fprintf(stderr, "%s", fullMsg);
            std::fflush(stderr);

            TriggerCrashHandler(fullMsg);
        }

        // Suppression check
        if (g_suppressFatalAsserts.load(std::memory_order_relaxed))
        {
            uint32_t count = g_suppressedAssertCount.fetch_add(1, std::memory_order_relaxed) + 1;

            if (shouldLog)
            {
                std::fprintf(stderr,
                             "===== ASSERTION SUPPRESSED (#%u) =====\n"
                             "  The engine is continuing with a VIOLATED INVARIANT.\n"
                             "  Subsequent behavior may be undefined.\n",
                             count);
                std::fflush(stderr);
            }

            if (g_debugBreakOnSuppressed.load(std::memory_order_relaxed))
            {
                DEBUG_BREAK();
            }
            return;
        }

        // The process dies here. abort() raises no SEH exception and Windows
        // installs no SIGABRT handler, so the unhandled-exception filter never
        // runs, and TriggerCrashHandler() is gated off in production — without
        // an unconditional report a fatal assertion leaves nothing on disk.
        // When the developer toggle IS on, the TriggerCrashHandler() call above
        // already reported: crash reporting is one-per-process, so this second
        // call returns without writing a duplicate.
        TriggerCrashReport(fullMsg[0] ? fullMsg : expr);

        DEBUG_BREAK();
        std::abort();
    }

    void FailHResult(const char* expr, const char* file, int line, long hr, const char* fmt, ...)
    {
        bool shouldLog = ShouldLogSite(file, line);

        // See Fail(): the fatal path needs this text after the logging block.
        char fullMsg[4096] = {};

        if (shouldLog)
        {
            char userMsg[512] = {};
            if (fmt)
            {
                va_list args;
                va_start(args, fmt);
                vsnprintf(userMsg, sizeof(userMsg), fmt, args);
                va_end(args);
            }

            auto stackTrace = Spark::StackTrace::Capture(2);
            std::string traceStr = stackTrace.ToString("    ");

            std::snprintf(fullMsg, sizeof(fullMsg),
                          "===== HRESULT FAILED =====\n"
                          "Expression : %s returned 0x%08lX\n"
                          "Location   : %s(%d)\n"
                          "Message    : %s\n"
                          "Thread ID  : 0x%08X\n"
                          "Stack Trace:\n%s",
                          expr, hr, file, line, userMsg[0] ? userMsg : "(none)",
#ifdef _WIN32
                          static_cast<unsigned>(GetCurrentThreadId()),
#else
                          0u,
#endif
                          traceStr.c_str());

            std::fprintf(stderr, "%s", fullMsg);
            std::fflush(stderr);

            TriggerCrashHandler(fullMsg);
        }

        if (g_suppressFatalAsserts.load(std::memory_order_relaxed))
        {
            uint32_t count = g_suppressedAssertCount.fetch_add(1, std::memory_order_relaxed) + 1;

            if (shouldLog)
            {
                std::fprintf(stderr,
                             "===== HRESULT ASSERTION SUPPRESSED (#%u) =====\n"
                             "  The engine is continuing with a VIOLATED INVARIANT.\n",
                             count);
                std::fflush(stderr);
            }

            if (g_debugBreakOnSuppressed.load(std::memory_order_relaxed))
            {
                DEBUG_BREAK();
            }
            return;
        }

        // The process dies here. abort() raises no SEH exception and Windows
        // installs no SIGABRT handler, so the unhandled-exception filter never
        // runs, and TriggerCrashHandler() is gated off in production — without
        // an unconditional report a fatal assertion leaves nothing on disk.
        // When the developer toggle IS on, the TriggerCrashHandler() call above
        // already reported: crash reporting is one-per-process, so this second
        // call returns without writing a duplicate.
        TriggerCrashReport(fullMsg[0] ? fullMsg : expr);

        DEBUG_BREAK();
        std::abort();
    }

    // =========================================================================
    // Suppression API
    // =========================================================================

    void SetSuppressionEnabled(bool enabled)
    {
        bool wasEnabled = g_suppressFatalAsserts.exchange(enabled, std::memory_order_relaxed);
        if (enabled && !wasEnabled)
        {
            std::fprintf(stderr, "\n"
                                 "========================================================\n"
                                 "  WARNING: Fatal assertion suppression is ENABLED.\n"
                                 "  The engine will continue past invariant violations.\n"
                                 "  This mode is for development/content iteration ONLY.\n"
                                 "========================================================\n"
                                 "\n");
            std::fflush(stderr);
        }
    }

    bool IsSuppressionEnabled()
    {
        return g_suppressFatalAsserts.load(std::memory_order_relaxed);
    }

    uint32_t GetSuppressedCount()
    {
        return g_suppressedAssertCount.load(std::memory_order_relaxed);
    }

    void ResetSuppressedCount()
    {
        g_suppressedAssertCount.store(0, std::memory_order_relaxed);
    }

    void SetDebugBreakOnSuppressed(bool enabled)
    {
        g_debugBreakOnSuppressed.store(enabled, std::memory_order_relaxed);
    }

    void RegisterConsoleCommands()
    {
        auto& console = Spark::SimpleConsole::GetInstance();

        console.RegisterCommand(
            "assert.status",
            [](const std::vector<std::string>&) -> std::string
            {
                bool suppressed = IsSuppressionEnabled();
                uint32_t count = GetSuppressedCount();
                bool breakOnSuppressed = g_debugBreakOnSuppressed.load(std::memory_order_relaxed);
                return std::format("Assertion suppression: {}\n"
                                   "Suppressed count: {}\n"
                                   "Debug break on suppressed: {}",
                                   suppressed ? "ON" : "OFF", count, breakOnSuppressed ? "ON" : "OFF");
            },
            "Show assertion suppression status", "Debug", "assert.status");

        console.RegisterCommand(
            "assert.suppress",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return std::format("Assertion suppression is currently {}", IsSuppressionEnabled() ? "ON" : "OFF");

                const auto& arg = args[0];
                if (arg == "on" || arg == "true" || arg == "1")
                {
                    SetSuppressionEnabled(true);
                    return "Assertion suppression ENABLED — the engine will continue past invariant violations";
                }
                else if (arg == "off" || arg == "false" || arg == "0")
                {
                    SetSuppressionEnabled(false);
                    return "Assertion suppression DISABLED — assertions will abort as normal";
                }
                return "Usage: assert.suppress <on|off>";
            },
            "Toggle fatal assertion suppression (on/off)", "Debug", "assert.suppress <on|off>");

        console.RegisterCommand(
            "assert.reset_count",
            [](const std::vector<std::string>&) -> std::string
            {
                uint32_t prev = GetSuppressedCount();
                ResetSuppressedCount();
                return std::format("Suppressed assertion count reset (was {})", prev);
            },
            "Reset the suppressed assertion counter", "Debug", "assert.reset_count");

        console.RegisterCommand(
            "assert.break_on_suppressed",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return std::format("Debug break on suppressed assertions: {}",
                                       g_debugBreakOnSuppressed.load(std::memory_order_relaxed) ? "ON" : "OFF");

                const auto& arg = args[0];
                if (arg == "on" || arg == "true" || arg == "1")
                {
                    SetDebugBreakOnSuppressed(true);
                    return "Debug break on suppressed assertions ENABLED";
                }
                else if (arg == "off" || arg == "false" || arg == "0")
                {
                    SetDebugBreakOnSuppressed(false);
                    return "Debug break on suppressed assertions DISABLED";
                }
                return "Usage: assert.break_on_suppressed <on|off>";
            },
            "Toggle debugger break on suppressed assertions (on/off)", "Debug", "assert.break_on_suppressed <on|off>");
    }

} // namespace Assert
