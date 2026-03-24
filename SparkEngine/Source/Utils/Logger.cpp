/**
 * @file Logger.cpp
 * @brief Unified logging system implementation
 * @author Spark Engine Team
 * @date 2026
 *
 * Implements the Logger singleton with:
 * - Async background writer thread that drains a message queue
 * - StderrSink with OutputDebugStringA on Windows
 * - FileSink with automatic size-based file rotation
 * - Per-category and global log level filtering
 * - Formatted output: [HH:MM:SS.mmm] [TID:XXXX] [LEVEL] [Category] message  (file:line)
 */

#include "Logger.h"
#include "DebugHookManager.h"

#include <iostream>
#include <cstring>
#include <filesystem>
#include <iomanip>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

namespace Spark
{

    // ============================================================================
    // FormatLogMessage (free function)
    // ============================================================================

    std::string FormatLogMessage(const LogMessage& msg)
    {
        // Format: [HH:MM:SS.mmm] [TID:XXXX] [LEVEL] [Category] message  (file:line)
        auto timeT = std::chrono::system_clock::to_time_t(msg.timestamp);
        auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(msg.timestamp.time_since_epoch()).count() % 1000;

        char timeBuf[32];
        std::strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", std::localtime(&timeT));

        // Thread ID to integer
        std::ostringstream tidStream;
        tidStream << msg.threadId;
        std::string tidStr = tidStream.str();

        // Build the formatted string
        char buf[4096];
        int offset = 0;

        auto levelStr = LogLevelToString(msg.level);
        auto catStr = LogCategoryToString(msg.category);
        offset += snprintf(buf + offset, sizeof(buf) - offset, "[%s.%03d] [TID:%s] [%-5.*s] [%-10.*s] %s", timeBuf,
                           static_cast<int>(ms), tidStr.c_str(), static_cast<int>(levelStr.size()), levelStr.data(),
                           static_cast<int>(catStr.size()), catStr.data(), msg.message.c_str());

        // Append source location if available
        if (!msg.file.empty())
        {
            // Extract just the filename from the full path
            const char* shortFile = msg.file.c_str();
            const char* slash = strrchr(shortFile, '\\');
            if (!slash)
            {
                slash = strrchr(shortFile, '/');
            }
            if (slash)
            {
                shortFile = slash + 1;
            }

            offset += snprintf(buf + offset, sizeof(buf) - offset, "  (%s:%d)", shortFile, msg.line);
        }

        return std::string(buf, offset);
    }

    // ============================================================================
    // Logger (static helper)
    // ============================================================================

    std::string Logger::FormatLogEntry(const LogMessage& msg)
    {
        return FormatLogMessage(msg);
    }

    // ============================================================================
    // Logger::Initialize
    // ============================================================================

    void Logger::Initialize(bool enableAsync)
    {
        if (m_initialized.load(std::memory_order_acquire))
        {
            return;
        }

        // Initialize per-category levels to Trace (accept all)
        for (size_t i = 0; i < static_cast<size_t>(LogCategory::Count); ++i)
        {
            m_categoryLevels[i].store(LogLevel::Trace, std::memory_order_relaxed);
        }

        m_asyncEnabled.store(enableAsync, std::memory_order_relaxed);
        m_stopThread.store(false, std::memory_order_relaxed);

        // Set initialized before starting the writer thread to prevent a racing
        // Initialize() call from creating a second thread.
        m_initialized.store(true, std::memory_order_release);

        if (enableAsync)
        {
            m_writerThread = std::thread(&Logger::AsyncWriterThread, this);
        }
    }

    // ============================================================================
    // Logger::Shutdown
    // ============================================================================

    void Logger::Shutdown()
    {
        if (!m_initialized.exchange(false, std::memory_order_acq_rel))
        {
            return;
        }

        // Signal the writer thread to stop
        if (m_asyncEnabled.load(std::memory_order_relaxed))
        {
            m_stopThread.store(true, std::memory_order_release);
            m_queueCV.notify_all();

            if (m_writerThread.joinable())
            {
                m_writerThread.join();
            }
        }

        // Flush all sinks
        {
            std::lock_guard<std::mutex> lock(m_sinkMutex);
            for (auto& sink : m_sinks)
            {
                sink->Flush();
            }
        }
    }

    // ============================================================================
    // Sink Management
    // ============================================================================

    void Logger::AddSink(std::unique_ptr<ILogSink> sink)
    {
        std::lock_guard<std::mutex> lock(m_sinkMutex);
        m_sinks.push_back(std::move(sink));
    }

