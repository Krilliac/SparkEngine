/**
 * @file GPUResourceLeakDetector.cpp
 * @brief GPU resource lifecycle tracking — leak detection and budget monitoring
 */

#include "../Core/Platform.h"
#include "Utils/GPUResourceLeakDetector.h"
#include "Utils/SparkConsole.h"

#include <cstdio>
#include <format>

namespace Spark
{

    GPUResourceLeakDetector& GPUResourceLeakDetector::GetInstance()
    {
        static GPUResourceLeakDetector instance;
        return instance;
    }

    void GPUResourceLeakDetector::Initialize()
    {
        if (m_initialized)
            return;

        m_resources.clear();
        m_allocatedByType.fill(0);
        m_freedByType.fill(0);
        m_currentMemoryBytes = 0;
        m_peakMemoryBytes = 0;
        m_frameCounter = 0;
        m_suspectedLeaks = 0;
        m_initialized = true;
    }

    void GPUResourceLeakDetector::Shutdown()
    {
        if (!m_initialized)
            return;

        // Report any still-alive resources at shutdown
        if (!m_resources.empty())
        {
            std::fprintf(stderr, "[GPUResourceLeakDetector] %zu resources still alive at shutdown\n",
                         m_resources.size());
        }

        m_resources.clear();
        m_initialized = false;
    }

    void GPUResourceLeakDetector::Configure(const GPUResourceLeakConfig& config)
    {
        m_config = config;
    }

    void GPUResourceLeakDetector::Update(float dt)
    {
        if (!m_config.enabled || !m_initialized)
            return;

        // Advance age timers for all tracked resources
        for (auto& [id, res] : m_resources)
        {
            res.ageSec += dt;
            res.lastAccessSec += dt;
        }

        m_frameCounter++;

        // Periodic leak scan
        if (m_frameCounter % m_config.checkIntervalFrames == 0)
            ScanForLeaks();
    }

    void GPUResourceLeakDetector::OnResourceCreated(GPUResourceType type, uint64_t id, size_t sizeBytes)
    {
        if (!m_config.enabled || !m_initialized)
            return;

        uint32_t typeIdx = static_cast<uint32_t>(type);
        if (typeIdx < GPU_RESOURCE_TYPE_COUNT)
            m_allocatedByType[typeIdx]++;

        m_resources[id] = TrackedResource{type, sizeBytes, 0.0f, 0.0f};
        m_currentMemoryBytes += sizeBytes;

        if (m_currentMemoryBytes > m_peakMemoryBytes)
            m_peakMemoryBytes = m_currentMemoryBytes;

        // Budget warning
        float currentMB = static_cast<float>(m_currentMemoryBytes) / (1024.0f * 1024.0f);
        if (currentMB > m_config.budgetMB)
        {
            std::fprintf(stderr,
                         "[GPUResourceLeakDetector] WARNING: GPU memory ({:.1f} MB) exceeds budget ({:.1f} MB)\n",
                         currentMB, m_config.budgetMB);
        }
    }

    void GPUResourceLeakDetector::OnResourceDestroyed(GPUResourceType type, uint64_t id)
    {
        if (!m_initialized)
            return;

        auto it = m_resources.find(id);
        if (it == m_resources.end())
            return;

        uint32_t typeIdx = static_cast<uint32_t>(type);
        if (typeIdx < GPU_RESOURCE_TYPE_COUNT)
            m_freedByType[typeIdx]++;

        if (m_currentMemoryBytes >= it->second.sizeBytes)
            m_currentMemoryBytes -= it->second.sizeBytes;
        else
            m_currentMemoryBytes = 0;

        m_resources.erase(it);
    }

    void GPUResourceLeakDetector::OnResourceAccessed(uint64_t id)
    {
        if (!m_initialized)
            return;

        auto it = m_resources.find(id);
        if (it != m_resources.end())
            it->second.lastAccessSec = 0.0f;
    }

    void GPUResourceLeakDetector::ScanForLeaks()
    {
        uint32_t leaks = 0;
        for (const auto& [id, res] : m_resources)
        {
            if (res.lastAccessSec >= m_config.leakAgeThresholdSec)
                leaks++;
        }
        m_suspectedLeaks = leaks;
    }

