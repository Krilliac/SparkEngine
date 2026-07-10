/**
 * @file FileLogger.h
 * @brief File-based logging system integrated with SparkError
 * @author Spark Engine Team
 * @date 2025
 *
 * Extends the engine's logging infrastructure with persistent file output.
 * Captures all SPARK_LOG_* messages to timestamped log files with automatic
 * rotation, severity filtering, and structured output formats.
 *
 * Features:
 * - Automatic log file creation with timestamp in filename
 * - Log file rotation by size or session
 * - Configurable severity filter (only write messages >= threshold)
 * - CSV and plain-text output formats
 * - Automatic flush on Error/Fatal messages
 * - Session header with system information
 * - Thread-safe file writing
 *
 * Typical usage:
 * @code
 *   auto& logger = Spark::FileLogger::GetInstance();
 *   logger.Initialize("Logs/", Spark::FileLogLevel::Debug);
 *   // ... engine runs, all SPARK_LOG_* calls are captured ...
 *   logger.Shutdown();
 * @endcode
 *
 * @see SparkError.h, EditorLogger.h
 */

#pragma once

#include "../Core/Platform.h"
#include <atomic>
#include <string>
#include <fstream>
#include <mutex>
#include <cstdio>
#include <ctime>
#include <chrono>

namespace Spark
{

    /**
 * @brief Log severity levels for file filtering
 */
    enum class FileLogLevel
    {
        Trace = 0,
        Debug = 1,
        Info = 2,
        Warn = 3,
        Error = 4,
        Fatal = 5
    };

    /**
 * @brief Log file output format
 */
    enum class LogFileFormat
    {
        PlainText, ///< Human-readable plain text
        CSV        ///< Machine-parseable CSV
    };

    /**
 * @brief Configuration for the file logger
 */
    struct FileLoggerConfig
    {
        std::string outputDirectory = "Logs/";              ///< Directory for log files
        std::string filenamePrefix = "SparkEngine";         ///< Prefix for log filenames
        FileLogLevel minimumLevel = FileLogLevel::Debug;    ///< Minimum severity to write
        LogFileFormat format = LogFileFormat::PlainText;    ///< Output format
        size_t maxFileSizeBytes = size_t{50} * 1024 * 1024; ///< Max file size before rotation (50 MB)
        bool autoFlushOnError = true;                       ///< Flush immediately on Error/Fatal
        bool includeTimestamp = true;                       ///< Include timestamp in each line
        bool includeThreadId = true;                        ///< Include thread ID
        bool includeSourceLocation = true;                  ///< Include file:line info
        bool writeSessionHeader = true;                     ///< Write system info at session start
    };

    /**
 * @brief Singleton file logger for persistent log output
 */
    class FileLogger
    {
      public:
        static FileLogger& GetInstance()
        {
            static FileLogger instance;
            return instance;
        }

        /**
     * @brief Initialize the file logger and open a new log file
     * @param directory Output directory (created if needed)
     * @param minLevel Minimum severity to log
     * @return true if the log file was opened successfully
     */
        bool Initialize(const std::string& directory = "Logs/", FileLogLevel minLevel = FileLogLevel::Debug)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_initialized)
                return true;

            m_config.outputDirectory = directory;
            m_config.minimumLevel = minLevel;

