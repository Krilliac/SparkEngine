/**
 * @file MemoryMonitor.cpp
 * @brief Proactive runtime memory health monitoring implementation
 *
 * Reads from MemoryDebugger each frame (at configurable intervals) to detect
 * anomalies: sustained heap growth, budget overruns, allocation spikes,
 * fragmentation trends, low stack, and double-frees. Reports anomalies
 * through Logger and DebugOverlay.
 */

#include "MemoryMonitor.h"
#include "DebugOverlay.h"
#include "LogMacros.h"
#include "MemoryDebugger.h"

#include <algorithm>
#include <cstdio>
#include <sstream>

// Platform-specific stack depth detection
#if defined(__linux__)
#include <pthread.h>
#elif defined(_WIN32)
// Windows stack query via TEB — not available in cross-compile, guarded below
#endif

namespace Spark
{

    // ========================================================================
    // Lifecycle
    // ========================================================================

    void MemoryMonitor::Initialize()
    {
        if (m_initialized)
            return;

#ifndef NDEBUG
        m_enabled = true;
#endif

        m_initialized = true;
        m_timeSinceLastCheck = 0.0f;
        m_healthStatus = MemoryHealthStatus::Healthy;
        m_anomalyWriteIndex = 0;
        m_anomalyCount = 0;
        m_growthSampleIndex = 0;
        m_growthSampleCount = 0;

        // Capture initial state for spike detection
        auto& debugger = MemoryDebugger::GetInstance();
        m_lastFrameBytes = debugger.GetCurrentBytes();
        m_lastFrameAllocCount = debugger.GetTotalAllocationCount();
        m_lastDoubleFreeCount = debugger.GetDoubleFreeCount();
        m_lastFragAllocCount = debugger.GetActiveAllocationCount();
        m_lastFragTotalBytes = debugger.GetCurrentBytes();

        // Take initial snapshot for comparisons
        m_lastSnapshot = TakeSnapshot();

        SPARK_LOG_INFO(Spark::LogCategory::Core, "MemoryMonitor initialized (check interval: %.1fs)", m_checkInterval);
    }

    void MemoryMonitor::Update(float dt)
    {
        if (!m_enabled || !m_initialized)
            return;

        // Quick per-frame checks (allocation spikes, double-frees)
        CheckAllocationSpike();
        CheckDoubleFrees();

        // Interval-based deeper checks
        m_timeSinceLastCheck += dt;
        if (m_timeSinceLastCheck >= m_checkInterval)
        {
            m_timeSinceLastCheck = 0.0f;
            RunHealthChecks();
        }

        UpdateOverlay();
    }

