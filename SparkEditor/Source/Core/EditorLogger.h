/**
 * @file EditorLogger.h
 * @brief Advanced logging system for the Spark Engine Editor
 * @author Spark Engine Team
 * @date 2025
 * 
 * This file implements a comprehensive logging system with multiple output targets,
 * severity levels, filtering, and real-time display capabilities integrated with
 * the editor's UI system.
 */

#pragma once

#include <vector>
#include <string>
#include <memory>
#include <mutex>
#include <fstream>
#include <chrono>
#include <unordered_map>
#include <functional>

namespace SparkEditor {

/**
 * @brief Log severity levels
 */
enum class LogLevel {
    TRACE = 0,    ///< Detailed trace information
    DEBUG = 1,    ///< Debug information
    INFO = 2,     ///< General information
    WARNING = 3,  ///< Warning messages
    ERROR_ = 4,   ///< Error messages (renamed to avoid conflicts)
    CRITICAL = 5  ///< Critical system errors
};

/**
 * @brief Log categories for classification
 */
enum class LogCategory {
    GENERAL = 0,
    ASSET,
    RENDERING,
    ENGINE,
    UI,
    SCRIPTING,
    PHYSICS,
    AUDIO,
    NETWORKING,
    PROFILING,
    CRASH
};

/**
 * @brief Convert LogCategory to string
 */
inline const char* LogCategoryToString(LogCategory category) {
    switch (category) {
        case LogCategory::GENERAL:    return "General";
        case LogCategory::ASSET:      return "Asset";
        case LogCategory::RENDERING:  return "Rendering";
        case LogCategory::ENGINE:     return "Engine";
        case LogCategory::UI:         return "UI";
        case LogCategory::SCRIPTING:  return "Scripting";
        case LogCategory::PHYSICS:    return "Physics";
        case LogCategory::AUDIO:      return "Audio";
        case LogCategory::NETWORKING: return "Networking";
        case LogCategory::PROFILING:  return "Profiling";
        case LogCategory::CRASH:      return "Crash";
        default:                      return "Unknown";
    }
}

/**
 * @brief Log entry structure
 */
struct LogEntry {
    std::chrono::system_clock::time_point timestamp;
    LogLevel level;
    std::string category;
    std::string message;
    std::string file;
    int line = 0;
    std::string function;
    uint64_t frameNumber = 0;
    
    LogEntry() = default;
    LogEntry(LogLevel lvl, const std::string& cat, const std::string& msg)
        : timestamp(std::chrono::system_clock::now()), level(lvl), category(cat), message(msg) {}
};

/**
 * @brief Log output target interface
 */
class LogTarget {
public:
    virtual ~LogTarget() = default;
    virtual void WriteLog(const LogEntry& entry) = 0;
    virtual void Flush() {}
};

/**
 * @brief Console output target
 */
class ConsoleLogTarget : public LogTarget {
public:
    void WriteLog(const LogEntry& entry) override;
};

/**
 * @brief File output target
 */
class FileLogTarget : public LogTarget {
public:
    explicit FileLogTarget(const std::string& filename);
    ~FileLogTarget() override;
    
    void WriteLog(const LogEntry& entry) override;
    void Flush() override;
    
private:
    std::ofstream m_file;
    std::mutex m_mutex;
};

/**
 * @brief Memory buffer target for UI display
 */
class MemoryLogTarget : public LogTarget {
public:
    explicit MemoryLogTarget(size_t maxEntries = 10000);
    
    void WriteLog(const LogEntry& entry) override;
    
    const std::vector<LogEntry>& GetEntries() const { return m_entries; }
    void Clear();
    void SetMaxEntries(size_t maxEntries);
    
private:
    std::vector<LogEntry> m_entries;
    size_t m_maxEntries;
    mutable std::mutex m_mutex;
};

/**
 * @brief Log statistics
 */
struct LogStatistics {
    struct {
        std::unordered_map<LogLevel, size_t> entriesByLevel;
        std::unordered_map<std::string, size_t> entriesByCategory;
        size_t totalEntries = 0;
        std::chrono::system_clock::time_point lastEntry;
    } stats;
};

/**
 * @brief Main logger class
 */
class EditorLogger {
public:
    EditorLogger();
    ~EditorLogger();

    /**
     * @brief Get singleton instance
     * @return Reference to the global EditorLogger instance
     */
    static EditorLogger& GetInstance();

    bool Initialize(const std::string& logDirectory = "Logs", size_t maxMemoryEntries = 10000);
    void Shutdown();

    void Log(LogLevel level, const std::string& category, const std::string& message,
             const std::string& file = "", int line = 0, const std::string& function = "");

