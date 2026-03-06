// TestTemporalEffects.cpp - Tests for TAA and motion blur systems
// Standalone implementations for CI testing

#include "TestFramework.h"
#include <cmath>
#include <vector>
#include <string>
#include <cstdint>

namespace TestTemporal {

// Halton sequence for jitter generation
namespace Halton {
    inline float Sequence(int index, int base) {
        float result = 0.0f;
        float f = 1.0f / static_cast<float>(base);
        int i = index;
        while (i > 0) {
            result += f * (i % base);
            i /= base;
            f /= static_cast<float>(base);
        }
        return result;
    }

    inline std::pair<float, float> Halton23(int index) {
        return {Sequence(index, 2), Sequence(index, 3)};
    }
}

struct JitterOffset {
    float x = 0.0f, y = 0.0f;
};

inline JitterOffset ComputeJitter(int frameIndex, int patternLength, float pixelWidth, float pixelHeight) {
    int idx = (frameIndex % patternLength) + 1;
    auto [hx, hy] = Halton::Halton23(idx);
    return {(hx - 0.5f) * pixelWidth, (hy - 0.5f) * pixelHeight};
}

// YCoCg color space conversion
struct Color3 {
    float r, g, b;
};

inline Color3 RGBToYCoCg(const Color3& rgb) {
    return {
         0.25f * rgb.r + 0.5f * rgb.g + 0.25f * rgb.b,
         0.5f * rgb.r - 0.5f * rgb.b,
        -0.25f * rgb.r + 0.5f * rgb.g - 0.25f * rgb.b
    };
}

inline Color3 YCoCgToRGB(const Color3& ycocg) {
    float y = ycocg.r, co = ycocg.g, cg = ycocg.b;
    return {
        y + co - cg,
        y + cg,
        y - co - cg
    };
}

// Variance clipping
inline float ClampValue(float val, float minV, float maxV) {
    return (val < minV) ? minV : ((val > maxV) ? maxV : val);
}

struct AABB {
    Color3 minC, maxC;
};

inline AABB ComputeAABB(const std::vector<Color3>& samples) {
    AABB box = {{1e9f, 1e9f, 1e9f}, {-1e9f, -1e9f, -1e9f}};
    for (const auto& s : samples) {
        box.minC.r = std::min(box.minC.r, s.r);
        box.minC.g = std::min(box.minC.g, s.g);
        box.minC.b = std::min(box.minC.b, s.b);
        box.maxC.r = std::max(box.maxC.r, s.r);
        box.maxC.g = std::max(box.maxC.g, s.g);
        box.maxC.b = std::max(box.maxC.b, s.b);
    }
    return box;
}

inline Color3 ClampToAABB(const Color3& color, const AABB& box) {
    return {
        ClampValue(color.r, box.minC.r, box.maxC.r),
        ClampValue(color.g, box.minC.g, box.maxC.g),
        ClampValue(color.b, box.minC.b, box.maxC.b)
    };
}

// Frame history ring buffer
struct FrameData {
    float jitterX = 0.0f, jitterY = 0.0f;
    int frameIndex = 0;
};

class FrameHistory {
public:
    void Push(const FrameData& data) {
        m_buffer[m_head] = data;
        m_head = (m_head + 1) % MAX_FRAMES;
        if (m_count < MAX_FRAMES) m_count++;
    }
    int GetCount() const { return m_count; }
    const FrameData& Get(int ago) const {
        int idx = (m_head - 1 - ago + MAX_FRAMES * 2) % MAX_FRAMES;
        return m_buffer[idx];
    }
    void Clear() { m_head = 0; m_count = 0; }
private:
    static constexpr int MAX_FRAMES = 4;
    FrameData m_buffer[MAX_FRAMES] = {};
    int m_head = 0, m_count = 0;
};

} // namespace TestTemporal

// =============================================================================
// Tests
// =============================================================================

TEST(Temporal_HaltonSequence) {
    // Halton base 2: 0.5, 0.25, 0.75, 0.125, ...
    float h1 = TestTemporal::Halton::Sequence(1, 2);
    EXPECT_NEAR(h1, 0.5f, 0.001f);

    float h2 = TestTemporal::Halton::Sequence(2, 2);
    EXPECT_NEAR(h2, 0.25f, 0.001f);

    float h3 = TestTemporal::Halton::Sequence(3, 2);
    EXPECT_NEAR(h3, 0.75f, 0.001f);

    // Base 3: 1/3, 2/3, 1/9, ...
    float h1b3 = TestTemporal::Halton::Sequence(1, 3);
    EXPECT_NEAR(h1b3, 1.0f / 3.0f, 0.001f);
}

TEST(Temporal_Halton23) {
    auto [x, y] = TestTemporal::Halton::Halton23(1);
    EXPECT_NEAR(x, 0.5f, 0.001f);
    EXPECT_NEAR(y, 1.0f / 3.0f, 0.001f);
}

