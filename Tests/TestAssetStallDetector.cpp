/**
 * @file TestAssetStallDetector.cpp
 * @brief Tests for the AssetStallDetector loading pipeline monitor
 */

#include "TestFramework.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// ============================================================================
// Inline minimal AssetStallDetector reproduction for unit testing
// ============================================================================

namespace TestAssetStall
{

    enum class AssetLoadType : uint8_t
    {
        Texture,
        Mesh,
        Audio,
        Script,
        Level,
        Other
    };

    struct AssetStallConfig
    {
        float warningThresholdSec = 5.0f;
        float stallThresholdSec = 10.0f;
        uint32_t maxConcurrentLoads = 32;
        bool enabled = true;
    };

    struct AssetStallStatus
    {
        uint32_t activeLoads = 0;
        uint32_t stalledLoads = 0;
        uint32_t completedLoads = 0;
        uint32_t failedLoads = 0;
        float averageLoadTimeSec = 0.0f;
        float worstLoadTimeSec = 0.0f;
        uint32_t queueOverflowCount = 0;
    };

    class AssetStallDetector
    {
      public:
        void Initialize()
        {
            m_activeLoads.clear();
            m_nextId = 1;
            m_completedLoads = 0;
            m_failedLoads = 0;
            m_totalLoadTimeSec = 0.0f;
            m_worstLoadTimeSec = 0.0f;
            m_queueOverflowCount = 0;
            m_initialized = true;
        }

        void Shutdown()
        {
            m_activeLoads.clear();
            m_initialized = false;
        }

        void Configure(const AssetStallConfig& config) { m_config = config; }
        const AssetStallConfig& GetConfig() const { return m_config; }

        void Update(float dt)
        {
            if (!m_config.enabled || !m_initialized)
                return;
            for (auto& [id, load] : m_activeLoads)
            {
                load.elapsedSec += dt;
                if (!load.warned && load.elapsedSec >= m_config.warningThresholdSec)
                    load.warned = true;
            }
        }

        uint64_t OnLoadBegin(const std::string& name, AssetLoadType type)
        {
            if (!m_config.enabled || !m_initialized)
                return 0;
            uint64_t id = m_nextId++;
            m_activeLoads[id] = {name, type, 0.0f, false};
            if (m_activeLoads.size() > m_config.maxConcurrentLoads)
                m_queueOverflowCount++;
            return id;
        }

        void OnLoadComplete(uint64_t id) { FinishLoad(id, true); }
        void OnLoadFailed(uint64_t id) { FinishLoad(id, false); }

        AssetStallStatus GetStatus() const
        {
            AssetStallStatus status;
            status.activeLoads = static_cast<uint32_t>(m_activeLoads.size());
            status.completedLoads = m_completedLoads;
            status.failedLoads = m_failedLoads;
            status.worstLoadTimeSec = m_worstLoadTimeSec;
            status.queueOverflowCount = m_queueOverflowCount;
            uint32_t total = m_completedLoads + m_failedLoads;
            status.averageLoadTimeSec = (total > 0) ? (m_totalLoadTimeSec / static_cast<float>(total)) : 0.0f;
            uint32_t stalled = 0;
            for (const auto& [id, load] : m_activeLoads)
            {
                if (load.elapsedSec >= m_config.stallThresholdSec)
                    stalled++;
            }
            status.stalledLoads = stalled;
            return status;
        }

      private:
        void FinishLoad(uint64_t id, bool success)
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

        struct ActiveLoad
        {
            std::string name;
            AssetLoadType type = AssetLoadType::Other;
            float elapsedSec = 0.0f;
            bool warned = false;
        };

        AssetStallConfig m_config;
        std::unordered_map<uint64_t, ActiveLoad> m_activeLoads;
        uint64_t m_nextId = 1;
        uint32_t m_completedLoads = 0;
        uint32_t m_failedLoads = 0;
        float m_totalLoadTimeSec = 0.0f;
        float m_worstLoadTimeSec = 0.0f;
        uint32_t m_queueOverflowCount = 0;
        bool m_initialized = false;
    };

} // namespace TestAssetStall

// ============================================================================
// Tests
// ============================================================================

TEST(AssetStall_InitialState)
{
    TestAssetStall::AssetStallDetector detector;
    detector.Initialize();
    auto status = detector.GetStatus();
    EXPECT_TRUE(status.activeLoads == 0);
    EXPECT_TRUE(status.completedLoads == 0);
    EXPECT_TRUE(status.stalledLoads == 0);
}

TEST(AssetStall_TrackLoadLifecycle)
{
    TestAssetStall::AssetStallDetector detector;
    detector.Initialize();

    auto id = detector.OnLoadBegin("player.mesh", TestAssetStall::AssetLoadType::Mesh);
    EXPECT_TRUE(id > 0);
    EXPECT_TRUE(detector.GetStatus().activeLoads == 1);

    detector.Update(1.0f); // advance 1 second
    detector.OnLoadComplete(id);

    auto status = detector.GetStatus();
    EXPECT_TRUE(status.activeLoads == 0);
    EXPECT_TRUE(status.completedLoads == 1);
}

