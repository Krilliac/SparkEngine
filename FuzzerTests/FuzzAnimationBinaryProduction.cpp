/** @brief libc++-compiled production adapter for the .skel/.sanim decoder fuzzing. */
#include "FuzzAnimationBinaryProduction.h"

#include "Engine/Animation/AnimationBinaryFormat.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace
{
    constexpr std::size_t kMaxInputBytes = 1024u * 1024u + 1u;
    constexpr std::size_t kVersionOffset = 4;
    constexpr std::size_t kCountOffset = 8;

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzAnimationBinary: %s\n", what);
        std::abort();
    }

    bool HasMagic(std::span<const std::uint8_t> bytes, const char* magic)
    {
        return bytes.size() >= 4 && std::memcmp(bytes.data(), magic, 4) == 0;
    }

    /// Independent header model: an accepted file is version 1 and holds exactly the records its header counts.
    void CheckHeader(std::span<const std::uint8_t> bytes, std::size_t decodedCount)
    {
        if (bytes.size() < kCountOffset + sizeof(std::uint32_t))
        {
            InvariantFailure("accepted a file shorter than its header");
        }
        std::uint32_t version = 0;
        std::uint32_t count = 0;
        std::memcpy(&version, bytes.data() + kVersionOffset, sizeof(version));
        std::memcpy(&count, bytes.data() + kCountOffset, sizeof(count));
        if (version != Spark::Animation::kAnimationBinaryVersion)
        {
            InvariantFailure("accepted a file with an unknown version");
        }
        if (count != decodedCount)
        {
            InvariantFailure("decoded record count disagrees with the header");
        }
    }

    bool IsFiniteMatrix(const XMFLOAT4X4& matrix)
    {
        for (int row = 0; row < 4; ++row)
        {
            for (int column = 0; column < 4; ++column)
            {
                if (!std::isfinite(matrix.m[row][column]))
                {
                    return false;
                }
            }
        }
        return true;
    }

    bool IsFiniteValue(const XMFLOAT3& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }

    bool IsFiniteValue(const XMFLOAT4& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
    }

    template <typename Key> void CheckTrack(const std::vector<Key>& keys)
    {
        for (std::size_t i = 0; i < keys.size(); ++i)
        {
            if (!std::isfinite(keys[i].time) || !IsFiniteValue(keys[i].value))
            {
                InvariantFailure("accepted key has a non-finite time or value");
            }
            if (i > 0 && keys[i].time < keys[i - 1].time)
            {
                InvariantFailure("accepted key times decrease");
            }
        }
    }

    void CheckSkeleton(std::span<const std::uint8_t> bytes)
    {
        Spark::Animation::Skeleton skeleton;
        skeleton.name = "sentinel";
        skeleton.bones.resize(1);
        skeleton.bones[0].name = "sentinel-bone";
        skeleton.boneNameToIndex["sentinel-bone"] = 0;
        std::string error;
        if (!Spark::Animation::DecodeSkeletonBinary(bytes, skeleton, error))
        {
            if (error.empty() || skeleton.name != "sentinel" || skeleton.bones.size() != 1 ||
                skeleton.bones[0].name != "sentinel-bone" || skeleton.boneNameToIndex.size() != 1 ||
                skeleton.FindBone("sentinel-bone") != 0)
            {
                InvariantFailure("rejected skeleton input modified the caller's skeleton or gave no reason");
            }
            return;
        }
        if (!HasMagic(bytes, "SKEL") || skeleton.name != "sentinel")
        {
            InvariantFailure("accepted skeleton lacks its magic or replaced the caller's name");
        }
        CheckHeader(bytes, skeleton.bones.size());
        const auto boneCount = static_cast<std::int32_t>(skeleton.bones.size());
        for (std::int32_t index = 0; index < boneCount; ++index)
        {
            const Spark::Animation::Bone& bone = skeleton.bones[static_cast<std::size_t>(index)];
            if (bone.parentIndex < -1 || bone.parentIndex >= index)
            {
                InvariantFailure("accepted bone's parent does not precede it");
            }
            if (!IsFiniteMatrix(bone.offsetMatrix) || !IsFiniteMatrix(bone.localBindPose))
            {
                InvariantFailure("accepted bone has a non-finite matrix");
            }
            // Duplicate names are allowed; the index names the last bone carrying the name.
            const std::int32_t named = skeleton.FindBone(bone.name);
            if (named < index || named >= boneCount ||
                skeleton.bones[static_cast<std::size_t>(named)].name != bone.name)
            {
                InvariantFailure("bone name index does not resolve to the last bone with that name");
            }
        }
        if (skeleton.boneNameToIndex.size() > skeleton.bones.size())
        {
            InvariantFailure("bone name index names more bones than the skeleton holds");
        }
    }

    void CheckClips(std::span<const std::uint8_t> bytes)
    {
        std::vector<Spark::Animation::AnimationClip> clips(1);
        clips[0].name = "sentinel";
        std::string error;
        if (!Spark::Animation::DecodeAnimationClipsBinary(bytes, clips, error))
        {
            if (error.empty() || clips.size() != 1 || clips[0].name != "sentinel" || !clips[0].channels.empty())
            {
                InvariantFailure("rejected clip input modified the caller's clips or gave no reason");
            }
            return;
        }
        if (!HasMagic(bytes, "ANIM"))
        {
            InvariantFailure("accepted clips lack their magic");
        }
        CheckHeader(bytes, clips.size());
        for (const Spark::Animation::AnimationClip& clip : clips)
        {
            if (!std::isfinite(clip.duration) || clip.duration < 0.0f || !std::isfinite(clip.ticksPerSecond) ||
                clip.ticksPerSecond < 0.0f)
            {
                InvariantFailure("accepted clip has non-finite or negative timing");
            }
            for (const Spark::Animation::BoneAnimation& channel : clip.channels)
            {
                CheckTrack(channel.positionKeys);
                CheckTrack(channel.rotationKeys);
                CheckTrack(channel.scaleKeys);
            }
        }
    }
} // namespace

extern "C" int SparkFuzzDecodeAnimationBinary(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes || (data == nullptr && size != 0))
    {
        return 0;
    }
    const std::span<const std::uint8_t> bytes(data, size);
    // Each decoder also sees the other format's files and arbitrary bytes, which it must reject cleanly.
    const bool skeletonFile = HasMagic(bytes, "SKEL");
    const bool clipFile = HasMagic(bytes, "ANIM");
    if (skeletonFile || !clipFile)
    {
        CheckSkeleton(bytes);
    }
    if (clipFile || !skeletonFile)
    {
        CheckClips(bytes);
    }
    return 0;
}