TEST(Temporal_JitterComputation) {
    float pixelW = 2.0f / 1920.0f;
    float pixelH = 2.0f / 1080.0f;

    auto j = TestTemporal::ComputeJitter(0, 8, pixelW, pixelH);
    // Jitter should be within half a pixel
    EXPECT_LT(std::abs(j.x), pixelW);
    EXPECT_LT(std::abs(j.y), pixelH);
}

TEST(Temporal_JitterPeriodic) {
    float pixelW = 2.0f / 1920.0f;
    float pixelH = 2.0f / 1080.0f;

    auto j1 = TestTemporal::ComputeJitter(0, 8, pixelW, pixelH);
    auto j2 = TestTemporal::ComputeJitter(8, 8, pixelW, pixelH);
    EXPECT_NEAR(j1.x, j2.x, 0.0001f);
    EXPECT_NEAR(j1.y, j2.y, 0.0001f);
}

TEST(Temporal_RGBToYCoCgRoundTrip) {
    TestTemporal::Color3 rgb = {0.8f, 0.5f, 0.3f};
    auto ycocg = TestTemporal::RGBToYCoCg(rgb);
    auto back = TestTemporal::YCoCgToRGB(ycocg);

    EXPECT_NEAR(back.r, rgb.r, 0.001f);
    EXPECT_NEAR(back.g, rgb.g, 0.001f);
    EXPECT_NEAR(back.b, rgb.b, 0.001f);
}

TEST(Temporal_YCoCgLuminance) {
    TestTemporal::Color3 white = {1.0f, 1.0f, 1.0f};
    auto y_white = TestTemporal::RGBToYCoCg(white);
    EXPECT_NEAR(y_white.r, 1.0f, 0.001f); // Y = 1 for white

    TestTemporal::Color3 black = {0.0f, 0.0f, 0.0f};
    auto y_black = TestTemporal::RGBToYCoCg(black);
    EXPECT_NEAR(y_black.r, 0.0f, 0.001f);
}

TEST(Temporal_AABBComputation) {
    std::vector<TestTemporal::Color3> samples = {
        {0.1f, 0.2f, 0.3f},
        {0.5f, 0.6f, 0.1f},
        {0.3f, 0.4f, 0.8f}
    };

    auto aabb = TestTemporal::ComputeAABB(samples);
    EXPECT_NEAR(aabb.minC.r, 0.1f, 0.001f);
    EXPECT_NEAR(aabb.maxC.r, 0.5f, 0.001f);
    EXPECT_NEAR(aabb.minC.b, 0.1f, 0.001f);
    EXPECT_NEAR(aabb.maxC.b, 0.8f, 0.001f);
}

TEST(Temporal_ClampToAABB) {
    TestTemporal::AABB box = {{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}};
    TestTemporal::Color3 inside = {0.5f, 0.5f, 0.5f};
    auto clamped = TestTemporal::ClampToAABB(inside, box);
    EXPECT_NEAR(clamped.r, 0.5f, 0.001f);

    TestTemporal::Color3 outside = {1.5f, -0.5f, 2.0f};
    clamped = TestTemporal::ClampToAABB(outside, box);
    EXPECT_NEAR(clamped.r, 1.0f, 0.001f);
    EXPECT_NEAR(clamped.g, 0.0f, 0.001f);
    EXPECT_NEAR(clamped.b, 1.0f, 0.001f);
}

TEST(Temporal_FrameHistory) {
    TestTemporal::FrameHistory history;
    EXPECT_EQ(history.GetCount(), 0);

    history.Push({0.1f, 0.2f, 0});
    EXPECT_EQ(history.GetCount(), 1);
    EXPECT_NEAR(history.Get(0).jitterX, 0.1f, 0.001f);

    history.Push({0.3f, 0.4f, 1});
    EXPECT_EQ(history.GetCount(), 2);
    EXPECT_NEAR(history.Get(0).jitterX, 0.3f, 0.001f);
    EXPECT_NEAR(history.Get(1).jitterX, 0.1f, 0.001f);
}

TEST(Temporal_FrameHistoryOverflow) {
    TestTemporal::FrameHistory history;
    for (int i = 0; i < 10; ++i) {
        history.Push({static_cast<float>(i), 0.0f, i});
    }
    // Should cap at MAX_FRAMES (4)
    EXPECT_EQ(history.GetCount(), 4);
    EXPECT_NEAR(history.Get(0).jitterX, 9.0f, 0.001f);
}

TEST(Temporal_FrameHistoryClear) {
    TestTemporal::FrameHistory history;
    history.Push({0.1f, 0.2f, 0});
    history.Push({0.3f, 0.4f, 1});
    history.Clear();
    EXPECT_EQ(history.GetCount(), 0);
}
