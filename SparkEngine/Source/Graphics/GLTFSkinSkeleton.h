/**
 * @file GLTFSkinSkeleton.h
 * @brief Internal glTF skin hierarchy validation and Skeleton construction for the skinned mesh loader.
 */

#pragma once

#if SPARK_HAS_CGLTF

#include "../Engine/Animation/Skeleton.h"
#include "GLTFValidation.h"

#include <cgltf.h>

#include <cstdint>
#include <string>
#include <vector>

namespace Spark::Graphics::Detail::GLTF
{
    /**
     * @brief Pre-load checks of the node graph and the document's single skin.
     *
     * Rejects cyclic node graphs, non-finite node transforms, anything other than exactly one skin,
     * empty or duplicate joint lists, more than kMaxBonesPerMesh joints, malformed
     * inverseBindMatrices accessors, mesh nodes without the skin, joints separated from their
     * parent joint by a non-joint node, and joints that do not form a single tree.
     *
     * @param jointParents Receives each skin joint's parent joint index, or -1 for the root.
     */
    bool ValidateSkinHierarchy(const cgltf_data& data, std::vector<int32_t>& jointParents, std::string& error);

    /**
     * @brief Build a parent-before-child Skeleton from the validated skin once buffers are loaded.
     *
     * @param boneOfJoint Receives the bone index of each skin joint index.
     */
    bool BuildSkinSkeleton(const cgltf_data& data, const std::vector<int32_t>& jointParents,
                           Spark::Animation::Skeleton& skeleton, std::vector<uint32_t>& boneOfJoint,
                           std::string& error);

    /**
     * @brief Validate a parsed skinned document, load its buffers and build its Skeleton.
     *
     * Runs ValidateDocumentStructure, ValidateSkinHierarchy, LoadAndValidateBuffers and
     * BuildSkinSkeleton in that order, so every consumer of a glTF skin (mesh, skeleton or
     * animation import) applies the same checks before trusting the file.
     */
    bool LoadSkinSkeleton(Document& document, Spark::Animation::Skeleton& skeleton, std::vector<uint32_t>& boneOfJoint,
                          std::string& error);
} // namespace Spark::Graphics::Detail::GLTF

#endif
