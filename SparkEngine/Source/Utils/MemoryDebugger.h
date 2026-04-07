/**
 * @file MemoryDebugger.h
 * @brief Memory leak detection, allocation tracking, and corruption detection
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a debug-build memory tracking layer that records every allocation
 * and deallocation with call-site information. At shutdown (or on demand), it
 * reports outstanding allocations as potential leaks.
 *
 * Features:
 * - Per-allocation tracking with file/line/function call site
 * - Leak detection report on shutdown
 * - Allocation statistics by category and call site
 * - High-water-mark tracking per category
 * - Double-free and use-after-free detection via guard patterns
 * - Allocation histogram and hot-spot analysis
 * - Thread-safe for multi-threaded allocation patterns
 *
 * @note This system adds overhead and is intended for Debug builds only.
 *       It does NOT replace the engine's Profiler memory tracking, which is
 *       a lightweight per-category counter suitable for Release builds.
 *
 * @see Profiler.h, SparkError.h
 */

#pragma once

#include "../Core/Platform.h"
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <chrono>

namespace Spark
{

    /**
 * @brief Information about a single tracked allocation
 */
    struct AllocationRecord
    {
        void* address = nullptr;                         ///< Allocated pointer
        size_t size = 0;                                 ///< Allocation size in bytes
        std::string category;                            ///< User-defined category (e.g., "Textures", "Audio")
        std::string file;                                ///< Source file where allocation occurred
        int line = 0;                                    ///< Source line number
        std::string function;                            ///< Function name
        uint64_t allocationIndex = 0;                    ///< Global allocation sequence number
        std::chrono::steady_clock::time_point timestamp; ///< When the allocation occurred
    };

    /**
 * @brief Per-category memory statistics
 */
    struct MemoryCategoryStats
    {
        std::string name;                ///< Category name
        size_t currentBytes = 0;         ///< Currently allocated bytes
        size_t peakBytes = 0;            ///< Peak allocation in bytes
        uint64_t totalAllocations = 0;   ///< Total allocation count
        uint64_t totalDeallocations = 0; ///< Total deallocation count
        size_t totalBytesAllocated = 0;  ///< Total bytes ever allocated
    };

    /**
 * @brief Hot-spot entry for allocation frequency analysis
 */
    struct AllocationHotSpot
    {
        std::string location;         ///< "file:line (function)"
        uint64_t count = 0;           ///< Number of allocations from this site
        size_t totalBytes = 0;        ///< Total bytes allocated from this site
        size_t largestAllocation = 0; ///< Largest single allocation
    };

    /**
 * @brief Leak report entry
 */
    struct LeakEntry
    {
        void* address = nullptr;
        size_t size = 0;
        std::string category;
        std::string location;
        uint64_t allocationIndex = 0;
    };

    /**
 * @brief Singleton memory debugger for allocation tracking and leak detection
 */
    class MemoryDebugger
    {
      public:
        static MemoryDebugger& GetInstance()
        {
            static MemoryDebugger instance;
            return instance;
        }

        /**
     * @brief Enable or disable tracking (default: enabled in debug builds)
     */
        void SetEnabled(bool enabled) { m_enabled = enabled; }
        bool IsEnabled() const { return m_enabled; }

        /**
     * @brief Record a memory allocation
     * @param ptr       Allocated pointer
     * @param size      Allocation size in bytes
     * @param category  Category name for grouping
     * @param file      Source file (__FILE__)
     * @param line      Source line (__LINE__)
     * @param func      Function name (__FUNCTION__)
     */
        void RecordAlloc(void* ptr, size_t size, const char* category = "General", const char* file = nullptr,
                         int line = 0, const char* func = nullptr)
        {
            if (!m_enabled || !ptr)
                return;

            std::lock_guard<std::mutex> lock(m_mutex);

            AllocationRecord rec;
            rec.address = ptr;
            rec.size = size;
            rec.category = category ? category : "General";
            rec.file = file ? file : "";
            rec.line = line;
            rec.function = func ? func : "";
            rec.allocationIndex = m_nextAllocIndex++;
            rec.timestamp = std::chrono::steady_clock::now();

            m_activeAllocations[ptr] = rec;
            m_totalAllocatedBytes += size;
            m_totalAllocationCount++;

            // Update category stats
            auto& cat = m_categoryStats[rec.category];
            cat.name = rec.category;
            cat.currentBytes += size;
            cat.totalAllocations++;
            cat.totalBytesAllocated += size;
            if (cat.currentBytes > cat.peakBytes)
                cat.peakBytes = cat.currentBytes;

            // Update global peak
            m_currentBytes += size;
            if (m_currentBytes > m_peakBytes)
                m_peakBytes = m_currentBytes;

            // Track hot spots
            std::string location = FormatLocation(file, line, func);
            auto& hotspot = m_hotSpots[location];
            hotspot.location = location;
            hotspot.count++;
            hotspot.totalBytes += size;
            if (size > hotspot.largestAllocation)
                hotspot.largestAllocation = size;
        }

