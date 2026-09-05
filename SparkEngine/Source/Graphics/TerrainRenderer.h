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
#include "Core/Platform.h"
#endif

#include <cstdint>
#include <istream>
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
     * @brief Canonical on-disk layout of a .sparkterrain asset.
     *
     * This namespace is the single definition of the format shared by SparkEditor's writer
     * (TerrainEditor::SaveTerrain) and both readers (TerrainEditor::LoadTerrain and
     * TerrainRenderer::LoadSparkTerrain). Every field is a fixed-width type, so the editor and the runtime
     * cannot disagree about a field's width. Little-endian, no padding:
     *
     * ```
     * uint32  magic                       kMagic
     * uint32  version                     kVersion
     * string  name                        uint32 length + bytes
     * float   size
     * float   positionX, positionY, positionZ
     * int32   lodLevels
     * float   lodBias
     * uint8   generateCollider
     * int32   heightmapWidth, heightmapHeight
     * float   heightScale, minHeight, maxHeight
     * float[] heights                     heightmapWidth * heightmapHeight entries
     * uint32  layerCount
     *   per layer:
     *     string name, diffuseTexture, normalTexture, maskTexture
     *     float  tilingX, tilingY, offsetX, offsetY
     *     float  opacity, metallic, roughness, normalStrength
     * int32   splatmapResolution
     * uint8[] splatmap                    splatmapResolution^2 * 4 entries (RGBA weights)
     * uint32  detailMeshCount
     *   per detail mesh:
     *     string name, meshPath, materialPath
     *     float  density, viewDistance
     *     uint32 instanceCount
     *     float  instanceX, instanceY, instanceZ (repeated instanceCount times)
     * ```
     *
     * Version 1 files (which had no version word and stored only layer names) are not readable; the
     * editor-to-runtime path never worked for them, so no shipped asset depends on that layout.
     */
    namespace SparkTerrain
    {
        inline constexpr uint32_t kMagic = 0x53504B54u; ///< 'SPKT'
        inline constexpr uint32_t kVersion = 2u;

        /// Hard limits. A file declaring more than these is rejected rather than trusted for an allocation.
        inline constexpr uint32_t kMaxStringLength = 4096u;
        inline constexpr int32_t kMinHeightmapResolution = 2;
        inline constexpr int32_t kMaxHeightmapResolution = 8193;
        inline constexpr uint32_t kMaxTextureLayers = 64u;
        inline constexpr int32_t kMaxSplatmapResolution = 8192;
        inline constexpr uint32_t kMaxDetailMeshes = 256u;
        inline constexpr uint32_t kMaxDetailInstancesPerMesh = 4000000u;

        /**
         * @brief Bounds-checked cursor over a .sparkterrain stream.
         *
         * Every read is checked against the real remaining byte count before it is attempted, so a truncated
         * or hostile header can never size an allocation. A failed read leaves the reader permanently failed;
         * callers check Failed() once at the end rather than at every field.
         */
        class Reader
        {
          public:
            explicit Reader(std::istream& stream) : m_stream(stream)
            {
                m_stream.seekg(0, std::ios::end);
                const std::streamoff end = m_stream.tellg();
                m_size = end < 0 ? 0 : static_cast<uint64_t>(end);
                m_stream.seekg(0, std::ios::beg);
                m_failed = !m_stream.good();
            }

            /// @brief Bytes not yet consumed, or 0 once the reader has failed.
            uint64_t Remaining() const
            {
                if (m_failed)
                    return 0;
                return m_size >= m_offset ? m_size - m_offset : 0;
            }

            bool Failed() const { return m_failed; }

            bool ReadBytes(void* destination, uint64_t count)
            {
                if (m_failed || count > Remaining())
                {
                    m_failed = true;
                    return false;
                }
                if (count == 0)
                    return true;
                m_stream.read(static_cast<char*>(destination), static_cast<std::streamsize>(count));
                if (!m_stream.good())
                {
                    m_failed = true;
                    return false;
                }
                m_offset += count;
                return true;
            }

            bool ReadU8(uint8_t& out) { return ReadBytes(&out, sizeof(out)); }
            bool ReadU32(uint32_t& out) { return ReadBytes(&out, sizeof(out)); }
            bool ReadI32(int32_t& out) { return ReadBytes(&out, sizeof(out)); }
            bool ReadF32(float& out) { return ReadBytes(&out, sizeof(out)); }

            /// @brief Read a uint32 length prefix followed by that many bytes, capped at kMaxStringLength.
            bool ReadString(std::string& out)
            {
                uint32_t length = 0;
                if (!ReadU32(length))
                    return false;
                if (length > kMaxStringLength)
                {
                    m_failed = true;
                    return false;
                }
                out.assign(length, '\0');
                return length == 0 ? true : ReadBytes(out.data(), length);
            }

          private:
            std::istream& m_stream;
            uint64_t m_size = 0;
            uint64_t m_offset = 0;
            bool m_failed = false;
        };
    } // namespace SparkTerrain

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
         * Reads version @ref SparkTerrain::kVersion of the binary format written by SparkEditor's
         * TerrainEditor::SaveTerrain(), using the shared field widths and limits in @ref SparkTerrain.
         * Populates heightmap, splatmap, texture layer diffuse paths, and terrain parameters.
         *
         * Every declared count is validated against @ref SparkTerrain limits and against the real remaining
         * file size before it is used to size an allocation, and a truncated file fails rather than
         * returning zero-filled buffers. @p outComponent is left untouched unless the whole file parses.
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
