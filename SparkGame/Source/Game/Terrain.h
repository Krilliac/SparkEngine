/**
 * @file Terrain.h
 * @brief Enhanced terrain system with LOD, texture splatting, and RAII safety
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides heightmap-based terrain with quadtree LOD, multi-texture splatting,
 * runtime heightmap editing, and proper RAII resource management.
 */

#pragma once
#include "Core/Platform.h"

#include "Utils/Assert.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d11.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <vector>
#include <memory>
#include <string>

using DirectX::XMFLOAT2;
using DirectX::XMFLOAT3;
using DirectX::XMFLOAT4;
using DirectX::XMMATRIX;
using Microsoft::WRL::ComPtr;

struct TerrainVertex
{
    XMFLOAT3 Position;
    XMFLOAT3 Normal;
    XMFLOAT2 TexCoord;
    XMFLOAT4 SplatWeights; ///< Texture blend weights (R=layer0, G=layer1, B=layer2, A=layer3)
};

/**
 * @brief Terrain texture layer for multi-texture splatting
 */
struct TerrainTextureLayer
{
    std::wstring diffusePath;
    std::wstring normalPath;
    float tileScale = 10.0f;      ///< UV tiling scale
    float heightThreshold = 0.0f; ///< Height at which this layer begins (for auto-splatting)
    float blendSharpness = 5.0f;  ///< How sharply to transition between layers
};

/**
 * @brief Quadtree node for LOD terrain rendering
 */
struct TerrainQuadNode
{
    XMFLOAT3 center; ///< Center of this terrain patch
    float halfSize;  ///< Half-size of this patch
    int lodLevel;    ///< LOD level (0 = highest detail)
    int startIndex;  ///< Start index in the index buffer
    int indexCount;  ///< Number of indices for this patch
    bool isLeaf;
    int children[4] = {-1, -1, -1, -1};
};

/**
 * @brief Terrain configuration
 */
struct TerrainDesc
{
    UINT width = 256;                                        ///< Heightmap width
    UINT height = 256;                                       ///< Heightmap height
    float cellSpacing = 1.0f;                                ///< World space distance between vertices
    float heightScale = 50.0f;                               ///< Maximum terrain height
    int maxLODLevels = 4;                                    ///< Maximum number of LOD levels
    float lodDistances[4] = {50.0f, 100.0f, 200.0f, 400.0f}; ///< LOD transition distances

    // Texture layers (up to 4 for splat map)
    TerrainTextureLayer layers[4];
};

/**
 * @brief Enhanced terrain with LOD and texture splatting
 */
class Terrain
{
  public:
    Terrain();
    ~Terrain() = default; // ComPtr handles cleanup via RAII

    /**
     * @brief Initialize terrain from a heightmap file
     */
    HRESULT Initialize(ID3D11Device* device, ID3D11DeviceContext* ctx, const wchar_t* heightmapFile,
                       const TerrainDesc& desc);

    /**
     * @brief Legacy initialize (backward compatible)
     */
    HRESULT Initialize(ID3D11Device* device, ID3D11DeviceContext* ctx, const wchar_t* heightmapFile, UINT width,
                       UINT height, float cellSpacing);

    /**
     * @brief Update LOD selection based on camera position
     */
    void Update(const XMFLOAT3& cameraPosition);

    /**
     * @brief Render the terrain
     */
    void Render(ID3D11DeviceContext* ctx);

    /**
     * @brief Render with explicit matrices
     */
    void Render(ID3D11DeviceContext* ctx, const XMMATRIX& view, const XMMATRIX& projection);

    // ============================================================================
    // Runtime Heightmap Editing
    // ============================================================================

    /**
     * @brief Raise/lower terrain at a world position
     * @param worldPos World position to modify
     * @param radius Brush radius
     * @param strength Height change amount (positive = raise, negative = lower)
     */
    void ModifyHeight(const XMFLOAT3& worldPos, float radius, float strength);

    /**
     * @brief Smooth terrain at a world position
     */
    void SmoothHeight(const XMFLOAT3& worldPos, float radius, float strength);

    /**
     * @brief Flatten terrain at a world position to a target height
     */
    void FlattenHeight(const XMFLOAT3& worldPos, float radius, float targetHeight);

    /**
     * @brief Paint splat map at a world position
     * @param worldPos World position to paint
     * @param radius Brush radius
     * @param layerIndex Texture layer to paint (0-3)
     * @param strength Painting strength
     */
    void PaintSplatMap(const XMFLOAT3& worldPos, float radius, int layerIndex, float strength);

    // ============================================================================
    // Queries
    // ============================================================================

    /**
     * @brief Get terrain height at a world XZ position (bilinear interpolated)
     */
    float GetHeightAt(float worldX, float worldZ) const;

    /**
     * @brief Get terrain normal at a world XZ position
     */
    XMFLOAT3 GetNormalAt(float worldX, float worldZ) const;

    /**
     * @brief Get the terrain bounding box
     */
    void GetBounds(XMFLOAT3& minBounds, XMFLOAT3& maxBounds) const;

    // ============================================================================
    // Accessors
    // ============================================================================

    const TerrainDesc& GetDesc() const { return m_desc; }
    UINT GetVertexCount() const { return static_cast<UINT>(m_vertices.size()); }
    UINT GetTriangleCount() const { return m_indexCount / 3; }
    int GetCurrentLODLevel() const { return m_currentLOD; }
    const std::vector<float>& GetHeightData() const { return m_heightData; }

    // Console integration
    std::string Console_GetInfo() const;

  private:
    void BuildMesh(const std::vector<uint8_t>& rawHeightData, UINT w, UINT h, float s);
    void CalculateNormals();
    void CalculateSplatWeights();
    void UpdateVertexBuffer();
    void BuildQuadTree();

    // Convert world position to heightmap coordinates
    bool WorldToHeightmap(float worldX, float worldZ, int& hx, int& hz) const;
    float SampleHeight(int x, int z) const;

    TerrainDesc m_desc;

    // RAII-safe GPU resources (no manual Release needed)
    ComPtr<ID3D11Buffer> m_vb;
    ComPtr<ID3D11Buffer> m_ib;

    UINT m_indexCount{0};
    std::vector<TerrainVertex> m_vertices;
    std::vector<UINT> m_indices;
    std::vector<float> m_heightData; ///< Normalized height data [0..1]

    // LOD
    std::vector<TerrainQuadNode> m_quadNodes;
    int m_currentLOD = 0;
    bool m_meshDirty = false;

    // Device references for runtime updates
    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
};
