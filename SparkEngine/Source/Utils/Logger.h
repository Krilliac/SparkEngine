/**
 * @file Logger.h
 * @brief Unified logging system for Spark Engine
 * @author Spark Engine Team
 * @date 2026
 *
 * Single entry point for all engine logging. Dispatches to multiple sinks
 * (console, file, stderr, editor, callback). Supports:
 * - Structured severity levels (Trace, Debug, Info, Warn, Error, Fatal)
 * - Log categories/channels (Graphics, Physics, AI, Audio, Network, etc.)
 * - File rotation when log exceeds size limit
 * - Async logging via a dedicated writer thread
 * - Runtime filtering by category and severity
 * - Formatted output with timestamp, thread ID, category, severity, file:line
 *
 * @code
 *   auto& logger = Spark::Logger::Get();
 *   logger.Initialize();
 *   logger.SetGlobalLevel(Spark::LogLevel::Debug);
 *   SPARK_LOG_INFO(Spark::LogCategory::Core, "Engine started version {}", 1);
 *   logger.Shutdown();
 * @endcode
 */

#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <memory>
#include <functional>
#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <queue>

namespace Spark
{

    // ============================================================================
    // Log Level
    // ============================================================================

    enum class LogLevel : uint8_t
    {
        Trace = 0,
        Debug = 1,
        Info = 2,
        Warn = 3,
        Error = 4,
        Fatal = 5,
        Off = 6
    };

    /**
     * @brief Convert LogLevel to string representation
     */
    inline const char* LogLevelToString(LogLevel level)
    {
        switch (level)
        {
        case LogLevel::Trace:
            return "TRACE";
        case LogLevel::Debug:
            return "DEBUG";
        case LogLevel::Info:
            return "INFO";
        case LogLevel::Warn:
            return "WARN";
        case LogLevel::Error:
            return "ERROR";
        case LogLevel::Fatal:
            return "FATAL";
        default:
            return "?????";
        }
    }

    // ============================================================================
    // Log Category
    // ============================================================================

    enum class LogCategory : uint8_t
    {
        Core = 0,
        Graphics,
        Physics,
        Audio,
        AI,
        Animation,
        ECS,
        Network,
        Input,
        Scripting,
        Scene,
        Save,
        Cinematic,
        Procedural,
        Editor,
        Game,
        Count
    };

    /**
     * @brief Convert LogCategory to string representation
     */
    inline const char* LogCategoryToString(LogCategory cat)
    {
        switch (cat)
        {
        case LogCategory::Core:
            return "Core";
        case LogCategory::Graphics:
            return "Graphics";
        case LogCategory::Physics:
            return "Physics";
        case LogCategory::Audio:
            return "Audio";
        case LogCategory::AI:
            return "AI";
        case LogCategory::Animation:
            return "Animation";
        case LogCategory::ECS:
            return "ECS";
        case LogCategory::Network:
            return "Network";
        case LogCategory::Input:
            return "Input";
        case LogCategory::Scripting:
            return "Scripting";
        case LogCategory::Scene:
            return "Scene";
        case LogCategory::Save:
            return "Save";
        case LogCategory::Cinematic:
            return "Cinematic";
        case LogCategory::Procedural:
            return "Procedural";
        case LogCategory::Editor:
            return "Editor";
        case LogCategory::Game:
            return "Game";
        default:
            return "Unknown";
        }
    }

