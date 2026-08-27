/**
 * @file TestPoseModifier.cpp
 * @brief Production tests for pose modifiers and the prioritized modifier stack.
 */

#include "TestFramework.h"
#include "Engine/Animation/PoseModifier.h"

#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using namespace Spark::Animation;

    constexpr float kTolerance = 0.001f;

    float QuaternionLength(const XMFLOAT4& quaternion)
    {
        return std::sqrt(quaternion.x * quaternion.x + quaternion.y * quaternion.y + quaternion.z * quaternion.z +
                         quaternion.w * quaternion.w);
    }

    class TrackingModifier final : public IPoseModifier
    {
      public:
        TrackingModifier(std::string name, int priority, int marker, std::vector<int>& order)
            : m_name(std::move(name)), m_priority(priority), m_marker(marker), m_order(order)
        {
        }

        void Apply(PoseContext& pose) override
        {
            m_order.push_back(m_marker);
            if (!pose.boneTransforms.empty())
                pose.boneTransforms[0].position.x = pose.boneTransforms[0].position.x * 10.0f + m_marker;
        }

        const char* GetName() const override { return m_name.c_str(); }
        int GetPriority() const override { return m_priority; }

      private:
        std::string m_name;
        int m_priority;
        int m_marker;
        std::vector<int>& m_order;
    };
} // namespace

TEST(PoseModifierStack_UsesPriorityAndOwnsProductionModifiers)
{
    PoseModifierStack stack;
    std::vector<int> order;

    stack.AddModifier(nullptr);
    stack.AddModifier(std::make_unique<TrackingModifier>("Late", 20, 2, order));
    stack.AddModifier(std::make_unique<TrackingModifier>("Early", 10, 1, order));
    EXPECT_EQ(stack.GetCount(), static_cast<size_t>(2));

    PoseContext pose;
    pose.boneTransforms.resize(1);
    stack.Apply(pose);

    ASSERT_EQ(order.size(), static_cast<size_t>(2));
    EXPECT_EQ(order[0], 1);
    EXPECT_EQ(order[1], 2);
    EXPECT_NEAR(pose.boneTransforms[0].position.x, 12.0f, kTolerance);

    EXPECT_TRUE(stack.RemoveModifier("Early"));
    EXPECT_FALSE(stack.RemoveModifier("Missing"));
    EXPECT_EQ(stack.GetCount(), static_cast<size_t>(1));
    stack.Clear();
    EXPECT_EQ(stack.GetCount(), static_cast<size_t>(0));
}

TEST(LookAtModifier_InvalidAndDegenerateTargetsLeavePoseUntouched)
{
    PoseContext pose;
    pose.boneTransforms.resize(1);

    LookAtModifier lookAt;
    lookAt.Apply(pose);
    EXPECT_NEAR(pose.boneTransforms[0].rotation.w, 1.0f, kTolerance);

    lookAt.SetBoneIndex(0);
    lookAt.SetTargetPosition(pose.boneTransforms[0].position);
    lookAt.Apply(pose);
    EXPECT_NEAR(pose.boneTransforms[0].rotation.w, 1.0f, kTolerance);

    lookAt.SetTargetPosition({0.0f, 0.0f, 10.0f});
    lookAt.Apply(pose);
    EXPECT_NEAR(pose.boneTransforms[0].rotation.w, 1.0f, kTolerance);
}

TEST(LookAtModifier_RotatesTowardTargetWithWeightAndAngleLimits)
{
    PoseContext pose;
    pose.boneTransforms.resize(1);

    LookAtModifier lookAt;
    lookAt.SetBoneIndex(0);
    lookAt.SetTargetPosition({10.0f, 0.0f, 0.0f});
    lookAt.SetMaxAngle(0.5f);
    lookAt.SetWeight(2.0f); // Clamped to one.
    lookAt.Apply(pose);

    EXPECT_NEAR(QuaternionLength(pose.boneTransforms[0].rotation), 1.0f, kTolerance);
    EXPECT_NEAR(pose.boneTransforms[0].rotation.y, std::sin(0.25f), kTolerance);
    EXPECT_NEAR(pose.boneTransforms[0].rotation.w, std::cos(0.25f), kTolerance);

    pose.boneTransforms[0].rotation = {0.0f, 0.0f, 0.0f, 1.0f};
    lookAt.SetWeight(-1.0f); // Clamped to zero.
    lookAt.Apply(pose);
    EXPECT_NEAR(pose.boneTransforms[0].rotation.y, 0.0f, kTolerance);
    EXPECT_NEAR(pose.boneTransforms[0].rotation.w, 1.0f, kTolerance);
}

