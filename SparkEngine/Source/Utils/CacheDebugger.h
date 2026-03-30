/**
 * @file CacheDebugger.h
 * @brief Generic cache performance tracking and efficiency analysis
 * @author Spark Engine Team
 * @date 2026
 *
 * Tracks hit/miss rates, evictions, and insertions for any engine cache.
 * Caches register by name and report events; the debugger aggregates
 * statistics and identifies inefficient caches.
 *
 * Features:
 * - Named cache registration with optional capacity/budget limits
 * - Per-cache hit/miss/eviction/insertion tracking
 * - Hit rate calculation (per-cache and aggregate)
 * - Inefficient cache detection (below configurable hit rate threshold)
 * - Current entry count and size tracking
 * - Thread-safe for caches accessed from multiple threads
 *
 * @note Active in Debug and Release builds. Compiled to no-ops only in
 *       SPARK_SHIPPING builds. Overhead is minimal per cache event.
 *
 * @see MemoryDebugger.h, Profiler.h
 */

#pragma once

#include "../Core/Platform.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark
{

    /**
     * @brief Aggregated statistics for a single named cache
     */
    struct CacheStats
    {
        std::string cacheName;
        uint64_t hits = 0;
        uint64_t misses = 0;
        uint64_t evictions = 0;
        uint64_t insertions = 0;
        size_t currentEntries = 0;
        size_t maxEntries = 0; ///< Capacity limit (0 = unlimited)
        size_t currentSizeBytes = 0;
        size_t maxSizeBytes = 0;     ///< Budget limit (0 = unlimited)
        float hitRatePercent = 0.0f; ///< hits / (hits + misses) * 100
    };

    /**
     * @brief Singleton cache performance debugger
     */
    class CacheDebugger
    {
      public:
        static CacheDebugger& GetInstance()
        {
            static CacheDebugger instance;
            return instance;
        }

        void SetEnabled(bool enabled) { m_enabled = enabled; }
        bool IsEnabled() const { return m_enabled; }

        // ---- Cache registration ----

        /**
         * @brief Register a named cache for tracking
         * @param name       Unique cache name (e.g., "TextureCache", "ShaderCache")
         * @param maxEntries Maximum entry capacity (0 = unlimited)
         * @param maxSizeBytes Maximum size budget in bytes (0 = unlimited)
         */
        void RegisterCache(const char* name, size_t maxEntries = 0, size_t maxSizeBytes = 0)
        {
            if (!m_enabled || !name)
                return;

            std::lock_guard<std::mutex> lock(m_mutex);

            auto& cache = m_caches[name];
            cache.cacheName = name;
            cache.maxEntries = maxEntries;
            cache.maxSizeBytes = maxSizeBytes;
        }

        /**
         * @brief Unregister a cache (removes all tracking data)
         */
        void UnregisterCache(const char* name)
        {
            if (!name)
                return;

            std::lock_guard<std::mutex> lock(m_mutex);
            m_caches.erase(name);
        }

        // ---- Event recording ----

        void RecordHit(const char* cacheName, const char* key = nullptr)
        {
            if (!m_enabled || !cacheName)
                return;
            (void)key; // Key is available for future per-key analysis

            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_caches.find(cacheName);
            if (it != m_caches.end())
            {
                it->second.hits++;
                UpdateHitRate(it->second);
            }
        }

        void RecordMiss(const char* cacheName, const char* key = nullptr)
        {
            if (!m_enabled || !cacheName)
                return;
            (void)key;

            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_caches.find(cacheName);
            if (it != m_caches.end())
            {
                it->second.misses++;
                UpdateHitRate(it->second);
            }
        }

        void RecordEviction(const char* cacheName, const char* key = nullptr, size_t sizeBytes = 0)
        {
            if (!m_enabled || !cacheName)
                return;
            (void)key;

            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_caches.find(cacheName);
            if (it != m_caches.end())
            {
                it->second.evictions++;
                if (it->second.currentEntries > 0)
                    it->second.currentEntries--;
                if (sizeBytes <= it->second.currentSizeBytes)
                    it->second.currentSizeBytes -= sizeBytes;
            }
        }

        void RecordInsertion(const char* cacheName, const char* key = nullptr, size_t sizeBytes = 0)
        {
            if (!m_enabled || !cacheName)
                return;
            (void)key;

            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_caches.find(cacheName);
            if (it != m_caches.end())
            {
                it->second.insertions++;
                it->second.currentEntries++;
                it->second.currentSizeBytes += sizeBytes;
            }
        }

        void RecordClear(const char* cacheName)
        {
            if (!m_enabled || !cacheName)
                return;

            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_caches.find(cacheName);
            if (it != m_caches.end())
            {
                it->second.currentEntries = 0;
                it->second.currentSizeBytes = 0;
            }
        }

        // ---- State updates ----

        void SetCurrentEntries(const char* cacheName, size_t count)
        {
            if (!m_enabled || !cacheName)
                return;

            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_caches.find(cacheName);
            if (it != m_caches.end())
                it->second.currentEntries = count;
        }

        void SetCurrentSizeBytes(const char* cacheName, size_t bytes)
        {
            if (!m_enabled || !cacheName)
                return;

            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_caches.find(cacheName);
            if (it != m_caches.end())
                it->second.currentSizeBytes = bytes;
        }

        // ---- Queries ----

        /**
         * @brief Get stats for a specific cache
         */
        CacheStats GetCacheStats(const std::string& name) const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_caches.find(name);
            if (it == m_caches.end())
                return {};
            return it->second;
        }

        /**
         * @brief Get all cache stats, sorted by miss rate (worst first)
         */
        std::vector<CacheStats> GetAllCacheStats() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            std::vector<CacheStats> result;
            result.reserve(m_caches.size());
            for (const auto& [name, cache] : m_caches)
                result.push_back(cache);

            // Sort by hit rate ascending (worst performing first)
            std::sort(result.begin(), result.end(),
                      [](const CacheStats& a, const CacheStats& b) { return a.hitRatePercent < b.hitRatePercent; });
            return result;
        }

        /**
         * @brief Get aggregate hit rate across all caches
         */
        float GetOverallHitRate() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            uint64_t totalHits = 0;
            uint64_t totalMisses = 0;
            for (const auto& [name, cache] : m_caches)
            {
                totalHits += cache.hits;
                totalMisses += cache.misses;
            }

            uint64_t total = totalHits + totalMisses;
            if (total == 0)
                return 0.0f;
            return static_cast<float>(totalHits) / static_cast<float>(total) * 100.0f;
        }

        /**
         * @brief Get the number of registered caches
         */
        size_t GetCacheCount() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_caches.size();
        }

        /**
         * @brief Get caches with hit rate below the given threshold
         */
        std::vector<std::string> GetInefficientCaches(float hitRateThreshold = 50.0f) const
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            std::vector<std::string> result;
            for (const auto& [name, cache] : m_caches)
            {
                // Only flag caches that have had enough activity to be meaningful
                uint64_t total = cache.hits + cache.misses;
                if (total > 0 && cache.hitRatePercent < hitRateThreshold)
                    result.push_back(name);
            }
            return result;
        }

        // ---- Reports ----

        /**
         * @brief Print detailed report of all caches to stderr
         */
        void PrintCacheReport() const
        {
            auto stats = GetAllCacheStats();

            fprintf(stderr, "\n===== CACHE DEBUGGER REPORT =====\n");
            fprintf(stderr, "%-25s %8s %8s %8s %8s %8s %8s\n", "Cache", "Hits", "Misses", "Rate%", "Evict", "Entries",
                    "SizeKB");
            fprintf(stderr, "%-25s %8s %8s %8s %8s %8s %8s\n", "-----", "----", "------", "-----", "-----", "-------",
                    "------");

            for (const auto& s : stats)
            {
                fprintf(stderr, "%-25s %8llu %8llu %7.1f%% %8llu %8zu %8zu\n", s.cacheName.c_str(),
                        static_cast<unsigned long long>(s.hits), static_cast<unsigned long long>(s.misses),
                        s.hitRatePercent, static_cast<unsigned long long>(s.evictions), s.currentEntries,
                        s.currentSizeBytes / 1024);
            }
            fprintf(stderr, "=================================\n\n");
        }

        /**
         * @brief Print a compact summary to stderr
         */
        void PrintSummary() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            uint64_t totalHits = 0, totalMisses = 0, totalEvictions = 0;
            for (const auto& [name, cache] : m_caches)
            {
                totalHits += cache.hits;
                totalMisses += cache.misses;
                totalEvictions += cache.evictions;
            }

            uint64_t total = totalHits + totalMisses;
            float overallRate = total > 0 ? static_cast<float>(totalHits) / static_cast<float>(total) * 100.0f : 0.0f;

            fprintf(stderr, "\n===== CACHE DEBUGGER SUMMARY =====\n");
            fprintf(stderr, "Registered caches: %zu\n", m_caches.size());
            fprintf(stderr, "Total hits:        %llu\n", static_cast<unsigned long long>(totalHits));
            fprintf(stderr, "Total misses:      %llu\n", static_cast<unsigned long long>(totalMisses));
            fprintf(stderr, "Overall hit rate:  %.1f%%\n", overallRate);
            fprintf(stderr, "Total evictions:   %llu\n", static_cast<unsigned long long>(totalEvictions));
            fprintf(stderr, "==================================\n\n");
        }

        /**
         * @brief Reset all tracking data
         */
        void Reset()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_caches.clear();
        }

      private:
        CacheDebugger() = default;
        CacheDebugger(const CacheDebugger&) = delete;
        CacheDebugger& operator=(const CacheDebugger&) = delete;

        static void UpdateHitRate(CacheStats& cache)
        {
            uint64_t total = cache.hits + cache.misses;
            if (total > 0)
                cache.hitRatePercent = static_cast<float>(cache.hits) / static_cast<float>(total) * 100.0f;
        }

        mutable std::mutex m_mutex;
        std::unordered_map<std::string, CacheStats> m_caches;
        bool m_enabled = true;
    };

} // namespace Spark

