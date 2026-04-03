/**
 * @file AssetStallDetector.cpp
 * @brief Asset loading stall detection — timeout tracking and queue monitoring
 */

#include "../Core/Platform.h"
#include "Utils/AssetStallDetector.h"
#include "Utils/SparkConsole.h"

#include <cstdio>
#include <format>

namespace Spark
{

    AssetStallDetector& AssetStallDetector::GetInstance()
    {
        static AssetStallDetector instance;
        return instance;
    }

    void AssetStallDetector::Initialize()
    {
        if (m_initialized)
            return;

        m_activeLoads.clear();
        m_nextId = 1;
        m_completedLoads = 0;
        m_failedLoads = 0;
        m_totalLoadTimeSec = 0.0f;
        m_worstLoadTimeSec = 0.0f;
        m_queueOverflowCount = 0;
        m_initialized = true;
    }

    void AssetStallDetector::Shutdown()
    {
        m_activeLoads.clear();
        m_initialized = false;
    }

    void AssetStallDetector::Configure(const AssetStallConfig& config)
    {
        m_config = config;
    }

    void AssetStallDetector::Update(float dt)
    {
        if (!m_config.enabled || !m_initialized)
            return;

        for (auto& [id, load] : m_activeLoads)
        {
            load.elapsedSec += dt;

            if (!load.warned && load.elapsedSec >= m_config.warningThresholdSec)
            {
                load.warned = true;
                std::fprintf(stderr, "[AssetStallDetector] WARNING: '%s' loading for %.1f seconds\n", load.name.c_str(),
                             load.elapsedSec);
            }
        }
    }

    uint64_t AssetStallDetector::OnLoadBegin(const std::string& name, AssetLoadType type)
    {
        if (!m_config.enabled || !m_initialized)
            return 0;

        uint64_t id = m_nextId++;
        m_activeLoads[id] = ActiveLoad{name, type, 0.0f, false};

        if (m_activeLoads.size() > m_config.maxConcurrentLoads)
        {
            m_queueOverflowCount++;
            std::fprintf(stderr, "[AssetStallDetector] WARNING: Concurrent loads (%zu) exceed limit (%u)\n",
                         m_activeLoads.size(), m_config.maxConcurrentLoads);
        }

        return id;
    }

    void AssetStallDetector::OnLoadComplete(uint64_t id)
    {
        FinishLoad(id, true);
    }

    void AssetStallDetector::OnLoadFailed(uint64_t id)
    {
        FinishLoad(id, false);
    }

    void AssetStallDetector::FinishLoad(uint64_t id, bool success)
    {
        auto it = m_activeLoads.find(id);
        if (it == m_activeLoads.end())
            return;

        float elapsed = it->second.elapsedSec;
        m_totalLoadTimeSec += elapsed;

        if (elapsed > m_worstLoadTimeSec)
            m_worstLoadTimeSec = elapsed;

        if (success)
            m_completedLoads++;
        else
            m_failedLoads++;

        m_activeLoads.erase(it);
    }

    AssetStallStatus AssetStallDetector::GetStatus() const
    {
        AssetStallStatus status;
        status.activeLoads = static_cast<uint32_t>(m_activeLoads.size());
        status.completedLoads = m_completedLoads;
        status.failedLoads = m_failedLoads;
        status.worstLoadTimeSec = m_worstLoadTimeSec;
        status.queueOverflowCount = m_queueOverflowCount;

        uint32_t totalFinished = m_completedLoads + m_failedLoads;
        status.averageLoadTimeSec =
            (totalFinished > 0) ? (m_totalLoadTimeSec / static_cast<float>(totalFinished)) : 0.0f;

        // Count currently stalled loads
        uint32_t stalled = 0;
        for (const auto& [id, load] : m_activeLoads)
        {
            if (load.elapsedSec >= m_config.stallThresholdSec)
                stalled++;
        }
        status.stalledLoads = stalled;

        return status;
    }

    std::string AssetStallDetector::FormatActiveLoads() const
    {
        if (m_activeLoads.empty())
            return "No active loads.";

        std::string result;
        for (const auto& [id, load] : m_activeLoads)
        {
            bool stalled = load.elapsedSec >= m_config.stallThresholdSec;
            result +=
                std::format("  [{}] '{}' — {:.2f}s{}\n", id, load.name, load.elapsedSec, stalled ? " [STALLED]" : "");
        }
        return result;
    }

    // ========================================================================
    // Console commands
    // ========================================================================

    void AssetStallDetector::RegisterConsoleCommands()
    {
        auto& console = SimpleConsole::GetInstance();

        console.RegisterCommand(
            "assetstall.status",
            [](const std::vector<std::string>&) -> std::string
            {
                auto status = AssetStallDetector::GetInstance().GetStatus();
                return std::format("Active: {} | Stalled: {} | Completed: {} | Failed: {}\n"
                                   "Avg load: {:.2f}s | Worst: {:.2f}s | Queue overflows: {}",
                                   status.activeLoads, status.stalledLoads, status.completedLoads, status.failedLoads,
                                   status.averageLoadTimeSec, status.worstLoadTimeSec, status.queueOverflowCount);
            },
            "Show asset stall detector status", "AssetStallDetector");

        console.RegisterCommand(
            "assetstall.config",
            [](const std::vector<std::string>&) -> std::string
            {
                const auto& cfg = AssetStallDetector::GetInstance().GetConfig();
                return std::format("Enabled: {} | Warning: {:.1f}s | Stall: {:.1f}s | Max concurrent: {}",
                                   cfg.enabled ? "yes" : "no", cfg.warningThresholdSec, cfg.stallThresholdSec,
                                   cfg.maxConcurrentLoads);
            },
            "Show asset stall detector configuration", "AssetStallDetector");

        console.RegisterCommand(
            "assetstall.active", [](const std::vector<std::string>&) -> std::string
            { return AssetStallDetector::GetInstance().FormatActiveLoads(); },
            "List currently loading assets with elapsed time", "AssetStallDetector");
    }

} // namespace Spark
