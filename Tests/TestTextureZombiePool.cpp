// TestTextureZombiePool.cpp — Unit tests for texture cache zombie pool and thrash detection
// Tests eviction to zombie pool, resurrection, expiry, capacity limits, and thrash auto-promotion.

#include "TestFramework.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

// ============================================================================
// Standalone zombie pool and thrash tracker for test isolation
// ============================================================================

namespace
{

    struct TestZombieEntry
    {
        std::string name;
        uint64_t evictedFrame = 0;
        int textureId = 0;
    };

    class TestZombiePool
    {
      public:
        static constexpr uint32_t MAX_ZOMBIES = 64;
        static constexpr uint32_t EXPIRY_FRAMES = 300;

        void Evict(const std::string& name, int textureId, uint64_t frame)
        {
            if (m_zombies.size() >= MAX_ZOMBIES)
                m_zombies.erase(m_zombies.begin());
            m_zombies.push_back({name, frame, textureId});
        }

        int TryResurrect(const std::string& name, uint64_t currentFrame)
        {
            for (auto it = m_zombies.begin(); it != m_zombies.end(); ++it)
            {
                if (it->name == name && (currentFrame - it->evictedFrame) < EXPIRY_FRAMES)
                {
                    int id = it->textureId;
                    m_zombies.erase(it);
                    m_resurrections++;
                    return id;
                }
            }
            return -1;
        }

        void PurgeExpired(uint64_t currentFrame)
        {
            m_zombies.erase(std::remove_if(m_zombies.begin(), m_zombies.end(), [currentFrame](const TestZombieEntry& e)
                                           { return (currentFrame - e.evictedFrame) >= EXPIRY_FRAMES; }),
                            m_zombies.end());
        }

        uint32_t GetCount() const { return static_cast<uint32_t>(m_zombies.size()); }
        uint32_t GetResurrections() const { return m_resurrections; }

      private:
        std::vector<TestZombieEntry> m_zombies;
        uint32_t m_resurrections = 0;
    };

    struct TestThrashTracker
    {
        static constexpr uint32_t THRESHOLD = 3;
        static constexpr uint32_t WINDOW = 120;

        uint64_t firstEvictFrame = 0;
        uint32_t evictReloadCount = 0;
        bool promoted = false;

        bool RecordEvictReload(uint64_t frame)
        {
            if (evictReloadCount == 0 || (frame - firstEvictFrame) > WINDOW)
            {
                firstEvictFrame = frame;
                evictReloadCount = 1;
                return false;
            }
            evictReloadCount++;
            if (evictReloadCount >= THRESHOLD && !promoted)
            {
                promoted = true;
                return true;
            }
            return false;
        }
    };

} // namespace

// ============================================================================
// Zombie pool tests
// ============================================================================

TEST("ZombiePool_EvictAndResurrect", "[texture]")
{
    TestZombiePool pool;
    pool.Evict("brick.png", 42, 100);
    pool.Evict("grass.png", 43, 100);

    ASSERT_EQ(pool.GetCount(), 2u);

    int resurrected = pool.TryResurrect("brick.png", 150);
    ASSERT_EQ(resurrected, 42);
    ASSERT_EQ(pool.GetCount(), 1u);
    ASSERT_EQ(pool.GetResurrections(), 1u);
}

TEST("ZombiePool_ExpiredNotResurrected", "[texture]")
{
    TestZombiePool pool;
    pool.Evict("old.png", 1, 100);

    int result = pool.TryResurrect("old.png", 500);
    ASSERT_EQ(result, -1);
}

TEST("ZombiePool_PurgeExpired", "[texture]")
{
    TestZombiePool pool;
    pool.Evict("old.png", 1, 100);
    pool.Evict("new.png", 2, 350);

    pool.PurgeExpired(400);
    ASSERT_EQ(pool.GetCount(), 1u);
}

TEST("ZombiePool_MaxCapacity", "[texture]")
{
    TestZombiePool pool;
    for (uint32_t i = 0; i < TestZombiePool::MAX_ZOMBIES + 10; ++i)
        pool.Evict("tex_" + std::to_string(i), static_cast<int>(i), i);
    ASSERT_EQ(pool.GetCount(), TestZombiePool::MAX_ZOMBIES);
}

// ============================================================================
// Thrash detection tests
// ============================================================================

TEST("ThrashDetection_PromotesAfterThreshold", "[texture]")
{
    TestThrashTracker tracker;

    bool promoted = tracker.RecordEvictReload(100);
    ASSERT_TRUE(!promoted);

    promoted = tracker.RecordEvictReload(110);
    ASSERT_TRUE(!promoted);

    promoted = tracker.RecordEvictReload(120);
    ASSERT_TRUE(promoted);
    ASSERT_TRUE(tracker.promoted);
}

TEST("ThrashDetection_ResetsAfterWindow", "[texture]")
{
    TestThrashTracker tracker;

    tracker.RecordEvictReload(100);
    tracker.RecordEvictReload(110);

    bool promoted = tracker.RecordEvictReload(300);
    ASSERT_TRUE(!promoted);
    ASSERT_EQ(tracker.evictReloadCount, 1u);
}
