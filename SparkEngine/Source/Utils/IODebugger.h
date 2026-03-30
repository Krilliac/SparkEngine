/**
 * @file IODebugger.h
 * @brief File I/O operation tracking, bandwidth analysis, and bottleneck detection
 * @author Spark Engine Team
 * @date 2026
 *
 * Records file I/O operations (read, write, open, close) with timing, byte
 * counts, and call-site information. Identifies I/O bottlenecks and hot files.
 *
 * Features:
 * - Per-operation tracking with file path, byte count, and duration
 * - Call-site recording (file/line/function) for hotspot analysis
 * - Per-file aggregated statistics (read/write counts, total bytes, peak time)
 * - Slowest-files and most-accessed-files rankings
 * - Hotspot analysis by call site
 * - Thread-safe for I/O from multiple threads
 *
 * @note Active in Debug and Release builds. Compiled to no-ops only in
 *       SPARK_SHIPPING builds. The caller is responsible for measuring
 *       I/O duration and passing it to the Record* methods.
 *
 * @see MemoryDebugger.h, CacheDebugger.h
 */

#pragma once

#include "../Core/Platform.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark
{

    /**
     * @brief Type of I/O operation
     */
    enum class IOOperationType
    {
        Open,
        Read,
        Write,
        Close,
        Seek
    };

    /**
     * @brief Aggregated I/O statistics for a single file path
     */
    struct IOFileStats
    {
        std::string filePath;
        uint64_t readCount = 0;
        uint64_t writeCount = 0;
        uint64_t openCount = 0;
        size_t totalBytesRead = 0;
        size_t totalBytesWritten = 0;
        float totalReadTimeMs = 0.0f;
        float totalWriteTimeMs = 0.0f;
        float totalOpenTimeMs = 0.0f;
        float peakReadTimeMs = 0.0f;
        float peakWriteTimeMs = 0.0f;
    };

    /**
     * @brief I/O hotspot entry by call site
     */
    struct IOHotSpot
    {
        std::string location; ///< "file:line (function)"
        uint64_t operationCount = 0;
        size_t totalBytes = 0;
        float totalTimeMs = 0.0f;
    };

    /**
     * @brief Singleton I/O debugger for file operation tracking and bottleneck detection
     */
    class IODebugger
    {
      public:
        static IODebugger& GetInstance()
        {
            static IODebugger instance;
            return instance;
        }

        void SetEnabled(bool enabled) { m_enabled = enabled; }
        bool IsEnabled() const { return m_enabled; }

        // ---- Recording ----

        void RecordRead(const char* filePath, size_t bytes, float durationMs, const char* file = nullptr, int line = 0,
                        const char* func = nullptr)
        {
            if (!m_enabled || !filePath)
                return;

            std::lock_guard<std::mutex> lock(m_mutex);

            auto& stats = m_fileStats[filePath];
            stats.filePath = filePath;
            stats.readCount++;
            stats.totalBytesRead += bytes;
            stats.totalReadTimeMs += durationMs;
            if (durationMs > stats.peakReadTimeMs)
                stats.peakReadTimeMs = durationMs;

            m_totalBytesRead += bytes;
            m_totalIOTimeMs += durationMs;
            m_totalOperations++;

            RecordHotSpot(file, line, func, bytes, durationMs);
        }

        void RecordWrite(const char* filePath, size_t bytes, float durationMs, const char* file = nullptr, int line = 0,
                         const char* func = nullptr)
        {
            if (!m_enabled || !filePath)
                return;

            std::lock_guard<std::mutex> lock(m_mutex);

            auto& stats = m_fileStats[filePath];
            stats.filePath = filePath;
            stats.writeCount++;
            stats.totalBytesWritten += bytes;
            stats.totalWriteTimeMs += durationMs;
            if (durationMs > stats.peakWriteTimeMs)
                stats.peakWriteTimeMs = durationMs;

            m_totalBytesWritten += bytes;
            m_totalIOTimeMs += durationMs;
            m_totalOperations++;

            RecordHotSpot(file, line, func, bytes, durationMs);
        }

        void RecordOpen(const char* filePath, float durationMs, const char* file = nullptr, int line = 0,
                        const char* func = nullptr)
        {
            if (!m_enabled || !filePath)
                return;

            std::lock_guard<std::mutex> lock(m_mutex);

            auto& stats = m_fileStats[filePath];
            stats.filePath = filePath;
            stats.openCount++;
            stats.totalOpenTimeMs += durationMs;

            m_totalIOTimeMs += durationMs;
            m_totalOperations++;

            RecordHotSpot(file, line, func, 0, durationMs);
        }

        void RecordClose(const char* filePath, float durationMs)
        {
            if (!m_enabled || !filePath)
                return;

            std::lock_guard<std::mutex> lock(m_mutex);
            m_totalIOTimeMs += durationMs;
            m_totalOperations++;
        }

        // ---- Queries ----

        /**
         * @brief Get all file stats, sorted by total I/O time (slowest first)
         */
        std::vector<IOFileStats> GetFileStats() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            std::vector<IOFileStats> result;
            result.reserve(m_fileStats.size());
            for (const auto& [path, stats] : m_fileStats)
                result.push_back(stats);

            std::sort(result.begin(), result.end(),
                      [](const IOFileStats& a, const IOFileStats& b)
                      {
                          return (a.totalReadTimeMs + a.totalWriteTimeMs + a.totalOpenTimeMs) >
                                 (b.totalReadTimeMs + b.totalWriteTimeMs + b.totalOpenTimeMs);
                      });
            return result;
        }

        /**
         * @brief Get top N hotspots by call site, sorted by operation count
         */
        std::vector<IOHotSpot> GetHotSpots(int topN = 20) const
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            std::vector<IOHotSpot> spots;
            spots.reserve(m_hotSpots.size());
            for (const auto& [loc, hs] : m_hotSpots)
                spots.push_back(hs);

            std::sort(spots.begin(), spots.end(),
                      [](const IOHotSpot& a, const IOHotSpot& b) { return a.operationCount > b.operationCount; });

            if (static_cast<int>(spots.size()) > topN)
                spots.resize(topN);
            return spots;
        }

        size_t GetTotalBytesRead() const { return m_totalBytesRead; }
        size_t GetTotalBytesWritten() const { return m_totalBytesWritten; }
        float GetTotalIOTimeMs() const { return m_totalIOTimeMs; }
        uint64_t GetTotalOperationCount() const { return m_totalOperations; }

        /**
         * @brief Get the number of tracked files
         */
        size_t GetTrackedFileCount() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_fileStats.size();
        }

        /**
         * @brief Get top N slowest files by total I/O time
         */
        std::vector<IOFileStats> GetSlowestFiles(int topN = 10) const
        {
            auto all = GetFileStats(); // already sorted by time
            if (static_cast<int>(all.size()) > topN)
                all.resize(topN);
            return all;
        }

        /**
         * @brief Get top N most accessed files by total operation count
         */
        std::vector<IOFileStats> GetMostAccessedFiles(int topN = 10) const
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            std::vector<IOFileStats> result;
            result.reserve(m_fileStats.size());
            for (const auto& [path, stats] : m_fileStats)
                result.push_back(stats);

            std::sort(
                result.begin(), result.end(), [](const IOFileStats& a, const IOFileStats& b)
                { return (a.readCount + a.writeCount + a.openCount) > (b.readCount + b.writeCount + b.openCount); });

            if (static_cast<int>(result.size()) > topN)
                result.resize(topN);
            return result;
        }

        // ---- Reports ----

        void PrintIOReport() const
        {
            auto stats = GetFileStats();

            fprintf(stderr, "\n===== I/O DEBUGGER FILE REPORT =====\n");
            fprintf(stderr, "%-40s %6s %6s %10s %10s %10s\n", "File", "Reads", "Writes", "ReadKB", "WriteKB", "TimeMs");
            fprintf(stderr, "%-40s %6s %6s %10s %10s %10s\n", "----", "-----", "------", "------", "-------", "------");

            for (const auto& s : stats)
            {
                // Shorten long paths
                std::string display = s.filePath;
                if (display.size() > 40)
                    display = "..." + display.substr(display.size() - 37);

                float totalTime = s.totalReadTimeMs + s.totalWriteTimeMs + s.totalOpenTimeMs;
                fprintf(stderr, "%-40s %6llu %6llu %10zu %10zu %10.2f\n", display.c_str(),
                        static_cast<unsigned long long>(s.readCount), static_cast<unsigned long long>(s.writeCount),
                        s.totalBytesRead / 1024, s.totalBytesWritten / 1024, totalTime);
            }
            fprintf(stderr, "====================================\n\n");
        }

        void PrintBottleneckReport(int topN = 10) const
        {
            auto slowest = GetSlowestFiles(topN);

            fprintf(stderr, "\n===== I/O BOTTLENECK REPORT (Top %d) =====\n", topN);
            for (const auto& s : slowest)
            {
                float totalTime = s.totalReadTimeMs + s.totalWriteTimeMs + s.totalOpenTimeMs;
                fprintf(stderr, "  %-50s  %.2f ms  (%zu KB read, %zu KB written)\n", s.filePath.c_str(), totalTime,
                        s.totalBytesRead / 1024, s.totalBytesWritten / 1024);
            }
            fprintf(stderr, "==========================================\n\n");
        }

        void PrintSummary() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            fprintf(stderr, "\n===== I/O DEBUGGER SUMMARY =====\n");
            fprintf(stderr, "Tracked files:     %zu\n", m_fileStats.size());
            fprintf(stderr, "Total operations:  %llu\n", static_cast<unsigned long long>(m_totalOperations));
            fprintf(stderr, "Total read:        %zu KB\n", m_totalBytesRead / 1024);
            fprintf(stderr, "Total written:     %zu KB\n", m_totalBytesWritten / 1024);
            fprintf(stderr, "Total I/O time:    %.3f ms\n", m_totalIOTimeMs);
            fprintf(stderr, "Hotspot sites:     %zu\n", m_hotSpots.size());
            fprintf(stderr, "================================\n\n");
        }

        void Reset()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_fileStats.clear();
            m_hotSpots.clear();
            m_totalBytesRead = 0;
            m_totalBytesWritten = 0;
            m_totalIOTimeMs = 0.0f;
            m_totalOperations = 0;
        }

      private:
        IODebugger() = default;
        IODebugger(const IODebugger&) = delete;
        IODebugger& operator=(const IODebugger&) = delete;

        // Must be called with m_mutex already held
        void RecordHotSpot(const char* file, int line, const char* func, size_t bytes, float durationMs)
        {
            std::string location = FormatLocation(file, line, func);
            auto& hs = m_hotSpots[location];
            hs.location = location;
            hs.operationCount++;
            hs.totalBytes += bytes;
            hs.totalTimeMs += durationMs;
        }

        static std::string FormatLocation(const char* file, int line, const char* func)
        {
            std::string result;
            if (file && file[0])
            {
                const char* slash = strrchr(file, '\\');
                if (!slash)
                    slash = strrchr(file, '/');
                result = slash ? (slash + 1) : file;
                result += ":" + std::to_string(line);
            }
            if (func && func[0])
            {
                if (!result.empty())
                    result += " ";
                result += "(";
                result += func;
                result += ")";
            }
            if (result.empty())
                result = "<unknown>";
            return result;
        }

        mutable std::mutex m_mutex;
        std::unordered_map<std::string, IOFileStats> m_fileStats;
        std::unordered_map<std::string, IOHotSpot> m_hotSpots;

        size_t m_totalBytesRead = 0;
        size_t m_totalBytesWritten = 0;
        float m_totalIOTimeMs = 0.0f;
        uint64_t m_totalOperations = 0;
        bool m_enabled = true;
    };

} // namespace Spark

