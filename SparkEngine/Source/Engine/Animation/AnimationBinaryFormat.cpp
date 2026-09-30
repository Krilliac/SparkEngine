/**
 * @file AnimationBinaryFormat.cpp
 * @brief Bounded decoders for the binary .skel and .sanim formats (see AnimationBinaryFormat.h).
 */

#include "AnimationBinaryFormat.h"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <format>
#include <limits>
#include <utility>

namespace Spark::Animation
{
    namespace
    {
        // Smallest encoding of one record, used to reject a declared count before anything is
        // allocated for it: a count is plausible only if that many minimal records still fit.
        constexpr std::size_t kBoneRecordMinBytes =
            sizeof(std::uint32_t) + sizeof(std::int32_t) + 2 * sizeof(XMFLOAT4X4);
        constexpr std::size_t kClipRecordMinBytes =
            sizeof(std::uint32_t) + 2 * sizeof(float) + 1 + sizeof(std::uint32_t);
        constexpr std::size_t kChannelRecordMinBytes =
            sizeof(std::uint32_t) + sizeof(std::int32_t) + 3 * sizeof(std::uint32_t);
        constexpr std::size_t kVectorKeyBytes = sizeof(float) + 3 * sizeof(float);
        constexpr std::size_t kQuatKeyBytes = sizeof(float) + 4 * sizeof(float);

        static_assert(sizeof(XMFLOAT3) == 3 * sizeof(float) && sizeof(XMFLOAT4) == 4 * sizeof(float) &&
                          sizeof(XMFLOAT4X4) == 64,
                      "the on-disk layout reads these types as packed floats");

        /// Forward-only reader over the input span. Every read is bounds-checked; a failed read consumes nothing.
        class ByteCursor
        {
          public:
            explicit ByteCursor(std::span<const std::uint8_t> bytes) : m_bytes(bytes) {}

            [[nodiscard]] std::size_t Remaining() const { return m_bytes.size() - m_offset; }

            /// True when `count` records of at least `recordBytes` each fit in what is left.
            [[nodiscard]] bool Fits(std::uint32_t count, std::size_t recordBytes) const
            {
                return count <= Remaining() / recordBytes;
            }

            template <typename T> bool Read(T& value)
            {
                if (Remaining() < sizeof(T))
                {
                    return false;
                }
                std::memcpy(&value, m_bytes.data() + m_offset, sizeof(T));
                m_offset += sizeof(T);
                return true;
            }

            bool ReadMagic(const char (&expected)[5])
            {
                if (Remaining() < 4 || std::memcmp(m_bytes.data() + m_offset, expected, 4) != 0)
                {
                    return false;
                }
                m_offset += 4;
                return true;
            }

            /// A length-prefixed name. A prefix at or above the cap is malformed, not skippable.
            bool ReadName(std::string& name)
            {
                std::uint32_t length = 0;
                if (!Read(length) || length >= kMaxAnimationAssetNameLength || Remaining() < length)
                {
                    return false;
                }
                name.assign(reinterpret_cast<const char*>(m_bytes.data() + m_offset), length);
                m_offset += length;
                return true;
            }

          private:
            std::span<const std::uint8_t> m_bytes;
            std::size_t m_offset = 0;
        };

        bool IsFinite(const XMFLOAT3& value)
        {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        }

        bool IsFinite(const XMFLOAT4& value)
        {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
        }

        /// A matrix read from a corrupt file carries NaNs straight into global transforms, skinning,
        /// and any physics driven from bone transforms.
        bool IsFiniteMatrix(const XMFLOAT4X4& matrix)
        {
            for (const auto& row : matrix.m)
            {
                for (const float value : row)
                {
                    if (!std::isfinite(value))
                    {
                        return false;
                    }
                }
            }
            return true;
        }

        bool ReadBone(ByteCursor& cursor, std::uint32_t index, Bone& bone, std::string& error)
        {
            if (!cursor.ReadName(bone.name))
            {
                error = std::format("bone {} has a truncated or over-long name", index);
                return false;
            }
            if (!cursor.Read(bone.parentIndex) || !cursor.Read(bone.offsetMatrix) || !cursor.Read(bone.localBindPose))
            {
                error = std::format("truncated at bone {}", index);
                return false;
            }
            // A parent must exist and must precede its child, so every consumer can walk the
            // hierarchy in index order without its own bounds check.
            if (bone.parentIndex < -1 || bone.parentIndex >= static_cast<std::int32_t>(index))
            {
                error = std::format("bone {} has invalid parent index {}", index, bone.parentIndex);
                return false;
            }
            if (!IsFiniteMatrix(bone.offsetMatrix) || !IsFiniteMatrix(bone.localBindPose))
            {
                error = std::format("bone {} has a non-finite matrix", index);
                return false;
            }
            return true;
        }

        /// One key track. Sampling binary-searches key times, so they must be finite and sorted.
        template <typename Key>
        bool ReadTrack(ByteCursor& cursor, std::size_t keyBytes, const char* kind, std::vector<Key>& keys,
                       std::string& error)
        {
            std::uint32_t count = 0;
            if (!cursor.Read(count))
            {
                error = std::format("truncated before the {} key count", kind);
                return false;
            }
            if (count > kMaxAnimationKeyframes || !cursor.Fits(count, keyBytes))
            {
                error = std::format("declares {} {} keys but only {} bytes remain", count, kind, cursor.Remaining());
                return false;
            }
            keys.resize(count);
            float previousTime = -std::numeric_limits<float>::infinity();
            for (Key& key : keys)
            {
                if (!cursor.Read(key.time) || !cursor.Read(key.value))
                {
                    error = std::format("truncated inside the {} keys", kind);
                    return false;
                }
                if (!std::isfinite(key.time) || key.time < previousTime || !IsFinite(key.value))
                {
                    error = std::format("has a non-finite or out-of-order {} key", kind);
                    return false;
                }
                previousTime = key.time;
            }
            return true;
        }