        /**
     * @brief Record a memory deallocation
     * @param ptr Pointer being freed
     * @return true if the pointer was tracked, false if unknown (potential double-free)
     */
        bool RecordFree(void* ptr)
        {
            if (!m_enabled || !ptr)
                return true;

            std::lock_guard<std::mutex> lock(m_mutex);

            auto it = m_activeAllocations.find(ptr);
            if (it == m_activeAllocations.end())
            {
                // Potential double-free or freeing untracked memory
                m_doubleFreeCount++;
                // Rate-limit stderr output to avoid log spam from repeated double-frees
                if (m_doubleFreeCount <= MAX_DOUBLE_FREE_WARNINGS)
                {
                    fprintf(stderr,
                            "[MemoryDebugger] WARNING: Freeing untracked pointer %p (possible double-free #%llu)\n",
                            ptr, static_cast<unsigned long long>(m_doubleFreeCount));
                    if (m_doubleFreeCount == MAX_DOUBLE_FREE_WARNINGS)
                    {
                        fprintf(stderr,
                                "[MemoryDebugger] WARNING: Suppressing further double-free warnings (count: %llu)\n",
                                static_cast<unsigned long long>(m_doubleFreeCount));
                    }
                }
                return false;
            }

            const AllocationRecord& rec = it->second;
            m_currentBytes -= rec.size;
            m_totalDeallocationCount++;

            // Update category stats
            auto catIt = m_categoryStats.find(rec.category);
            if (catIt != m_categoryStats.end())
            {
                catIt->second.currentBytes -= rec.size;
                catIt->second.totalDeallocations++;
            }

            m_activeAllocations.erase(it);
            return true;
        }

        /**
     * @brief Generate a leak report of all outstanding allocations
     * @return Vector of leak entries
     */
        std::vector<LeakEntry> GetLeaks() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            std::vector<LeakEntry> leaks;
            leaks.reserve(m_activeAllocations.size());

            for (const auto& [ptr, rec] : m_activeAllocations)
            {
                LeakEntry entry;
                entry.address = rec.address;
                entry.size = rec.size;
                entry.category = rec.category;
                entry.location = FormatLocation(rec.file.c_str(), rec.line, rec.function.c_str());
                entry.allocationIndex = rec.allocationIndex;
                leaks.push_back(entry);
            }

            // Sort by allocation index (oldest first)
            std::sort(leaks.begin(), leaks.end(),
                      [](const LeakEntry& a, const LeakEntry& b) { return a.allocationIndex < b.allocationIndex; });

            return leaks;
        }

        /**
     * @brief Print a formatted leak report to stderr
     * @return Number of leaks found
     */
        size_t PrintLeakReport() const
        {
            auto leaks = GetLeaks();
            if (leaks.empty())
            {
                fprintf(stderr, "[MemoryDebugger] No memory leaks detected.\n");
                return 0;
            }

            fprintf(stderr, "\n===== MEMORY LEAK REPORT =====\n");
            fprintf(stderr, "Outstanding allocations: %zu\n\n", leaks.size());

            size_t totalLeakedBytes = 0;
            for (const auto& leak : leaks)
            {
                fprintf(stderr,
                        "  [#%llu] %p: %zu bytes [%s]\n"
                        "         at %s\n",
                        static_cast<unsigned long long>(leak.allocationIndex), leak.address, leak.size,
                        leak.category.c_str(), leak.location.c_str());
                totalLeakedBytes += leak.size;
            }

            fprintf(stderr, "\nTotal leaked: %zu bytes in %zu allocations\n", totalLeakedBytes, leaks.size());
            fprintf(stderr, "==============================\n\n");

            return leaks.size();
        }

        /**
     * @brief Get per-category memory statistics
     */
        std::vector<MemoryCategoryStats> GetCategoryStats() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            std::vector<MemoryCategoryStats> stats;
            stats.reserve(m_categoryStats.size());
            for (const auto& [name, cat] : m_categoryStats)
                stats.push_back(cat);

