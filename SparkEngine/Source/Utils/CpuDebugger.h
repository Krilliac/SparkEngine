/**
 * @file CpuDebugger.h
 * @brief Per-section CPU timing with statistical analysis and hotspot detection
 * @author Spark Engine Team
 * @date 2026
 *
 * Tracks CPU time spent in named code sections with call-site information.
 * Provides min/max/avg/p99 statistics, hotspot ranking, and category grouping.
 *
 * Features:
 * - Begin/End timing pairs with call-site tracking (file/line/function)
 * - RAII scope guard for automatic section timing
 * - Per-section statistics: hit count, min, max, avg, p99, total time
 * - Hotspot analysis sorted by total time or hit count
 * - Category grouping for logical organization
 * - Ring buffer of recent samples per section for percentile calculation
 * - Thread-safe for multi-threaded timing
 *
 * @note Active in Debug and Release builds. Compiled to no-ops only in
 *       SPARK_SHIPPING builds. Adds measurable overhead per tracked section.
 *
 * @see MemoryDebugger.h, Profiler.h, ChromeTracing.h
 */

#pragma once

#include "../Core/Platform.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark
{

    /**
     * @brief Statistical data for a single timed code section
     */
    struct CpuSectionStats
    {
        std::string name;
        std::string category;
        uint64_t hitCount = 0;
        float totalTimeMs = 0.0f;
        float minTimeMs = std::numeric_limits<float>::max();
        float maxTimeMs = 0.0f;
        float avgTimeMs = 0.0f;
        float lastTimeMs = 0.0f;
        float p99TimeMs = 0.0f;
        uint32_t recentSampleCount = 0;
    };

    /**
     * @brief Hotspot entry identifying expensive code locations
     */
    struct CpuHotSpot
    {
        std::string name;
        std::string location; ///< "file:line (function)"
        float totalTimeMs = 0.0f;
        uint64_t hitCount = 0;
        float avgTimeMs = 0.0f;
    };

    /**
     * @brief Singleton CPU timing debugger for section-level performance analysis
     */
    class CpuDebugger
    {
      public:
        static CpuDebugger& GetInstance()
        {
            static CpuDebugger instance;
            return instance;
        }

        void SetEnabled(bool enabled) { m_enabled = enabled; }
        bool IsEnabled() const { return m_enabled; }

        /**
         * @brief Begin timing a named code section
         */
        void BeginSection(const char* name, const char* category = "General", const char* file = nullptr, int line = 0,
                          const char* func = nullptr)
        {
            if (!m_enabled || !name)
                return;

            std::lock_guard<std::mutex> lock(m_mutex);

            PendingSection pending;
            pending.startTime = std::chrono::steady_clock::now();
            pending.category = category ? category : "General";
            pending.file = file ? file : "";
            pending.line = line;
            pending.function = func ? func : "";

            m_pendingSections[name] = pending;
        }

        /**
         * @brief End timing a named code section and record the sample
         */
        void EndSection(const char* name)
        {
            if (!m_enabled || !name)
                return;

            auto endTime = std::chrono::steady_clock::now();

            std::lock_guard<std::mutex> lock(m_mutex);

            auto it = m_pendingSections.find(name);
            if (it == m_pendingSections.end())
                return;

            const PendingSection& pending = it->second;
            float durationMs = std::chrono::duration<float, std::milli>(endTime - pending.startTime).count();

            // Update section data
            auto& section = m_sections[name];
            if (section.name.empty())
            {
                section.name = name;
                section.category = pending.category;
            }
            section.hitCount++;
            section.totalTimeMs += durationMs;
            section.lastTimeMs = durationMs;
            if (durationMs < section.minTimeMs)
                section.minTimeMs = durationMs;
            if (durationMs > section.maxTimeMs)
                section.maxTimeMs = durationMs;
            // Add to ring buffer for percentile calculation
            section.recentSamples[section.ringIndex % RING_BUFFER_SIZE] = durationMs;
            section.ringIndex++;
            if (section.sampleCount < RING_BUFFER_SIZE)
                section.sampleCount++;

            // Track hotspots by call site
            std::string location = FormatLocation(pending.file.c_str(), pending.line, pending.function.c_str());
            auto& hotspot = m_hotSpots[location];
            hotspot.name = name;
            hotspot.location = location;
            hotspot.hitCount++;
            hotspot.totalTimeMs += durationMs;
            hotspot.avgTimeMs = hotspot.totalTimeMs / static_cast<float>(hotspot.hitCount);

            m_pendingSections.erase(it);
        }

        /**
         * @brief Get statistics for a specific named section
         */
        CpuSectionStats GetSectionStats(const std::string& name) const
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            auto it = m_sections.find(name);
            if (it == m_sections.end())
                return {};

            return BuildStats(it->second);
        }

        /**
         * @brief Get statistics for all tracked sections, sorted by total time
         */
        std::vector<CpuSectionStats> GetAllStats() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            std::vector<CpuSectionStats> result;
            result.reserve(m_sections.size());
            for (const auto& [key, section] : m_sections)
                result.push_back(BuildStats(section));

            std::sort(result.begin(), result.end(),
                      [](const CpuSectionStats& a, const CpuSectionStats& b) { return a.totalTimeMs > b.totalTimeMs; });
            return result;
        }

        /**
         * @brief Get top N hotspots sorted by total time
         */
        std::vector<CpuHotSpot> GetHotSpots(int topN = 20) const
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            std::vector<CpuHotSpot> spots;
            spots.reserve(m_hotSpots.size());
            for (const auto& [loc, hs] : m_hotSpots)
                spots.push_back(hs);

            std::sort(spots.begin(), spots.end(),
                      [](const CpuHotSpot& a, const CpuHotSpot& b) { return a.totalTimeMs > b.totalTimeMs; });

            if (static_cast<int>(spots.size()) > topN)
                spots.resize(topN);
            return spots;
        }

        /**
         * @brief Get total CPU time across all sections
         */
        float GetTotalTimeMs() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            float total = 0.0f;
            for (const auto& [name, section] : m_sections)
                total += section.totalTimeMs;
            return total;
        }

        /**
         * @brief Get the number of tracked sections
         */
        size_t GetSectionCount() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_sections.size();
        }

        /**
         * @brief Print a formatted report of all section timings to stderr
         */
        void PrintSectionReport() const
        {
            auto stats = GetAllStats();

            fprintf(stderr, "\n===== CPU DEBUGGER SECTION REPORT =====\n");
            fprintf(stderr, "%-30s %8s %10s %10s %10s %10s %10s\n", "Section", "Hits", "Total(ms)", "Avg(ms)",
                    "Min(ms)", "Max(ms)", "P99(ms)");
            fprintf(stderr, "%-30s %8s %10s %10s %10s %10s %10s\n", "-------", "----", "---------", "-------",
                    "-------", "-------", "-------");

            for (const auto& s : stats)
            {
                fprintf(stderr, "%-30s %8llu %10.3f %10.3f %10.3f %10.3f %10.3f\n", s.name.c_str(),
                        static_cast<unsigned long long>(s.hitCount), s.totalTimeMs, s.avgTimeMs, s.minTimeMs,
                        s.maxTimeMs, s.p99TimeMs);
            }
            fprintf(stderr, "=======================================\n\n");
        }

        /**
         * @brief Print top N hotspots to stderr
         */
        void PrintHotSpotReport(int topN = 20) const
        {
            auto spots = GetHotSpots(topN);

            fprintf(stderr, "\n===== CPU DEBUGGER HOTSPOT REPORT =====\n");
            fprintf(stderr, "%-40s %8s %10s %10s\n", "Location", "Hits", "Total(ms)", "Avg(ms)");
            fprintf(stderr, "%-40s %8s %10s %10s\n", "--------", "----", "---------", "-------");

            for (const auto& hs : spots)
            {
                fprintf(stderr, "%-40s %8llu %10.3f %10.3f\n", hs.location.c_str(),
                        static_cast<unsigned long long>(hs.hitCount), hs.totalTimeMs, hs.avgTimeMs);
            }
            fprintf(stderr, "=======================================\n\n");
        }

        /**
         * @brief Print a compact summary to stderr
         */
        void PrintSummary() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            float totalTime = 0.0f;
            uint64_t totalHits = 0;
            for (const auto& [name, section] : m_sections)
            {
                totalTime += section.totalTimeMs;
                totalHits += section.hitCount;
            }

            fprintf(stderr, "\n===== CPU DEBUGGER SUMMARY =====\n");
            fprintf(stderr, "Tracked sections:  %zu\n", m_sections.size());
            fprintf(stderr, "Total hits:        %llu\n", static_cast<unsigned long long>(totalHits));
            fprintf(stderr, "Total CPU time:    %.3f ms\n", totalTime);
            fprintf(stderr, "Hotspot sites:     %zu\n", m_hotSpots.size());
            fprintf(stderr, "================================\n\n");
        }

        /**
         * @brief Reset all tracking data
         */
        void Reset()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_sections.clear();
            m_hotSpots.clear();
            m_pendingSections.clear();
        }

      private:
        CpuDebugger() = default;
        CpuDebugger(const CpuDebugger&) = delete;
        CpuDebugger& operator=(const CpuDebugger&) = delete;

        static constexpr int RING_BUFFER_SIZE = 256;

        struct PendingSection
        {
            std::chrono::steady_clock::time_point startTime;
            std::string category;
            std::string file;
            int line = 0;
            std::string function;
        };

        struct SectionData
        {
            std::string name;
            std::string category;
            uint64_t hitCount = 0;
            float totalTimeMs = 0.0f;
            float minTimeMs = std::numeric_limits<float>::max();
            float maxTimeMs = 0.0f;
            float lastTimeMs = 0.0f;

            // Ring buffer for percentile calculation
            std::array<float, RING_BUFFER_SIZE> recentSamples = {};
            uint32_t ringIndex = 0;
            uint32_t sampleCount = 0;
        };

        CpuSectionStats BuildStats(const SectionData& data) const
        {
            CpuSectionStats stats;
            stats.name = data.name;
            stats.category = data.category;
            stats.hitCount = data.hitCount;
            stats.totalTimeMs = data.totalTimeMs;
            stats.minTimeMs = data.minTimeMs;
            stats.maxTimeMs = data.maxTimeMs;
            stats.lastTimeMs = data.lastTimeMs;
            stats.avgTimeMs = data.hitCount > 0 ? data.totalTimeMs / static_cast<float>(data.hitCount) : 0.0f;
            stats.recentSampleCount = data.sampleCount;

            // Calculate P99 from ring buffer
            if (data.sampleCount > 0)
            {
                std::vector<float> sorted;
                sorted.reserve(data.sampleCount);
                for (uint32_t i = 0; i < data.sampleCount; ++i)
                    sorted.push_back(data.recentSamples[i]);
                std::sort(sorted.begin(), sorted.end());

                size_t p99Index = static_cast<size_t>(static_cast<float>(sorted.size()) * 0.99f);
                if (p99Index >= sorted.size())
                    p99Index = sorted.size() - 1;
                stats.p99TimeMs = sorted[p99Index];
            }

            return stats;
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
        std::unordered_map<std::string, SectionData> m_sections;
        std::unordered_map<std::string, CpuHotSpot> m_hotSpots;
        std::unordered_map<std::string, PendingSection> m_pendingSections;
        bool m_enabled = true;
    };

    /**
     * @brief RAII scope guard for automatic CPU section timing
     */
    class ScopedCpuSection
    {
      public:
        ScopedCpuSection(const char* name, const char* category = "General", const char* file = nullptr, int line = 0,
                         const char* func = nullptr)
            : m_name(name)
        {
            CpuDebugger::GetInstance().BeginSection(name, category, file, line, func);
        }

        ~ScopedCpuSection() { CpuDebugger::GetInstance().EndSection(m_name); }

        ScopedCpuSection(const ScopedCpuSection&) = delete;
        ScopedCpuSection& operator=(const ScopedCpuSection&) = delete;

      private:
        const char* m_name;
    };

} // namespace Spark

