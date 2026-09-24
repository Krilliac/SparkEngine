/**
 * @file OBJStaticMeshLoader.h
 * @brief Fail-closed, platform-neutral CPU loader for Wavefront OBJ static meshes.
 *
 * Shared by the Windows (D3D11) and portable MeshAsset implementations so both
 * import the same corners from the same file.
 */

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Spark::Graphics::Detail
{
    struct OBJStaticVertex
    {
        std::array<float, 3> position{};
        std::array<float, 3> normal{0.0f, 1.0f, 0.0f};
        std::array<float, 2> texCoord{};
    };

    /// Contiguous index range that shares one OBJ `usemtl` material.
    struct OBJStaticSubmesh
    {
        uint32_t indexStart = 0;
        uint32_t indexCount = 0;
        int materialId = -1; ///< Index into the OBJ's MTL materials, or -1 for none.
    };

    struct OBJStaticMeshData
    {
        std::vector<OBJStaticVertex> vertices;
        std::vector<uint32_t> indices;
        std::vector<OBJStaticSubmesh> submeshes;
    };

    /**
     * @brief Load triangulated OBJ geometry without creating GPU resources.
     *
     * Every face form (`v`, `v/vt`, `v//vn`, `v/vt/vn`) is parsed by
     * tinyobjloader and polygons are triangulated. Texture coordinates are
     * converted from OBJ's bottom-left origin to the engine's top-left origin
     * (v' = 1 - v). Authored normals are normalized; corners without a usable
     * authored normal receive their triangle's geometric normal. Polygons are
     * triangulated locally into exactly n - 2 triangles. Out-of-range indices,
     * non-finite values, and files that produce no triangles are rejected.
     *
     * @param path Source .obj path.
     * @param meshData Replaced with validated geometry on success; cleared on failure.
     * @param error Receives a diagnostic on failure.
     * @return true when a non-empty static triangle mesh was loaded.
     */
    bool LoadOBJStaticMesh(const std::filesystem::path& path, OBJStaticMeshData& meshData, std::string& error);
} // namespace Spark::Graphics::Detail
