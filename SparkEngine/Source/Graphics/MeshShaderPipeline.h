/**
 * @file MeshShaderPipeline.h
 * @brief Mesh shader rendering pipeline for meshlet-based geometry
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides an alternative rendering path using mesh shaders (D3D12 SM6.5)
 * for meshlet-based geometry. Falls back to traditional vertex pipeline
 * on D3D11 or when mesh shaders are not supported.
 *
 * The pipeline uses amplification shaders for LOD selection and coarse
 * culling, and mesh shaders for per-meshlet processing with fine-grained
 * backface/frustum culling.
 *
 * @see MeshClusterSystem.h, MeshletMS.hlsl, MeshletAS.hlsl
 */

#pragma once

#include "../Core/Platform.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Spark::Graphics
{

    /// @brief Meshlet data for GPU upload
    struct GPUMeshlet
    {
        uint32_t vertexOffset = 0;
        uint32_t triangleOffset = 0;
        uint32_t vertexCount = 0;
        uint32_t triangleCount = 0;

        float boundCenter[3] = {};
        float boundRadius = 0.0f;

        float coneAxis[3] = {};
        float coneCutoff = 1.0f; ///< cos(half_angle + 90), negative = valid
    };

    /// @brief Group of meshlets for LOD selection
    struct GPUMeshletGroup
    {
        uint32_t meshletOffset = 0;
        uint32_t meshletCount = 0;
        float lodError = 0.0f;
        float parentLodError = 0.0f;

        float boundCenter[3] = {};
        float boundRadius = 0.0f;
    };

    /// @brief Mesh converted to meshlet format
    struct MeshletMesh
    {
        std::string name;
        std::vector<GPUMeshlet> meshlets;
        std::vector<GPUMeshletGroup> groups;
        std::vector<uint32_t> uniqueVertexIndices; ///< Meshlet-local to mesh-global
        std::vector<uint32_t> primitiveIndices;    ///< Packed triangle indices (3 bytes each)
        uint32_t totalVertices = 0;
        uint32_t totalTriangles = 0;
    };

    /// @brief Performance statistics
    struct MeshShaderStats
    {
        uint32_t totalMeshlets = 0;
        uint32_t visibleMeshlets = 0;
        uint32_t culledByBackface = 0;
        uint32_t culledByFrustum = 0;
        uint32_t totalTriangles = 0;
        uint32_t dispatchCount = 0;
        bool meshShadersAvailable = false;
    };

    /**
     * @brief Mesh shader rendering pipeline
     *
     * Manages the mesh shader pipeline state, meshlet buffer uploads,
     * and dispatch calls. Falls back to traditional rendering when
     * mesh shaders are not available.
     */
    class MeshShaderPipeline
    {
      public:
        [[deprecated("Use EngineContext::Get()->GetSystem<MeshShaderPipeline>() instead")]]
        static MeshShaderPipeline& GetInstance()
        {
            static MeshShaderPipeline instance;
            return instance;
        }

        /// @brief Check if mesh shaders are supported on current hardware
        static bool CheckMeshShaderSupport();

        /// @brief Initialize the pipeline
        /// @param device Native graphics device (ID3D12Device* for D3D12)
        /// @return true if mesh shader pipeline initialized (false = fallback to traditional)
        bool Initialize(void* device);

        /// @brief Shutdown and release resources
        void Shutdown();

        /// @brief Convert a traditional mesh to meshlet format
        /// @param vertices Vertex positions (float3 per vertex)
        /// @param vertexCount Number of vertices
        /// @param indices Index buffer
        /// @param indexCount Number of indices
        /// @param maxVerticesPerMeshlet Maximum vertices per meshlet (default 64)
        /// @param maxTrianglesPerMeshlet Maximum triangles per meshlet (default 124)
        /// @return Meshlet mesh ready for GPU upload
        MeshletMesh BuildMeshlets(const float* vertices, uint32_t vertexCount, const uint32_t* indices,
                                  uint32_t indexCount, uint32_t maxVerticesPerMeshlet = 64,
                                  uint32_t maxTrianglesPerMeshlet = 124) const;

        /// @brief Render a meshlet mesh using mesh shaders
        /// @param mesh Meshlet mesh to render
        /// @param worldViewProjection Combined WVP matrix
        /// @param worldMatrix World transform
        /// @param cameraPosition Camera position for LOD/culling
        void RenderMeshletMesh(const MeshletMesh& mesh, const float* worldViewProjection, const float* worldMatrix,
                               const float* cameraPosition);

        /// @brief Get whether mesh shaders are being used (vs fallback)
        bool IsUsingMeshShaders() const { return m_stats.meshShadersAvailable; }

        const MeshShaderStats& GetStats() const { return m_stats; }

        /// @brief Console status report
        std::string Console_GetStatus() const;

      private:
        MeshShaderPipeline() = default;
        ~MeshShaderPipeline() = default;
        MeshShaderPipeline(const MeshShaderPipeline&) = delete;
        MeshShaderPipeline& operator=(const MeshShaderPipeline&) = delete;

        /// @brief Build normal cone for backface culling
        void ComputeNormalCone(GPUMeshlet& meshlet, const float* vertices, const uint32_t* indices) const;

        /// @brief Build bounding sphere for a set of vertices
        void ComputeBoundingSphere(float* outCenter, float* outRadius, const float* vertices, const uint32_t* indices,
                                   uint32_t indexCount) const;

        MeshShaderStats m_stats;
        bool m_initialized = false;
    };

} // namespace Spark::Graphics
