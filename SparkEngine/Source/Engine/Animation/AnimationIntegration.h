/**
 * @file AnimationIntegration.h
 * @brief Cross-links AnimationCompression with the AnimationSystem pipeline
 *
 * Integrates the animation compression system (R5.3) with the existing
 * AnimationManager and AnimationEvaluator to provide compressed clip
 * loading and on-the-fly decompression during playback.
 *
 * Also re-exports the split animation headers (R5.1) for convenience.
 *
 * @see AnimationSystem.h, AnimationCompression.h
 */

#pragma once

// Core animation system
#include "AnimationSystem.h"

// Split headers (R5.1)
#include "Skeleton.h"
#include "AnimationClip.h"
#include "AnimationBlender.h"
#include "AnimationStateMachine.h"
#include "AnimationEvaluator.h"
#include "IKSolver.h"

// Compression (R5.3)
#include "AnimationCompression.h"

#include <string>
#include <unordered_map>
#include <memory>
#include <sstream>

namespace Spark::Animation
{

    /**
     * @brief Extended AnimationManager with compression support.
     *
     * Wraps AnimationManager to add compressed clip storage and
     * transparent decompression during sampling. Compressed clips
     * use ~10-25% of the original memory footprint.
     *
     * ## Usage
     * @code
     *   AnimationPipeline pipeline;
     *
     *   // Compress a clip for storage
     *   pipeline.CompressAndStore("walk", walkClip, skeleton);
     *
     *   // Sample the compressed clip (decompresses on the fly)
     *   auto transforms = pipeline.SampleCompressed("walk", skeleton, time);
     *
     *   // Get compression statistics
     *   auto stats = pipeline.GetCompressionStats("walk");
     * @endcode
     */
    class AnimationPipeline
    {
      public:
        AnimationPipeline() = default;
        ~AnimationPipeline() = default;

        // Non-copyable
        AnimationPipeline(const AnimationPipeline&) = delete;
        AnimationPipeline& operator=(const AnimationPipeline&) = delete;

        /**
         * @brief Compress an animation clip and store it for later sampling.
         *
         * @param name      Identifier for the compressed clip.
         * @param clip      Source uncompressed clip.
         * @param skeleton  Target skeleton for bone validation.
         * @param positionThreshold  Keyframe reduction threshold for positions.
         * @param rotationThreshold  Keyframe reduction threshold for rotations.
         * @param scaleThreshold     Keyframe reduction threshold for scales.
         */
        void CompressAndStore(const std::string& name, const AnimationClip& clip, const Skeleton& skeleton,
                              float positionThreshold = 0.001f, float rotationThreshold = 0.001f,
                              float scaleThreshold = 0.001f)
        {
            auto compressed =
                Compression::CompressClip(clip, skeleton, positionThreshold, rotationThreshold, scaleThreshold);

            // Track original size for statistics
            CompressionStats stats;
            stats.originalKeyframeCount = 0;
            stats.compressedKeyframeCount = 0;
            for (const auto& channel : clip.channels)
            {
                stats.originalKeyframeCount +=
                    channel.positionKeys.size() + channel.rotationKeys.size() + channel.scaleKeys.size();
            }
            for (const auto& channel : compressed.channels)
            {
                stats.compressedKeyframeCount +=
                    channel.positions.size() + channel.rotations.size() + channel.scales.size();
            }
            stats.compressionRatio = stats.originalKeyframeCount > 0
                                         ? static_cast<float>(stats.compressedKeyframeCount) /
                                               static_cast<float>(stats.originalKeyframeCount)
                                         : 1.0f;
            stats.clipName = name;
            stats.duration = clip.duration;

            m_compressedClips[name] = std::move(compressed);
            m_stats[name] = stats;
        }

