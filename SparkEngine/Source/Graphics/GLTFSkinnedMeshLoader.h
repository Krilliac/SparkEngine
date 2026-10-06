/**
 * @file GLTFSkinnedMeshLoader.h
 * @brief Fail-closed CPU loader for the supported glTF 2.0 skinned-mesh subset.
 */

#pragma once

#include "../Engine/Animation/Skeleton.h"
#include "GLTFStaticMeshLoader.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Spark::Graphics::Detail
{
    /**
     * @brief Tolerance on the sum of a vertex's four skin weights.
     *
     * Sums within [1 - tolerance, 1 + tolerance] are renormalized to exactly 1. The value covers
     * the quantization error of four UNSIGNED_BYTE-normalized weights (4 x 0.5/255 ~= 0.0078).
     * Any other sum, including zero, is rejected instead of being silently rescaled.
     */
    inline constexpr float kGLTFSkinWeightSumTolerance = 0.01f;

    /// One skinned vertex with exactly four influences.
    struct GLTFSkinnedVertex
    {
        std::array<float, 3> position{};
        std::array<float, 3> normal{0.0f, 1.0f, 0.0f};
        std::array<float, 2> texCoord{};
        /// Indices into GLTFSkinnedMeshData::skeleton.bones (already remapped from glTF joint order).
        std::array<uint32_t, 4> joints{};
        /// Non-negative weights that sum to 1.
        std::array<float, 4> weights{};
    };

    struct GLTFSkinnedMeshData
    {
        std::vector<GLTFSkinnedVertex> vertices;
        std::vector<uint32_t> indices;
        std::vector<GLTFStaticPrimitive> primitives;

        /**
         * @brief Joint hierarchy ordered so every bone follows its parent.
         *
         * Matrices use DirectXMath row-vector layout (translation in _41.._43), which is the glTF
         * column-major float array copied verbatim. offsetMatrix is the skin's inverse bind matrix
         * (identity when absent). localBindPose is the joint node's local transform; for the root
         * bone it also folds in every non-joint ancestor, so it is the root's model-space pose.
         */
        Spark::Animation::Skeleton skeleton;
    };

    /**
     * @brief Load one glTF skin and the triangle geometry bound to it without creating GPU resources.
     *
     * The document must contain exactly one skin; every mesh node must reference it and every
     * primitive must provide POSITION, JOINTS_0 and WEIGHTS_0 (NORMAL and TEXCOORD_0 optional).
     * The loader rejects, with a diagnostic: JOINTS_1/WEIGHTS_1 or other extra attributes, invalid
     * JOINTS/WEIGHTS component types, joint indices outside the skin, negative or non-finite
     * weights, weight sums outside kGLTFSkinWeightSumTolerance, more than 256 joints (the GPU
     * skinning palette size), duplicate joints or joint names, non-affine or singular inverse bind
     * matrices, cyclic node graphs, and joints that do not form one connected tree. Everything the
     * static loader rejects (sparse accessors, morph targets, required extensions, ...) is also
     * rejected. Animations are ignored by this loader.
     *
     * @param path Source .gltf or .glb path.
     * @param meshData Replaced with validated geometry and skeleton on success; cleared on failure.
     * @param error Receives a diagnostic on failure.
     * @return true when a non-empty, validated skinned mesh was loaded.
     */
    bool LoadGLTFSkinnedMesh(const std::filesystem::path& path, GLTFSkinnedMeshData& meshData, std::string& error);

    /**
     * @brief Report whether a .gltf/.glb declares any skin, so callers can choose the skinned or static loader.
     *
     * Parses only the JSON (and GLB container) with the same root confinement and size limits as
     * the loaders; binary buffers are not read and nothing else is validated.
     *
     * @param path Source .gltf or .glb path.
     * @param hasSkin Set to true when the document declares at least one skin.
     * @param error Receives a diagnostic when the document cannot be parsed.
     * @return false when the file cannot be parsed.
     */
    bool GLTFFileHasSkin(const std::filesystem::path& path, bool& hasSkin, std::string& error);
} // namespace Spark::Graphics::Detail
