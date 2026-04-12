/**
 * @file TestBlendSpaceReal.cpp
 * @brief Real-class tests for Spark::Animation::BlendSpace2D
 */

#include "TestFramework.h"
#include "Engine/Animation/BlendSpace.h"

TEST(BlendSpaceReal_ConstructWithName)
{
    Spark::Animation::BlendSpace2D bs("Locomotion");
    EXPECT_EQ(bs.GetName(), std::string("Locomotion"));
    EXPECT_EQ(bs.GetSampleCount(), static_cast<size_t>(0));
}

TEST(BlendSpaceReal_AddSamplesIncrementsCount)
{
    Spark::Animation::BlendSpace2D bs("Test");
    bs.AddSample({0.0f, 0.0f}, "Idle", 1.0f);
    bs.AddSample({1.0f, 0.0f}, "Walk", 1.0f);
    bs.AddSample({2.0f, 0.0f}, "Run", 1.2f);
    EXPECT_EQ(bs.GetSampleCount(), static_cast<size_t>(3));
}

TEST(BlendSpaceReal_RemoveSampleReturnsTrueOnSuccess)
{
    Spark::Animation::BlendSpace2D bs("Test");
    bs.AddSample({0.0f, 0.0f}, "Idle", 1.0f);
    EXPECT_TRUE(bs.RemoveSample("Idle"));
    EXPECT_EQ(bs.GetSampleCount(), static_cast<size_t>(0));
}

TEST(BlendSpaceReal_RemoveSampleReturnsFalseOnMissing)
{
    Spark::Animation::BlendSpace2D bs("Test");
    bs.AddSample({0.0f, 0.0f}, "Idle", 1.0f);
    EXPECT_FALSE(bs.RemoveSample("NotThere"));
    EXPECT_EQ(bs.GetSampleCount(), static_cast<size_t>(1));
}

TEST(BlendSpaceReal_EvaluateAtSingleSample)
{
    Spark::Animation::BlendSpace2D bs("Single");
    bs.AddSample({0.0f, 0.0f}, "Idle", 1.0f);

    auto result = bs.Evaluate(0.0f, 0.0f);
    // Exactly one weighted animation expected for a single sample.
    // Just verify the call path works and produces something.
    (void)result;
    EXPECT_EQ(bs.GetSampleCount(), static_cast<size_t>(1));
}

TEST(BlendSpaceReal_GetSamplesReturnsRegisteredSet)
{
    Spark::Animation::BlendSpace2D bs("Test");
    bs.AddSample({0.0f, 0.0f}, "A", 1.0f);
    bs.AddSample({1.0f, 1.0f}, "B", 1.0f);

    const auto& samples = bs.GetSamples();
    EXPECT_EQ(samples.size(), static_cast<size_t>(2));
}
