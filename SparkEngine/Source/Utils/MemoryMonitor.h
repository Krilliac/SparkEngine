/**
 * @file MemoryMonitor.h
 * @brief Proactive runtime memory health monitoring and anomaly detection
 * @author Spark Engine Team
 * @date 2026
 *
 * Watches memory trends in real-time by reading from MemoryDebugger and flags
 * anomalies (sustained growth, allocation spikes, budget overruns, low stack)
 * before they cause crashes or degraded performance.
 *
 * This is a debug-only complement to MemoryDebugger (which tracks individual
 * allocations) and Profiler (which tracks per-category counters). MemoryMonitor
 * adds trend analysis, budget enforcement, and proactive anomaly detection.
 *
 * Features:
 * - Heap growth rate tracking with configurable leak-rate threshold
 * - Per-category memory budget enforcement with warning/error thresholds
 * - Single-frame allocation spike detection
 * - Allocation fragmentation trend estimation
 * - Stack depth monitoring (platform-specific)
 * - Double-free event monitoring (reads from MemoryDebugger)
 * - On-demand memory snapshots with diff comparison
 * - Ring buffer of recent anomaly events with descriptions
 * - DebugOverlay integration (health status HUD line)
 * - Console commands for manual inspection
 *
 * @note Debug-only — no overhead in Release builds. Depends on MemoryDebugger
 *       being enabled to provide meaningful data.
 *
 * @see MemoryDebugger.h, Profiler.h, DebugOverlay.h
 */

#pragma once

