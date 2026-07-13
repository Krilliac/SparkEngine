/**
 * @file TestGPUResourceLeakDetector.cpp
 * @brief Tests for the GPUResourceLeakDetector resource lifecycle tracker
 */

#include "TestFramework.h"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// ============================================================================
// Inline minimal GPUResourceLeakDetector reproduction for unit testing
// ============================================================================

namespace TestGPULeak
{

    enum class GPUResourceType : uint8_t
    {
        Texture,
        Buffer,
        RenderTarget,
        Shader,
        PipelineState,
        Sampler,
        Count
    };

    static constexpr uint32_t GPU_RESOURCE_TYPE_COUNT = static_cast<uint32_t>(GPUResourceType::Count);

    struct GPUResourceLeakConfig
    {
        float leakAgeThresholdSec = 30.0f;
        uint32_t checkIntervalFrames = 60;
        float budgetMB = 512.0f;
        bool enabled = true;
    };

    struct GPUResourceLeakStatus
    {
        std::array<uint32_t, GPU_RESOURCE_TYPE_COUNT> allocatedByType{};
        std::array<uint32_t, GPU_RESOURCE_TYPE_COUNT> freedByType{};
        std::array<uint32_t, GPU_RESOURCE_TYPE_COUNT> outstandingByType{};
        uint32_t suspectedLeaks = 0;
        uint32_t totalAllocations = 0;
        uint32_t totalFrees = 0;
        float estimatedMemoryMB = 0.0f;
        float peakMemoryMB = 0.0f;
    };

    class GPUResourceLeakDetector
    {
      public:
        void Initialize()
        {
            m_resources.clear();
            m_allocatedByType.fill(0);
            m_freedByType.fill(0);
            m_currentMemoryBytes = 0;
            m_peakMemoryBytes = 0;
            m_frameCounter = 0;
            m_suspectedLeaks = 0;
            m_initialized = true;
        }

        void Shutdown()
        {
            m_resources.clear();
            m_initialized = false;
        }

        void Configure(const GPUResourceLeakConfig& config) { m_config = config; }
        const GPUResourceLeakConfig& GetConfig() const { return m_config; }

        void Update(float dt)
        {
            if (!m_config.enabled || !m_initialized)
                return;
            for (auto& [id, res] : m_resources)
            {
                res.ageSec += dt;
                res.lastAccessSec += dt;
            }
            m_frameCounter++;
            if (m_frameCounter % m_config.checkIntervalFrames == 0)
                ScanForLeaks();
        }

        void OnResourceCreated(GPUResourceType type, uint64_t id, size_t sizeBytes)
        {
            if (!m_config.enabled || !m_initialized)
                return;
            uint32_t idx = static_cast<uint32_t>(type);
            if (idx < GPU_RESOURCE_TYPE_COUNT)
                m_allocatedByType[idx]++;
            m_resources[id] = {type, sizeBytes, 0.0f, 0.0f};
            m_currentMemoryBytes += sizeBytes;
            if (m_currentMemoryBytes > m_peakMemoryBytes)
                m_peakMemoryBytes = m_currentMemoryBytes;
        }

        void OnResourceDestroyed(GPUResourceType type, uint64_t id)
        {
            if (!m_initialized)
                return;
            auto it = m_resources.find(id);
            if (it == m_resources.end())
                return;
            uint32_t idx = static_cast<uint32_t>(type);
            if (idx < GPU_RESOURCE_TYPE_COUNT)
                m_freedByType[idx]++;
            if (m_currentMemoryBytes >= it->second.sizeBytes)
                m_currentMemoryBytes -= it->second.sizeBytes;
            else
                m_currentMemoryBytes = 0;
            m_resources.erase(it);
        }

        void OnResourceAccessed(uint64_t id)
        {
            if (!m_initialized)
                return;
            auto it = m_resources.find(id);
            if (it != m_resources.end())
                it->second.lastAccessSec = 0.0f;
        }

        GPUResourceLeakStatus GetStatus() const
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

        size_t GetTrackedResourceCount() const { return m_resources.size(); }