        bool ReadChannel(ByteCursor& cursor, BoneAnimation& channel, std::string& error)
        {
            if (!cursor.ReadName(channel.boneName))
            {
                error = "has a truncated or over-long bone name";
                return false;
            }
            // The cached bone index is only a hint: the evaluator bounds-checks it against the skeleton.
            if (!cursor.Read(channel.boneIndex))
            {
                error = "is truncated before its bone index";
                return false;
            }
            return ReadTrack(cursor, kVectorKeyBytes, "position", channel.positionKeys, error) &&
                   ReadTrack(cursor, kQuatKeyBytes, "rotation", channel.rotationKeys, error) &&
                   ReadTrack(cursor, kVectorKeyBytes, "scale", channel.scaleKeys, error);
        }

        bool ReadClip(ByteCursor& cursor, std::uint32_t clipIndex, AnimationClip& clip, std::string& error)
        {
            std::uint8_t loopByte = 0;
            std::uint32_t channelCount = 0;
            if (!cursor.ReadName(clip.name))
            {
                error = std::format("clip {} has a truncated or over-long name", clipIndex);
                return false;
            }
            if (!cursor.Read(clip.duration) || !cursor.Read(clip.ticksPerSecond) || !cursor.Read(loopByte) ||
                !cursor.Read(channelCount))
            {
                error = std::format("truncated in the header of clip {}", clipIndex);
                return false;
            }
            clip.loop = (loopByte != 0);
            // Playback computes fmod(time, duration) and time / duration.
            if (!std::isfinite(clip.duration) || clip.duration < 0.0f || !std::isfinite(clip.ticksPerSecond) ||
                clip.ticksPerSecond < 0.0f)
            {
                error = std::format("clip {} has a non-finite or negative duration or tick rate", clipIndex);
                return false;
            }
            if (channelCount > kMaxAnimationChannels || !cursor.Fits(channelCount, kChannelRecordMinBytes))
            {
                error = std::format("clip {} declares {} channels but only {} bytes remain", clipIndex, channelCount,
                                    cursor.Remaining());
                return false;
            }
            clip.channels.resize(channelCount);
            for (std::uint32_t channelIndex = 0; channelIndex < channelCount; ++channelIndex)
            {
                std::string channelError;
                if (!ReadChannel(cursor, clip.channels[channelIndex], channelError))
                {
                    error = std::format("channel {} of clip {} {}", channelIndex, clipIndex, channelError);
                    return false;
                }
            }
            return true;
        }
    } // namespace

    bool DecodeSkeletonBinary(std::span<const std::uint8_t> bytes, Skeleton& out, std::string& error)
    {
        ByteCursor cursor(bytes);
        if (!cursor.ReadMagic("SKEL"))
        {
            error = "not a .skel file (bad magic)";
            return false;
        }
        std::uint32_t version = 0;
        std::uint32_t boneCount = 0;
        if (!cursor.Read(version) || !cursor.Read(boneCount))
        {
            error = "truncated in the header";
            return false;
        }
        if (version != kAnimationBinaryVersion)
        {
            error = std::format("unsupported version {} (expected {})", version, kAnimationBinaryVersion);
            return false;
        }
        if (boneCount > kMaxSkeletonBones || !cursor.Fits(boneCount, kBoneRecordMinBytes))
        {
            error = std::format("declares {} bones but only {} bytes remain", boneCount, cursor.Remaining());
            return false;
        }

        std::vector<Bone> bones(boneCount);
        std::unordered_map<std::string, std::int32_t> boneNameToIndex;
        for (std::uint32_t index = 0; index < boneCount; ++index)
        {
            if (!ReadBone(cursor, index, bones[index], error))
            {
                return false;
            }
            boneNameToIndex[bones[index].name] = static_cast<std::int32_t>(index);
        }

        out.bones = std::move(bones);
        out.boneNameToIndex = std::move(boneNameToIndex);
        return true;
    }

    bool DecodeAnimationClipsBinary(std::span<const std::uint8_t> bytes, std::vector<AnimationClip>& out,
                                    std::string& error)
    {
        ByteCursor cursor(bytes);
        if (!cursor.ReadMagic("ANIM"))
        {
            error = "not a .sanim file (bad magic)";
            return false;
        }
        std::uint32_t version = 0;
        std::uint32_t clipCount = 0;
        if (!cursor.Read(version) || !cursor.Read(clipCount))
        {
            error = "truncated in the header";
            return false;
        }
        if (version != kAnimationBinaryVersion)
        {
            error = std::format("unsupported version {} (expected {})", version, kAnimationBinaryVersion);
            return false;
        }
        if (clipCount > kMaxAnimationClips || !cursor.Fits(clipCount, kClipRecordMinBytes))
        {
            error = std::format("declares {} clips but only {} bytes remain", clipCount, cursor.Remaining());
            return false;
        }

        std::vector<AnimationClip> clips(clipCount);
        for (std::uint32_t clipIndex = 0; clipIndex < clipCount; ++clipIndex)
        {
            if (!ReadClip(cursor, clipIndex, clips[clipIndex], error))
            {
                return false;
            }
        }

        out = std::move(clips);
        return true;
    }

} // namespace Spark::Animation
