/**
 * @file GPUClusterCulling.h
 * @brief GPU compute shader-based clustered light assignment
 * @author Spark Engine Team
 * @date 2026
 *
 * Moves the light-to-cluster assignment from CPU to GPU compute shader.
 * Replaces the CPU loop in ClusteredLightCulling with a single compute
 * dispatch that tests all lights against all clusters in parallel.
 *
 * @see ClusteredLightCulling.h, ClusterCull.hlsl
 */

#pragma once

#include "../Core/Platform.h"

#include <cstdint>
#include <string>
#include <vector>

#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d11.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
#endif

namespace Spark::Graphics
{

    /// @brief GPU-uploadable light data matching ClusterCull.hlsl layout
    struct GPULightData
    {
        float position[3] = {};
        float radius = 0.0f;
        float color[3] = {1.0f, 1.0f, 1.0f};
        float intensity = 1.0f;
        float direction[3] = {0.0f, -1.0f, 0.0f};
        float spotAngle = 0.0f;
        uint32_t lightType = 0; // 0=point, 1=spot, 2=directional
        uint32_t castShadows = 0;
        float pad[2] = {};
    };

    /// @brief GPU-uploadable cluster AABB
    struct GPUClusterAABB
    {
        float minPoint[3] = {};
        float pad0 = 0.0f;
        float maxPoint[3] = {};
        float pad1 = 0.0f;
    };

    /// @brief Cluster grid configuration
    struct ClusterGridConfig
    {
        uint32_t gridX = 16;
        uint32_t gridY = 8;
        uint32_t gridZ = 24;
        uint32_t maxLightsPerCluster = 200;
    };

    /// @brief Performance statistics
    struct GPUClusterCullStats
    {
        uint32_t totalClusters = 0;
        uint32_t activeLights = 0;
        uint32_t dispatchCount = 0;
        float lastDispatchTimeMs = 0.0f;
    };

    /**
     * @brief GPU-accelerated clustered light culling
     *
     * Dispatches a compute shader to assign lights to 3D frustum clusters.
     * The output structured buffers are bound directly to the Forward+ pixel
     * shader for per-pixel light list lookup.
     */
    class GPUClusterCulling
    {
      public:
        [[deprecated("Use EngineContext::Get()->GetSystem<GPUClusterCulling>() instead")]]
        static GPUClusterCulling& GetInstance()
        {
            static GPUClusterCulling instance;
            return instance;
        }

#ifdef SPARK_PLATFORM_WINDOWS
        /// @brief Initialize GPU resources
        /// @param device D3D11 device for resource creation
        /// @param config Cluster grid configuration
        /// @return true on success
        bool Initialize(ID3D11Device* device, const ClusterGridConfig& config = {});

        /// @brief Release all GPU resources
        void Shutdown();

        /// @brief Upload cluster AABBs (call once or when projection changes)
        void UploadClusterGrid(ID3D11DeviceContext* context, const GPUClusterAABB* clusters, uint32_t count);

        /// @brief Upload lights and dispatch the cull compute shader
        /// @param context Device context for dispatch
        /// @param lights Array of GPU light data
        /// @param lightCount Number of active lights
        /// @param inverseProjection Inverse projection matrix (16 floats, column-major)
        /// @param nearPlane Camera near plane distance
        /// @param farPlane Camera far plane distance
        void DispatchCull(ID3D11DeviceContext* context, const GPULightData* lights, uint32_t lightCount,
                          const float* inverseProjection, float nearPlane, float farPlane);

        /// @brief Get the cluster light index SRV for binding to pixel shader
        ID3D11ShaderResourceView* GetClusterLightIndicesSRV() const { return m_lightIndicesSRV.Get(); }

        /// @brief Get the cluster light count SRV for binding to pixel shader
        ID3D11ShaderResourceView* GetClusterLightCountsSRV() const { return m_lightCountsSRV.Get(); }

        /// @brief Get the light data SRV for binding to pixel shader
        ID3D11ShaderResourceView* GetLightDataSRV() const { return m_lightBufferSRV.Get(); }
#endif

        const ClusterGridConfig& GetConfig() const { return m_config; }
        const GPUClusterCullStats& GetStats() const { return m_stats; }
        bool IsInitialized() const { return m_initialized; }

        /// @brief Console status report
        std::string Console_GetStatus() const;

      private:
        GPUClusterCulling() = default;
        ~GPUClusterCulling() = default;
        GPUClusterCulling(const GPUClusterCulling&) = delete;
        GPUClusterCulling& operator=(const GPUClusterCulling&) = delete;

        ClusterGridConfig m_config;
        GPUClusterCullStats m_stats;
        bool m_initialized = false;

#ifdef SPARK_PLATFORM_WINDOWS
        // Compute shader
        ComPtr<ID3D11ComputeShader> m_cullShader;

        // Light structured buffer
        ComPtr<ID3D11Buffer> m_lightBuffer;
        ComPtr<ID3D11ShaderResourceView> m_lightBufferSRV;

        // Cluster AABBs
        ComPtr<ID3D11Buffer> m_clusterBuffer;
        ComPtr<ID3D11ShaderResourceView> m_clusterBufferSRV;

        // Output: per-cluster light indices
        ComPtr<ID3D11Buffer> m_lightIndicesBuffer;
        ComPtr<ID3D11UnorderedAccessView> m_lightIndicesUAV;
        ComPtr<ID3D11ShaderResourceView> m_lightIndicesSRV;

        // Output: per-cluster light counts
        ComPtr<ID3D11Buffer> m_lightCountsBuffer;
        ComPtr<ID3D11UnorderedAccessView> m_lightCountsUAV;
        ComPtr<ID3D11ShaderResourceView> m_lightCountsSRV;

        // Constants
        ComPtr<ID3D11Buffer> m_constantBuffer;
#endif
    };

} // namespace Spark::Graphics