#include "../Core/Platform.h"
#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark
{

    /**
     * @brief Types of memory anomalies the monitor can detect
     */
    enum class MemoryAnomalyType
    {
        LeakSuspected,     ///< Sustained heap growth above threshold
        BudgetWarning,     ///< Category approaching memory budget (>= warning %)
        BudgetExceeded,    ///< Category exceeded memory budget
        AllocationSpike,   ///< Large allocation burst in a single frame
        FragmentationHigh, ///< Rising allocation count with stable total bytes
        StackLow,          ///< Remaining stack space below threshold
        DoubleFree         ///< New double-free events detected this frame
    };

    /**
     * @brief A single detected memory anomaly event
     */
    struct MemoryAnomaly
    {
        MemoryAnomalyType type = MemoryAnomalyType::LeakSuspected;
        std::string description;
        std::chrono::steady_clock::time_point timestamp;
        size_t relevantBytes = 0;
        std::string category; ///< Empty for global anomalies
    };

    /**
     * @brief Point-in-time snapshot of memory state for comparison
     */
    struct MemorySnapshot
    {
        std::chrono::steady_clock::time_point timestamp;
        size_t totalBytes = 0;
        size_t peakBytes = 0;
        size_t activeAllocations = 0;
        uint64_t totalAllocationCount = 0;
        uint64_t doubleFreeCount = 0;

        struct CategoryEntry
        {
            std::string name;
            size_t currentBytes = 0;
            size_t peakBytes = 0;
            uint64_t totalAllocations = 0;
        };
        std::vector<CategoryEntry> categories;
    };

    /**
     * @brief Overall health status of the memory subsystem
     */
    enum class MemoryHealthStatus
    {
        Healthy, ///< No anomalies detected
        Warning, ///< Non-critical anomalies (budget warnings, mild growth)
        Critical ///< Critical anomalies (budget exceeded, suspected leak, low stack)
    };

    /**
     * @brief Proactive memory health monitor — reads from MemoryDebugger
     *        and flags anomalies before they cause problems.
     *
     * Debug-only singleton. In Release builds, Update() is a no-op.
     */
    class MemoryMonitor
    {
      public:
        static constexpr int ANOMALY_RING_SIZE = 64;
        static constexpr int GROWTH_SAMPLE_COUNT = 10;        ///< Samples for growth rate averaging
        static constexpr float DEFAULT_CHECK_INTERVAL = 1.0f; ///< Seconds between full checks

        static MemoryMonitor& GetInstance()
        {
            static MemoryMonitor instance;
            return instance;
        }

        // ====================================================================
        // Lifecycle
        // ====================================================================

        void Initialize();
        void Update(float dt);
        void Shutdown();

        void SetEnabled(bool enabled) { m_enabled = enabled; }
        bool IsEnabled() const { return m_enabled; }

        // ====================================================================
        // Threshold configuration
        // ====================================================================

        /** @brief Set sustained growth rate that triggers a leak warning (bytes/sec) */
        void SetLeakRateThreshold(size_t bytesPerSecond) { m_leakRateThreshold = bytesPerSecond; }

        /** @brief Set single-frame allocation burst threshold (bytes) */
        void SetSpikeThreshold(size_t bytesPerFrame) { m_spikeThreshold = bytesPerFrame; }

        /** @brief Set budget warning percentage (0.0–1.0, default 0.8) */
        void SetBudgetWarningPercent(float percent) { m_budgetWarningPercent = percent; }

        /** @brief Set minimum remaining stack bytes before warning (default 64KB) */
        void SetStackWarningBytes(size_t remaining) { m_stackWarningBytes = remaining; }

        /** @brief Set interval between full health checks (seconds) */
        void SetCheckInterval(float seconds) { m_checkInterval = seconds; }

        // ====================================================================
        // Budget management
        // ====================================================================

        /** @brief Set a memory budget for a specific category (0 = no budget) */
        void SetCategoryBudget(const std::string& category, size_t budgetBytes);

        /** @brief Set a global memory budget (0 = no budget) */
        void SetGlobalBudget(size_t budgetBytes) { m_globalBudget = budgetBytes; }

        /** @brief Get all configured budgets */
        std::unordered_map<std::string, size_t> GetCategoryBudgets() const;

        // ====================================================================
        // Snapshots
        // ====================================================================

        /** @brief Capture current memory state as a snapshot */
        MemorySnapshot TakeSnapshot() const;

        /** @brief Compare two snapshots and return a human-readable diff */
        static std::string CompareSnapshots(const MemorySnapshot& before, const MemorySnapshot& after);

        // ====================================================================
        // Query
        // ====================================================================

        /** @brief Get a formatted status report string */
        std::string GetStatusReport() const;

        /** @brief Get recent anomaly events (most recent first) */
        std::vector<MemoryAnomaly> GetRecentAnomalies() const;

        /** @brief Quick health check */
        MemoryHealthStatus GetHealthStatus() const { return m_healthStatus; }

        /** @brief Check if the monitor considers memory healthy */
        bool IsHealthy() const { return m_healthStatus == MemoryHealthStatus::Healthy; }

      private:
        MemoryMonitor() = default;
        MemoryMonitor(const MemoryMonitor&) = delete;
        MemoryMonitor& operator=(const MemoryMonitor&) = delete;

        // Internal check routines (called from Update at intervals)
        void RunHealthChecks();
        void CheckGrowthRate();
        void CheckCategoryBudgets();
        void CheckAllocationSpike();
        void CheckFragmentation();
        void CheckStackDepth();
        void CheckDoubleFrees();
        void UpdateOverlay();

        void RecordAnomaly(MemoryAnomalyType type, const std::string& description, size_t bytes = 0,
                           const std::string& category = "");

        /** @brief Platform-specific: get remaining stack bytes, or 0 if unavailable */
        static size_t GetRemainingStackBytes();

        // State
        bool m_enabled = false;
        bool m_initialized = false;
        MemoryHealthStatus m_healthStatus = MemoryHealthStatus::Healthy;

        // Timing
        float m_timeSinceLastCheck = 0.0f;
        float m_checkInterval = DEFAULT_CHECK_INTERVAL;

        // Thresholds
        size_t m_leakRateThreshold = 1 * 1024 * 1024; // 1 MB/s
        size_t m_spikeThreshold = 10 * 1024 * 1024;   // 10 MB/frame
        float m_budgetWarningPercent = 0.80f;         // 80%
        size_t m_stackWarningBytes = 64 * 1024;       // 64 KB
        size_t m_globalBudget = 0;                    // 0 = no limit

        // Growth rate tracking (ring buffer of samples)
        struct GrowthSample
        {
            size_t bytes = 0;
            std::chrono::steady_clock::time_point time;
        };
        std::array<GrowthSample, GROWTH_SAMPLE_COUNT> m_growthSamples{};
        int m_growthSampleIndex = 0;
        int m_growthSampleCount = 0;

        // Spike detection (per-frame delta)
        size_t m_lastFrameBytes = 0;
        uint64_t m_lastFrameAllocCount = 0;

        // Fragmentation tracking
        size_t m_lastFragAllocCount = 0;
        size_t m_lastFragTotalBytes = 0;

        // Double-free tracking
        uint64_t m_lastDoubleFreeCount = 0;

        // Budgets
        mutable std::mutex m_mutex;
        std::unordered_map<std::string, size_t> m_categoryBudgets;

        // Anomaly ring buffer
        std::array<MemoryAnomaly, ANOMALY_RING_SIZE> m_anomalies{};
        int m_anomalyWriteIndex = 0;
        int m_anomalyCount = 0;

        // Saved snapshot for comparison
        MemorySnapshot m_lastSnapshot;
    };

} // namespace Spark
