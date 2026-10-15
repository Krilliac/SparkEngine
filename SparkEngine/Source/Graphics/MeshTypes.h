/**
 * @file MeshTypes.h
 * @brief Native mesh value types and engine-side geometry construction data.
 */

#pragma once

#include "../Core/Platform.h"
#include <Spark/MeshTypes.h>
#include "Utils/Assert.h"

#include <vector>

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