    /**
     * @brief Convert a string category name to LogCategory enum
     *
     * Allows SPARK_LOG macros to accept string literals like "Game" or "Audio"
     * in addition to LogCategory enum values, for backward compatibility.
     */
    inline LogCategory StringToLogCategory(const char* str)
    {
        if (!str)
            return LogCategory::Core;
        // Simple comparison chain (case-sensitive)
        if (str[0] == 'C' && str[1] == 'o')
            return LogCategory::Core;
        if (str[0] == 'G' && str[1] == 'r')
            return LogCategory::Graphics;
        if (str[0] == 'P' && str[1] == 'h')
            return LogCategory::Physics;
        if (str[0] == 'A' && str[1] == 'u')
            return LogCategory::Audio;
        if (str[0] == 'A' && str[1] == 'I')
            return LogCategory::AI;
        if (str[0] == 'A' && str[1] == 'n')
            return LogCategory::Animation;
        if (str[0] == 'E' && str[1] == 'C')
            return LogCategory::ECS;
        if (str[0] == 'N' && str[1] == 'e')
            return LogCategory::Network;
        if (str[0] == 'I' && str[1] == 'n')
            return LogCategory::Input;
        if (str[0] == 'S' && str[1] == 'c')
            return LogCategory::Scripting;
        if (str[0] == 'S' && str[1] == 'a')
            return LogCategory::Save;
        if (str[0] == 'C' && str[1] == 'i')
            return LogCategory::Cinematic;
        if (str[0] == 'P' && str[1] == 'r')
            return LogCategory::Procedural;
        if (str[0] == 'E' && str[1] == 'd')
            return LogCategory::Editor;
        if (str[0] == 'G' && str[1] == 'a')
            return LogCategory::Game;
        if (str[0] == 'S' && str[1] == 'e')
            return LogCategory::Scene;
        return LogCategory::Core;
    }

    // ============================================================================
    // Log Message
    // ============================================================================

    /**
     * @brief A single log message with all associated metadata
     */
    struct LogMessage
    {
        LogLevel level = LogLevel::Info;
        LogCategory category = LogCategory::Core;
        std::string message;
        std::string file;
        int line = 0;
        std::string function;
        std::chrono::system_clock::time_point timestamp;
        std::thread::id threadId;
    };

    // ============================================================================
    // Log Sink Interface
    // ============================================================================

    /**
     * @brief Interface for log output destinations
     */
    class ILogSink
    {
      public:
        virtual ~ILogSink() = default;

        /**
         * @brief Write a formatted log message to this sink
         */
        virtual void Write(const LogMessage& msg) = 0;

        /**
         * @brief Flush any buffered output
         */
        virtual void Flush() = 0;
    };

    // ============================================================================
    // Built-in Sinks
    // ============================================================================

    /**
     * @brief Writes log messages to stderr (and OutputDebugStringA on Windows)
     */
    class StderrSink : public ILogSink
    {
      public:
        void Write(const LogMessage& msg) override;
        void Flush() override;
    };

    /**
     * @brief Writes log messages to a file with automatic rotation
     */
    class FileSink : public ILogSink
    {
      public:
        struct Config
        {
            std::string directory = "Logs/";
            std::string prefix = "SparkEngine";
            size_t maxFileSizeBytes = 50 * 1024 * 1024; ///< 50 MB
            int maxBackupFiles = 5;
        };

        FileSink();
        explicit FileSink(const Config& config);
        ~FileSink();

        void Write(const LogMessage& msg) override;
        void Flush() override;

        std::string GetCurrentFilePath() const { return m_currentFilePath; }
        size_t GetCurrentFileSize() const { return m_currentFileSize; }

      private:
        bool OpenFile();
        void RotateFiles();
        std::string FormatLogEntry(const LogMessage& msg) const;

        Config m_config;
        std::ofstream m_file;
        std::string m_currentFilePath;
        size_t m_currentFileSize = 0;
        int m_fileIndex = 0;
        bool m_initialized = false;
    };

    /**
     * @brief Calls a user-provided callback for each log message
     */
    class CallbackSink : public ILogSink
    {
      public:
        using Callback = std::function<void(const LogMessage&)>;

        explicit CallbackSink(Callback cb) : m_callback(std::move(cb)) {}

        void Write(const LogMessage& msg) override
        {
            if (m_callback)
            {
                m_callback(msg);
            }
        }

        void Flush() override {}

      private:
        Callback m_callback;
    };

    // ============================================================================
    // Logger
    // ============================================================================

