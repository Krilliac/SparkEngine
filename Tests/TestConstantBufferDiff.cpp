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

TEST(CBDiff_EmptyToNonEmpty)
{
    TestCBDiffManager mgr;
    uint8_t small = 42;
    EXPECT_TRUE(mgr.ShouldUpdate("slot", &small, sizeof(small)));
    EXPECT_FALSE(mgr.ShouldUpdate("slot", &small, sizeof(small)));
    EXPECT_EQ(mgr.GetUpdates(), 1u);
    EXPECT_EQ(mgr.GetSkips(), 1u);
}

TEST(CBDiff_SizeChange)
{
    TestCBDiffManager mgr;
    float f = 1.0f;
    double d = 1.0;

    EXPECT_TRUE(mgr.ShouldUpdate("slot", &f, sizeof(f)));
    // Different size triggers update even if first bytes match
    EXPECT_TRUE(mgr.ShouldUpdate("slot", &d, sizeof(d)));
    EXPECT_EQ(mgr.GetUpdates(), 2u);
}

TEST(CBDiff_LargeBuffer)
{
    TestCBDiffManager mgr;
    std::vector<float> buf(256, 0.0f);
    EXPECT_TRUE(mgr.ShouldUpdate("big", buf.data(), buf.size() * sizeof(float)));
    EXPECT_FALSE(mgr.ShouldUpdate("big", buf.data(), buf.size() * sizeof(float)));

    buf[255] = 1.0f; // Change last element
    EXPECT_TRUE(mgr.ShouldUpdate("big", buf.data(), buf.size() * sizeof(float)));
    EXPECT_EQ(mgr.GetUpdates(), 2u);
}

TEST(CBDiff_SingleByteChange)
{
    TestCBDiffManager mgr;
    uint8_t data[4] = {0, 0, 0, 0};
    EXPECT_TRUE(mgr.ShouldUpdate("slot", data, sizeof(data)));
    EXPECT_FALSE(mgr.ShouldUpdate("slot", data, sizeof(data)));

    data[2] = 1; // Flip one byte
    EXPECT_TRUE(mgr.ShouldUpdate("slot", data, sizeof(data)));
}

TEST(CBDiff_ManySlots)
{
    TestCBDiffManager mgr;
    constexpr int N = 32;
    float values[N];
    for (int i = 0; i < N; ++i)
    {
        values[i] = static_cast<float>(i);
        std::string key = "Slot_" + std::to_string(i);
        EXPECT_TRUE(mgr.ShouldUpdate(key, &values[i], sizeof(float)));
    }
    EXPECT_EQ(mgr.GetUpdates(), static_cast<uint64_t>(N));

    // All should skip on second pass
    for (int i = 0; i < N; ++i)
    {
        std::string key = "Slot_" + std::to_string(i);
        EXPECT_FALSE(mgr.ShouldUpdate(key, &values[i], sizeof(float)));
    }
    EXPECT_EQ(mgr.GetSkips(), static_cast<uint64_t>(N));
}

TEST(CBDiff_ZeroSizedData)
{
    TestCBDiffManager mgr;
    uint8_t dummy = 0;
    // Zero-size data matches empty slot (both size 0), so skips immediately
    EXPECT_FALSE(mgr.ShouldUpdate("empty", &dummy, 0));
    EXPECT_EQ(mgr.GetSkips(), 1u);

    // Changing from zero to non-zero size should trigger update
    float val = 1.0f;
    EXPECT_TRUE(mgr.ShouldUpdate("empty", &val, sizeof(val)));
    EXPECT_EQ(mgr.GetUpdates(), 1u);
}
