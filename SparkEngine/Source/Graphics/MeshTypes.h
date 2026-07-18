/**
 * @file MeshTypes.h
 * @brief Vertex, mesh data, and submesh structures for the mesh system
 * @author Spark Engine Team
 * @date 2025
 *
 * Plain data types shared by the mesh system: the standard Vertex format,
 * the MeshData vertex/index container, and the per-material MeshSubmesh
 * index range descriptor. Part of the `Graphics/Mesh.h` umbrella header.
 */

#pragma once
#include "../Core/Platform.h"

#include "Utils/Assert.h"
#include <vector>
#include <string>
#include <cmath>

using DirectX::XMFLOAT2;
using DirectX::XMFLOAT3;
using DirectX::XMMATRIX;

/**
 * @brief 3D vertex structure with position, normal, and texture coordinates
 * 
 * Standard vertex format used throughout the Spark Engine for 3D geometry.
 * Includes validation to ensure all floating-point values are finite.
 */
struct Vertex
{
    XMFLOAT3 Position; ///< 3D world position
    XMFLOAT3 Normal;   ///< Surface normal vector for lighting calculations
    XMFLOAT2 TexCoord; ///< Texture coordinates for UV mapping

    /**
     * @brief Default constructor with safe default values
     */
    Vertex() : Position{0, 0, 0}, Normal{0, 1, 0}, TexCoord{0, 0} {}

    /**
     * @brief Parameterized constructor with validation
     * 
     * @param p 3D position vector
     * @param n Normal vector (should be normalized)
     * @param t Texture coordinate vector
     * 
     * @note Includes assertions to validate that all values are finite
     */
    Vertex(const XMFLOAT3& p, const XMFLOAT3& n, const XMFLOAT2& t) : Position(p), Normal(n), TexCoord(t)
    {
        ASSERT(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z));
        ASSERT(std::isfinite(n.x) && std::isfinite(n.y) && std::isfinite(n.z));
        ASSERT(std::isfinite(t.x) && std::isfinite(t.y));
    }
};

/**
 * @brief Container for mesh vertex and index data
 * 
 * Simple data structure that holds the raw vertex and index arrays
 * for mesh construction and manipulation.
 */
struct MeshData
{
    std::vector<Vertex> vertices;      ///< Array of mesh vertices
    std::vector<unsigned int> indices; ///< Array of vertex indices for triangles
};

/**
 * @brief Per-material index range within a mesh loaded from an OBJ file.
 *
 * OBJ files reference MTL materials per face. LoadFromFile() groups faces by
 * material into contiguous index ranges so a renderer can draw each range with
 * its own diffuse color / texture. Empty for procedural primitives.
 */
struct MeshSubmesh
{
    unsigned int indexStart = 0;                ///< First index of this range
    unsigned int indexCount = 0;                ///< Number of indices in this range
    DirectX::XMFLOAT4 diffuseColor{1, 1, 1, 1}; ///< MTL Kd color (white if none)
    std::string diffuseTexture;                 ///< MTL map_Kd path resolved relative to the OBJ (empty if none)
};