    void MemoryMonitor::Shutdown()
    {
        if (!m_initialized)
            return;

        // Log final anomaly summary
        if (m_anomalyCount > 0)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core,
                           "MemoryMonitor shutting down — %d anomalies detected during session", m_anomalyCount);
        }
        else
        {
            SPARK_LOG_INFO(Spark::LogCategory::Core, "MemoryMonitor shutting down — no anomalies detected");
        }

        DebugOverlay::GetInstance().RemoveCustomLine("MemHealth");
        m_initialized = false;
        m_enabled = false;
    }

    // ========================================================================
    // Health checks (called at intervals)
    // ========================================================================

    void MemoryMonitor::RunHealthChecks()
    {
        CheckGrowthRate();
        CheckCategoryBudgets();
        CheckFragmentation();
        CheckStackDepth();

        // Determine overall health based on recent anomalies
        auto now = std::chrono::steady_clock::now();
        bool hasCritical = false;
        bool hasWarning = false;

        auto recentAnomalies = GetRecentAnomalies();
        for (const auto& anomaly : recentAnomalies)
        {
            auto age = std::chrono::duration_cast<std::chrono::seconds>(now - anomaly.timestamp).count();
            if (age > 30)
                continue; // Only consider recent anomalies for health status

            switch (anomaly.type)
            {
            case MemoryAnomalyType::BudgetExceeded:
            case MemoryAnomalyType::LeakSuspected:
            case MemoryAnomalyType::StackLow:
            case MemoryAnomalyType::DoubleFree:
                hasCritical = true;
                break;
            case MemoryAnomalyType::BudgetWarning:
            case MemoryAnomalyType::AllocationSpike:
            case MemoryAnomalyType::FragmentationHigh:
                hasWarning = true;
                break;
            }
        }

        MemoryHealthStatus previousStatus = m_healthStatus;
        if (hasCritical)
            m_healthStatus = MemoryHealthStatus::Critical;
        else if (hasWarning)
            m_healthStatus = MemoryHealthStatus::Warning;
        else
            m_healthStatus = MemoryHealthStatus::Healthy;

        // Fire pressure callbacks on transition to Warning or Critical
        if (m_healthStatus != MemoryHealthStatus::Healthy && m_healthStatus != previousStatus)
        {
            InvokePressureCallbacks(m_healthStatus);
        }
    }

    void MemoryMonitor::RegisterPressureCallback(const std::string& name, PressureCallback callback)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pressureCallbacks[name] = std::move(callback);
    }

    void MemoryMonitor::UnregisterPressureCallback(const std::string& name)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pressureCallbacks.erase(name);
    }

    void MemoryMonitor::InvokePressureCallbacks(MemoryHealthStatus status)
    {
        auto now = std::chrono::steady_clock::now();
        auto elapsed =
            std::chrono::duration_cast<std::chrono::duration<float>>(now - m_lastPressureCallbackTime).count();
        if (elapsed < PRESSURE_CALLBACK_COOLDOWN)
            return;
        m_lastPressureCallbackTime = now;

        // Copy callbacks under lock, invoke outside lock to avoid deadlocks
        std::vector<std::pair<std::string, PressureCallback>> callbacksCopy;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            callbacksCopy.reserve(m_pressureCallbacks.size());
            for (const auto& [name, cb] : m_pressureCallbacks)
                callbacksCopy.emplace_back(name, cb);
        }

        SPARK_LOG_WARN(Spark::LogCategory::Core, "[MemoryMonitor] Invoking %zu pressure callbacks (status: %s)",
                       callbacksCopy.size(), status == MemoryHealthStatus::Critical ? "CRITICAL" : "WARNING");

        for (const auto& [name, cb] : callbacksCopy)
        {
            try
            {
                cb(status);
                SPARK_LOG_INFO(Spark::LogCategory::Core, "[MemoryMonitor] Pressure callback '%s' completed",
                               name.c_str());
            }
            catch (const std::exception& ex)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Core, "[MemoryMonitor] Pressure callback '%s' threw: %s",
                                name.c_str(), ex.what());
            }
        }
    }

    void MemoryMonitor::CheckGrowthRate()
    {
        auto& debugger = MemoryDebugger::GetInstance();
        if (!debugger.IsEnabled())
            return;

        // Record current sample
        GrowthSample sample;
        sample.bytes = debugger.GetCurrentBytes();
        sample.time = std::chrono::steady_clock::now();
        m_growthSamples[m_growthSampleIndex] = sample;
        m_growthSampleIndex = (m_growthSampleIndex + 1) % GROWTH_SAMPLE_COUNT;
        if (m_growthSampleCount < GROWTH_SAMPLE_COUNT)
            m_growthSampleCount++;

        // Need at least 3 samples to compute a meaningful rate
        if (m_growthSampleCount < 3)
            return;

        // Compare oldest available sample to newest
        int oldestIndex = (m_growthSampleIndex - m_growthSampleCount + GROWTH_SAMPLE_COUNT) % GROWTH_SAMPLE_COUNT;
        int newestIndex = (m_growthSampleIndex - 1 + GROWTH_SAMPLE_COUNT) % GROWTH_SAMPLE_COUNT;
        const auto& oldest = m_growthSamples[oldestIndex];
        const auto& newest = m_growthSamples[newestIndex];

        auto elapsed = std::chrono::duration<double>(newest.time - oldest.time).count();
        if (elapsed < 1.0)
            return; // Too short to measure

        // Only flag if memory is growing (not shrinking)
        if (newest.bytes <= oldest.bytes)
            return;

        double growthRate = static_cast<double>(newest.bytes - oldest.bytes) / elapsed;
        if (growthRate > static_cast<double>(m_leakRateThreshold))
        {
            double growthMBPerSec = growthRate / (1024.0 * 1024.0);
            char desc[256];
            snprintf(desc, sizeof(desc), "Sustained heap growth: %.2f MB/s over %.1fs (threshold: %zu KB/s)",
                     growthMBPerSec, elapsed, m_leakRateThreshold / 1024);
            RecordAnomaly(MemoryAnomalyType::LeakSuspected, desc, static_cast<size_t>(growthRate));
        }
    }

    void MemoryMonitor::CheckCategoryBudgets()
    {
        auto& debugger = MemoryDebugger::GetInstance();
        if (!debugger.IsEnabled())
            return;

        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_categoryBudgets.empty() && m_globalBudget == 0)
            return;

        auto categories = debugger.GetCategoryStats();

        // Global budget check
        if (m_globalBudget > 0)
        {
            size_t totalBytes = debugger.GetCurrentBytes();
            float usage = static_cast<float>(totalBytes) / static_cast<float>(m_globalBudget);
            if (totalBytes > m_globalBudget)
            {
                char desc[256];
                snprintf(desc, sizeof(desc), "Global memory budget exceeded: %zu MB / %zu MB (%.0f%%)",
                         totalBytes / (1024 * 1024), m_globalBudget / (1024 * 1024), usage * 100.0f);
                RecordAnomaly(MemoryAnomalyType::BudgetExceeded, desc, totalBytes);
            }
            else if (usage >= m_budgetWarningPercent)
            {
                char desc[256];
                snprintf(desc, sizeof(desc), "Global memory approaching budget: %zu MB / %zu MB (%.0f%%)",
                         totalBytes / (1024 * 1024), m_globalBudget / (1024 * 1024), usage * 100.0f);
                RecordAnomaly(MemoryAnomalyType::BudgetWarning, desc, totalBytes);
            }
        }

        // Per-category budget check
        for (const auto& cat : categories)
        {
            auto budgetIt = m_categoryBudgets.find(cat.name);
            if (budgetIt == m_categoryBudgets.end())
                continue;

            size_t budget = budgetIt->second;
            if (budget == 0)
                continue;

            float usage = static_cast<float>(cat.currentBytes) / static_cast<float>(budget);
            if (cat.currentBytes > budget)
            {
                char desc[256];
                snprintf(desc, sizeof(desc), "%s budget exceeded: %zu KB / %zu KB (%.0f%%)", cat.name.c_str(),
                         cat.currentBytes / 1024, budget / 1024, usage * 100.0f);
                RecordAnomaly(MemoryAnomalyType::BudgetExceeded, desc, cat.currentBytes, cat.name);
            }
            else if (usage >= m_budgetWarningPercent)
            {
                char desc[256];
                snprintf(desc, sizeof(desc), "%s approaching budget: %zu KB / %zu KB (%.0f%%)", cat.name.c_str(),
                         cat.currentBytes / 1024, budget / 1024, usage * 100.0f);
                RecordAnomaly(MemoryAnomalyType::BudgetWarning, desc, cat.currentBytes, cat.name);
            }
        }
    }

    void MemoryMonitor::CheckAllocationSpike()
    {
        auto& debugger = MemoryDebugger::GetInstance();
        if (!debugger.IsEnabled())
            return;

        size_t currentBytes = debugger.GetCurrentBytes();

        // Only flag positive spikes (sudden growth)
        if (currentBytes > m_lastFrameBytes)
        {
            size_t delta = currentBytes - m_lastFrameBytes;
            if (delta > m_spikeThreshold)
            {
                char desc[256];
                snprintf(desc, sizeof(desc), "Allocation spike: +%zu KB in one frame (threshold: %zu KB)", delta / 1024,
                         m_spikeThreshold / 1024);
                RecordAnomaly(MemoryAnomalyType::AllocationSpike, desc, delta);
            }
        }

        m_lastFrameBytes = currentBytes;
        m_lastFrameAllocCount = debugger.GetTotalAllocationCount();
    }

    void MemoryMonitor::CheckFragmentation()
    {
        auto& debugger = MemoryDebugger::GetInstance();
        if (!debugger.IsEnabled())
            return;

        size_t currentAllocCount = debugger.GetActiveAllocationCount();
        size_t currentTotalBytes = debugger.GetCurrentBytes();

        // Fragmentation signal: allocation count rising while total bytes stable
        // (many small allocations replacing fewer large ones)
        if (m_lastFragTotalBytes > 0 && currentTotalBytes > 0)
        {
            // Only check if total bytes hasn't grown much (within 10%)
            double bytesRatio = static_cast<double>(currentTotalBytes) / static_cast<double>(m_lastFragTotalBytes);
            if (bytesRatio >= 0.9 && bytesRatio <= 1.1 && m_lastFragAllocCount > 0)
            {
                double allocRatio = static_cast<double>(currentAllocCount) / static_cast<double>(m_lastFragAllocCount);
                // Allocation count grew by >50% while bytes stayed stable
                if (allocRatio > 1.5 && currentAllocCount > 100)
                {
                    char desc[256];
                    snprintf(desc, sizeof(desc),
                             "Fragmentation trend: alloc count %zu -> %zu (+%.0f%%) "
                             "while bytes stable at ~%zu KB",
                             m_lastFragAllocCount, currentAllocCount, (allocRatio - 1.0) * 100.0,
                             currentTotalBytes / 1024);
                    RecordAnomaly(MemoryAnomalyType::FragmentationHigh, desc, currentTotalBytes);
                }
            }
        }

        m_lastFragAllocCount = currentAllocCount;
        m_lastFragTotalBytes = currentTotalBytes;
    }

    void MemoryMonitor::CheckStackDepth()
    {
        size_t remaining = GetRemainingStackBytes();
        if (remaining == 0)
            return; // Platform doesn't support this check

        if (remaining < m_stackWarningBytes)
        {
            char desc[256];
            snprintf(desc, sizeof(desc), "Low stack space: %zu KB remaining (threshold: %zu KB)", remaining / 1024,
                     m_stackWarningBytes / 1024);
            RecordAnomaly(MemoryAnomalyType::StackLow, desc, remaining);
        }
    }

    void MemoryMonitor::CheckDoubleFrees()
    {
        auto& debugger = MemoryDebugger::GetInstance();
        if (!debugger.IsEnabled())
            return;

        uint64_t current = debugger.GetDoubleFreeCount();
        if (current > m_lastDoubleFreeCount)
        {
            uint64_t newCount = current - m_lastDoubleFreeCount;
            char desc[256];
            snprintf(desc, sizeof(desc), "Double-free detected: %llu new event(s) (total: %llu)",
                     static_cast<unsigned long long>(newCount), static_cast<unsigned long long>(current));
            RecordAnomaly(MemoryAnomalyType::DoubleFree, desc);
        }
        m_lastDoubleFreeCount = current;
    }

    // ========================================================================
    // Overlay integration
    // ========================================================================

    void MemoryMonitor::UpdateOverlay()
    {
        auto& overlay = DebugOverlay::GetInstance();
        if (!overlay.IsEnabled())
            return;

        auto& debugger = MemoryDebugger::GetInstance();
        size_t currentMB = debugger.GetCurrentBytes() / (1024 * 1024);
        size_t peakMB = debugger.GetPeakBytes() / (1024 * 1024);

        char status[128];
        DirectX::XMFLOAT4 color;

        switch (m_healthStatus)
        {
        case MemoryHealthStatus::Healthy:
            snprintf(status, sizeof(status), "OK | %zu MB (peak %zu MB)", currentMB, peakMB);
            color = {0.0f, 1.0f, 0.0f, 1.0f}; // Green
            break;
        case MemoryHealthStatus::Warning:
            snprintf(status, sizeof(status), "WARN | %zu MB (peak %zu MB) | %d anomalies", currentMB, peakMB,
                     m_anomalyCount);
            color = {1.0f, 1.0f, 0.0f, 1.0f}; // Yellow
            break;
        case MemoryHealthStatus::Critical:
            snprintf(status, sizeof(status), "CRIT | %zu MB (peak %zu MB) | %d anomalies", currentMB, peakMB,
                     m_anomalyCount);
            color = {1.0f, 0.0f, 0.0f, 1.0f}; // Red
            break;
        }

        overlay.AddCustomLine("MemHealth", status, color);
    }

    // ========================================================================
    // Anomaly recording
    // ========================================================================

    void MemoryMonitor::RecordAnomaly(MemoryAnomalyType type, const std::string& description, size_t bytes,
                                      const std::string& category)
    {
        MemoryAnomaly anomaly;
        anomaly.type = type;
        anomaly.description = description;
        anomaly.timestamp = std::chrono::steady_clock::now();
        anomaly.relevantBytes = bytes;
        anomaly.category = category;

        m_anomalies[m_anomalyWriteIndex] = anomaly;
        m_anomalyWriteIndex = (m_anomalyWriteIndex + 1) % ANOMALY_RING_SIZE;
        if (m_anomalyCount < ANOMALY_RING_SIZE)
            m_anomalyCount++;

        // Log based on severity
        bool isCritical = (type == MemoryAnomalyType::BudgetExceeded || type == MemoryAnomalyType::LeakSuspected ||
                           type == MemoryAnomalyType::StackLow || type == MemoryAnomalyType::DoubleFree);

        if (isCritical)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "[MemoryMonitor] %s", description.c_str());
        }
        else
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "[MemoryMonitor] %s", description.c_str());
        }
    }

    // ========================================================================
    // Budget management
    // ========================================================================

    void MemoryMonitor::SetCategoryBudget(const std::string& category, size_t budgetBytes)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (budgetBytes == 0)
            m_categoryBudgets.erase(category);
        else
            m_categoryBudgets[category] = budgetBytes;
    }

    std::unordered_map<std::string, size_t> MemoryMonitor::GetCategoryBudgets() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_categoryBudgets;
    }

    // ========================================================================
    // Snapshots
    // ========================================================================

    MemorySnapshot MemoryMonitor::TakeSnapshot() const
    {
        auto& debugger = MemoryDebugger::GetInstance();

        MemorySnapshot snap;
        snap.timestamp = std::chrono::steady_clock::now();
        snap.totalBytes = debugger.GetCurrentBytes();
        snap.peakBytes = debugger.GetPeakBytes();
        snap.activeAllocations = debugger.GetActiveAllocationCount();
        snap.totalAllocationCount = debugger.GetTotalAllocationCount();
        snap.doubleFreeCount = debugger.GetDoubleFreeCount();

        auto categories = debugger.GetCategoryStats();
        snap.categories.reserve(categories.size());
        for (const auto& cat : categories)
        {
            MemorySnapshot::CategoryEntry entry;
            entry.name = cat.name;
            entry.currentBytes = cat.currentBytes;
            entry.peakBytes = cat.peakBytes;
            entry.totalAllocations = cat.totalAllocations;
            snap.categories.push_back(entry);
        }

        return snap;
    }

    std::string MemoryMonitor::CompareSnapshots(const MemorySnapshot& before, const MemorySnapshot& after)
    {
        std::ostringstream ss;
        ss << "=== Memory Snapshot Comparison ===\n";

        auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(after.timestamp - before.timestamp).count();
        ss << "Time elapsed: " << elapsed << " ms\n";

        // Total bytes diff
        auto bytesDiff = static_cast<int64_t>(after.totalBytes) - static_cast<int64_t>(before.totalBytes);
        ss << "Total bytes: " << before.totalBytes / 1024 << " KB -> " << after.totalBytes / 1024 << " KB ("
           << (bytesDiff >= 0 ? "+" : "") << bytesDiff / 1024 << " KB)\n";

        // Allocation count diff
        auto allocDiff = static_cast<int64_t>(after.activeAllocations) - static_cast<int64_t>(before.activeAllocations);
        ss << "Active allocations: " << before.activeAllocations << " -> " << after.activeAllocations << " ("
           << (allocDiff >= 0 ? "+" : "") << allocDiff << ")\n";

        // Per-category changes (only show categories that changed)
        ss << "\nPer-category changes:\n";
        std::unordered_map<std::string, size_t> beforeMap;
        for (const auto& cat : before.categories)
            beforeMap[cat.name] = cat.currentBytes;

        for (const auto& cat : after.categories)
        {
            size_t prev = 0;
            auto it = beforeMap.find(cat.name);
            if (it != beforeMap.end())
                prev = it->second;

            auto diff = static_cast<int64_t>(cat.currentBytes) - static_cast<int64_t>(prev);
            if (diff != 0)
            {
                ss << "  " << cat.name << ": " << prev / 1024 << " KB -> " << cat.currentBytes / 1024 << " KB ("
                   << (diff >= 0 ? "+" : "") << diff / 1024 << " KB)\n";
            }
        }

        return ss.str();
    }

    // ========================================================================
    // Query
    // ========================================================================

    std::string MemoryMonitor::GetStatusReport() const
    {
        auto& debugger = MemoryDebugger::GetInstance();

        std::ostringstream ss;
        ss << "=== Memory Monitor Status ===\n";

        const char* healthStr = "HEALTHY";
        if (m_healthStatus == MemoryHealthStatus::Warning)
            healthStr = "WARNING";
        else if (m_healthStatus == MemoryHealthStatus::Critical)
            healthStr = "CRITICAL";
        ss << "Health: " << healthStr << "\n";

        ss << "Current: " << debugger.GetCurrentBytes() / 1024 << " KB\n";
        ss << "Peak: " << debugger.GetPeakBytes() / 1024 << " KB\n";
        ss << "Active allocations: " << debugger.GetActiveAllocationCount() << "\n";
        ss << "Total allocs: " << debugger.GetTotalAllocationCount() << "\n";
        ss << "Double-frees: " << debugger.GetDoubleFreeCount() << "\n";
        ss << "Anomalies recorded: " << m_anomalyCount << "\n";

        // Show budgets
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_globalBudget > 0)
                ss << "Global budget: " << m_globalBudget / (1024 * 1024) << " MB\n";
            if (!m_categoryBudgets.empty())
            {
                ss << "Category budgets:\n";
                for (const auto& [name, budget] : m_categoryBudgets)
                    ss << "  " << name << ": " << budget / (1024 * 1024) << " MB\n";
            }
        }

        return ss.str();
    }

    std::vector<MemoryAnomaly> MemoryMonitor::GetRecentAnomalies() const
    {
        std::vector<MemoryAnomaly> result;
        result.reserve(m_anomalyCount);

        // Read from ring buffer, most recent first
        for (int i = 0; i < m_anomalyCount; i++)
        {
            int idx = (m_anomalyWriteIndex - 1 - i + ANOMALY_RING_SIZE) % ANOMALY_RING_SIZE;
            result.push_back(m_anomalies[idx]);
        }
        return result;
    }

    // ========================================================================
    // Platform-specific stack depth
    // ========================================================================

    size_t MemoryMonitor::GetRemainingStackBytes()
    {
#if defined(__linux__)
        pthread_attr_t attr;
        if (pthread_getattr_np(pthread_self(), &attr) != 0)
            return 0;

        void* stackAddr = nullptr;
        size_t stackSize = 0;
        if (pthread_attr_getstack(&attr, &stackAddr, &stackSize) != 0)
        {
            pthread_attr_destroy(&attr);
            return 0;
        }
        pthread_attr_destroy(&attr);

        // Stack grows downward: remaining = current SP - stack base
        auto* stackBase = static_cast<char*>(stackAddr);
        char localVar = 0;
        auto* currentSP = &localVar;

        if (currentSP < stackBase || currentSP > stackBase + stackSize)
            return 0; // Sanity check

        size_t remaining = static_cast<size_t>(currentSP - stackBase);
        return remaining;
#elif defined(_WIN32)
        // Windows: use NtCurrentTeb() to get stack limits
        // Not available in cross-compile environments; return 0 as fallback
        return 0;
#else
        return 0;
#endif
    }

} // namespace Spark
