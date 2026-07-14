/**
 * @file Skeleton.h
 * @brief Bone and Skeleton structures for skeletal animation hierarchies
 * @author Spark Engine Team
 * @date 2025
 */

#pragma once
#include "../../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>


namespace Spark::Animation
{

    /**
 * @brief Represents a single bone in a skeletal hierarchy.
 *
 * Bones are the fundamental building blocks of a skeleton. Each bone has:
 * - A **parent index** that defines the hierarchy (the root bone has `parentIndex == -1`).
 * - An **offset matrix** (inverse bind pose): transforms vertices from model space to
 *   bone space, allowing the bone to deform the mesh relative to its rest position.
 * - A **local bind pose**: the bone's transform relative to its parent in the rest pose.
 *
 * During animation evaluation, each bone's local transform is overridden by the
 * interpolated keyframe data and the resulting chain is multiplied with parent transforms
 * to produce the final world-space skinning matrices.
 *
 * @note Bone names must be unique within a Skeleton. The `boneNameToIndex` map in
 *       `Skeleton` provides O(1) lookup by name.
 */
    struct Bone
    {
        /** @brief Human-readable name matching the name exported from the 3D authoring tool (e.g. "Bip01_R_Hand"). */
        std::string name;

        /**
     * @brief Index of this bone's parent within the `Skeleton::bones` array.
     *
     * The root bone has `parentIndex == -1`. All other bones have a valid parent index.
     * The hierarchy forms a tree rooted at the single root bone.
     */
        int32_t parentIndex = -1;

        /**
     * @brief Inverse bind pose matrix (mesh-to-bone space transform).
     *
     * Stored as a 4x4 row-major matrix. This matrix transforms a vertex from its
     * original model-space position into the local space of this bone in the rest pose.
     * During skinning: `finalMatrix = offsetMatrix * localAnimatedTransform * parentChain`.
     */
        XMFLOAT4X4 offsetMatrix; ///< Inverse bind pose matrix

        /**
     * @brief Bone's transform relative to its parent in the bind/rest pose.
     *
     * Stored as a 4x4 row-major matrix. Used as the fallback when no animation clip
     * provides a keyframe for this bone, ensuring the mesh is displayed in its correct
     * rest shape.
     */
        XMFLOAT4X4 localBindPose; ///< Local bind pose transform
    };

    /**
 * @brief Complete bone hierarchy for a skinned character or object.
 *
 * A Skeleton is a shared asset loaded once and referenced by multiple
 * `AnimationInstance` objects. It defines the fixed hierarchy of bones but does
 * NOT contain any animation state — that lives in `AnimationInstance`.
 *
 * ### Loading
 * Use `AnimationManager::LoadSkeleton()` to load a Skeleton from an FBX or GLTF file.
 * The manager caches skeletons by file path so that multiple instances of the same
 * character share a single Skeleton in memory.
 *
 * @code
 *   auto skeleton = AnimationManager::GetInstance().LoadSkeleton("Assets/Soldier.fbx");
 *   int32_t spineIdx = skeleton->FindBone("Bip01_Spine");
 * @endcode
 */
    struct Skeleton
    {
        /** @brief Human-readable name, typically derived from the source file. */
        std::string name;

        /**
     * @brief Flat array of all bones, ordered such that every bone appears AFTER its parent.
     *
     * This ordering ensures that when bone transforms are computed in index order,
     * a bone's parent world transform is always already computed before the bone itself.
     */
        std::vector<Bone> bones;

        /**
     * @brief Map from bone name to its index in the `bones` array.
     *
     * Provides O(1) lookup by name, which is needed when matching animation channels
     * to skeleton bones. Built automatically when the skeleton is loaded.
     */
        std::unordered_map<std::string, int32_t> boneNameToIndex;

        /**
     * @brief Look up a bone index by name.
     * @param boneName  Name of the bone to find.
     * @return          Index in `bones`, or -1 if the bone was not found.
     */
        int32_t FindBone(const std::string& boneName) const
        {
            auto it = boneNameToIndex.find(boneName);
            return (it != boneNameToIndex.end()) ? it->second : -1;
        }

        /**
     * @brief Return the total number of bones in the skeleton.
     * @return  Size of the `bones` array.
     */
        size_t GetBoneCount() const { return bones.size(); }
    };

} // namespace Spark::Animation