        /**
         * @brief Sample a compressed clip at a given time, producing local bone transforms.
         *
         * Decompresses on-the-fly using the smallest-three quaternion quantization
         * and interpolated keyframe lookup.
         *
         * @param name       Compressed clip identifier.
         * @param skeleton   Target skeleton.
         * @param time       Playback time in seconds.
         * @param outLocalTransforms  Output array (sized to skeleton bone count).
         * @return true if the clip was found and sampled.
         */
        bool SampleCompressed(const std::string& name, const Skeleton& skeleton, float time,
                              std::vector<DirectX::XMFLOAT4X4>& outLocalTransforms) const
        {
            auto it = m_compressedClips.find(name);
            if (it == m_compressedClips.end())
                return false;

            const auto& compressed = it->second;
            size_t boneCount = skeleton.GetBoneCount();
            outLocalTransforms.resize(boneCount);

            // Wrap time to clip duration
            float wrappedTime = time;
            if (compressed.duration > 0.0f)
            {
                wrappedTime = std::fmod(time, compressed.duration);
                if (wrappedTime < 0.0f)
                    wrappedTime += compressed.duration;
            }

            // Sample each bone from compressed data
            for (size_t boneIdx = 0; boneIdx < boneCount && boneIdx < compressed.channels.size(); ++boneIdx)
            {
                DirectX::XMFLOAT3 pos =
                    Compression::SamplePosition(compressed, static_cast<uint32_t>(boneIdx), wrappedTime);
                DirectX::XMFLOAT4 rot =
                    Compression::SampleRotation(compressed, static_cast<uint32_t>(boneIdx), wrappedTime);
                DirectX::XMFLOAT3 scl =
                    Compression::SampleScale(compressed, static_cast<uint32_t>(boneIdx), wrappedTime);

                // Build transform matrix from SRT
                DirectX::XMMATRIX S = DirectX::XMMatrixScaling(scl.x, scl.y, scl.z);
                DirectX::XMMATRIX R = DirectX::XMMatrixRotationQuaternion(DirectX::XMLoadFloat4(&rot));
                DirectX::XMMATRIX T = DirectX::XMMatrixTranslation(pos.x, pos.y, pos.z);
                DirectX::XMMATRIX transform = S * R * T;
                DirectX::XMStoreFloat4x4(&outLocalTransforms[boneIdx], transform);
            }

            // Bones without compressed channels use bind pose
            for (size_t boneIdx = compressed.channels.size(); boneIdx < boneCount; ++boneIdx)
            {
                outLocalTransforms[boneIdx] = skeleton.bones[boneIdx].localBindPose;
            }

            return true;
        }

        /**
         * @brief Check if a compressed clip exists.
         */
        bool HasCompressedClip(const std::string& name) const
        {
            return m_compressedClips.find(name) != m_compressedClips.end();
        }

        /**
         * @brief Get compression statistics for a clip.
         */
        struct CompressionStats
        {
            std::string clipName;
            float duration = 0.0f;
            size_t originalKeyframeCount = 0;
            size_t compressedKeyframeCount = 0;
            float compressionRatio = 1.0f;
        };

        const CompressionStats* GetCompressionStats(const std::string& name) const
        {
            auto it = m_stats.find(name);
            return it != m_stats.end() ? &it->second : nullptr;
        }

        /**
         * @brief Console integration: list all compressed clips with stats.
         */
        std::string Console_ListCompressedClips() const
        {
            std::stringstream ss;
            ss << "=== Compressed Animation Clips ===\n";
            for (const auto& [name, stats] : m_stats)
            {
                ss << "  " << name << ": " << stats.duration << "s, " << "ratio=" << stats.compressionRatio << " ("
                   << stats.compressedKeyframeCount << "/" << stats.originalKeyframeCount << " keys)\n";
            }
            if (m_stats.empty())
            {
                ss << "  (no compressed clips)\n";
            }
            return ss.str();
        }

      private:
        std::unordered_map<std::string, Compression::CompressedClip> m_compressedClips;
        std::unordered_map<std::string, CompressionStats> m_stats;
    };

} // namespace Spark::Animation
