/**
 * @file ModelVertex.h
 * @brief Standard vertex structure for imported 3D models
 * @author Spark Engine Team
 * @date 2025
 *
 * Defines the ModelVertex structure used when loading 3D models from external
 * file formats (e.g., via Assimp or tinyobjloader). ModelVertex is functionally
 * identical to the engine's core Vertex struct (see Mesh.h) but exists as a
 * separate type to decouple model-import code from the rendering pipeline.
 *
 * Every field is validated at construction time in debug builds to ensure that
 * no NaN or infinity values slip into the vertex buffers.
 *
 * @see Vertex (in Mesh.h), Mesh::CreateFromVertices
 */

#pragma once
#include "../Core/Platform.h"
#include "../Utils/Assert.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <cmath>               // add this for std::isfinite

/**
 * @brief Vertex data for imported 3D models (position, normal, UV)
 *
 * ModelVertex stores the minimum per-vertex data needed for textured,
 * lit rendering: a 3D position, a surface normal, and a 2D texture
 * coordinate. Both constructors guarantee zero-initialized or validated data.
 *
 * Memory layout (32 bytes, tightly packed):
 * | Offset | Size | Field    |
 * |--------|------|----------|
 * |  0     | 12   | Position |
 * | 12     | 12   | Normal   |
 * | 24     |  8   | TexCoord |
 *
 * @note This struct uses the same memory layout as the core Vertex struct
 *       and can be reinterpret_cast'd when building GPU vertex buffers.
 */
struct ModelVertex
{
    DirectX::XMFLOAT3 Position;  ///< 3D world-space position of the vertex
    DirectX::XMFLOAT3 Normal;    ///< Surface normal vector for lighting calculations
    DirectX::XMFLOAT2 TexCoord;  ///< 2D texture coordinates for UV mapping

    /**
     * @brief Default constructor — zero-initializes all fields
     *
     * Produces a vertex at the origin with a zero normal and zero UVs.
     * This is safe for placeholder geometry but the normal should be
     * overwritten before rendering to avoid incorrect lighting.
     */
    ModelVertex() noexcept
        : Position(0.f, 0.f, 0.f)
        , Normal(0.f, 0.f, 0.f)
        , TexCoord(0.f, 0.f) {
    }

    /**
     * @brief Parameterized constructor with debug validation
     *
     * Constructs a vertex from explicit position, normal, and UV data.
     * In debug builds, every floating-point component is checked with
     * std::isfinite() to catch NaN/infinity values early.
     *
     * @param pos  3D position vector
     * @param norm Surface normal vector (should be normalized for correct lighting)
     * @param uv   Texture coordinate vector
     *
     * @warning Triggers an assertion failure in debug builds if any component
     *          is NaN or infinity.
     */
    ModelVertex(const DirectX::XMFLOAT3& pos,
        const DirectX::XMFLOAT3& norm,
        const DirectX::XMFLOAT2& uv) noexcept
        : Position(pos)
        , Normal(norm)
        , TexCoord(uv)
    {
        // Validate incoming data
        ASSERT(std::isfinite(pos.x) && std::isfinite(pos.y) && std::isfinite(pos.z));
        ASSERT(std::isfinite(norm.x) && std::isfinite(norm.y) && std::isfinite(norm.z));
        ASSERT(std::isfinite(uv.x) && std::isfinite(uv.y));
    }
};
