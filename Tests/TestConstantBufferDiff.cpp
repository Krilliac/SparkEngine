/**
 * @file TestConstantBufferDiff.cpp
 * @brief Tests for content-diff constant buffer upload skipping
 */

#include "TestFramework.h"
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{

    class TestCBDiffManager
    {
      public:
        bool ShouldUpdate(const std::string& key, const void* data, size_t size)
        {
            auto& slot = m_slots[key];
            if (slot.size() != size || std::memcmp(slot.data(), data, size) != 0)
            {
                slot.resize(size);
                std::memcpy(slot.data(), data, size);
                m_updates++;
                return true;
            }
            m_skips++;
            return false;
        }

        uint64_t GetUpdates() const { return m_updates; }
        uint64_t GetSkips() const { return m_skips; }

      private:
        std::unordered_map<std::string, std::vector<uint8_t>> m_slots;
        uint64_t m_updates = 0;
        uint64_t m_skips = 0;
    };

} // anonymous namespace

TEST(CBDiff_SkipsRedundant)
{
    TestCBDiffManager mgr;
    struct PerObjectCB
    {
        float transform[16] = {};
    };

    PerObjectCB cb;
    cb.transform[0] = 1.0f;
    EXPECT_TRUE(mgr.ShouldUpdate("VS_PerObject", &cb, sizeof(cb)));
    EXPECT_FALSE(mgr.ShouldUpdate("VS_PerObject", &cb, sizeof(cb))); // Same data
    EXPECT_EQ(mgr.GetUpdates(), 1u);
    EXPECT_EQ(mgr.GetSkips(), 1u);

    cb.transform[0] = 2.0f;
    EXPECT_TRUE(mgr.ShouldUpdate("VS_PerObject", &cb, sizeof(cb))); // Changed
    EXPECT_EQ(mgr.GetUpdates(), 2u);
}

TEST(CBDiff_MultipleSlots)
{
    TestCBDiffManager mgr;
    float data1 = 1.0f;
    float data2 = 2.0f;

    EXPECT_TRUE(mgr.ShouldUpdate("VS_Slot0", &data1, sizeof(data1)));
    EXPECT_TRUE(mgr.ShouldUpdate("PS_Slot0", &data2, sizeof(data2)));
    EXPECT_FALSE(mgr.ShouldUpdate("VS_Slot0", &data1, sizeof(data1)));
    EXPECT_FALSE(mgr.ShouldUpdate("PS_Slot0", &data2, sizeof(data2)));
    EXPECT_EQ(mgr.GetSkips(), 2u);
}