    void Logger::ClearSinks()
    {
        std::lock_guard<std::mutex> lock(m_sinkMutex);
        m_sinks.clear();
    }

    // ============================================================================
    // Filtering
    // ============================================================================

    void Logger::SetCategoryLevel(LogCategory cat, LogLevel level)
    {
        auto idx = static_cast<size_t>(cat);
        if (idx < static_cast<size_t>(LogCategory::Count))
        {
            m_categoryLevels[idx].store(level, std::memory_order_relaxed);
        }
    }

    LogLevel Logger::GetCategoryLevel(LogCategory cat) const
    {
        auto idx = static_cast<size_t>(cat);
        if (idx < static_cast<size_t>(LogCategory::Count))
        {
            return m_categoryLevels[idx].load(std::memory_order_relaxed);
        }
        return LogLevel::Trace;
    }

    bool Logger::ShouldLog(LogLevel level, LogCategory category) const
    {
        // Check global level first (fast path)
        if (level < m_globalLevel.load(std::memory_order_relaxed))
        {
            return false;
        }

        // Check per-category level
        auto idx = static_cast<size_t>(category);
        if (idx < static_cast<size_t>(LogCategory::Count))
        {
            if (level < m_categoryLevels[idx].load(std::memory_order_relaxed))
            {
                return false;
            }
        }

        return true;
    }

    // ============================================================================
    // Logging
    // ============================================================================