// ============================================================================
// Convenience macros — active in Debug + Release, no-op in Shipping
// ============================================================================

#if !defined(SPARK_SHIPPING)

/** @brief Register a cache for tracking */
#define SPARK_CACHE_REGISTER(name, maxEntries, maxSize)                                                                \
    Spark::CacheDebugger::GetInstance().RegisterCache(name, maxEntries, maxSize)

/** @brief Record a cache hit */
#define SPARK_CACHE_HIT(name) Spark::CacheDebugger::GetInstance().RecordHit(name)

/** @brief Record a cache miss */
#define SPARK_CACHE_MISS(name) Spark::CacheDebugger::GetInstance().RecordMiss(name)

/** @brief Record a cache eviction */
#define SPARK_CACHE_EVICT(name, key, size) Spark::CacheDebugger::GetInstance().RecordEviction(name, key, size)

/** @brief Record a cache insertion */
#define SPARK_CACHE_INSERT(name, key, size) Spark::CacheDebugger::GetInstance().RecordInsertion(name, key, size)

/** @brief Print cache performance summary */
#define SPARK_PRINT_CACHE_REPORT() Spark::CacheDebugger::GetInstance().PrintSummary()

#else

#define SPARK_CACHE_REGISTER(name, maxEntries, maxSize) ((void)0)
#define SPARK_CACHE_HIT(name) ((void)0)
#define SPARK_CACHE_MISS(name) ((void)0)
#define SPARK_CACHE_EVICT(name, key, size) ((void)0)
#define SPARK_CACHE_INSERT(name, key, size) ((void)0)
#define SPARK_PRINT_CACHE_REPORT() ((void)0)

#endif
