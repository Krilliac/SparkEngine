/**
 * @file GPUDrivenRenderer.h
 * @brief GPU-driven indirect rendering pipeline with compute-based culling
 * @author Spark Engine Team
 * @date 2026
 *
 * Implements a fully GPU-driven rendering pipeline that performs frustum and
 * hierarchical Z-buffer (HiZ) occlusion culling via compute shaders, generates
 * indirect draw arguments on the GPU, and issues geometry draws through
 * ExecuteIndirect / DrawIndexedInstancedIndirect.
 *
 * Pipeline each frame:
 *   1. BeginFrame()  - Build HiZ mip chain from previous frame's depth
 *   2. CullAndDraw() - Upload AABBs, dispatch cull CS, read back visible count,
 *                       issue indirect draws for visible geometry
 *
 * @see GPUOcclusionCulling.h, GPUSceneBuffer.h, MeshClusterSystem.h
 */

#pragma once

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#include <d3d11.h>
#include <wrl/client.h>
#endif

#include <cstdint>
#include <string>

namespace Spark::Graphics
{

    // =========================================================================
    // Cull Settings
    // =========================================================================

    /// @brief Configuration for the GPU culling pass
    struct CullSettings
    {
        bool enableFrustumCull = true; ///< Frustum plane culling
        bool enableHiZCull = true;     ///< Hierarchical Z-buffer occlusion culling
        bool freezeCulling = false;    ///< Debug: freeze culling at current camera
    };

    // =========================================================================
    // Culling Statistics
    // =========================================================================

    /// @brief Per-frame statistics from the GPU culling pass
    struct CullStatistics
    {
        uint32_t totalInstances = 0;   ///< Total instances submitted for culling
        uint32_t visibleInstances = 0; ///< Instances that passed all cull tests
        uint32_t culledByFrustum = 0;  ///< Instances removed by frustum test
        uint32_t culledByHiZ = 0;      ///< Instances removed by HiZ occlusion test
    };

    // =========================================================================
    // GPU Instance AABB (matches HLSL structured buffer layout)
    // =========================================================================

    /// @brief Axis-aligned bounding box uploaded to the GPU cull shader
    struct alignas(16) GPUInstanceAABB
    {
        float minX = 0.0f, minY = 0.0f, minZ = 0.0f;
        float padding0 = 0.0f;
        float maxX = 0.0f, maxY = 0.0f, maxZ = 0.0f;
        float padding1 = 0.0f;
    };

    // =========================================================================
    // GPU Indirect Draw Arguments (matches D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS)
    // =========================================================================

    /// @brief Indirect draw arguments written by the cull compute shader
    struct IndirectDrawArgs
    {
        uint32_t indexCountPerInstance = 0;
        uint32_t instanceCount = 0;
        uint32_t startIndexLocation = 0;
        int32_t baseVertexLocation = 0;
        uint32_t startInstanceLocation = 0;
    };

    // =========================================================================
    // GPU-Driven Renderer
    // =========================================================================

    /**
     * @brief GPU-driven indirect rendering pipeline
     *
     * Performs frustum + HiZ occlusion culling entirely on the GPU via compute
     * shaders, generates indirect draw arguments, and renders visible geometry
     * with DrawIndexedInstancedIndirect.
     *
     * Singleton accessed via GetInstance(). Must be initialized after the D3D11
     * device is created and shut down before device destruction.
     */
    class GPUDrivenRenderer
    {
      public:
        static GPUDrivenRenderer& GetInstance()
        {
            static GPUDrivenRenderer instance;
            return instance;
        }

        GPUDrivenRenderer(const GPUDrivenRenderer&) = delete;
        GPUDrivenRenderer& operator=(const GPUDrivenRenderer&) = delete;

#ifdef SPARK_PLATFORM_WINDOWS
        /// @brief Initialize GPU resources (buffers, shaders, UAVs)
        /// @param device   D3D11 device
        /// @param context  D3D11 immediate context
        /// @param maxInstances Maximum number of instances to cull per frame
        /// @return true on success
        bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context, uint32_t maxInstances = 8192);
#endif

