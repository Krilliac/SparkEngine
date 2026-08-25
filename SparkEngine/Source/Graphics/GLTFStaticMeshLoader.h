/**
 * @file GLTFStaticMeshLoader.h
 * @brief Fail-closed CPU loader for the supported glTF 2.0 static-mesh subset.
 */

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Spark::Graphics::Detail
{
    struct GLTFStaticVertex
    {
        std::array<float, 3> position{};
        std::array<float, 3> normal{0.0f, 1.0f, 0.0f};
        std::array<float, 2> texCoord{};
    };

    struct GLTFStaticPrimitive
    {
        uint32_t indexStart = 0;
        uint32_t indexCount = 0;
    };

    struct GLTFStaticMeshData
    {
        std::vector<GLTFStaticVertex> vertices;
        std::vector<uint32_t> indices;
        std::vector<GLTFStaticPrimitive> primitives;
    };

    /**
     * @brief Load core glTF 2.0 triangle geometry without creating GPU resources.
     *
     * Supported attributes are POSITION, NORMAL, and TEXCOORD_0, with optional
     * unsigned scalar indices. Sparse accessors, required extensions, morph
     * targets, skins, animations, and non-triangle primitives are rejected.
     * Missing normals are generated from triangle geometry.
     *
     * @param path Source .gltf or .glb path.
     * @param meshData Replaced with validated geometry on success; cleared on failure.
     * @param error Receives a diagnostic on failure.
     * @return true when a non-empty, validated static mesh was loaded.
     */
    bool LoadGLTFStaticMesh(const std::filesystem::path& path, GLTFStaticMeshData& meshData, std::string& error);
} // namespace Spark::Graphics::Detail
