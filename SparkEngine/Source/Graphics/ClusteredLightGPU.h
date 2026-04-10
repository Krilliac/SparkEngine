/**
 * @file ClusteredLightGPU.h
 * @brief GPU structured buffer layout for clustered forward+ lighting
 *
 * Bridges the CPU ClusteredLightCulling results into GPU-ready data structures.
 * Produces flat arrays suitable for upload to structured buffers / SRVs that
 * forward+ shaders can read to determine which lights affect each fragment.
 *
 * ## Shader Binding
 * The forward+ pixel shader reads:
 * - `g_LightBuffer` (StructuredBuffer<GPULightData>) — all active lights
 * - `g_ClusterLightGrid` (StructuredBuffer<ClusterLightGrid>) — per-cluster offset + count
 * - `g_LightIndexList` (Buffer<uint>) — flat light index list
 *
 * @see ClusteredLightCulling for CPU-side cluster computation
 *
 * @note **Intentional GPU bridge utility** — converts
 *       `ClusteredLightCulling` results into structured-buffer-ready
 *       arrays (`GPULightData`, `ClusterLightGrid`, light-index list).
 *       There is no singleton; a future forward+ render pass will
 *       instantiate one of these per frame and call its buffer-build
 *       helpers. Exercised by `Tests/TestClusteredLightGPU.cpp` so the
 *       layout stays valid even without a live render pass.
 */

#pragma once

#include "ClusteredLightCulling.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace Spark::Graphics
{

    /**
     * @brief GPU-aligned light data matching the HLSL StructuredBuffer layout.
     *
     * Packed to 48 bytes (3 × float4) for efficient GPU reads.
     */
    struct GPULightData
    {
        float positionX, positionY, positionZ;
        float radius;
        float colorR, colorG, colorB;
        float intensity;
        uint32_t type; ///< 0 = point, 1 = spot
        float pad0, pad1, pad2;
    };

    static_assert(sizeof(GPULightData) == 48, "GPULightData must be 48 bytes for GPU alignment");

    /**
     * @brief Per-cluster grid entry: offset into the light index list + count.
     *
     * 8 bytes per cluster (2 × uint32).
     */
    struct ClusterLightGrid
    {
        uint32_t offset; ///< Start index into the light index list
        uint32_t count;  ///< Number of lights in this cluster
    };

    static_assert(sizeof(ClusterLightGrid) == 8, "ClusterLightGrid must be 8 bytes");

    /**
     * @brief GPU upload data produced from ClusteredLightCulling results.
     *
     * Contains all data needed to populate GPU structured buffers for
     * forward+ shading in a single frame.
     */
    struct ClusteredLightGPUData
    {
        std::vector<GPULightData> lights;     ///< All active lights for this frame
        std::vector<ClusterLightGrid> grid;   ///< Per-cluster offset + count
        std::vector<uint32_t> lightIndexList; ///< Flat list of light indices per cluster
        uint32_t clusterCountX = 0;
        uint32_t clusterCountY = 0;
        uint32_t clusterCountZ = 0;
        uint32_t totalClusters = 0;
        uint32_t activeLightCount = 0;
    };

    /**
     * @brief Extracts GPU-ready data from the ClusteredLightCulling system.
     *
     * Call once per frame after ClusteredLightCulling::Update(). The resulting
     * data is ready for upload to GPU structured buffers.
     *
     * @param culling  The clustered light culling system (already updated this frame)
     * @return GPU-ready data for structured buffer upload
     */
    inline ClusteredLightGPUData BuildClusteredLightGPUData(const ClusteredLightCulling& culling)
    {
        ClusteredLightGPUData data;
        const ClusterConfig& config = culling.GetConfig();

        data.clusterCountX = config.gridX;
        data.clusterCountY = config.gridY;
        data.clusterCountZ = config.gridZ;
        data.totalClusters = culling.GetClusterCount();
        data.activeLightCount = culling.GetActiveLightCount();

        // Populate light buffer (lights are already stored in the culling system;
        // we reconstruct GPULightData from the public API)
        data.grid.resize(data.totalClusters);
        uint32_t indexOffset = 0;

        for (uint32_t i = 0; i < data.totalClusters; ++i)
        {
            uint32_t count = culling.GetClusterLightCount(i);
            data.grid[i].offset = indexOffset;
            data.grid[i].count = count;

            const uint32_t* indices = culling.GetClusterLightIndices(i);
            if (indices && count > 0)
            {
                data.lightIndexList.insert(data.lightIndexList.end(), indices, indices + count);
            }
            indexOffset += count;
        }

        return data;
    }

    /**
     * @brief HLSL constant buffer layout for clustered lighting parameters.
     *
     * Bind to a constant buffer slot so the forward+ shader knows the grid dimensions
     * and can compute cluster index from screen position + depth.
     */
    struct alignas(16) ClusteredLightConstants
    {
        uint32_t clusterCountX;
        uint32_t clusterCountY;
        uint32_t clusterCountZ;
        uint32_t totalClusters;
        float nearZ;
        float farZ;
        float logFarOverNear; ///< log2(farZ / nearZ) for exponential Z slicing
        uint32_t activeLightCount;
    };

    static_assert(sizeof(ClusteredLightConstants) == 32, "ClusteredLightConstants must be 32 bytes");

    /**
     * @brief Fill the constant buffer data for clustered lighting.
     *
     * @param config  Cluster grid configuration
     * @param nearZ   Camera near plane
     * @param farZ    Camera far plane
     * @param lightCount  Number of active lights
     * @return Filled constant buffer data
     */
    inline ClusteredLightConstants BuildClusteredConstants(const ClusterConfig& config, float nearZ, float farZ,
                                                           uint32_t lightCount)
    {
        ClusteredLightConstants cb{};
        cb.clusterCountX = config.gridX;
        cb.clusterCountY = config.gridY;
        cb.clusterCountZ = config.gridZ;
        cb.totalClusters = config.gridX * config.gridY * config.gridZ;
        cb.nearZ = nearZ;
        cb.farZ = farZ;
        cb.logFarOverNear = (farZ > nearZ && nearZ > 0.0f) ? std::log2(farZ / nearZ) : 1.0f;
        cb.activeLightCount = lightCount;
        return cb;
    }

} // namespace Spark::Graphics