TEST(AimIKModifier_ValidatesBoneChainAndHandlesDegenerateTarget)
{
    PoseContext pose;
    pose.boneTransforms.resize(3);

    AimIKModifier aim;
    aim.Apply(pose);
    EXPECT_NEAR(pose.boneTransforms[2].rotation.w, 1.0f, kTolerance);

    aim.SetBoneChain(0, 1, 2);
    aim.SetTargetPosition(pose.boneTransforms[2].position);
    aim.Apply(pose);
    EXPECT_NEAR(pose.boneTransforms[0].rotation.w, 1.0f, kTolerance);

    aim.SetTargetPosition({0.0f, 0.0f, 10.0f});
    aim.Apply(pose);
    EXPECT_NEAR(pose.boneTransforms[1].rotation.w, 1.0f, kTolerance);
}

TEST(AimIKModifier_DistributesNormalizedRotationAcrossChain)
{
    PoseContext pose;
    pose.boneTransforms.resize(3);

    AimIKModifier aim;
    aim.SetBoneChain(0, 1, 2);
    aim.SetTargetPosition({10.0f, 0.0f, 0.0f});
    aim.SetWeight(1.0f);
    aim.Apply(pose);

    EXPECT_NEAR(QuaternionLength(pose.boneTransforms[0].rotation), 1.0f, kTolerance);
    EXPECT_NEAR(QuaternionLength(pose.boneTransforms[1].rotation), 1.0f, kTolerance);
    EXPECT_NEAR(QuaternionLength(pose.boneTransforms[2].rotation), 1.0f, kTolerance);
    EXPECT_GT(pose.boneTransforms[0].rotation.y, pose.boneTransforms[1].rotation.y);
    EXPECT_NEAR(pose.boneTransforms[1].rotation.y, pose.boneTransforms[2].rotation.y, kTolerance);
}

TEST(FootPlacementModifier_NoValidFeetLeavesPoseUntouched)
{
    PoseContext pose;
    pose.boneTransforms.resize(3);
    pose.boneTransforms[0].position.y = 2.0f;

    FootPlacementModifier feet;
    feet.SetPelvisIndex(0);
    feet.SetGroundHeights(-1.0f, 1.0f);
    feet.Apply(pose);

    EXPECT_NEAR(pose.boneTransforms[0].position.y, 2.0f, kTolerance);
}

TEST(FootPlacementModifier_AdjustsFeetPelvisAndClampsOffsets)
{
    PoseContext pose;
    pose.boneTransforms.resize(3);
    pose.boneTransforms[0].position.y = 2.0f;
    pose.boneTransforms[1].position.y = 1.0f;
    pose.boneTransforms[2].position.y = 1.0f;

    FootPlacementModifier feet;
    feet.SetPelvisIndex(0);
    feet.SetLeftFootIndex(1);
    feet.SetRightFootIndex(2);
    feet.SetGroundHeights(0.0f, 2.0f);
    feet.SetMaxAdjustment(0.5f);
    feet.SetWeight(1.0f);
    feet.Apply(pose);

    EXPECT_NEAR(pose.boneTransforms[0].position.y, 1.5f, kTolerance);
    EXPECT_NEAR(pose.boneTransforms[1].position.y, 0.5f, kTolerance);
    EXPECT_NEAR(pose.boneTransforms[2].position.y, 1.5f, kTolerance);
}

TEST(FootPlacementModifier_SupportsSingleFootAndClampedWeight)
{
    PoseContext pose;
    pose.boneTransforms.resize(2);
    pose.boneTransforms[0].position.y = 1.0f;
    pose.boneTransforms[1].position.y = 0.0f;

    FootPlacementModifier feet;
    feet.SetPelvisIndex(0);
    feet.SetLeftFootIndex(1);
    feet.SetGroundHeights(1.0f, 0.0f);
    feet.SetMaxAdjustment(2.0f);
    feet.SetWeight(2.0f); // Clamped to one.
    feet.Apply(pose);

    EXPECT_NEAR(pose.boneTransforms[0].position.y, 1.0f, kTolerance);
    EXPECT_NEAR(pose.boneTransforms[1].position.y, 1.0f, kTolerance);
}
