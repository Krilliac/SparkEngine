// TestPoseModifier.cpp - Tests for pose modifier stack: add, apply, and clear
// Standalone implementations for CI testing

#include "TestFramework.h"
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace TestPoseMod
{

    struct Vec3
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
    };

    struct Quat
    {
        float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
    };

    struct BoneTransform
    {
        Vec3 position;
        Quat rotation;
        Vec3 scale = {1.0f, 1.0f, 1.0f};
    };

    struct Pose
    {
        std::vector<BoneTransform> bones;
    };

    // A modifier is a function that transforms a pose
    using ModifierFunc = std::function<void(Pose&)>;

    struct PoseModifier
    {
        std::string name;
        int priority = 0;
        ModifierFunc func;
    };

    struct PoseModifierStack
    {
        std::vector<PoseModifier> modifiers;

        void AddModifier(const std::string& name, int priority, ModifierFunc func)
        {
            modifiers.push_back({name, priority, std::move(func)});
            // Sort by priority (lower runs first)
            for (size_t i = modifiers.size() - 1; i > 0; i--)
            {
                if (modifiers[i].priority < modifiers[i - 1].priority)
                    std::swap(modifiers[i], modifiers[i - 1]);
            }
        }

        void Apply(Pose& pose) const
        {
            for (const auto& mod : modifiers)
                mod.func(pose);
        }

        void Clear() { modifiers.clear(); }

        int GetModifierCount() const { return static_cast<int>(modifiers.size()); }
    };

} // namespace TestPoseMod

// ============================================================================
// Add and Count
// ============================================================================

TEST(PoseModifier_AddModifier)
{
    TestPoseMod::PoseModifierStack stack;
    stack.AddModifier("IKLeg", 10, [](TestPoseMod::Pose&) {});
    stack.AddModifier("LookAt", 5, [](TestPoseMod::Pose&) {});
    EXPECT_EQ(stack.GetModifierCount(), 2);

    // Lower priority should be first
    EXPECT_EQ(stack.modifiers[0].name, std::string("LookAt"));
    EXPECT_EQ(stack.modifiers[1].name, std::string("IKLeg"));
}

// ============================================================================
// Apply — verify transforms change
// ============================================================================

TEST(PoseModifier_Apply_TransformsChange)
{
    TestPoseMod::PoseModifierStack stack;

    // Modifier that offsets all bones upward
    stack.AddModifier("LiftAll", 0,
                      [](TestPoseMod::Pose& pose)
                      {
                          for (auto& bone : pose.bones)
                              bone.position.y += 5.0f;
                      });

    TestPoseMod::Pose pose;
    pose.bones.resize(3);
    pose.bones[0].position = {0, 0, 0};
    pose.bones[1].position = {1, 1, 0};
    pose.bones[2].position = {2, 2, 0};

    stack.Apply(pose);
    EXPECT_NEAR(pose.bones[0].position.y, 5.0f, 0.001f);
    EXPECT_NEAR(pose.bones[1].position.y, 6.0f, 0.001f);
    EXPECT_NEAR(pose.bones[2].position.y, 7.0f, 0.001f);
}

TEST(PoseModifier_Apply_MultipleModifiers)
{
    TestPoseMod::PoseModifierStack stack;

    // First: scale positions by 2
    stack.AddModifier("Scale", 0,
                      [](TestPoseMod::Pose& pose)
                      {
                          for (auto& bone : pose.bones)
                          {
                              bone.position.x *= 2.0f;
                              bone.position.y *= 2.0f;
                              bone.position.z *= 2.0f;
                          }
                      });

    // Second: offset by 1
    stack.AddModifier("Offset", 10,
                      [](TestPoseMod::Pose& pose)
                      {
                          for (auto& bone : pose.bones)
                              bone.position.x += 1.0f;
                      });

    TestPoseMod::Pose pose;
    pose.bones.resize(1);
    pose.bones[0].position = {3.0f, 0.0f, 0.0f};

    stack.Apply(pose);
    // 3 * 2 = 6, then + 1 = 7
    EXPECT_NEAR(pose.bones[0].position.x, 7.0f, 0.001f);
}

// ============================================================================
// Clear
// ============================================================================

TEST(PoseModifier_Clear)
{
    TestPoseMod::PoseModifierStack stack;
    stack.AddModifier("A", 0, [](TestPoseMod::Pose&) {});
    stack.AddModifier("B", 1, [](TestPoseMod::Pose&) {});
    EXPECT_EQ(stack.GetModifierCount(), 2);

    stack.Clear();
    EXPECT_EQ(stack.GetModifierCount(), 0);

    // Apply on empty stack should not modify pose
    TestPoseMod::Pose pose;
    pose.bones.resize(1);
    pose.bones[0].position = {1, 2, 3};
    stack.Apply(pose);
    EXPECT_NEAR(pose.bones[0].position.x, 1.0f, 0.001f);
}