    /**
     * @brief Unified logging system with async writing and runtime filtering
     *
     * Thread-safe singleton. Log messages are queued and written from a
     * dedicated background thread to avoid blocking the caller.
     */
    class Logger
    {
      public:
        static Logger& Get()
        {
            static Logger instance;
            return instance;
        }

        /**
         * @brief Initialize the logger and start the async writer thread
         * @param enableAsync If true, messages are queued and written from a background thread
         */
        void Initialize(bool enableAsync = true);

        /**
         * @brief Shutdown the logger: flush all sinks and stop the writer thread
         */
        void Shutdown();

        // ---- Sink Management ----

        /**
         * @brief Add a log sink. The logger takes ownership.
         */
        void AddSink(std::unique_ptr<ILogSink> sink);

        /**
         * @brief Remove all sinks
         */
        void ClearSinks();

        // ---- Filtering ----

        /**
         * @brief Set the global minimum log level
         */
        void SetGlobalLevel(LogLevel level) { m_globalLevel.store(level, std::memory_order_relaxed); }

        /**
         * @brief Get the current global minimum log level
         */
        LogLevel GetGlobalLevel() const { return m_globalLevel.load(std::memory_order_relaxed); }

        /**
         * @brief Set minimum level for a specific category
         */
        void SetCategoryLevel(LogCategory cat, LogLevel level);

        /**
         * @brief Get minimum level for a specific category
         */
        LogLevel GetCategoryLevel(LogCategory cat) const;

        /**
         * @brief Check if a message at the given level and category would be logged
         */
        bool ShouldLog(LogLevel level, LogCategory category) const;

        /** @brief ShouldLog overload accepting string category for backward compatibility */
        bool ShouldLog(LogLevel level, const char* category) const
        {
            return ShouldLog(level, StringToLogCategory(category));
        }

        // ---- Logging ----

        /**
         * @brief Log a message with full metadata
         */
        void Log(LogLevel level, LogCategory category, const char* file, int line, const char* func,
                 const std::string& message);

        /** @brief Log overload accepting string category for backward compatibility */
        void Log(LogLevel level, const char* category, const char* file, int line, const char* func,
                 const std::string& message)
        {
            Log(level, StringToLogCategory(category), file, line, func, message);
        }

        /**
         * @brief Flush all sinks immediately (blocks until complete)
         */
        void FlushAll();

        bool IsInitialized() const { return m_initialized.load(std::memory_order_acquire); }

      private:
        Logger() = default;
        ~Logger() { Shutdown(); }
        Logger(const Logger&) = delete;
        Logger& operator=(const Logger&) = delete;

        /**
         * @brief Background thread function that drains the message queue
         */
        void AsyncWriterThread();

        /**
         * @brief Dispatch a message to all sinks (called from writer thread or inline)
         */
        void DispatchToSinks(const LogMessage& msg);

        /**
         * @brief Format a log message into a human-readable string
         */
        static std::string FormatLogEntry(const LogMessage& msg);

        // State
        std::atomic<bool> m_initialized{false};
        std::atomic<bool> m_asyncEnabled{false};
        std::atomic<LogLevel> m_globalLevel{LogLevel::Trace};

        // Per-category filtering
        std::array<std::atomic<LogLevel>, static_cast<size_t>(LogCategory::Count)> m_categoryLevels;

        // Sinks
        std::vector<std::unique_ptr<ILogSink>> m_sinks;
        std::mutex m_sinkMutex;

        // Async queue
        std::queue<LogMessage> m_messageQueue;
        std::mutex m_queueMutex;
        std::condition_variable m_queueCV;
        std::atomic<bool> m_stopThread{false};
        std::thread m_writerThread;
    };

    // ============================================================================
    // Helper: format message with timestamp, thread ID, category, severity, etc.
    // ============================================================================

    /**
     * @brief Format a LogMessage for human-readable output
     * Format: [HH:MM:SS.mmm] [TID:XXXX] [LEVEL] [Category] message  (file:line)
     */
    std::string FormatLogMessage(const LogMessage& msg);

} // namespace Spark
