/**
 * @file TerrainRenderer.h
 * @brief Engine-level terrain rendering for ECS TerrainComponent entities
 *
 * Provides heightmap-based terrain mesh generation and rendering that integrates
 * with the engine's SceneRenderer draw command pipeline. Inspired by Terrain3D
 * (Redot) and Godot's clipmap approach.
 *
 * The TerrainRenderer is the engine-side counterpart to the game-module Terrain
 * class in SparkGame. Game modules can use TerrainComponent entities without
 * re-implementing rendering.
 *
 * ### Architecture
 * ```
 * TerrainComponent (ECS) → TerrainSystem (LOD) → TerrainRenderer (GPU mesh/draw)
 * ```
 *
 * @threadsafety Main thread only (GPU resource access).
 */

#pragma once

#include "../Core/Platform.h"
#include "../Engine/ECS/Components/TerrainComponents.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d11.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#endif

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// Forward declaration for ECS entity type
namespace entt
{
    enum class entity : uint32_t;
}

namespace Spark::Graphics
{

    /**
     * @brief Per-terrain GPU resources for a single terrain entity.
     */
    struct TerrainGPUData
    {
        uint32_t entityID = 0;
        uint32_t vertexCount = 0;
        uint32_t indexCount = 0;
        int currentLOD = 0;
        bool meshDirty = true; ///< Set when heightmap changes and mesh needs rebuild

#ifdef SPARK_PLATFORM_WINDOWS
        Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer;
        Microsoft::WRL::ComPtr<ID3D11Buffer> indexBuffer;
#endif
    };

    /**
     * @brief Terrain vertex layout matching the engine's PBR vertex format.
     */
    struct TerrainVertex
    {
        DirectX::XMFLOAT3 position;
        DirectX::XMFLOAT3 normal;
        DirectX::XMFLOAT2 texCoord;
        DirectX::XMFLOAT4 splatWeights; ///< RGBA blend weights for 4 texture layers
    };

    /**
     * @brief Engine-level terrain renderer for ECS TerrainComponent entities.
     *
     * Creates and manages GPU mesh resources for terrain entities. Each frame:
     * 1. Checks for new/removed terrain entities
     * 2. Rebuilds dirty meshes (heightmap changed)
     * 3. Renders terrain geometry with LOD-appropriate detail
     *
     * Integrates with SceneRenderer by generating draw commands for visible
     * terrain patches.
     */
    class TerrainRenderer
    {
      public:
        TerrainRenderer() = default;
        ~TerrainRenderer() = default;

#ifdef SPARK_PLATFORM_WINDOWS
        /**
         * @brief Initialize the terrain renderer.
         * @param device D3D11 device for GPU resource creation.
         * @return true on success.
         */
        bool Initialize(ID3D11Device* device);
#else
        bool Initialize();
#endif

        /**
         * @brief Shut down and release all GPU resources.
         */
        void Shutdown();

        /**
         * @brief Update terrain meshes for changed components.
         *
         * Call once per frame. Rebuilds GPU meshes for any TerrainComponent
         * whose heightmap has been modified (dirty flag).
         *
         * @param terrains Map of entity ID → TerrainComponent data.
         */
        void UpdateTerrains(const std::unordered_map<uint32_t, const TerrainComponent*>& terrains);

        /**
         * @brief Mark a terrain as needing mesh rebuild (e.g., after sculpting).
         * @param entityID Entity ID of the terrain to mark dirty.
         */
        void MarkDirty(uint32_t entityID);

#ifdef SPARK_PLATFORM_WINDOWS
        /**
         * @brief Render all active terrains.
         * @param context D3D11 device context.
         * @param viewMatrix Camera view matrix.
         * @param projMatrix Camera projection matrix.
         */
        void Render(ID3D11DeviceContext* context, const DirectX::XMMATRIX& viewMatrix,
                    const DirectX::XMMATRIX& projMatrix);
#endif

        /** @brief Get the number of active terrain entities being rendered. */
        uint32_t GetActiveTerrainCount() const { return static_cast<uint32_t>(m_terrainData.size()); }

        /** @brief Remove a terrain entity's GPU resources. */
        void RemoveTerrain(uint32_t entityID);

        /**
         * @brief Load a .sparkterrain file and populate a TerrainComponent
         *
         * Reads the binary format written by SparkEditor's TerrainEditor::SaveTerrain().
         * Populates heightmap, splatmap, texture layers, and terrain parameters.
         *
         * @param filepath Path to the .sparkterrain file
         * @param outComponent TerrainComponent to populate
         * @return true on success
         */
        static bool LoadSparkTerrain(const std::string& filepath, TerrainComponent& outComponent);

      private:
        /**
         * @brief Build mesh data from a TerrainComponent's heightmap.
         *
         * Generates a grid mesh with:
         * - Vertex positions from heightmap samples
         * - Normals computed from height gradients
         * - UV coordinates for texture tiling
         * - Splat weights from splatmap data
         *
         * @param component Source terrain data.
         * @param outVertices Generated vertices.
         * @param outIndices Generated triangle indices.
         */
        void BuildMeshFromComponent(const TerrainComponent& component, std::vector<TerrainVertex>& outVertices,
                                    std::vector<uint32_t>& outIndices);

#ifdef SPARK_PLATFORM_WINDOWS
        /**
         * @brief Create or update GPU buffers for a terrain.
         */
        bool CreateGPUBuffers(TerrainGPUData& data, const std::vector<TerrainVertex>& vertices,
                              const std::vector<uint32_t>& indices);

        ID3D11Device* m_device = nullptr;
#endif

        std::unordered_map<uint32_t, TerrainGPUData> m_terrainData;
    };

} // namespace Spark::Graphics
