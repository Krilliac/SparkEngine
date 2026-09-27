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

namespace tinyobj
{
    struct attrib_t;
    struct shape_t;
} // namespace tinyobj

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

    /**
     * @brief Fail-closed index check for code that walks tinyobjloader output itself.
     *
     * tinyobjloader turns any positive OBJ face index into idx - 1 without an
     * upper bound and only warns about out-of-range indices, so a crafted file
     * parses "successfully" with indices past the attribute arrays. Every
     * consumer that indexes attrib.vertices/normals/texcoords directly must call
     * this after parsing and reject the file when it returns false.
     *
     * Thread affinity: any thread (pure function). Allocation: none.
     *
     * @param attrib Parsed attribute arrays.
     * @param shapes Parsed shapes whose face indices are checked.
     * @param error Receives a diagnostic on failure.
     * @return true when every vertex index addresses a position and every
     *         non-negative normal/texcoord index addresses an element.
     */
    bool ValidateOBJIndices(const tinyobj::attrib_t& attrib, const std::vector<tinyobj::shape_t>& shapes,
                            std::string& error);
} // namespace Spark::Graphics::Detail