    template<typename... Args>
    void LogFormat(LogLevel level, const std::string& category, const char* format, Args&&... args);

    void AddTarget(std::unique_ptr<LogTarget> target);
    void RemoveTarget(LogTarget* target);

    void SetLogLevel(LogLevel level) { m_logLevel = level; }
    LogLevel GetLogLevel() const { return m_logLevel; }

    void SetCategoryEnabled(const std::string& category, bool enabled);
    bool IsCategoryEnabled(const std::string& category) const;

    const std::vector<LogEntry>& GetMemoryLogs() const;
    LogStatistics GetStatistics() const;

    void SetFrameNumber(uint64_t frameNumber) { m_frameNumber = frameNumber; }

    bool ExportLogs(const std::string& filename, 
                   std::function<bool(const LogEntry&)> filter = nullptr) const;

    void Clear();

private:
    std::string LogLevelToString(LogLevel level) const;
    std::string FormatLogEntry(const LogEntry& entry) const;
    void UpdateStatistics(const LogEntry& entry);

private:
    std::vector<std::unique_ptr<LogTarget>> m_targets;
    std::unordered_map<std::string, bool> m_categoryFilters;
    LogLevel m_logLevel = LogLevel::TRACE;
    bool m_initialized = false;
    uint64_t m_frameNumber = 0;
    mutable LogStatistics m_statistics;
    mutable std::mutex m_statsMutex;
    
    std::chrono::system_clock::time_point m_lastStatsUpdate;
    
    mutable std::mutex m_mutex;
};

// Template implementation
template<typename... Args>
void EditorLogger::LogFormat(LogLevel level, const std::string& category, const char* format, Args&&... args) {
    static constexpr size_t BUFFER_SIZE = 1024;
    char buffer[BUFFER_SIZE];
    
    int result = snprintf(buffer, BUFFER_SIZE, format, std::forward<Args>(args)...);
    if (result > 0) {
        Log(level, category, std::string(buffer));
    }
}

} // namespace SparkEditor

// Convenience macros
#define SE_LOG_TRACE(category, message) \
    SparkEditor::EditorLogger::GetInstance().Log(SparkEditor::LogLevel::TRACE, category, message, __FILE__, __LINE__, __FUNCTION__)

#define SE_LOG_DEBUG(category, message) \
    SparkEditor::EditorLogger::GetInstance().Log(SparkEditor::LogLevel::DEBUG, category, message, __FILE__, __LINE__, __FUNCTION__)

#define SE_LOG_INFO(category, message) \
    SparkEditor::EditorLogger::GetInstance().Log(SparkEditor::LogLevel::INFO, category, message, __FILE__, __LINE__, __FUNCTION__)

#define SE_LOG_WARNING(category, message) \
    SparkEditor::EditorLogger::GetInstance().Log(SparkEditor::LogLevel::WARNING, category, message, __FILE__, __LINE__, __FUNCTION__)

#define SE_LOG_ERROR(category, message) \
    SparkEditor::EditorLogger::GetInstance().Log(SparkEditor::LogLevel::ERROR_, category, message, __FILE__, __LINE__, __FUNCTION__)

#define SE_LOG_CRITICAL(category, message) \
    SparkEditor::EditorLogger::GetInstance().Log(SparkEditor::LogLevel::CRITICAL, category, message, __FILE__, __LINE__, __FUNCTION__)

// Format versions
#define SE_LOG_TRACE_F(category, format, ...) \
    SparkEditor::EditorLogger::GetInstance().LogFormat(SparkEditor::LogLevel::TRACE, category, format, __VA_ARGS__)

#define SE_LOG_DEBUG_F(category, format, ...) \
    SparkEditor::EditorLogger::GetInstance().LogFormat(SparkEditor::LogLevel::DEBUG, category, format, __VA_ARGS__)

#define SE_LOG_INFO_F(category, format, ...) \
    SparkEditor::EditorLogger::GetInstance().LogFormat(SparkEditor::LogLevel::INFO, category, format, __VA_ARGS__)

#define SE_LOG_WARNING_F(category, format, ...) \
    SparkEditor::EditorLogger::GetInstance().LogFormat(SparkEditor::LogLevel::WARNING, category, format, __VA_ARGS__)

#define SE_LOG_ERROR_F(category, format, ...) \
    SparkEditor::EditorLogger::GetInstance().LogFormat(SparkEditor::LogLevel::ERROR_, category, format, __VA_ARGS__)

#define SE_LOG_CRITICAL_F(category, format, ...) \
    SparkEditor::EditorLogger::GetInstance().LogFormat(SparkEditor::LogLevel::CRITICAL, category, format, __VA_ARGS__)