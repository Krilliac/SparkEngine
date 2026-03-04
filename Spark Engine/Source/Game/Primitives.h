/**
 * @file Primitives.h
 * @brief Procedural primitive mesh generators for common 3D shapes
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a set of factory functions that generate MeshData (vertex + index
 * arrays) for common geometric primitives: cubes, planes, and spheres. These
 * are used by primitive game objects (CubeObject, PlaneObject, SphereObject)
 * as fallback geometry when OBJ model files are not available on disk.
 *
 * All generated meshes include proper vertex normals for lighting and UV
 * texture coordinates for material mapping.
 *
 * @code
 *   MeshData cube = Primitives::CreateCube(2.0f);
 *   // cube.vertices and cube.indices are now ready for GPU upload
 * @endcode
 *
 * @see MeshData, Mesh, LoadOrPlaceholderMesh
 */

#pragma once
#include "../Core/Platform.h"

#include "Utils/Assert.h"
#include "../Graphics/Mesh.h"    // MeshData
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
using DirectX::XMFLOAT3;
using DirectX::XMFLOAT2;

/**
 * @brief Namespace containing procedural primitive mesh generators
 *
 * Each function returns a MeshData struct populated with vertices and indices
 * that can be uploaded directly to GPU buffers via Mesh::CreateFromVertices().
 * All primitives are centered at the origin with configurable dimensions.
 */
namespace Primitives
{
    /**
     * @brief Generate a procedural cube centered at the origin
     *
     * Creates a cube with 24 vertices (4 per face for proper normals) and
     * 36 indices (2 triangles per face, 6 faces). Each face has outward-facing
     * normals and UV coordinates mapped from (0,0) to (1,1).
     *
     * @param size Edge length of the cube (default: 1.0f). The cube extends
     *             from -size/2 to +size/2 along each axis.
     * @return MeshData containing the cube's vertices and indices
     */
    MeshData CreateCube(float size = 1.0f);

    /**
     * @brief Generate a procedural flat plane centered at the origin
     *
     * Creates a horizontal plane lying on the XZ-plane at Y=0, with the
     * surface normal facing upward (+Y). UV coordinates tile once across
     * the plane from (0,0) at one corner to (1,1) at the opposite.
     *
     * @param width  Extent along the X-axis (default: 1.0f)
     * @param depth  Extent along the Z-axis (default: 1.0f)
     * @return MeshData containing the plane's vertices and indices
     */
    MeshData CreatePlane(float width = 1.0f, float depth = 1.0f);

    /**
     * @brief Generate a procedural UV-sphere centered at the origin
     *
     * Creates a sphere using latitude/longitude subdivision. The number of
     * triangles is approximately 2 * slices * stacks. Higher tessellation
     * produces a smoother surface at the cost of more geometry.
     *
     * @param radius Radius of the sphere (default: 0.5f)
     * @param slices Number of vertical slices / longitude divisions (default: 16)
     * @param stacks Number of horizontal stacks / latitude divisions (default: 16)
     * @return MeshData containing the sphere's vertices and indices
     *
     * @note Texture coordinates use standard spherical mapping, which produces
     *       a visible seam along one meridian. For seamless texturing consider
     *       using a cube-mapped sphere instead.
     */
    MeshData CreateSphere(float radius = 0.5f, int slices = 16, int stacks = 16);
}
