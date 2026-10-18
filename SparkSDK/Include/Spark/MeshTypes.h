/**
 * @file MeshTypes.h
 * @brief Canonical native mesh vertex and material-range value types.
 *
 * The validating Vertex constructor links to SparkEngineLib. Its debug
 * assertions use the same engine assertion handler as internal mesh creation.
 */

#pragma once

#include <Spark/NativeTypes.h>

#include <string>

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
    Vertex(const XMFLOAT3& p, const XMFLOAT3& n, const XMFLOAT2& t);
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