    void Logger::Log(LogLevel level, LogCategory category, const char* file, int line, const char* func,
                     const std::string& message)
    {
        if (!m_initialized.load(std::memory_order_acquire))
        {
            return;
        }

        if (!ShouldLog(level, category))
        {
            return;
        }

        LogMessage msg;
        msg.level = level;
        msg.category = category;
        msg.message = message;
        msg.file = file ? file : "";
        msg.line = line;
        msg.function = func ? func : "";
        msg.timestamp = std::chrono::system_clock::now();
        msg.threadId = std::this_thread::get_id();

        // Fire debug hooks for errors and warnings so instrumentation can observe them
        if (level == LogLevel::Error || level == LogLevel::Fatal)
        {
            SPARK_DEBUG_HOOK_MESSAGE(ErrorRaised, message);
        }
        else if (level == LogLevel::Warn)
        {
            SPARK_DEBUG_HOOK_MESSAGE(WarningRaised, message);
        }

        if (m_asyncEnabled.load(std::memory_order_relaxed))
        {
            // Enqueue for background writing
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                m_messageQueue.push(std::move(msg));
            }
            m_queueCV.notify_one();
        }
        else
        {
            // Synchronous dispatch
            DispatchToSinks(msg);
        }
    }

    void Logger::FlushAll()
    {
        // If async, drain the queue first
        if (m_asyncEnabled.load(std::memory_order_relaxed))
        {
            // Drain remaining messages
            std::queue<LogMessage> pending;
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                std::swap(pending, m_messageQueue);
            }
            while (!pending.empty())
            {
                DispatchToSinks(pending.front());
                pending.pop();
            }
        }

        std::lock_guard<std::mutex> lock(m_sinkMutex);
        for (auto& sink : m_sinks)
        {
            sink->Flush();
        }
    }

    // ============================================================================
    // Background Writer Thread
    // ============================================================================

    void Logger::AsyncWriterThread()
    {
        while (true)
        {
            std::queue<LogMessage> batch;

            {
                std::unique_lock<std::mutex> lock(m_queueMutex);
                m_queueCV.wait(lock, [this]
                               { return !m_messageQueue.empty() || m_stopThread.load(std::memory_order_acquire); });

                // Drain the entire queue into a local batch
                std::swap(batch, m_messageQueue);
            }

            // Dispatch all messages outside the queue lock
            while (!batch.empty())
            {
                DispatchToSinks(batch.front());
                batch.pop();
            }

            // Check stop condition after draining
            if (m_stopThread.load(std::memory_order_acquire))
            {
                // Final drain
                std::queue<LogMessage> finalBatch;
                {
                    std::lock_guard<std::mutex> lock(m_queueMutex);
                    std::swap(finalBatch, m_messageQueue);
                }
                while (!finalBatch.empty())
                {
                    DispatchToSinks(finalBatch.front());
                    finalBatch.pop();
                }
                break;
            }
        }
    }

    void Logger::DispatchToSinks(const LogMessage& msg)
    {
        std::lock_guard<std::mutex> lock(m_sinkMutex);
        for (auto& sink : m_sinks)
        {
            sink->Write(msg);
        }
    }

    // ============================================================================
    // StderrSink
    // ============================================================================

    void StderrSink::Write(const LogMessage& msg)
    {
        std::string formatted = FormatLogMessage(msg);
        fprintf(stderr, "%s\n", formatted.c_str());

#ifdef _WIN32
        formatted += "\n";
        OutputDebugStringA(formatted.c_str());
#endif
    }

    void StderrSink::Flush()
    {
        fflush(stderr);
    }

    // ============================================================================
    // FileSink
    // ============================================================================

    FileSink::FileSink() : FileSink(Config{}) {}

    FileSink::FileSink(const Config& config) : m_config(config)
    {
        OpenFile();
    }

    FileSink::~FileSink()
    {
        if (m_file.is_open())
        {
            m_file.flush();
            m_file.close();
        }
    }

    bool FileSink::OpenFile()
    {
        // Create directory if it doesn't exist
        try
        {
            std::filesystem::create_directories(m_config.directory);
        }
        catch (const std::filesystem::filesystem_error& e)
        {
            fprintf(stderr, "[FileSink] Failed to create log directory '%s': %s\n", m_config.directory.c_str(),
                    e.what());
        }

        // Generate timestamped filename
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        char timeBuf[64];
        std::strftime(timeBuf, sizeof(timeBuf), "%Y%m%d_%H%M%S", std::localtime(&t));

        m_currentFilePath = m_config.directory + m_config.prefix + "_" + timeBuf + ".log";
        m_file.open(m_currentFilePath, std::ios::out | std::ios::trunc);

        if (!m_file.is_open())
        {
            fprintf(stderr, "[FileSink] Failed to open log file: %s\n", m_currentFilePath.c_str());
            return false;
        }

        m_currentFileSize = 0;
        m_initialized = true;

        // Write session header
        std::string header = "===============================================\n"
                             " Spark Engine Log\n"
                             "===============================================\n"
                             " Started: " +
                             std::string(timeBuf) +
                             "\n"
                             "===============================================\n\n";

        m_file << header;
        m_currentFileSize += header.size();

        return true;
    }

    void FileSink::RotateFiles()
    {
        if (!m_file.is_open())
        {
            return;
        }

        m_file.flush();
        m_file.close();

        // Rotate existing backup files: delete oldest, shift others
        // file.4.log -> deleted
        // file.3.log -> file.4.log
        // file.2.log -> file.3.log
        // file.1.log -> file.2.log
        // file.log   -> file.1.log

        std::string basePath = m_currentFilePath;

        // Remove the oldest backup if it exists
        std::string oldestBackup = basePath + "." + std::to_string(m_config.maxBackupFiles);
        try
        {
            std::filesystem::remove(oldestBackup);
        }
        catch (...)
        {
        }

        // Shift backup files up
        for (int i = m_config.maxBackupFiles - 1; i >= 1; --i)
        {
            std::string src = basePath + "." + std::to_string(i);
            std::string dst = basePath + "." + std::to_string(i + 1);
            try
            {
                if (std::filesystem::exists(src))
                {
                    std::filesystem::rename(src, dst);
                }
            }
            catch (...)
            {
            }
        }

        // Move current file to .1
        try
        {
            std::string backup1 = basePath + ".1";
            if (std::filesystem::exists(basePath))
            {
                std::filesystem::rename(basePath, backup1);
            }
        }
        catch (...)
        {
        }

        // Open a fresh file at the same path
        m_file.open(m_currentFilePath, std::ios::out | std::ios::trunc);
        m_currentFileSize = 0;

        if (m_file.is_open())
        {
            std::string header = "--- Log rotated ---\n\n";
            m_file << header;
            m_currentFileSize += header.size();
        }
    }

    std::string FileSink::FormatLogEntry(const LogMessage& msg) const
    {
        return FormatLogMessage(msg);
    }

    void FileSink::Write(const LogMessage& msg)
    {
        if (!m_initialized || !m_file.is_open())
        {
            return;
        }

        // Check for rotation
        if (m_currentFileSize >= m_config.maxFileSizeBytes)
        {
            RotateFiles();
        }

        std::string formatted = FormatLogEntry(msg) + "\n";
        m_file << formatted;
        m_currentFileSize += formatted.size();

        // Auto-flush on Error/Fatal
        if (msg.level >= LogLevel::Error)
        {
            m_file.flush();
        }
    }

    void FileSink::Flush()
    {
        if (m_file.is_open())
        {
            m_file.flush();
        }
    }

} // namespace Spark
