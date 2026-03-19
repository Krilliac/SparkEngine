// TestBlendSpace.cpp - Tests for blend space sample management and weight evaluation
// Standalone implementations for CI testing

#include "TestFramework.h"
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace TestBlend
{

    struct Vec2
    {
        float x = 0.0f, y = 0.0f;

        float DistanceTo(const Vec2& o) const
        {
            float dx = x - o.x, dy = y - o.y;
            return std::sqrt(dx * dx + dy * dy);
        }
    };

    struct BlendSample
    {
        std::string animationName;
        Vec2 position; // Position in parameter space
        float weight = 0.0f;
    };

    struct BlendSpace
    {
        std::vector<BlendSample> samples;

        void AddSample(const std::string& animName, Vec2 pos) { samples.push_back({animName, pos, 0.0f}); }

        int GetSampleCount() const { return static_cast<int>(samples.size()); }

        void Evaluate(Vec2 parameterValue)
        {
            if (samples.empty())
                return;

            // Inverse-distance weighting
            float totalWeight = 0.0f;
            bool exactMatch = false;

            for (auto& s : samples)
            {
                float dist = parameterValue.DistanceTo(s.position);
                if (dist < 0.0001f)
                {
                    // Exact match
                    for (auto& s2 : samples)
                        s2.weight = 0.0f;
                    s.weight = 1.0f;
                    exactMatch = true;
                    break;
                }
                s.weight = 1.0f / (dist * dist);
                totalWeight += s.weight;
            }

            if (!exactMatch && totalWeight > 0.0f)
            {
                for (auto& s : samples)
                    s.weight /= totalWeight;
            }
        }

        float GetTotalWeight() const
        {
            float total = 0.0f;
            for (const auto& s : samples)
                total += s.weight;
            return total;
        }
    };

} // namespace TestBlend

// ============================================================================
// Sample Management
// ============================================================================

TEST(BlendSpace_AddSample)
{
    TestBlend::BlendSpace bs;
    bs.AddSample("Idle", {0, 0});
    bs.AddSample("WalkForward", {0, 1});
    bs.AddSample("RunForward", {0, 2});
    EXPECT_EQ(bs.GetSampleCount(), 3);
}

TEST(BlendSpace_GetSampleCount_Empty)
{
    TestBlend::BlendSpace bs;
    EXPECT_EQ(bs.GetSampleCount(), 0);
}

// ============================================================================
// Evaluation — Weights Sum to 1.0
// ============================================================================

TEST(BlendSpace_Evaluate_WeightsSumToOne)
{
    TestBlend::BlendSpace bs;
    bs.AddSample("Idle", {0, 0});
    bs.AddSample("Walk", {0, 1});
    bs.AddSample("Run", {0, 2});
    bs.AddSample("StrafeLeft", {-1, 0});
    bs.AddSample("StrafeRight", {1, 0});

    // Evaluate at a point between samples
    bs.Evaluate({0.3f, 0.7f});
    EXPECT_NEAR(bs.GetTotalWeight(), 1.0f, 0.001f);

    // All weights should be non-negative
    for (const auto& s : bs.samples)
    {
        EXPECT_GE(s.weight, 0.0f);
    }
}

TEST(BlendSpace_Evaluate_ExactMatch)
{
    TestBlend::BlendSpace bs;
    bs.AddSample("Idle", {0, 0});
    bs.AddSample("Walk", {0, 1});

    // Evaluate exactly at Idle position
    bs.Evaluate({0, 0});
    EXPECT_NEAR(bs.samples[0].weight, 1.0f, 0.001f);
    EXPECT_NEAR(bs.samples[1].weight, 0.0f, 0.001f);
    EXPECT_NEAR(bs.GetTotalWeight(), 1.0f, 0.001f);
}

TEST(BlendSpace_Evaluate_Midpoint)
{
    TestBlend::BlendSpace bs;
    bs.AddSample("A", {0, 0});
    bs.AddSample("B", {2, 0});

    // Evaluate at the exact midpoint — both should have equal weight
    bs.Evaluate({1, 0});
    EXPECT_NEAR(bs.samples[0].weight, 0.5f, 0.001f);
    EXPECT_NEAR(bs.samples[1].weight, 0.5f, 0.001f);
    EXPECT_NEAR(bs.GetTotalWeight(), 1.0f, 0.001f);
}

TEST(BlendSpace_Evaluate_CloserSampleGetsMoreWeight)
{
    TestBlend::BlendSpace bs;
    bs.AddSample("Near", {1, 0});
    bs.AddSample("Far", {10, 0});

    // Evaluate close to "Near"
    bs.Evaluate({0, 0});
    EXPECT_GT(bs.samples[0].weight, bs.samples[1].weight);
    EXPECT_NEAR(bs.GetTotalWeight(), 1.0f, 0.001f);
}