    GPUResourceLeakStatus GPUResourceLeakDetector::GetStatus() const
    {
        GPUResourceLeakStatus status;
        status.allocatedByType = m_allocatedByType;
        status.freedByType = m_freedByType;
        status.suspectedLeaks = m_suspectedLeaks;
        status.estimatedMemoryMB = static_cast<float>(m_currentMemoryBytes) / (1024.0f * 1024.0f);
        status.peakMemoryMB = static_cast<float>(m_peakMemoryBytes) / (1024.0f * 1024.0f);

        status.totalAllocations = 0;
        status.totalFrees = 0;
        for (uint32_t i = 0; i < GPU_RESOURCE_TYPE_COUNT; i++)
        {
            status.outstandingByType[i] = m_allocatedByType[i] - m_freedByType[i];
            status.totalAllocations += m_allocatedByType[i];
            status.totalFrees += m_freedByType[i];
        }

        return status;
    }

    static const char* ResourceTypeToString(GPUResourceType type)
    {
        switch (type)
        {
        case GPUResourceType::Texture:
            return "Texture";
        case GPUResourceType::Buffer:
            return "Buffer";
        case GPUResourceType::RenderTarget:
            return "RenderTarget";
        case GPUResourceType::Shader:
            return "Shader";
        case GPUResourceType::PipelineState:
            return "PipelineState";
        case GPUResourceType::Sampler:
            return "Sampler";
        default:
            return "Unknown";
        }
    }

    std::string GPUResourceLeakDetector::FormatDetails() const
    {
        auto status = GetStatus();
        std::string result;
        result += std::format("GPU Memory: {:.1f} MB (peak {:.1f} MB, budget {:.1f} MB)\n", status.estimatedMemoryMB,
                              status.peakMemoryMB, m_config.budgetMB);
        result += std::format("Suspected leaks: {}\n\n", status.suspectedLeaks);
        result += "Type           | Alloc | Freed | Outstanding\n";
        result += "---------------|-------|-------|------------\n";

        for (uint32_t i = 0; i < GPU_RESOURCE_TYPE_COUNT; i++)
        {
            auto type = static_cast<GPUResourceType>(i);
            result += std::format("{:<15}| {:<6}| {:<6}| {}\n", ResourceTypeToString(type), status.allocatedByType[i],
                                  status.freedByType[i], status.outstandingByType[i]);
        }
        return result;
    }

    // ========================================================================
    // Console commands
    // ========================================================================

    void GPUResourceLeakDetector::RegisterConsoleCommands()
    {
        auto& console = SimpleConsole::GetInstance();

        console.RegisterCommand(
            "gpuleak.status",
            [](const std::vector<std::string>&) -> std::string
            {
                auto status = GPUResourceLeakDetector::GetInstance().GetStatus();
                return std::format("Memory: {:.1f} MB (peak {:.1f} MB) | Allocs: {} | Frees: {} | Outstanding: {}\n"
                                   "Suspected leaks: {}",
                                   status.estimatedMemoryMB, status.peakMemoryMB, status.totalAllocations,
                                   status.totalFrees, status.totalAllocations - status.totalFrees,
                                   status.suspectedLeaks);
            },
            "Show GPU resource leak detector status", "GPULeakDetector");

        console.RegisterCommand(
            "gpuleak.config",
            [](const std::vector<std::string>&) -> std::string
            {
                const auto& cfg = GPUResourceLeakDetector::GetInstance().GetConfig();
                return std::format("Enabled: {} | Leak age: {:.1f}s | Check interval: {} frames | Budget: {:.1f} MB",
                                   cfg.enabled ? "yes" : "no", cfg.leakAgeThresholdSec, cfg.checkIntervalFrames,
                                   cfg.budgetMB);
            },
            "Show GPU resource leak detector configuration", "GPULeakDetector");

        console.RegisterCommand(
            "gpuleak.details", [](const std::vector<std::string>&) -> std::string
            { return GPUResourceLeakDetector::GetInstance().FormatDetails(); }, "Show per-type GPU resource breakdown",
            "GPULeakDetector");
    }

} // namespace Spark