// ============================================================================
// Convenience macros — active in Debug + Release, no-op in Shipping
// ============================================================================

#if !defined(SPARK_SHIPPING)

/** @brief Track a file read operation */
#define SPARK_TRACK_IO_READ(path, bytes, durationMs)                                                                   \
    Spark::IODebugger::GetInstance().RecordRead(path, bytes, durationMs, __FILE__, __LINE__, __FUNCTION__)

/** @brief Track a file write operation */
#define SPARK_TRACK_IO_WRITE(path, bytes, durationMs)                                                                  \
    Spark::IODebugger::GetInstance().RecordWrite(path, bytes, durationMs, __FILE__, __LINE__, __FUNCTION__)

/** @brief Track a file open operation */
#define SPARK_TRACK_IO_OPEN(path, durationMs)                                                                          \
    Spark::IODebugger::GetInstance().RecordOpen(path, durationMs, __FILE__, __LINE__, __FUNCTION__)

/** @brief Print I/O summary */
#define SPARK_PRINT_IO_REPORT() Spark::IODebugger::GetInstance().PrintSummary()

#else

#define SPARK_TRACK_IO_READ(path, bytes, durationMs) ((void)0)
#define SPARK_TRACK_IO_WRITE(path, bytes, durationMs) ((void)0)
#define SPARK_TRACK_IO_OPEN(path, durationMs) ((void)0)
#define SPARK_PRINT_IO_REPORT() ((void)0)

#endif
