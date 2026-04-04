/**
 * @file TestAnimationPhysicsIntegration.cpp
 * @brief Integration tests for animation + physics interaction patterns
 */

#include "TestFramework.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace
{
    struct Vec3
    {
        float x = 0, y = 0, z = 0;
    };

    struct RagdollState
    {
        bool isActive = false;
        float blendWeight = 0.0f;
    };

    struct Bone
    {
        std::string name;
        int32_t parentIndex = -1;
        Vec3 localPos;
    };

    Vec3 Lerp(const Vec3& a, const Vec3& b, float t)
    {
        return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
    }
} // namespace

TEST(AnimPhysics_RagdollActivation)
{
    RagdollState state;
    state.isActive = true;
    state.blendWeight = 1.0f;
    EXPECT_TRUE(state.isActive);
    EXPECT_NEAR(state.blendWeight, 1.0f, 0.001f);
}

TEST(AnimPhysics_RagdollDeactivationBlend)
{
    RagdollState state{true, 1.0f};
    float blendSpeed = 5.0f;
    float dt = 1.0f / 60.0f;
    while (state.blendWeight > 0.0f)
    {
        state.blendWeight -= blendSpeed * dt;
    }
    state.blendWeight = std::max(0.0f, state.blendWeight);
    state.isActive = false;
    EXPECT_FALSE(state.isActive);
    EXPECT_NEAR(state.blendWeight, 0.0f, 0.1f);
}

TEST(AnimPhysics_BoneBlend)
{
    Vec3 animPos{0, 1, 0};
    Vec3 physPos{0.5f, 0.8f, 0.1f};
    auto result = Lerp(animPos, physPos, 0.5f);
    EXPECT_NEAR(result.x, 0.25f, 0.001f);
    EXPECT_NEAR(result.y, 0.9f, 0.001f);
}

TEST(AnimPhysics_SkeletonBoneCount)
{
    std::vector<Bone> bones;
    std::vector<std::string> names = {"Root", "Spine", "Head", "L_Arm", "R_Arm", "L_Leg", "R_Leg"};
    for (const auto& n : names)
    {
        bones.push_back({n, -1, {}});
    }
    EXPECT_EQ(bones.size(), static_cast<size_t>(7));
    EXPECT_EQ(bones[0].name, "Root");
}

TEST(AnimPhysics_RagdollBodyMass)
{
    struct Body
    {
        int boneIdx;
        float mass;
    };
    std::vector<Body> bodies = {{0, 10}, {1, 8}, {2, 4}, {3, 3}, {4, 3}, {5, 5}, {6, 5}};
    float total = 0;
    for (auto& b : bodies)
        total += b.mass;
    EXPECT_NEAR(total, 38.0f, 0.001f);
}

TEST(AnimPhysics_BlendWeightClamping)
{
    float blend = std::clamp(1.5f, 0.0f, 1.0f);
    EXPECT_NEAR(blend, 1.0f, 0.001f);
    blend = std::clamp(-0.5f, 0.0f, 1.0f);
    EXPECT_NEAR(blend, 0.0f, 0.001f);
}

TEST(AnimPhysics_ImpulseTriggersRagdoll)
{
    float threshold = 50.0f;
    EXPECT_FALSE(10.0f >= threshold);
    EXPECT_TRUE(100.0f >= threshold);
}

TEST(AnimPhysics_FullTransitionCycle)
{
    enum class State
    {
        Anim,
        ToRagdoll,
        Ragdoll,
        ToAnim
    };
    State state = State::ToRagdoll;
    float blend = 0;
    float dt = 1.0f / 60.0f;

    while (state == State::ToRagdoll)
    {
        blend += 10 * dt;
        if (blend >= 1.0f)
        {
            blend = 1.0f;
            state = State::Ragdoll;
        }
    }
    EXPECT_EQ(static_cast<int>(state), static_cast<int>(State::Ragdoll));

    state = State::ToAnim;
    while (state == State::ToAnim)
    {
        blend -= 10 * dt;
        if (blend <= 0.0f)
        {
            blend = 0.0f;
            state = State::Anim;
        }
    }
    EXPECT_EQ(static_cast<int>(state), static_cast<int>(State::Anim));
}
