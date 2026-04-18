/**
 * @file TestAnimationRetargeting.cpp
 * @brief Tests for Spark::Animation::AnimationRetargeting
 */

#include "TestFramework.h"
#include "Engine/Animation/AnimationRetargeting.h"

using namespace Spark::Animation;

// Helper to create a simple test skeleton
static Skeleton MakeTestSkeleton(const std::vector<std::string>& boneNames, const std::vector<int>& parentIndices)
{
    Skeleton skel;
    for (size_t i = 0; i < boneNames.size(); ++i)
    {
        Bone bone;
        bone.name = boneNames[i];
        bone.parentIndex = parentIndices[i];
        // Set a simple offset matrix with height info
        bone.offsetMatrix = {};
        bone.offsetMatrix.m[0][0] = 1.0f;
        bone.offsetMatrix.m[1][1] = 1.0f;
        bone.offsetMatrix.m[2][2] = 1.0f;
        bone.offsetMatrix.m[3][3] = 1.0f;
        bone.offsetMatrix.m[3][1] = static_cast<float>(i) * 0.5f; // Increasing height
        bone.localBindPose = bone.offsetMatrix;
        skel.bones.push_back(bone);
        skel.boneNameToIndex[bone.name] = static_cast<int>(i);
    }
    return skel;
}

TEST(Retarget_ExactNameMatch)
{
    auto source = MakeTestSkeleton({"Root", "Spine", "Head"}, {-1, 0, 1});
    auto target = MakeTestSkeleton({"Root", "Spine", "Head"}, {-1, 0, 1});

    SkeletonMapping mapping;
    int count = mapping.BuildFromBoneNames(source, target);

    EXPECT_EQ(count, 3); // All 3 bones should match
    EXPECT_EQ(mapping.GetTargetBone(0), 0);
    EXPECT_EQ(mapping.GetTargetBone(1), 1);
    EXPECT_EQ(mapping.GetTargetBone(2), 2);
}

TEST(Retarget_PrefixStripping)
{
    // Source uses Mixamo-style names, target uses plain names
    auto source = MakeTestSkeleton({"mixamorig:Hips", "mixamorig:Spine", "mixamorig:Head"}, {-1, 0, 1});
    auto target = MakeTestSkeleton({"Hips", "Spine", "Head"}, {-1, 0, 1});

    SkeletonMapping mapping;
    int count = mapping.BuildFromBoneNames(source, target);

    EXPECT_EQ(count, 3);
    EXPECT_EQ(mapping.GetTargetBone(0), 0);
}

TEST(Retarget_Bip01PrefixStripping)
{
    auto source = MakeTestSkeleton({"Bip01_Pelvis", "Bip01_Spine"}, {-1, 0});
    auto target = MakeTestSkeleton({"Pelvis", "Spine"}, {-1, 0});

    SkeletonMapping mapping;
    int count = mapping.BuildFromBoneNames(source, target);

    EXPECT_EQ(count, 2);
}

TEST(Retarget_CaseInsensitiveMatch)
{
    auto source = MakeTestSkeleton({"root", "spine"}, {-1, 0});
    auto target = MakeTestSkeleton({"ROOT", "SPINE"}, {-1, 0});

    SkeletonMapping mapping;
    int count = mapping.BuildFromBoneNames(source, target);

    EXPECT_EQ(count, 2);
}

TEST(Retarget_PartialMatch)
{
    auto source = MakeTestSkeleton({"Root", "Spine", "LeftArm", "RightArm"}, {-1, 0, 1, 1});
    auto target = MakeTestSkeleton({"Root", "Spine", "LeftLeg"}, {-1, 0, 1});

    SkeletonMapping mapping;
    int count = mapping.BuildFromBoneNames(source, target);

    EXPECT_EQ(count, 2); // Only Root and Spine match
    EXPECT_EQ(mapping.GetTargetBone(0), 0);
    EXPECT_EQ(mapping.GetTargetBone(1), 1);
    EXPECT_EQ(mapping.GetTargetBone(2), -1); // LeftArm has no match
}

TEST(Retarget_ManualMapping)
{
    auto source = MakeTestSkeleton({"BoneA", "BoneB"}, {-1, 0});
    auto target = MakeTestSkeleton({"BoneX", "BoneY"}, {-1, 0});

    SkeletonMapping mapping;
    mapping.SetBoneMapping("BoneA", "BoneX");
    mapping.SetBoneMapping("BoneB", "BoneY");
    int resolved = mapping.ResolveNameMappings(source, target);

    EXPECT_EQ(resolved, 2);
    EXPECT_EQ(mapping.GetTargetBone(0), 0);
    EXPECT_EQ(mapping.GetTargetBone(1), 1);
}

TEST(Retarget_UnmappedBoneReturnsMinusOne)
{
    SkeletonMapping mapping;
    EXPECT_EQ(mapping.GetTargetBone(42), -1);
}

TEST(Retarget_RetargetPose)
{
    auto source = MakeTestSkeleton({"Root", "Spine", "Head"}, {-1, 0, 1});
    auto target = MakeTestSkeleton({"Root", "Spine", "Head"}, {-1, 0, 1});

    SkeletonMapping mapping;
    mapping.BuildFromBoneNames(source, target);

    // Create source pose with a rotation on Spine
    std::vector<BoneLocalTransform> srcPose(3);
    srcPose[0].position = {0.0f, 1.0f, 0.0f};
    srcPose[0].rotation = {0.0f, 0.0f, 0.0f, 1.0f};     // Identity
    srcPose[1].rotation = {0.0f, 0.707f, 0.0f, 0.707f}; // 90 deg Y rotation
    srcPose[2].rotation = {0.0f, 0.0f, 0.0f, 1.0f};

    auto result = AnimationRetargeter::Retarget(source, target, mapping, srcPose);

    EXPECT_EQ(static_cast<int>(result.localTransforms.size()), 3);
    EXPECT_EQ(result.mappedBones, 3);
    EXPECT_EQ(result.unmappedBones, 0);

    // Spine rotation should be transferred
    EXPECT_NEAR(result.localTransforms[1].rotation.y, 0.707f, 0.01f);
}

TEST(Retarget_UnmappedBonesGetBindPose)
{
    auto source = MakeTestSkeleton({"Root", "Spine"}, {-1, 0});
    auto target = MakeTestSkeleton({"Root", "Spine", "ExtraBone"}, {-1, 0, 1});

    SkeletonMapping mapping;
    mapping.BuildFromBoneNames(source, target);

    std::vector<BoneLocalTransform> srcPose(2);
    srcPose[0].rotation = {0.0f, 0.0f, 0.0f, 1.0f};
    srcPose[1].rotation = {0.0f, 0.5f, 0.0f, 0.866f};

    auto result = AnimationRetargeter::Retarget(source, target, mapping, srcPose);

    EXPECT_EQ(result.mappedBones, 2);
    EXPECT_EQ(result.unmappedBones, 1);

    // ExtraBone (unmapped) should have identity rotation
    EXPECT_NEAR(result.localTransforms[2].rotation.w, 1.0f, 0.01f);
}