            return OpenLogFile();
        }

        /**
     * @brief Initialize with full configuration
     */
        bool Initialize(const FileLoggerConfig& config)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_initialized)
                return true;
            m_config = config;
            return OpenLogFile();
        }

        /**
     * @brief Flush and close the log file
     */
        void Shutdown()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_file.is_open())
            {
                WriteFooter();
                m_file.flush();
                m_file.close();
            }
            m_initialized = false;
        }

        /**
     * @brief Write a log message to the file
     * @param level   Severity level
     * @param category  Log category
     * @param message   Message text
     * @param file      Source file (optional)
     * @param line      Source line (optional)
     * @param func      Function name (optional)
     */
        void Write(FileLogLevel level, const char* category, const char* message, const char* file = nullptr,
                   int line = 0, const char* func = nullptr)
        {
            if (!m_initialized)
                return;
            if (level < m_config.minimumLevel)
                return;

            std::lock_guard<std::mutex> lock(m_mutex);

            // Check for file rotation
            if (m_currentFileSize >= m_config.maxFileSizeBytes)
                RotateLogFile();

            if (m_config.format == LogFileFormat::CSV)
                WriteCSVLine(level, category, message, file, line, func);
            else
                WritePlainTextLine(level, category, message, file, line, func);

            // Auto-flush on high severity
            if (m_config.autoFlushOnError && level >= FileLogLevel::Error)
                m_file.flush();
        }

        /**
     * @brief Force flush the log file
     */
        void Flush()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_file.is_open())
                m_file.flush();
        }

        /**
     * @brief Get the current log file path
     */
        std::string GetCurrentFilePath() const { return m_currentFilePath; }

        /**
     * @brief Get the number of messages written
     */
        uint64_t GetMessageCount() const { return m_messageCount; }

        /**
     * @brief Get current file size in bytes
     */
        size_t GetCurrentFileSize() const { return m_currentFileSize; }

        /**
     * @brief Update the minimum log level at runtime
     */
        void SetMinimumLevel(FileLogLevel level) { m_config.minimumLevel = level; }
        FileLogLevel GetMinimumLevel() const { return m_config.minimumLevel; }

        bool IsInitialized() const { return m_initialized; }

      private:
        FileLogger() = default;
        ~FileLogger() { Shutdown(); }
        FileLogger(const FileLogger&) = delete;
        FileLogger& operator=(const FileLogger&) = delete;

        bool OpenLogFile()
        {
            // Generate timestamped filename
            auto now = std::chrono::system_clock::now();
            std::time_t t = std::chrono::system_clock::to_time_t(now);
            char timeBuf[64];
            std::strftime(timeBuf, sizeof(timeBuf), "%Y%m%d_%H%M%S", std::localtime(&t));

            std::string ext = (m_config.format == LogFileFormat::CSV) ? ".csv" : ".log";
            m_currentFilePath = m_config.outputDirectory + m_config.filenamePrefix + "_" + timeBuf + ext;

            m_file.open(m_currentFilePath, std::ios::out | std::ios::trunc);
            if (!m_file.is_open())
            {
                fprintf(stderr, "[FileLogger] Failed to open log file: %s\n", m_currentFilePath.c_str());
                return false;
            }

            m_initialized = true;
            m_currentFileSize = 0;
            m_messageCount = 0;
            m_fileIndex = 0;

            if (m_config.writeSessionHeader)
                WriteHeader();

            if (m_config.format == LogFileFormat::CSV)
                WriteCSVHeader();

            return true;
        }

        void RotateLogFile()
        {
            WriteFooter();
            m_file.close();

            m_fileIndex++;
            auto now = std::chrono::system_clock::now();
            std::time_t t = std::chrono::system_clock::to_time_t(now);
            char timeBuf[64];
            std::strftime(timeBuf, sizeof(timeBuf), "%Y%m%d_%H%M%S", std::localtime(&t));

            std::string ext = (m_config.format == LogFileFormat::CSV) ? ".csv" : ".log";
            m_currentFilePath = m_config.outputDirectory + m_config.filenamePrefix + "_" + timeBuf + "_" +
                                std::to_string(m_fileIndex) + ext;

            m_file.open(m_currentFilePath, std::ios::out | std::ios::trunc);
            m_currentFileSize = 0;

            if (m_config.writeSessionHeader)
                WriteHeader();
            if (m_config.format == LogFileFormat::CSV)
                WriteCSVHeader();
        }

        void WriteHeader()
        {
            std::string header = "===============================================\n"
                                 " Spark Engine Log Session\n"
                                 "===============================================\n";

            auto now = std::chrono::system_clock::now();
            std::time_t t = std::chrono::system_clock::to_time_t(now);
            char timeBuf[64];
            std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));

            header += " Started: " + std::string(timeBuf) + "\n";
            header += " Min Level: " + LevelToString(m_config.minimumLevel) + "\n";
            header += "===============================================\n\n";

            m_file << header;
            m_currentFileSize += header.size();
        }

        void WriteFooter()
        {
            char buf[256];
            snprintf(buf, sizeof(buf), "\n--- End of log (%llu messages written) ---\n",
                     static_cast<unsigned long long>(m_messageCount));
            m_file << buf;
        }

        void WriteCSVHeader()
        {
            std::string header = "Timestamp,Level,Category,Message,File,Line,Function\n";
            m_file << header;
            m_currentFileSize += header.size();
        }

        void WritePlainTextLine(FileLogLevel level, const char* category, const char* message, const char* file,
                                int line, const char* func)
        {
            char buf[2048];
            int offset = 0;

            if (m_config.includeTimestamp)
            {
                auto now = std::chrono::system_clock::now();
                std::time_t t = std::chrono::system_clock::to_time_t(now);
                char timeBuf[32];
                std::strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", std::localtime(&t));

                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;

                offset += snprintf(buf + offset, sizeof(buf) - offset, "[%s.%03d] ", timeBuf, static_cast<int>(ms));
            }

            offset += snprintf(buf + offset, sizeof(buf) - offset, "[%s][%-8s] %s", LevelToString(level).c_str(),
                               category ? category : "Engine", message ? message : "");

            if (m_config.includeSourceLocation && file && file[0])
            {
                const char* shortFile = file;
                const char* slash = strrchr(file, '\\');
                if (!slash)
                    slash = strrchr(file, '/');
                if (slash)
                    shortFile = slash + 1;

                offset += snprintf(buf + offset, sizeof(buf) - offset, "  (%s:%d", shortFile, line);
                if (func && func[0])
                    offset += snprintf(buf + offset, sizeof(buf) - offset, " in %s", func);
                offset += snprintf(buf + offset, sizeof(buf) - offset, ")");
            }

            offset += snprintf(buf + offset, sizeof(buf) - offset, "\n");

            m_file << buf;
            m_currentFileSize += offset;
            m_messageCount++;
        }

        void WriteCSVLine(FileLogLevel level, const char* category, const char* message, const char* file, int line,
                          const char* func)
        {
            auto now = std::chrono::system_clock::now();
            std::time_t t = std::chrono::system_clock::to_time_t(now);
            char timeBuf[32];
            std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));

            // Escape message for CSV (double-quote any quotes)
            std::string escapedMsg = message ? message : "";
            std::string escaped;
            escaped.reserve(escapedMsg.size() + 2);
            escaped += '"';
            for (char c : escapedMsg)
            {
                if (c == '"')
                    escaped += '"';
                escaped += c;
            }
            escaped += '"';

            char buf[2048];
            int len = snprintf(buf, sizeof(buf), "%s,%s,%s,%s,%s,%d,%s\n", timeBuf, LevelToString(level).c_str(),
                               category ? category : "", escaped.c_str(), file ? file : "", line, func ? func : "");

            m_file << buf;
            m_currentFileSize += len;
            m_messageCount++;
        }

        static std::string LevelToString(FileLogLevel level)
        {
            switch (level)
            {
            case FileLogLevel::Trace:
                return "TRACE";
            case FileLogLevel::Debug:
                return "DEBUG";
            case FileLogLevel::Info:
                return "INFO";
            case FileLogLevel::Warn:
                return "WARN";
            case FileLogLevel::Error:
                return "ERROR";
            case FileLogLevel::Fatal:
                return "FATAL";
            default:
                return "?????";
            }
        }

        FileLoggerConfig m_config;
        std::ofstream m_file;
        std::string m_currentFilePath;
        size_t m_currentFileSize = 0;
        uint64_t m_messageCount = 0;
        int m_fileIndex = 0;
        std::atomic<bool> m_initialized{false};
        std::mutex m_mutex;
    };

} // namespace Spark