// ============================================================================
// Convenience macros — active in Debug + Release, no-op in Shipping
// ============================================================================

#if !defined(SPARK_SHIPPING)

/** @brief RAII-scoped CPU section timer with call-site info */
#define SPARK_CPU_SECTION(name)                                                                                        \
    Spark::ScopedCpuSection _cpuSection##__LINE__(name, "General", __FILE__, __LINE__, __FUNCTION__)

/** @brief RAII-scoped CPU section timer with category */
#define SPARK_CPU_SECTION_CAT(name, category)                                                                          \
    Spark::ScopedCpuSection _cpuSection##__LINE__(name, category, __FILE__, __LINE__, __FUNCTION__)

/** @brief Begin a named CPU section manually */
#define SPARK_CPU_BEGIN(name)                                                                                          \
    Spark::CpuDebugger::GetInstance().BeginSection(name, "General", __FILE__, __LINE__, __FUNCTION__)

/** @brief End a named CPU section manually */
#define SPARK_CPU_END(name) Spark::CpuDebugger::GetInstance().EndSection(name)

/** @brief Print CPU timing summary */
#define SPARK_CPU_PRINT_REPORT() Spark::CpuDebugger::GetInstance().PrintSummary()

#else

#define SPARK_CPU_SECTION(name) ((void)0)
#define SPARK_CPU_SECTION_CAT(name, category) ((void)0)
#define SPARK_CPU_BEGIN(name) ((void)0)
#define SPARK_CPU_END(name) ((void)0)
#define SPARK_CPU_PRINT_REPORT() ((void)0)

#endif