        /// @brief Release all GPU resources
        void Shutdown();

#ifdef SPARK_PLATFORM_WINDOWS
        /// @brief Build HiZ mip chain from the previous frame's depth buffer
        /// @param depthSRV  Shader resource view of the depth buffer
        /// @param width     Depth buffer width
        /// @param height    Depth buffer height
        void BeginFrame(ID3D11ShaderResourceView* depthSRV, uint32_t width, uint32_t height);

        /**
         * @brief Cull instances and issue indirect draws
         *
         * Uploads instance AABBs, dispatches the cull compute shader,
         * reads back the visible count for statistics, and issues
         * DrawIndexedInstancedIndirect for each visible draw call.
         *
         * @param aabbs         Array of instance AABBs to cull
         * @param instanceCount Number of instances
         * @param viewMatrix    Camera view matrix
         * @param projMatrix    Camera projection matrix
         * @param indexBuffer   Index buffer for the geometry
         * @param vertexBuffer  Vertex buffer for the geometry
         * @param stride        Vertex stride in bytes
         * @param indexCount    Total index count in the index buffer
         */
        void CullAndDraw(const GPUInstanceAABB* aabbs, uint32_t instanceCount, const DirectX::XMMATRIX& viewMatrix,
                         const DirectX::XMMATRIX& projMatrix, ID3D11Buffer* indexBuffer, ID3D11Buffer* vertexBuffer,
                         uint32_t stride, uint32_t indexCount);
#endif

        /// @brief Get the number of visible instances from the last cull pass
        uint32_t GetVisibleCount() const { return m_stats.visibleInstances; }

        /// @brief Get full culling statistics from the last frame
        const CullStatistics& GetStatistics() const { return m_stats; }

        /// @brief Get/set culling settings
        CullSettings& GetSettings() { return m_settings; }
        const CullSettings& GetSettings() const { return m_settings; }

        /// @brief Check if the renderer has been initialized
        bool IsInitialized() const { return m_initialized; }

        /// @brief Get a human-readable status string for the console
        std::string Console_GetStatus() const;

      private:
        GPUDrivenRenderer() = default;
        ~GPUDrivenRenderer() = default;

#ifdef SPARK_PLATFORM_WINDOWS
        /// @brief Compile a compute shader from file
        bool CompileComputeShader(const std::wstring& path, const std::string& entryPoint,
                                  ID3D11ComputeShader** shaderOut);

        /// @brief Create structured buffer + UAV/SRV pair
        bool CreateStructuredBuffer(uint32_t elementSize, uint32_t elementCount, ID3D11Buffer** bufferOut,
                                    ID3D11UnorderedAccessView** uavOut, ID3D11ShaderResourceView** srvOut,
                                    bool cpuReadback = false);

        /// @brief Create the HiZ mip chain UAV textures
        bool CreateHiZResources(uint32_t width, uint32_t height);

        // D3D11 device references (non-owning)
        ID3D11Device* m_device = nullptr;
        ID3D11DeviceContext* m_context = nullptr;

        // Compute shaders
        Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_cullShader;
        Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_hiZBuildShader;

        // Instance AABB buffer (upload)
        Microsoft::WRL::ComPtr<ID3D11Buffer> m_aabbBuffer;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_aabbSRV;

        // Visibility results buffer (GPU write, CPU readback)
        Microsoft::WRL::ComPtr<ID3D11Buffer> m_visibilityBuffer;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_visibilityUAV;
        Microsoft::WRL::ComPtr<ID3D11Buffer> m_visibilityStaging;

        // Indirect draw arguments buffer
        Microsoft::WRL::ComPtr<ID3D11Buffer> m_indirectArgsBuffer;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_indirectArgsUAV;

        // Cull constants (frustum planes, HiZ dimensions, settings)
        Microsoft::WRL::ComPtr<ID3D11Buffer> m_cullConstantBuffer;

        // HiZ mip chain
        static constexpr int kMaxHiZMips = 12;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_hiZTexture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_hiZSRV;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_hiZMipUAVs[kMaxHiZMips];
        int m_hiZMipCount = 0;
        uint32_t m_hiZWidth = 0;
        uint32_t m_hiZHeight = 0;
#endif

        CullSettings m_settings;
        CullStatistics m_stats;
        uint32_t m_maxInstances = 0;
        bool m_initialized = false;
    };

} // namespace Spark::Graphics
