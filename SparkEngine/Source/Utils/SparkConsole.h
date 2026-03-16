/**
 * @file SparkConsole.h
 * @brief Thread-safe in-engine log sink and command registry
 *
 * SimpleConsole is the engine-side log sink and command registry. It does NOT
 * provide an interactive console UI — the external SparkConsole.exe process
 * handles interactive I/O via ConsoleProcessManager pipes.
 *
 * @see ConsoleVariable.h for the CVar system
 * @see ConsoleSink.h for Logger integration
 * @see ConsoleProcessManager.h for the SparkConsole.exe subprocess
 */

#pragma once
#include "../Core/Platform.h"
#include "Hash.h"
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <functional>
#include <deque>
#include <mutex>
#include <map>
#include <chrono>
#include <cstdint>
#include <atomic>

namespace Spark
{

    // ========================================================================
    // Log Severity
    // ========================================================================

    enum class ConsoleSeverity : uint8_t
    {
        Trace,
        Debug,
        Info,
        Success,
        Warning,
        Error,
        Critical
    };

    /** @brief Convert severity enum to display string */
    const char* ConsoleSeverityToString(ConsoleSeverity severity);

    /** @brief Convert display string to severity enum (returns Info on unknown) */
    ConsoleSeverity StringToConsoleSeverity(const std::string& str);

    // ========================================================================
    // SimpleConsole — log sink and command registry
    // ========================================================================

    // Thread safety: Thread-safe. All public methods are mutex-protected.
    // Safe to call Log() from any thread.
    class SimpleConsole
    {
      public:
        using CommandHandler = std::function<std::string(const std::vector<std::string>&)>;

        struct LogEntry
        {
            std::string message;
            std::string type;
            std::string timestamp;
            ConsoleSeverity severity;
            uint64_t sequenceNumber;
        };

        struct CommandInfo
        {
            CommandHandler handler;
            std::string description;
            std::string category;
            std::string usage;
            uint64_t nameHash = 0;
        };

        struct ConsoleStats
        {
            uint64_t totalLogsWritten = 0;
            uint64_t totalCommandsExecuted = 0;
            uint64_t totalCommandsFailed = 0;
            uint32_t registeredCommands = 0;
            uint32_t registeredAliases = 0;
        };

        static SimpleConsole& GetInstance();

        bool Initialize();
        void Shutdown();
        void Update();

        // Logging
        void Log(const std::string& message, const std::string& type = "INFO");
        void Log(ConsoleSeverity severity, const std::string& message);
        void LogInfo(const std::string& message);
        void LogWarning(const std::string& message);
        void LogError(const std::string& message);
        void LogSuccess(const std::string& message);
        void LogCritical(const std::string& message);
        void LogTrace(const std::string& message);
        void LogDebug(const std::string& message);

        // Commands
        void RegisterCommand(const std::string& name, CommandHandler handler, const std::string& description = "",
                             const std::string& category = "General", const std::string& usage = "");
        bool UnregisterCommand(const std::string& name);
        bool HasCommand(const std::string& name) const;
        bool ExecuteCommand(const std::string& commandLine);

        // Visibility (no-op stubs — external console handles its own window)
        void Show();
        void Hide();
        void Toggle();
        bool IsVisible() const { return m_visible; }
        void Clear();

        // Accessors
        std::vector<LogEntry> GetLogHistory() const;
        std::vector<std::string> GetCommandHistory() const;
        ConsoleStats GetStats() const;

        // Aliases
        void SetAlias(const std::string& alias, const std::string& command);
        void RemoveAlias(const std::string& alias);

      private:
        SimpleConsole() = default;
        ~SimpleConsole() = default;
        SimpleConsole(const SimpleConsole&) = delete;
        SimpleConsole& operator=(const SimpleConsole&) = delete;

        std::vector<std::string> ParseCommand(const std::string& commandLine);
        std::string GetTimestamp() const;
        std::string FindClosestCommand(const std::string& input) const;
        std::string ResolveAliases(const std::string& commandLine);

        std::unordered_map<std::string, CommandInfo> m_commands;
        std::deque<LogEntry> m_logHistory;
        std::deque<std::string> m_commandHistory;

        std::atomic<bool> m_initialized{false};
        bool m_visible = true;

        mutable std::mutex m_logMutex;
        mutable std::mutex m_commandMutex;
        mutable std::mutex m_historyMutex;

        std::unordered_map<std::string, std::string> m_aliases;

        std::atomic<uint64_t> m_logSequence{0};
        ConsoleStats m_stats{};

        static constexpr size_t MaxLogHistory = 2000;
        static constexpr size_t MaxCommandHistory = 500;
    };

} // namespace Spark
