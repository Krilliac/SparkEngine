/**
 * @file EditorLogger.cpp
 * @brief Implementation of the editor logging system
 * @author Spark Engine Team
 * @date 2025
 */

#include "EditorLogger.h"
#include <iostream>
#include <iomanip>
#include <sstream>

namespace SparkEditor {

// ConsoleLogTarget implementation
void ConsoleLogTarget::WriteLog(const LogEntry& entry) {
    std::cout << "[" << static_cast<int>(entry.level) << "] " 
              << entry.category << ": " << entry.message << std::endl;
}

// FileLogTarget implementation
FileLogTarget::FileLogTarget(const std::string& filename) {
    m_file.open(filename, std::ios::app);
}

FileLogTarget::~FileLogTarget() {
    if (m_file.is_open()) {
        m_file.close();
    }
}

void FileLogTarget::WriteLog(const LogEntry& entry) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_file.is_open()) {
        auto time_t = std::chrono::system_clock::to_time_t(entry.timestamp);
        m_file << "[" << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S") << "] "
               << "[" << static_cast<int>(entry.level) << "] "
               << entry.category << ": " << entry.message << std::endl;
    }
}

void FileLogTarget::Flush() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_file.is_open()) {
        m_file.flush();
    }
}

// MemoryLogTarget implementation
MemoryLogTarget::MemoryLogTarget(size_t maxEntries) : m_maxEntries(maxEntries) {
    m_entries.reserve(maxEntries);
}

void MemoryLogTarget::WriteLog(const LogEntry& entry) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_entries.size() >= m_maxEntries) {
        m_entries.erase(m_entries.begin());
    }
    
    m_entries.push_back(entry);
}

void MemoryLogTarget::Clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.clear();
}

void MemoryLogTarget::SetMaxEntries(size_t maxEntries) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_maxEntries = maxEntries;
    
    while (m_entries.size() > m_maxEntries) {
        m_entries.erase(m_entries.begin());
    }
}

// EditorLogger singleton
EditorLogger& EditorLogger::GetInstance() {
    static EditorLogger instance;
    return instance;
}

// EditorLogger implementation
EditorLogger::EditorLogger() = default;

EditorLogger::~EditorLogger() {
    if (m_initialized) {
        Shutdown();
    }
}

bool EditorLogger::Initialize(const std::string& logDirectory, size_t maxMemoryEntries) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_initialized) {
        return true;
    }
    
    // Add console target
    AddTarget(std::make_unique<ConsoleLogTarget>());
    
    // Add memory target
    AddTarget(std::make_unique<MemoryLogTarget>(maxMemoryEntries));
    
    // Add file target
    std::string logFile = logDirectory + "/editor.log";
    AddTarget(std::make_unique<FileLogTarget>(logFile));
    
    m_initialized = true;
    
    Log(LogLevel::INFO, "Logger", "Editor logger initialized");
    return true;
}

void EditorLogger::Shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (!m_initialized) {
        return;
    }
    
    Log(LogLevel::INFO, "Logger", "Editor logger shutting down");
    
    // Flush all targets
    for (auto& target : m_targets) {
        target->Flush();
    }
    
    m_targets.clear();
    m_initialized = false;
}

void EditorLogger::Log(LogLevel level, const std::string& category, const std::string& message,
                      const std::string& file, int line, const std::string& function) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (!m_initialized || level < m_logLevel) {
        return;
    }
    
    // Check category filter
    auto catIt = m_categoryFilters.find(category);
    if (catIt != m_categoryFilters.end() && !catIt->second) {
        return;
    }
    
    // Create log entry
    LogEntry entry;
    entry.timestamp = std::chrono::system_clock::now();
    entry.level = level;
    entry.category = category;
    entry.message = message;
    entry.file = file;
    entry.line = line;
    entry.function = function;
    entry.frameNumber = m_frameNumber;
    
    // Update statistics
    UpdateStatistics(entry);
    
    // Write to all targets
    for (auto& target : m_targets) {
        target->WriteLog(entry);
    }
}

void EditorLogger::AddTarget(std::unique_ptr<LogTarget> target) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_targets.push_back(std::move(target));
}

void EditorLogger::RemoveTarget(LogTarget* target) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_targets.erase(
        std::remove_if(m_targets.begin(), m_targets.end(),
                      [target](const std::unique_ptr<LogTarget>& ptr) {
                          return ptr.get() == target;
                      }),
        m_targets.end());
}

void EditorLogger::SetCategoryEnabled(const std::string& category, bool enabled) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_categoryFilters[category] = enabled;
}

bool EditorLogger::IsCategoryEnabled(const std::string& category) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_categoryFilters.find(category);
    return it != m_categoryFilters.end() ? it->second : true; // Default to enabled
}

const std::vector<LogEntry>& EditorLogger::GetMemoryLogs() const {
    // Find memory target and return its entries
    for (const auto& target : m_targets) {
        if (auto* memTarget = dynamic_cast<const MemoryLogTarget*>(target.get())) {
            return memTarget->GetEntries();
        }
    }
    
    static std::vector<LogEntry> empty;
    return empty;
}

LogStatistics EditorLogger::GetStatistics() const {
    std::lock_guard<std::mutex> lock(m_statsMutex);
    return m_statistics;
}

bool EditorLogger::ExportLogs(const std::string& filename,
                             std::function<bool(const LogEntry&)> filter) const {
    std::ofstream file(filename);
    if (!file.is_open()) {
        return false;
    }

    // Snapshot log entries under lock to avoid data race with concurrent writers.
    // GetMemoryLogs() returns a reference to the internal vector which can be
    // modified by other threads calling Log() while we iterate.
    std::vector<LogEntry> logsCopy;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        logsCopy = GetMemoryLogs();
    }

    for (const auto& entry : logsCopy) {
        if (!filter || filter(entry)) {
            file << FormatLogEntry(entry) << std::endl;
        }
    }

    return true;
}

void EditorLogger::Clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    for (auto& target : m_targets) {
        if (auto* memTarget = dynamic_cast<MemoryLogTarget*>(target.get())) {
            memTarget->Clear();
        }
    }
    
    // Reset statistics
    std::lock_guard<std::mutex> statsLock(m_statsMutex);
    m_statistics = LogStatistics{};
}

std::string EditorLogger::LogLevelToString(LogLevel level) const {
    switch (level) {
        case LogLevel::TRACE: return "TRACE";
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO: return "INFO";
        case LogLevel::WARNING: return "WARNING";
        case LogLevel::ERROR_: return "ERROR";
        case LogLevel::CRITICAL: return "CRITICAL";
        default: return "UNKNOWN";
    }
}

std::string EditorLogger::FormatLogEntry(const LogEntry& entry) const {
    std::ostringstream oss;
    
    auto time_t = std::chrono::system_clock::to_time_t(entry.timestamp);
    oss << "[" << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S") << "] "
        << "[" << LogLevelToString(entry.level) << "] "
        << "[" << entry.category << "] "
        << entry.message;
    
    if (!entry.file.empty()) {
        oss << " (" << entry.file << ":" << entry.line << ")";
    }
    
    return oss.str();
}

void EditorLogger::UpdateStatistics(const LogEntry& entry) {
    std::lock_guard<std::mutex> lock(m_statsMutex);
    
    m_statistics.stats.entriesByLevel[entry.level]++;
    m_statistics.stats.entriesByCategory[entry.category]++;
    m_statistics.stats.totalEntries++;
    m_statistics.stats.lastEntry = entry.timestamp;
}

} // namespace SparkEditor