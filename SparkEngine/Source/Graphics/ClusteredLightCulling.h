/**
 * @file ClusteredLightCulling.h
 * @brief GPU-based clustered light culling with a 3D frustum grid
 * @author Spark Engine Team
 * @date 2026
 *
 * Divides the view frustum into a 3D grid of clusters and assigns lights
 * to each cluster for efficient per-tile shading. CPU-side management with
 * results suitable for GPU compute dispatch.
 *
 * @see LightManager, LightingSystem
 */

#pragma once

#include "../Core/Platform.h"
#include "RHI/RHI.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Spark::Graphics
{

    /**
     * @brief Describes a single light source for clustered culling.
     */
    struct LightData
    {
        XMFLOAT3 position{0.0f, 0.0f, 0.0f};
        float radius = 0.0f;
        XMFLOAT3 color{1.0f, 1.0f, 1.0f};
        float intensity = 1.0f;
        uint32_t type = 0; ///< 0 = point, 1 = spot
    };

    /**
     * @brief Configuration for the cluster grid dimensions and limits.
     */
    struct ClusterConfig
    {
        uint32_t gridX = 16;
        uint32_t gridY = 8;
        uint32_t gridZ = 24;
        uint32_t maxLightsPerCluster = 200;
        uint32_t maxTotalLights = 1000;
    };

    /**
     * @brief Axis-aligned bounding box for a single cluster in view space.
     */
    struct ClusterAABB
    {
        XMFLOAT3 minBounds{0.0f, 0.0f, 0.0f};
        XMFLOAT3 maxBounds{0.0f, 0.0f, 0.0f};
    };

    /**
     * @brief GPU-based clustered light culling system.
     *
     * Subdivides the camera frustum into a 3D grid and determines which
     * lights affect each cluster. The results are stored in flat arrays
     * suitable for upload to GPU constant/structured buffers.
     *
     * ## Usage
     * @code
     *   auto& culling = ClusteredLightCulling::GetInstance();
     *   culling.Initialize();
     *
     *   culling.ClearLights();
     *   culling.AddLight(myLight);
     *   culling.Update(viewMat, projMat, nearZ, farZ);
     *
     *   uint32_t count = culling.GetActiveLightCount();
     * @endcode
     */
    class ClusteredLightCulling
    {
      public:
        /// @brief Get the singleton instance.
        static ClusteredLightCulling& GetInstance()
        {
            static ClusteredLightCulling instance;
            return instance;
        }

        ClusteredLightCulling(const ClusteredLightCulling&) = delete;
        ClusteredLightCulling& operator=(const ClusteredLightCulling&) = delete;

        /**
         * @brief Initialize the culling system with the given configuration.
         * @param config Grid dimensions and light limits.
         * @return True if initialization succeeded.
         */
        bool Initialize(const ClusterConfig& config = {});

        /**
         * @brief Rebuild cluster AABBs and assign lights to clusters.
         * @param viewMatrix Current camera view matrix.
         * @param projMatrix Current camera projection matrix.
         * @param nearZ Near clip plane distance.
         * @param farZ Far clip plane distance.
         */
        void Update(const XMFLOAT4X4& viewMatrix, const XMFLOAT4X4& projMatrix, float nearZ, float farZ);

        /**
         * @brief Add a light to be considered during the next Update.
         * @param light Light data describing position, radius, color, etc.
         */
        void AddLight(const LightData& light);

        /// @brief Remove all lights from the active list.
        void ClearLights();

        /// @brief Total number of clusters (gridX * gridY * gridZ).
        uint32_t GetClusterCount() const;

        /// @brief Number of lights currently in the active list.
        uint32_t GetActiveLightCount() const;

        /// @brief Get the current grid configuration.
        const ClusterConfig& GetConfig() const;

        /// @brief Get the light count for a specific cluster index.
        uint32_t GetClusterLightCount(uint32_t clusterIndex) const;

        /// @brief Get the light indices for a specific cluster.
        const uint32_t* GetClusterLightIndices(uint32_t clusterIndex) const;

        /// @brief Console status string for debugging.
        std::string Console_GetStatus() const;

        /// @brief Release all resources.
        void Shutdown();

        /// @brief Create GPU structured buffers for cluster light counts and light index lists
        /// @param device RHI device used to create the buffers
        /// @return True if buffers were created successfully
        bool CreateGPUBuffers(Spark::RHI::IRHIDevice* device);

        /// @brief Upload CPU-computed cluster data to the GPU buffers after Update()
        /// @param device RHI device used to update buffer contents
        void UploadToGPU(Spark::RHI::IRHIDevice* device);

        /// @brief Get the GPU buffer containing per-cluster light counts
        /// @return Pointer to the cluster count buffer, or nullptr if not created
        Spark::RHI::IRHIBuffer* GetClusterBuffer() const;

        /// @brief Get the GPU buffer containing the flat light index list
        /// @return Pointer to the light index buffer, or nullptr if not created
        Spark::RHI::IRHIBuffer* GetLightIndexBuffer() const;

      private:
        ClusteredLightCulling() = default;

        /// @brief Compute cluster AABBs from the frustum parameters.
        void RebuildClusterGrid(const XMFLOAT4X4& projMatrix, float nearZ, float farZ);

        /// @brief Assign each light to the clusters it overlaps.
        void AssignLightsToClusters(const XMFLOAT4X4& viewMatrix);

        /// @brief Test whether a sphere intersects an AABB.
        static bool SphereIntersectsAABB(const XMFLOAT3& center, float radius, const ClusterAABB& aabb);

        /// @brief Transform a point from world space to view space.
        static XMFLOAT3 TransformPoint(const XMFLOAT3& point, const XMFLOAT4X4& matrix);

        bool m_initialized = false;
        ClusterConfig m_config{};

        std::vector<LightData> m_lights;
        std::vector<ClusterAABB> m_clusterAABBs;

        /// Per-cluster light counts. Size = clusterCount.
        std::vector<uint32_t> m_clusterLightCounts;

        /// Flat array of light indices per cluster. Size = clusterCount * maxLightsPerCluster.
        std::vector<uint32_t> m_clusterLightIndices;

        std::unique_ptr<Spark::RHI::IRHIBuffer> m_gpuClusterBuffer;    ///< GPU buffer for per-cluster light counts
        std::unique_ptr<Spark::RHI::IRHIBuffer> m_gpuLightIndexBuffer; ///< GPU buffer for light index lists
    };

} // namespace Spark::Graphics