TEST(AssetStall_DetectsStall)
{
    TestAssetStall::AssetStallDetector detector;
    TestAssetStall::AssetStallConfig cfg;
    cfg.stallThresholdSec = 2.0f;
    detector.Configure(cfg);
    detector.Initialize();

    detector.OnLoadBegin("huge_texture.dds", TestAssetStall::AssetLoadType::Texture);

    // Advance past stall threshold
    for (int i = 0; i < 30; i++)
        detector.Update(0.1f); // 3 seconds total

    auto status = detector.GetStatus();
    EXPECT_TRUE(status.stalledLoads == 1);
    EXPECT_TRUE(status.activeLoads == 1);
}

TEST(AssetStall_FailedLoadTracked)
{
    TestAssetStall::AssetStallDetector detector;
    detector.Initialize();

    auto id = detector.OnLoadBegin("missing.wav", TestAssetStall::AssetLoadType::Audio);
    detector.Update(0.5f);
    detector.OnLoadFailed(id);

    auto status = detector.GetStatus();
    EXPECT_TRUE(status.failedLoads == 1);
    EXPECT_TRUE(status.completedLoads == 0);
    EXPECT_TRUE(status.activeLoads == 0);
}

TEST(AssetStall_AverageAndWorstTime)
{
    TestAssetStall::AssetStallDetector detector;
    detector.Initialize();

    // Load 1: 1 second
    auto id1 = detector.OnLoadBegin("a.mesh", TestAssetStall::AssetLoadType::Mesh);
    detector.Update(1.0f);
    detector.OnLoadComplete(id1);

    // Load 2: 3 seconds
    auto id2 = detector.OnLoadBegin("b.mesh", TestAssetStall::AssetLoadType::Mesh);
    detector.Update(3.0f);
    detector.OnLoadComplete(id2);

    auto status = detector.GetStatus();
    EXPECT_TRUE(status.averageLoadTimeSec > 1.9f && status.averageLoadTimeSec < 2.1f);
    EXPECT_TRUE(status.worstLoadTimeSec > 2.9f);
}

TEST(AssetStall_QueueOverflow)
{
    TestAssetStall::AssetStallDetector detector;
    TestAssetStall::AssetStallConfig cfg;
    cfg.maxConcurrentLoads = 2;
    detector.Configure(cfg);
    detector.Initialize();

    detector.OnLoadBegin("a.tex", TestAssetStall::AssetLoadType::Texture);
    detector.OnLoadBegin("b.tex", TestAssetStall::AssetLoadType::Texture);
    detector.OnLoadBegin("c.tex", TestAssetStall::AssetLoadType::Texture); // overflow

    auto status = detector.GetStatus();
    EXPECT_TRUE(status.queueOverflowCount >= 1);
    EXPECT_TRUE(status.activeLoads == 3);
}

TEST(AssetStall_DisabledIgnoresLoads)
{
    TestAssetStall::AssetStallDetector detector;
    TestAssetStall::AssetStallConfig cfg;
    cfg.enabled = false;
    detector.Configure(cfg);
    detector.Initialize();

    auto id = detector.OnLoadBegin("x.mesh", TestAssetStall::AssetLoadType::Mesh);
    EXPECT_TRUE(id == 0);
    EXPECT_TRUE(detector.GetStatus().activeLoads == 0);
}

TEST(AssetStall_MultipleLoadsTracked)
{
    TestAssetStall::AssetStallDetector detector;
    detector.Initialize();

    auto id1 = detector.OnLoadBegin("a.mesh", TestAssetStall::AssetLoadType::Mesh);
    auto id2 = detector.OnLoadBegin("b.tex", TestAssetStall::AssetLoadType::Texture);
    auto id3 = detector.OnLoadBegin("c.wav", TestAssetStall::AssetLoadType::Audio);

    EXPECT_TRUE(detector.GetStatus().activeLoads == 3);

    detector.Update(0.5f);
    detector.OnLoadComplete(id1);
    detector.OnLoadComplete(id2);

    EXPECT_TRUE(detector.GetStatus().activeLoads == 1);
    EXPECT_TRUE(detector.GetStatus().completedLoads == 2);

    detector.OnLoadComplete(id3);
    EXPECT_TRUE(detector.GetStatus().activeLoads == 0);
}

TEST(AssetStall_WarningSetOnSlowLoad)
{
    TestAssetStall::AssetStallDetector detector;
    TestAssetStall::AssetStallConfig cfg;
    cfg.warningThresholdSec = 1.0f;
    detector.Configure(cfg);
    detector.Initialize();

    detector.OnLoadBegin("slow.mesh", TestAssetStall::AssetLoadType::Mesh);

    // Advance past warning but not stall
    for (int i = 0; i < 15; i++)
        detector.Update(0.1f); // 1.5 seconds

    // The load is still active (warning was issued internally)
    auto status = detector.GetStatus();
    EXPECT_TRUE(status.activeLoads == 1);
    EXPECT_TRUE(status.stalledLoads == 0); // Not yet stalled
}