// ============================================================================
// Integration macros -- route through FileLogger when initialized
// ============================================================================

/**
 * @brief Write to file log with severity and category
 */
#define SPARK_FILE_LOG(level, category, fmt, ...)                                                                      \
    do                                                                                                                 \
    {                                                                                                                  \
        if (Spark::FileLogger::GetInstance().IsInitialized())                                                          \
        {                                                                                                              \
            char _msg[1024];                                                                                           \
            snprintf(_msg, sizeof(_msg), fmt __VA_OPT__(, ) __VA_ARGS__);                                              \
            Spark::FileLogger::GetInstance().Write(level, category, _msg, __FILE__, __LINE__, __FUNCTION__);           \
        }                                                                                                              \
    } while (0)

#define SPARK_FILE_LOG_TRACE(cat, fmt, ...)                                                                            \
    SPARK_FILE_LOG(Spark::FileLogLevel::Trace, cat, fmt __VA_OPT__(, ) __VA_ARGS__)
#define SPARK_FILE_LOG_DEBUG(cat, fmt, ...)                                                                            \
    SPARK_FILE_LOG(Spark::FileLogLevel::Debug, cat, fmt __VA_OPT__(, ) __VA_ARGS__)
#define SPARK_FILE_LOG_INFO(cat, fmt, ...)                                                                             \
    SPARK_FILE_LOG(Spark::FileLogLevel::Info, cat, fmt __VA_OPT__(, ) __VA_ARGS__)
#define SPARK_FILE_LOG_WARN(cat, fmt, ...)                                                                             \
    SPARK_FILE_LOG(Spark::FileLogLevel::Warn, cat, fmt __VA_OPT__(, ) __VA_ARGS__)
#define SPARK_FILE_LOG_ERROR(cat, fmt, ...)                                                                            \
    SPARK_FILE_LOG(Spark::FileLogLevel::Error, cat, fmt __VA_OPT__(, ) __VA_ARGS__)
#define SPARK_FILE_LOG_FATAL(cat, fmt, ...)                                                                            \
    SPARK_FILE_LOG(Spark::FileLogLevel::Fatal, cat, fmt __VA_OPT__(, ) __VA_ARGS__)
