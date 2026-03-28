/**
 * @file Mesh.h
 * @brief 3D mesh management and primitive generation system
 * @author Spark Engine Team
 * @date 2025
 * 
 * This class provides comprehensive 3D mesh functionality including vertex/index
 * buffer management, primitive shape generation (cube, sphere, plane), and mesh
 * loading capabilities for the Spark Engine's rendering system.
 */

#pragma once
#include "../Core/Platform.h"

#include "Utils/Assert.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d11.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#endif // SPARK_PLATFORM_WINDOWS
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
 * @brief 3D mesh management and rendering class
 * 
 * Handles creation, loading, and rendering of 3D meshes. Supports procedural
 * generation of primitive shapes and loading from external files. Manages
 * DirectX vertex and index buffers for efficient GPU rendering.
 * 
 * @note All procedural primitives are generated with proper normals and texture coordinates
 * @warning Ensure Initialize() is called before creating or loading any mesh data
 */
class Mesh
{
  public:
    /**
     * @brief Default constructor
     * 
     * Initializes member variables to safe default values.
     */
    Mesh();

    /**
     * @brief Destructor
     * 
     * Automatically calls Shutdown() to ensure proper resource cleanup.
     */
    ~Mesh();

    /**
     * @brief Initialize the mesh system
     * 
     * Sets up the mesh manager with DirectX device and context references
     * required for buffer creation and rendering operations.
     * 
     * @param device DirectX 11 device for resource creation
     * @param context DirectX 11 device context for rendering operations
     * @return HRESULT indicating success or failure of initialization
     */
    HRESULT Initialize(ID3D11Device* device, ID3D11DeviceContext* context);

    /**
     * @brief Clean up all mesh resources
     * 
     * Releases vertex and index buffers and clears mesh data. Safe to call multiple times.
     */
    void Shutdown();

    /**
     * @brief Generate a procedural cube mesh
     * 
     * Creates a cube with 6 faces, proper normals, and texture coordinates.
     * 
     * @param size Edge length of the cube (default: 1.0f)
     * @return HRESULT indicating success or failure of mesh creation
     */
    HRESULT CreateCube(float size = 1.0f);

    /**
     * @brief Generate a simple triangle mesh
     * 
     * Creates a single triangle for basic testing and debugging purposes.
     * 
     * @param size Scale factor for the triangle (default: 1.0f)
     * @return HRESULT indicating success or failure of mesh creation
     */
    HRESULT CreateTriangle(float size = 1.0f);

    /**
     * @brief Generate a flat plane mesh
     * 
     * Creates a horizontal plane centered at the origin with proper UV mapping.
     * 
     * @param width Width of the plane along X-axis (default: 10.0f)
     * @param depth Depth of the plane along Z-axis (default: 10.0f)
     * @return HRESULT indicating success or failure of mesh creation
     */
    HRESULT CreatePlane(float width = 10.0f, float depth = 10.0f);

    /**
     * @brief Generate a procedural sphere mesh
     * 
     * Creates a UV sphere using latitude/longitude subdivision with proper
     * normals and texture coordinates for seamless texturing.
     * 
     * @param radius Radius of the sphere (default: 1.0f)
     * @param slices Number of vertical slices (longitude divisions, default: 20)
     * @param stacks Number of horizontal stacks (latitude divisions, default: 20)
     * @return HRESULT indicating success or failure of mesh creation
     * @note Higher slice/stack values create smoother spheres but increase polygon count
     */
    HRESULT CreateSphere(float radius = 1.0f, int slices = 20, int stacks = 20);

    /**
     * @brief Generate a procedural pyramid mesh
     * 
     * Creates a 4-sided pyramid (square base) with proper normals and texture coordinates.
     * 
     * @param size Base size of the pyramid (default: 1.0f)
     * @param height Height of the pyramid (default: 1.0f)
     * @return HRESULT indicating success or failure of mesh creation
     */
    HRESULT CreatePyramid(float size = 1.0f, float height = 1.0f);

    /**
     * @brief Create mesh from custom vertex and index arrays
     * 
     * Allows creation of custom meshes from user-provided vertex and index data.
     * 
     * @param verts Vector of vertices defining the mesh geometry
     * @param inds Vector of indices defining triangle connectivity
     * @return HRESULT indicating success or failure of mesh creation
     */
    HRESULT CreateFromVertices(const std::vector<Vertex>& verts, const std::vector<unsigned int>& inds);

    /**
     * @brief Load mesh data from an external file
     * 
     * Loads 3D model data from supported file formats (implementation specific).
     * 
     * @param path File path to the mesh data file
     * @return true if loading succeeded, false otherwise
     * @note File format support depends on implementation
     */
    bool LoadFromFile(const std::wstring& path);

    /**
     * @brief Set the placeholder flag for the mesh
     * @param p true to mark as placeholder, false otherwise
     */
    void SetPlaceholder(bool p) { m_placeholder = p; }

    /**
     * @brief Check if this mesh is a placeholder
     * @return true if mesh is a placeholder, false otherwise
     */
    bool IsPlaceholder() const { return m_placeholder; }

    /**
     * @brief Render the mesh using the provided device context
     * 
     * Binds vertex and index buffers and issues draw calls to render the mesh.
     * 
     * @param ctx DirectX 11 device context for rendering
     * @note Shaders must be set before calling this method
     */
    void Render(ID3D11DeviceContext* ctx);

    /**
     * @brief Get the number of vertices in the mesh
     * @return Number of vertices
     */
    UINT GetVertexCount() const { return m_vertexCount; }

    /**
     * @brief Get the number of indices in the mesh
     * @return Number of indices
     */
    UINT GetIndexCount() const { return m_indexCount; }

  private:
    /**
     * @brief Create DirectX vertex and index buffers from mesh data
     * @return HRESULT indicating success or failure of buffer creation
     */
    HRESULT CreateBuffers();

    /**
     * @brief Calculate vertex normals for the mesh
     * 
     * Computes face normals and averages them for each vertex to provide
     * smooth shading across the mesh surface.
     */
    void CalculateNormals();

    Microsoft::WRL::ComPtr<ID3D11Buffer> m_vb; ///< DirectX vertex buffer (RAII-managed)
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_ib; ///< DirectX index buffer (RAII-managed)
    ID3D11Device* m_device{nullptr};           ///< Non-owning; lifetime tied to GraphicsEngine
    ID3D11DeviceContext* m_context{nullptr};   ///< Non-owning; lifetime tied to GraphicsEngine

    std::vector<Vertex> m_vertices;      ///< CPU vertex data
    std::vector<unsigned int> m_indices; ///< CPU index data
    unsigned int m_vertexCount{0};       ///< Number of vertices
    unsigned int m_indexCount{0};        ///< Number of indices
    bool m_placeholder{false};           ///< Placeholder mesh flag
};