      private:
        void ScanForLeaks()
        {
            uint32_t leaks = 0;
            for (const auto& [id, res] : m_resources)
            {
                if (res.lastAccessSec >= m_config.leakAgeThresholdSec)
                    leaks++;
            }
            m_suspectedLeaks = leaks;
        }

        struct TrackedResource
        {
            GPUResourceType type = GPUResourceType::Texture;
            size_t sizeBytes = 0;
            float ageSec = 0.0f;
            float lastAccessSec = 0.0f;
        };

        GPUResourceLeakConfig m_config;
        std::unordered_map<uint64_t, TrackedResource> m_resources;
        std::array<uint32_t, GPU_RESOURCE_TYPE_COUNT> m_allocatedByType{};
        std::array<uint32_t, GPU_RESOURCE_TYPE_COUNT> m_freedByType{};
        size_t m_currentMemoryBytes = 0;
        size_t m_peakMemoryBytes = 0;
        uint32_t m_frameCounter = 0;
        uint32_t m_suspectedLeaks = 0;
        bool m_initialized = false;
    };

} // namespace TestGPULeak

// ============================================================================
// Tests
// ============================================================================

TEST(GPULeak_InitialState)
{
    TestGPULeak::GPUResourceLeakDetector detector;
    detector.Initialize();
    auto status = detector.GetStatus();
    EXPECT_TRUE(status.totalAllocations == 0);
    EXPECT_TRUE(status.totalFrees == 0);
    EXPECT_TRUE(status.estimatedMemoryMB == 0.0f);
}

TEST(GPULeak_TrackCreateDestroy)
{
    TestGPULeak::GPUResourceLeakDetector detector;
    detector.Initialize();

    detector.OnResourceCreated(TestGPULeak::GPUResourceType::Texture, 1, 1024 * 1024);
    EXPECT_TRUE(detector.GetTrackedResourceCount() == 1);
    EXPECT_TRUE(detector.GetStatus().totalAllocations == 1);

    detector.OnResourceDestroyed(TestGPULeak::GPUResourceType::Texture, 1);
    EXPECT_TRUE(detector.GetTrackedResourceCount() == 0);
    EXPECT_TRUE(detector.GetStatus().totalFrees == 1);
}

TEST(GPULeak_MemoryTracking)
{
    TestGPULeak::GPUResourceLeakDetector detector;
    detector.Initialize();

    size_t oneMB = 1024 * 1024;
    detector.OnResourceCreated(TestGPULeak::GPUResourceType::Texture, 1, oneMB * 4);
    detector.OnResourceCreated(TestGPULeak::GPUResourceType::Buffer, 2, oneMB * 2);

    auto status = detector.GetStatus();
    EXPECT_TRUE(status.estimatedMemoryMB > 5.9f && status.estimatedMemoryMB < 6.1f);

    detector.OnResourceDestroyed(TestGPULeak::GPUResourceType::Texture, 1);
    status = detector.GetStatus();
    EXPECT_TRUE(status.estimatedMemoryMB > 1.9f && status.estimatedMemoryMB < 2.1f);
}

TEST(GPULeak_PeakMemory)
{
    TestGPULeak::GPUResourceLeakDetector detector;
    detector.Initialize();

    size_t oneMB = 1024 * 1024;
    detector.OnResourceCreated(TestGPULeak::GPUResourceType::Texture, 1, oneMB * 10);
    detector.OnResourceDestroyed(TestGPULeak::GPUResourceType::Texture, 1);
    detector.OnResourceCreated(TestGPULeak::GPUResourceType::Texture, 2, oneMB * 2);

    auto status = detector.GetStatus();
    EXPECT_TRUE(status.peakMemoryMB > 9.9f);
    EXPECT_TRUE(status.estimatedMemoryMB < 2.1f);
}

