#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <deque>
#include <mutex>

namespace Spark
{

    /**
 * @brief Simplified console system for SparkEditor integration
 * This provides engine-style logging that automatically sends to external console
 */
    class SimpleConsole
    {
      public:
        using CommandHandler = std::function<std::string(const std::vector<std::string>&)>;

        struct LogEntry
        {
            std::string message;
            std::string type;
            std::string timestamp;
        };

        struct CommandInfo
        {
            CommandHandler handler;
            std::string description;
        };

      private:
        bool m_initialized = false;
        std::deque<LogEntry> m_logHistory;
        std::unordered_map<std::string, CommandInfo> m_commands;
        mutable std::mutex m_logMutex;

      public:
        static SimpleConsole& GetInstance();

        bool Initialize();
        void Shutdown();
        void Update();

        // Main logging methods (these automatically send to external console)
        void Log(const std::string& message, const std::string& type = "INFO");
        void LogInfo(const std::string& message);
        void LogWarning(const std::string& message);
        void LogError(const std::string& message);
        void LogSuccess(const std::string& message);
        void LogCritical(const std::string& message);
        void LogTrace(const std::string& message);

        // Command system
        void RegisterCommand(const std::string& name, CommandHandler handler, const std::string& description = "");
        bool ExecuteCommand(const std::string& commandLine);

        // Utility methods
        std::vector<LogEntry> GetLogHistory() const;

      private:
        SimpleConsole() = default;
        ~SimpleConsole() = default;
        SimpleConsole(const SimpleConsole&) = delete;
        SimpleConsole& operator=(const SimpleConsole&) = delete;

        std::string GetTimestamp();
        std::vector<std::string> ParseCommand(const std::string& commandLine);
        void RegisterDefaultCommands();

        // External console communication
        void SendToExternalConsole(const std::string& message, const std::string& type);
    };

} // namespace Spark