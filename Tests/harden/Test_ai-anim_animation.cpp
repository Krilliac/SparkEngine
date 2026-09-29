// Test_ai-anim_animation.cpp - Hardening regression tests for the Animation subsystem.
//
// Covers two audit findings, plus the SEC-120 .skel/.sanim value checks (unknown versions,
// non-finite or negative clip timing, unsorted or non-finite key times):
//   * P1: AnimationManager::LoadSkeleton did `bones.reserve(boneCount)` with boneCount
//         read straight from an untrusted .skel file — a corrupt count near 0xFFFFFFFF
//         triggered a multi-GB allocation (bad_alloc / DoS) before the read loop ran.
//   * P2: AnimationEvaluator::SolveLookAtIK aimed using the bone's LOCAL-space position
//         and overwrote the bone's rotation & scale instead of blending an aim rotation.
//
// These exercise the real engine implementation (SparkTests links SparkEngineLib).

#include "TestFramework.h"

#include "Engine/Animation/AnimationBinaryFormat.h"
#include "Engine/Animation/AnimationSystem.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
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
        PutBytes(buf, uint32_t{1});       // version
        PutBytes(buf, declaredBoneCount); // boneCount (untrusted)

        for (uint32_t i = 0; i < realBones; ++i)
        {
            const std::string name = "bone";
            PutBytes(buf, static_cast<uint32_t>(name.size()));
            buf.insert(buf.end(), name.begin(), name.end());
            PutBytes(buf, int32_t{-1}); // parentIndex
            XMFLOAT4X4 identity;
            XMStoreFloat4x4(&identity, XMMatrixIdentity());
            PutBytes(buf, identity); // offsetMatrix
            PutBytes(buf, identity); // localBindPose
        }

        std::filesystem::path path = std::filesystem::temp_directory_path() / ("spark_harden_" + tag + ".skel");
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
        out.close();
        return path.string();
    }

    std::string WriteTempFile(const std::vector<char>& buf, const std::string& fileName)
    {
        const std::filesystem::path path = std::filesystem::temp_directory_path() / ("spark_harden_" + fileName);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
        out.close();
        return path.string();
    }

    // A .sanim with one clip holding one channel, matching AnimationManager::LoadAnimations'
    // binary layout. The channel carries one position key per entry of `positionKeyTimes`
    // and one identity rotation and unit scale key at time 0.
    std::vector<char> OneClipAnimation(float duration, float ticksPerSecond, const std::vector<float>& positionKeyTimes)
    {
        std::vector<char> buf;
        buf.insert(buf.end(), {'A', 'N', 'I', 'M'});
        PutBytes(buf, uint32_t{1}); // version
        PutBytes(buf, uint32_t{1}); // clipCount
        const std::string clipName = "walk";
        PutBytes(buf, static_cast<uint32_t>(clipName.size()));
        buf.insert(buf.end(), clipName.begin(), clipName.end());
        PutBytes(buf, duration);
        PutBytes(buf, ticksPerSecond);
        buf.push_back(1);           // loop
        PutBytes(buf, uint32_t{1}); // channelCount
        const std::string boneName = "root";
        PutBytes(buf, static_cast<uint32_t>(boneName.size()));
        buf.insert(buf.end(), boneName.begin(), boneName.end());
        PutBytes(buf, int32_t{0}); // boneIndex
        PutBytes(buf, static_cast<uint32_t>(positionKeyTimes.size()));
        for (const float time : positionKeyTimes)
        {
            PutBytes(buf, time);
            PutBytes(buf, XMFLOAT3{0.0f, 0.0f, 0.0f});
        }
        PutBytes(buf, uint32_t{1}); // rotKeyCount
        PutBytes(buf, 0.0f);
        PutBytes(buf, XMFLOAT4{0.0f, 0.0f, 0.0f, 1.0f});
        PutBytes(buf, uint32_t{1}); // sclKeyCount
        PutBytes(buf, 0.0f);
        PutBytes(buf, XMFLOAT3{1.0f, 1.0f, 1.0f});
        return buf;
    }

    size_t LoadedClipCount(const std::vector<char>& buf, const std::string& fileName)
    {
        const std::string path = WriteTempFile(buf, fileName);
        const size_t count = AnimationManager::GetInstance().LoadAnimations(path).size();
        std::error_code ec;
        std::filesystem::remove(path, ec);
        return count;
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
// SEC-120 animation-skel-sanim: values the layout can carry but playback cannot use.
// ============================================================================

TEST(Animation_LoadAnimations_RejectsNonFiniteClipTiming)
{
    // Playback computes fmod(time, duration) and time / duration from these fields.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    EXPECT_EQ(LoadedClipCount(OneClipAnimation(nan, 30.0f, {0.0f}), "nan_duration.sanim"), 0u);
    EXPECT_EQ(LoadedClipCount(OneClipAnimation(-1.0f, 30.0f, {0.0f}), "negative_duration.sanim"), 0u);
    EXPECT_EQ(LoadedClipCount(OneClipAnimation(1.0f, inf, {0.0f}), "infinite_rate.sanim"), 0u);
    // The same clip with finite timing loads, so the rejections above are about the timing alone.
    EXPECT_EQ(LoadedClipCount(OneClipAnimation(1.0f, 30.0f, {0.0f}), "finite_timing.sanim"), 1u);
}

TEST(Animation_LoadAnimations_RejectsDecreasingKeyTimes)
{
    // Key sampling searches key times assuming they are sorted ascending (AnimationTypes.h).
    const float nan = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(LoadedClipCount(OneClipAnimation(1.0f, 30.0f, {1.0f, 0.5f}), "decreasing_keys.sanim"), 0u);
    EXPECT_EQ(LoadedClipCount(OneClipAnimation(1.0f, 30.0f, {0.0f, nan}), "nan_key.sanim"), 0u);
    // Equal and ascending key times stay loadable.
    EXPECT_EQ(LoadedClipCount(OneClipAnimation(1.0f, 30.0f, {0.0f, 0.5f, 0.5f, 1.0f}), "sorted_keys.sanim"), 1u);
}

TEST(Animation_LoadSkeleton_RejectsUnknownVersion)
{
    std::vector<char> buf;
    buf.insert(buf.end(), {'S', 'K', 'E', 'L'});
    PutBytes(buf, uint32_t{7}); // version: every engine-written .skel is version 1
    PutBytes(buf, uint32_t{1}); // boneCount
    const std::string name = "root";
    PutBytes(buf, static_cast<uint32_t>(name.size()));
    buf.insert(buf.end(), name.begin(), name.end());
    PutBytes(buf, int32_t{-1});
    XMFLOAT4X4 identity;
    XMStoreFloat4x4(&identity, XMMatrixIdentity());
    PutBytes(buf, identity);
    PutBytes(buf, identity);
    const std::string path = WriteTempFile(buf, "version7.skel");

    auto skeleton = AnimationManager::GetInstance().LoadSkeleton(path);
    ASSERT_TRUE(skeleton != nullptr);
    EXPECT_EQ(skeleton->GetBoneCount(), 0u);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(Animation_DecodeClips_RejectsKeyCountBeyondInput)
{
    // A 60-byte file declaring 1,000,000 rotation keys. The streaming loader resized the key
    // vector (20 MB) before reading a single key; the decoder checks the count against the
    // bytes left first and leaves the caller's clips untouched.
    std::vector<char> buf;
    buf.insert(buf.end(), {'A', 'N', 'I', 'M'});
    PutBytes(buf, uint32_t{1});         // version
    PutBytes(buf, uint32_t{1});         // clipCount
    PutBytes(buf, uint32_t{0});         // clip nameLen
    PutBytes(buf, 1.0f);                // duration
    PutBytes(buf, 30.0f);               // ticksPerSecond
    buf.push_back(0);                   // loop
    PutBytes(buf, uint32_t{1});         // channelCount
    PutBytes(buf, uint32_t{0});         // boneNameLen
    PutBytes(buf, int32_t{-1});         // boneIndex
    PutBytes(buf, uint32_t{0});         // posKeyCount
    PutBytes(buf, uint32_t{1'000'000}); // rotKeyCount, no keys follow
    buf.resize(60, 0);
    const std::vector<std::uint8_t> bytes(buf.begin(), buf.end());

    std::vector<AnimationClip> clips(1);
    clips[0].name = "sentinel";
    std::string error;
    EXPECT_FALSE(DecodeAnimationClipsBinary(bytes, clips, error));
    EXPECT_FALSE(error.empty());
    ASSERT_EQ(clips.size(), size_t{1});
    EXPECT_EQ(clips[0].name, std::string("sentinel"));

    // The same bytes with the key count matching what is present decode.
    const size_t rotKeyCountOffset = 4 + 4 + 4 + 4 + 4 + 4 + 1 + 4 + 4 + 4 + 4;
    std::vector<std::uint8_t> fitting(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(rotKeyCountOffset));
    const std::uint32_t oneKey = 1;
    const auto* oneKeyBytes = reinterpret_cast<const std::uint8_t*>(&oneKey);
    fitting.insert(fitting.end(), oneKeyBytes, oneKeyBytes + sizeof(oneKey));
    const float rotationKey[5] = {0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    const auto* rotationKeyBytes = reinterpret_cast<const std::uint8_t*>(rotationKey);
    fitting.insert(fitting.end(), rotationKeyBytes, rotationKeyBytes + sizeof(rotationKey));
    const std::uint32_t noScaleKeys = 0;
    const auto* noScaleKeyBytes = reinterpret_cast<const std::uint8_t*>(&noScaleKeys);
    fitting.insert(fitting.end(), noScaleKeyBytes, noScaleKeyBytes + sizeof(noScaleKeys));
    EXPECT_TRUE(DecodeAnimationClipsBinary(fitting, clips, error));
    ASSERT_EQ(clips.size(), size_t{1});
    ASSERT_EQ(clips[0].channels.size(), size_t{1});
    EXPECT_EQ(clips[0].channels[0].rotationKeys.size(), size_t{1});

    // A skeleton declaring more bones than its bytes can hold is rejected the same way.
    Skeleton skeleton;
    skeleton.name = "sentinel";
    skeleton.bones.resize(1);
    std::vector<std::uint8_t> skel = {'S', 'K', 'E', 'L', 1, 0, 0, 0, 0xA0, 0x86, 0x01, 0x00}; // 100,000 bones
    EXPECT_FALSE(DecodeSkeletonBinary(skel, skeleton, error));
    EXPECT_EQ(skeleton.name, std::string("sentinel"));
    EXPECT_EQ(skeleton.GetBoneCount(), 1u);
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
