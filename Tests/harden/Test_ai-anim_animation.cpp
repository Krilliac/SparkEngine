// Test_ai-anim_animation.cpp - Hardening regression tests for the Animation subsystem.
//
// Covers two audit findings:
//   * P1: AnimationManager::LoadSkeleton did `bones.reserve(boneCount)` with boneCount
//         read straight from an untrusted .skel file — a corrupt count near 0xFFFFFFFF
//         triggered a multi-GB allocation (bad_alloc / DoS) before the read loop ran.
//   * P2: AnimationEvaluator::SolveLookAtIK aimed using the bone's LOCAL-space position
//         and overwrote the bone's rotation & scale instead of blending an aim rotation.
//
// These exercise the real engine implementation (SparkTests links SparkEngineLib).

#include "TestFramework.h"

#include "Engine/Animation/AnimationSystem.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace Spark::Animation;
using namespace DirectX;

namespace
{
    template <typename T> void PutBytes(std::vector<char>& buf, const T& value)
    {
        const char* p = reinterpret_cast<const char*>(&value);
        buf.insert(buf.end(), p, p + sizeof(T));
    }

    // Write a .skel file matching AnimationManager::LoadSkeleton's binary layout, with a
    // caller-supplied bone count (which may be deliberately corrupt) and `realBones`
    // actual bone records appended.
    std::string WriteSkel(uint32_t declaredBoneCount, uint32_t realBones, const std::string& tag)
    {
        std::vector<char> buf;
        buf.insert(buf.end(), {'S', 'K', 'E', 'L'});
        PutBytes(buf, uint32_t{1});        // version
        PutBytes(buf, declaredBoneCount);  // boneCount (untrusted)

        for (uint32_t i = 0; i < realBones; ++i)
        {
            const std::string name = "bone";
            PutBytes(buf, static_cast<uint32_t>(name.size()));
            buf.insert(buf.end(), name.begin(), name.end());
            PutBytes(buf, int32_t{-1});     // parentIndex
            XMFLOAT4X4 identity;
            XMStoreFloat4x4(&identity, XMMatrixIdentity());
            PutBytes(buf, identity);        // offsetMatrix
            PutBytes(buf, identity);        // localBindPose
        }

        std::filesystem::path path =
            std::filesystem::temp_directory_path() / ("spark_harden_" + tag + ".skel");
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
        out.close();
        return path.string();
    }
} // namespace

// ============================================================================
// P1 regression: an absurd bone count is rejected instead of over-allocating.
// ============================================================================

TEST(Animation_LoadSkeleton_RejectsHugeBoneCount)
{
    // Declares ~4 billion bones but supplies none. Pre-fix this reserved multiple GB
    // and crashed; post-fix it must bail out and yield an empty (but valid) skeleton.
    std::string path = WriteSkel(0xFFFFFFFFu, /*realBones=*/0, "hugebones");

    auto& mgr = AnimationManager::GetInstance();
    auto skeleton = mgr.LoadSkeleton(path);

    EXPECT_TRUE(skeleton != nullptr);
    if (skeleton)
        EXPECT_EQ(skeleton->GetBoneCount(), 0u);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(Animation_LoadSkeleton_AcceptsValidSkeleton)
{
    // The bound must not reject a legitimate small skeleton.
    std::string path = WriteSkel(/*declaredBoneCount=*/2, /*realBones=*/2, "validbones");

    auto& mgr = AnimationManager::GetInstance();
    auto skeleton = mgr.LoadSkeleton(path);

    EXPECT_TRUE(skeleton != nullptr);
    if (skeleton)
        EXPECT_EQ(skeleton->GetBoneCount(), 2u);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

// ============================================================================
// P2 regression: LookAtIK preserves scale/translation and aims in world space.
// ============================================================================

TEST(Animation_SolveLookAtIK_PreservesScaleAndAimsAtTarget)
{
    // Single root bone with uniform scale 2 and translation (5,0,0). Its local +Z
    // forward starts pointing at world +Z; the target sits at +X, so the solver must
    // rotate the bone to face +X while keeping its scale (2) and translation intact.
    Skeleton skeleton;
    Bone bone;
    bone.name = "root";
    bone.parentIndex = -1;
    skeleton.bones.push_back(bone);

    std::vector<XMFLOAT4X4> localTransforms(1);
    XMMATRIX m = XMMatrixScaling(2.0f, 2.0f, 2.0f) * XMMatrixTranslation(5.0f, 0.0f, 0.0f);
    XMStoreFloat4x4(&localTransforms[0], m);

    IKChain chain;
    chain.enabled = true;
    chain.boneIndices = {0};
    chain.targetPosition = XMFLOAT3{15.0f, 0.0f, 0.0f};
    chain.weight = 1.0f;

    AnimationEvaluator::SolveLookAtIK(localTransforms, skeleton, chain);

    const XMFLOAT4X4& out = localTransforms[0];

    // Translation preserved (row 3 of the matrix).
    EXPECT_NEAR(out.m[3][0], 5.0f, 1e-3f);
    EXPECT_NEAR(out.m[3][1], 0.0f, 1e-3f);
    EXPECT_NEAR(out.m[3][2], 0.0f, 1e-3f);

    // Scale preserved: the length of each basis row stays 2 (pre-fix the solver threw
    // the scale away by rebuilding the matrix as pure rotation * translation).
    float row0Len = std::sqrt(out.m[0][0] * out.m[0][0] + out.m[0][1] * out.m[0][1] + out.m[0][2] * out.m[0][2]);
    float row2Len = std::sqrt(out.m[2][0] * out.m[2][0] + out.m[2][1] * out.m[2][1] + out.m[2][2] * out.m[2][2]);
    EXPECT_NEAR(row0Len, 2.0f, 1e-2f);
    EXPECT_NEAR(row2Len, 2.0f, 1e-2f);

    // Forward (+Z basis, normalized) now aims at the target direction (+X).
    float fx = out.m[2][0] / row2Len;
    float fy = out.m[2][1] / row2Len;
    float fz = out.m[2][2] / row2Len;
    EXPECT_NEAR(fx, 1.0f, 1e-2f);
    EXPECT_NEAR(fy, 0.0f, 1e-2f);
    EXPECT_NEAR(fz, 0.0f, 1e-2f);
}