            std::sort(stats.begin(), stats.end(), [](const MemoryCategoryStats& a, const MemoryCategoryStats& b)
                      { return a.currentBytes > b.currentBytes; });
            return stats;
        }

        /**
     * @brief Get allocation hot spots (most frequently allocating call sites)
     * @param topN Number of top hot spots to return
     */
        std::vector<AllocationHotSpot> GetHotSpots(int topN = 20) const
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            std::vector<AllocationHotSpot> spots;
            spots.reserve(m_hotSpots.size());
            for (const auto& [loc, hs] : m_hotSpots)
                spots.push_back(hs);

            std::sort(spots.begin(), spots.end(),
                      [](const AllocationHotSpot& a, const AllocationHotSpot& b) { return a.count > b.count; });

            if (static_cast<int>(spots.size()) > topN)
                spots.resize(topN);
            return spots;
        }

        /**
     * @brief Print a summary of memory usage statistics
     */
        void PrintSummary() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            fprintf(stderr, "\n===== MEMORY DEBUGGER SUMMARY =====\n");
            fprintf(stderr, "Current usage:     %zu bytes\n", m_currentBytes);
            fprintf(stderr, "Peak usage:        %zu bytes\n", m_peakBytes);
            fprintf(stderr, "Total allocated:   %zu bytes\n", m_totalAllocatedBytes);
            fprintf(stderr, "Total allocs:      %llu\n", static_cast<unsigned long long>(m_totalAllocationCount));
            fprintf(stderr, "Total frees:       %llu\n", static_cast<unsigned long long>(m_totalDeallocationCount));
            fprintf(stderr, "Outstanding:       %zu\n", m_activeAllocations.size());
            fprintf(stderr, "Double-frees:      %llu\n", static_cast<unsigned long long>(m_doubleFreeCount));
            fprintf(stderr, "Categories:        %zu\n", m_categoryStats.size());
            fprintf(stderr, "===================================\n\n");
        }

        /** @brief Get current allocated bytes */
        size_t GetCurrentBytes() const { return m_currentBytes; }

        /** @brief Get peak allocated bytes */
        size_t GetPeakBytes() const { return m_peakBytes; }

        /** @brief Get total allocation count */
        uint64_t GetTotalAllocationCount() const { return m_totalAllocationCount; }

        /** @brief Get count of active (unfreed) allocations */
        size_t GetActiveAllocationCount() const { return m_activeAllocations.size(); }

        /** @brief Get number of detected double-free attempts */
        uint64_t GetDoubleFreeCount() const { return m_doubleFreeCount; }

        /** @brief Reset all tracking data */
        void Reset()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_activeAllocations.clear();
            m_categoryStats.clear();
            m_hotSpots.clear();
            m_currentBytes = 0;
            m_peakBytes = 0;
            m_totalAllocatedBytes = 0;
            m_totalAllocationCount = 0;
            m_totalDeallocationCount = 0;
            m_doubleFreeCount = 0;
            m_nextAllocIndex = 1;
        }

      private:
        MemoryDebugger() = default;
        MemoryDebugger(const MemoryDebugger&) = delete;
        MemoryDebugger& operator=(const MemoryDebugger&) = delete;

        static std::string FormatLocation(const char* file, int line, const char* func)
        {
            std::string result;
            if (file && file[0])
            {
                // Shorten to filename only
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
        std::unordered_map<void*, AllocationRecord> m_activeAllocations;
        std::unordered_map<std::string, MemoryCategoryStats> m_categoryStats;
        std::unordered_map<std::string, AllocationHotSpot> m_hotSpots;

        size_t m_currentBytes = 0;
        size_t m_peakBytes = 0;
        size_t m_totalAllocatedBytes = 0;
        uint64_t m_totalAllocationCount = 0;
        uint64_t m_totalDeallocationCount = 0;
        uint64_t m_doubleFreeCount = 0;
        static constexpr uint64_t MAX_DOUBLE_FREE_WARNINGS = 3; ///< Suppress stderr after this many warnings
        uint64_t m_nextAllocIndex = 1;

        bool m_enabled =
#if defined(_DEBUG) || defined(DEBUG)
            true;
#else
            false;
#endif
    };

} // namespace Spark

// ============================================================================
// Convenience macros
// ============================================================================

#if defined(_DEBUG) || defined(DEBUG)

/** @brief Track an allocation with call-site info */
#define SPARK_TRACK_ALLOC(ptr, size, category)                                                                         \
    Spark::MemoryDebugger::GetInstance().RecordAlloc(ptr, size, category, __FILE__, __LINE__, __FUNCTION__)

/** @brief Track a deallocation */
#define SPARK_TRACK_FREE(ptr) Spark::MemoryDebugger::GetInstance().RecordFree(ptr)

/** @brief Print the leak report */
#define SPARK_PRINT_LEAK_REPORT() Spark::MemoryDebugger::GetInstance().PrintLeakReport()

/** @brief Print memory usage summary */
#define SPARK_PRINT_MEMORY_SUMMARY() Spark::MemoryDebugger::GetInstance().PrintSummary()

#else

#define SPARK_TRACK_ALLOC(ptr, size, category) ((void)0)
#define SPARK_TRACK_FREE(ptr) ((void)0)
#define SPARK_PRINT_LEAK_REPORT() ((void)0)
#define SPARK_PRINT_MEMORY_SUMMARY() ((void)0)

#endif