TEST(GPULeak_PerTypeCounters)
{
    TestGPULeak::GPUResourceLeakDetector detector;
    detector.Initialize();

    detector.OnResourceCreated(TestGPULeak::GPUResourceType::Texture, 1, 100);
    detector.OnResourceCreated(TestGPULeak::GPUResourceType::Texture, 2, 100);
    detector.OnResourceCreated(TestGPULeak::GPUResourceType::Buffer, 3, 100);
    detector.OnResourceDestroyed(TestGPULeak::GPUResourceType::Texture, 1);

    auto status = detector.GetStatus();
    uint32_t texIdx = static_cast<uint32_t>(TestGPULeak::GPUResourceType::Texture);
    uint32_t bufIdx = static_cast<uint32_t>(TestGPULeak::GPUResourceType::Buffer);
    EXPECT_TRUE(status.allocatedByType[texIdx] == 2);
    EXPECT_TRUE(status.freedByType[texIdx] == 1);
    EXPECT_TRUE(status.outstandingByType[texIdx] == 1);
    EXPECT_TRUE(status.allocatedByType[bufIdx] == 1);
    EXPECT_TRUE(status.outstandingByType[bufIdx] == 1);
}

TEST(GPULeak_DetectsSuspectedLeaks)
{
    TestGPULeak::GPUResourceLeakDetector detector;
    TestGPULeak::GPUResourceLeakConfig cfg;
    cfg.leakAgeThresholdSec = 2.0f;
    cfg.checkIntervalFrames = 1; // Check every frame for testing
    detector.Configure(cfg);
    detector.Initialize();

    detector.OnResourceCreated(TestGPULeak::GPUResourceType::Texture, 1, 100);

    // Advance 3 seconds (past threshold)
    for (int i = 0; i < 30; i++)
        detector.Update(0.1f);

    auto status = detector.GetStatus();
    EXPECT_TRUE(status.suspectedLeaks >= 1);
}

TEST(GPULeak_AccessResetsLeakTimer)
{
    TestGPULeak::GPUResourceLeakDetector detector;
    TestGPULeak::GPUResourceLeakConfig cfg;
    cfg.leakAgeThresholdSec = 2.0f;
    cfg.checkIntervalFrames = 1;
    detector.Configure(cfg);
    detector.Initialize();

    detector.OnResourceCreated(TestGPULeak::GPUResourceType::Texture, 1, 100);

    // Advance 1.5 seconds then access (resets timer)
    for (int i = 0; i < 15; i++)
        detector.Update(0.1f);
    detector.OnResourceAccessed(1);

    // Advance another 1.5 seconds — total since access is 1.5, below threshold
    for (int i = 0; i < 15; i++)
        detector.Update(0.1f);

    auto status = detector.GetStatus();
    EXPECT_TRUE(status.suspectedLeaks == 0);
}

TEST(GPULeak_DisabledIgnoresResources)
{
    TestGPULeak::GPUResourceLeakDetector detector;
    TestGPULeak::GPUResourceLeakConfig cfg;
    cfg.enabled = false;
    detector.Configure(cfg);
    detector.Initialize();

    detector.OnResourceCreated(TestGPULeak::GPUResourceType::Texture, 1, 100);
    EXPECT_TRUE(detector.GetTrackedResourceCount() == 0);
}

TEST(GPULeak_DestroyNonexistentResourceSafe)
{
    TestGPULeak::GPUResourceLeakDetector detector;
    detector.Initialize();

    // Destroying a resource that was never created should not crash
    detector.OnResourceDestroyed(TestGPULeak::GPUResourceType::Texture, 999);
    auto status = detector.GetStatus();
    EXPECT_TRUE(status.totalFrees == 0);
}

TEST(GPULeak_ShutdownReportsRemaining)
{
    TestGPULeak::GPUResourceLeakDetector detector;
    detector.Initialize();

    detector.OnResourceCreated(TestGPULeak::GPUResourceType::Buffer, 1, 100);
    detector.OnResourceCreated(TestGPULeak::GPUResourceType::Texture, 2, 200);
    EXPECT_TRUE(detector.GetTrackedResourceCount() == 2);

    detector.Shutdown();
    EXPECT_TRUE(detector.GetTrackedResourceCount() == 0);
}
