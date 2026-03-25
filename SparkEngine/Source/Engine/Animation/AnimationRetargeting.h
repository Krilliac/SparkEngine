/**
 * @file AnimationRetargeting.h
 * @brief Skeleton retargeting for sharing animations between different characters
 *
 * Allows playing animations authored for one skeleton on a different skeleton
 * by mapping bones by name or explicit user-defined mapping tables.
 *
 * ## Usage
 * @code
 *   SkeletonMapping mapping;
 *   mapping.BuildFromBoneNames(sourceSkeleton, targetSkeleton);
 *   // or: mapping.SetBoneMapping("mixamorig:Hips", "Bip01_Pelvis");
 *
 *   RetargetedPose pose = AnimationRetargeter::Retarget(
 *       sourceSkeleton, targetSkeleton, mapping, sourceLocalTransforms);
 * @endcode
 */

#pragma once

#include "../../Core/Platform.h"
#include "../../Utils/Validate.h"
#include "Skeleton.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::Animation
{

    // ============================================================================
    // Bone Transform
    // ============================================================================

    struct BoneLocalTransform
    {
        XMFLOAT3 position = {0.0f, 0.0f, 0.0f};
        XMFLOAT4 rotation = {0.0f, 0.0f, 0.0f, 1.0f}; ///< Quaternion (x, y, z, w)
        XMFLOAT3 scale = {1.0f, 1.0f, 1.0f};
    };

    // ============================================================================
    // Skeleton Mapping
    // ============================================================================

    /**
 * @brief Maps bones from a source skeleton to a target skeleton
 */
    class SkeletonMapping
    {
      public:
        /**
     * @brief Automatically build mapping by matching bone names
     *
     * Uses exact name match first, then tries common prefix stripping
     * (e.g., "mixamorig:Spine" → "Spine", "Bip01_Spine" → "Spine").
     *
     * @param source Source skeleton
     * @param target Target skeleton
     * @return Number of successfully mapped bones
     */
        int BuildFromBoneNames(const Skeleton& source, const Skeleton& target)
        {
            m_sourceToTarget.clear();
            int mapped = 0;

            for (size_t i = 0; i < source.bones.size(); ++i)
            {
                const std::string& srcName = source.bones[i].name;

                // Try exact match first
                int targetIdx = target.FindBone(srcName);
                if (targetIdx >= 0)
                {
                    m_sourceToTarget[static_cast<int>(i)] = targetIdx;
                    mapped++;
                    continue;
                }

                // Try stripped name match (remove common prefixes)
                std::string strippedSrc = StripPrefix(srcName);
                for (size_t j = 0; j < target.bones.size(); ++j)
                {
                    std::string strippedTgt = StripPrefix(target.bones[j].name);
                    if (CaseInsensitiveEqual(strippedSrc, strippedTgt))
                    {
                        m_sourceToTarget[static_cast<int>(i)] = static_cast<int>(j);
                        mapped++;
                        break;
                    }
                }
            }

            SPARK_LOG_INFO(LogCategory::Animation,
                           "SkeletonMapping: built bone mapping (%d/%zu source bones mapped to %zu target bones)",
                           mapped, source.bones.size(), target.bones.size());
            return mapped;
        }

        /**
     * @brief Manually set a bone mapping by name
     * @param sourceBoneName Bone name in the source skeleton
     * @param targetBoneName Bone name in the target skeleton
     */
        void SetBoneMapping(const std::string& sourceBoneName, const std::string& targetBoneName)
        {
            m_nameMapping[sourceBoneName] = targetBoneName;
        }

        /**
     * @brief Apply name mappings to resolve indices for given skeletons
     */
        int ResolveNameMappings(const Skeleton& source, const Skeleton& target)
        {
            int resolved = 0;
            for (const auto& [srcName, tgtName] : m_nameMapping)
            {
                int srcIdx = source.FindBone(srcName);
                int tgtIdx = target.FindBone(tgtName);
                if (srcIdx >= 0 && tgtIdx >= 0)
                {
                    m_sourceToTarget[srcIdx] = tgtIdx;
                    resolved++;
                    SPARK_LOG_DEBUG(LogCategory::Animation,
                                    "SkeletonMapping: mapped bone '%s' (idx=%d) -> '%s' (idx=%d)", srcName.c_str(),
                                    srcIdx, tgtName.c_str(), tgtIdx);
                }
                else
                {
                    SPARK_LOG_WARN(LogCategory::Animation,
                                   "SkeletonMapping: failed to resolve mapping '%s' -> '%s' (srcIdx=%d, tgtIdx=%d)",
                                   srcName.c_str(), tgtName.c_str(), srcIdx, tgtIdx);
                }
            }
            SPARK_LOG_INFO(LogCategory::Animation, "SkeletonMapping: resolved %d/%zu name mappings", resolved,
                           m_nameMapping.size());
            return resolved;
        }

        /**
     * @brief Get target bone index for a source bone index
     * @return Target bone index, or -1 if not mapped
     */
        int GetTargetBone(int sourceBoneIndex) const
        {
            auto it = m_sourceToTarget.find(sourceBoneIndex);
            return (it != m_sourceToTarget.end()) ? it->second : -1;
        }

        int GetMappedBoneCount() const { return static_cast<int>(m_sourceToTarget.size()); }

        const std::unordered_map<int, int>& GetMapping() const { return m_sourceToTarget; }

      private:
        std::unordered_map<int, int> m_sourceToTarget;              ///< source bone idx → target bone idx
        std::unordered_map<std::string, std::string> m_nameMapping; ///< source name → target name

        static std::string StripPrefix(const std::string& name)
        {
            // Strip common prefixes: "mixamorig:", "Bip01_", "Bone_", "SK_"
            size_t colonPos = name.find(':');
            if (colonPos != std::string::npos)
            {
                return name.substr(colonPos + 1);
            }

            static const char* prefixes[] = {"Bip01_", "Bone_", "SK_", "DEF_", "ORG_"};
            for (const char* prefix : prefixes)
            {
                size_t prefixLen = std::strlen(prefix);
                if (name.size() > prefixLen && name.compare(0, prefixLen, prefix) == 0)
                {
                    return name.substr(prefixLen);
                }
            }

            return name;
        }

        static bool CaseInsensitiveEqual(const std::string& a, const std::string& b)
        {
            if (a.size() != b.size())
                return false;
            for (size_t i = 0; i < a.size(); ++i)
            {
                if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                {
                    return false;
                }
            }
            return true;
        }
    };

    // ============================================================================
    // Retargeted Pose
    // ============================================================================

    struct RetargetedPose
    {
        std::vector<BoneLocalTransform> localTransforms; ///< Per-bone local transforms for target skeleton
        int mappedBones = 0;                             ///< How many bones were mapped from source
        int unmappedBones = 0;                           ///< Bones that used bind pose fallback
    };

    // ============================================================================
    // Animation Retargeter
    // ============================================================================

    /**
 * @brief Retargets animation poses from one skeleton to another
 */
    class AnimationRetargeter
    {
      public:
        /**
     * @brief Retarget a pose from source skeleton to target skeleton
     *
     * Mapped bones get the source animation's rotation (with proportion adjustment
     * for position). Unmapped bones use the target skeleton's bind pose.
     *
     * @param source        Source skeleton
     * @param target        Target skeleton
     * @param mapping       Bone mapping from source to target
     * @param sourceTransforms Local-space transforms from the source animation
     * @return Retargeted pose for the target skeleton
     */
        static RetargetedPose Retarget(const Skeleton& source, const Skeleton& target, const SkeletonMapping& mapping,
                                       const std::vector<BoneLocalTransform>& sourceTransforms)
        {
            RetargetedPose result;
            result.localTransforms.resize(target.bones.size());

            // Initialize all bones to target bind pose
            for (size_t i = 0; i < target.bones.size(); ++i)
            {
                result.localTransforms[i].position = {target.bones[i].localBindPose.m[3][0],
                                                      target.bones[i].localBindPose.m[3][1],
                                                      target.bones[i].localBindPose.m[3][2]};
                result.localTransforms[i].rotation = {0.0f, 0.0f, 0.0f, 1.0f};
                result.localTransforms[i].scale = {1.0f, 1.0f, 1.0f};
            }

            // Apply source animation to mapped bones
            for (const auto& [srcIdx, tgtIdx] : mapping.GetMapping())
            {
                if (srcIdx < 0 || srcIdx >= static_cast<int>(sourceTransforms.size()))
                    continue;
                if (tgtIdx < 0 || tgtIdx >= static_cast<int>(target.bones.size()))
                    continue;

                const auto& srcTransform = sourceTransforms[static_cast<size_t>(srcIdx)];

                // Copy rotation directly (rotations transfer between different proportioned skeletons)
                result.localTransforms[static_cast<size_t>(tgtIdx)].rotation = srcTransform.rotation;

                // For position: only transfer root bone position directly.
                // For other bones, keep target bind pose position (different skeleton proportions).
                if (source.bones[static_cast<size_t>(srcIdx)].parentIndex < 0)
                {
                    // Root bone: transfer position with height scaling
                    float srcHeight = EstimateSkeletonHeight(source);
                    float tgtHeight = EstimateSkeletonHeight(target);
                    float heightRatio = (srcHeight > 0.01f) ? tgtHeight / srcHeight : 1.0f;

                    result.localTransforms[static_cast<size_t>(tgtIdx)].position = {
                        srcTransform.position.x * heightRatio, srcTransform.position.y * heightRatio,
                        srcTransform.position.z * heightRatio};
                }

                // Transfer scale
                result.localTransforms[static_cast<size_t>(tgtIdx)].scale = srcTransform.scale;

                result.mappedBones++;
            }

            result.unmappedBones = static_cast<int>(target.bones.size()) - result.mappedBones;
            SPARK_LOG_INFO(LogCategory::Animation, "Retarget complete: %d mapped, %d unmapped (target has %zu bones)",
                           result.mappedBones, result.unmappedBones, target.bones.size());
            return result;
        }

      private:
        /**
     * @brief Rough estimate of skeleton height (distance from root to highest bone)
     */
        static float EstimateSkeletonHeight(const Skeleton& skeleton)
        {
            if (skeleton.bones.empty())
                return 1.0f;

            float maxY = 0.0f;
            for (const auto& bone : skeleton.bones)
            {
                // Use the offset matrix translation as approximate world position
                float y = std::abs(bone.offsetMatrix.m[3][1]);
                if (y > maxY)
                    maxY = y;
            }
            return maxY > 0.01f ? maxY : 1.0f;
        }
    };

} // namespace Spark::Animation
