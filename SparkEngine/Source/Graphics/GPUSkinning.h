/**
 * @file GPUSkinning.h
 * @brief GPU compute shader skinning manager for offloading vertex skinning from the vertex shader
 * @author Spark Engine Team
 * @date 2026
 *
 * Manages per-mesh skinning resources (source vertex buffer, bone matrix buffer, output vertex
 * buffer) and dispatches a compute shader to transform vertices on the GPU. This frees the
 * vertex shader from per-vertex matrix blending, improving throughput for dense skeletal meshes.
 *
 * ## Pipeline
 * ```
 * AnimationEvaluator (CPU)
 *     -> bone matrices uploaded to structured buffer
 *     -> SkinningCS.hlsl dispatched (compute shader)
 *     -> skinned vertices written to output structured buffer
 *     -> output buffer bound as vertex SRV for rendering
 * ```
 *
 * ## Usage
 * @code
 *   auto& skinning = Spark::Graphics::GPUSkinning::GetInstance();
 *   skinning.Initialize(device);
 *
 *   MeshHandle handle = skinning.RegisterMesh(meshId, sourceVertices, vertexCount, maxBones);
 *
 *   // Each frame, after animation evaluation:
 *   skinning.DispatchSkinning(context, meshId, boneMatrices, boneCount);
 *
 *   // Bind the skinned output for rendering
 *   auto* srv = skinning.GetSkinnedBuffer(meshId);
 * @endcode
 *
 * @see AnimationSystem, Skeleton, GPUSceneBuffer
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
#include <unordered_map>
#include <vector>

namespace Spark::Graphics
{

    /// Maximum bones per skinned mesh (matches HLSL constant).
    static constexpr uint32_t kMaxBonesPerMesh = 256;

    /// Thread group size for the skinning compute shader (must match SkinningCS.hlsl).
    static constexpr uint32_t kSkinningThreadGroupSize = 64;

    /**
     * @brief Per-vertex data fed into the skinning compute shader.
     *
     * Matches the StructuredBuffer<SkinnedVertex> layout in SkinningCS.hlsl.
     * Each vertex carries position, normal, bone indices, and blend weights.
     */
    struct alignas(16) SkinningSourceVertex
    {
        DirectX::XMFLOAT3 position;
        float padding0 = 0.0f;
        DirectX::XMFLOAT3 normal;
        float padding1 = 0.0f;
        DirectX::XMFLOAT2 texCoord;
        uint32_t boneIndices[4] = {0, 0, 0, 0}; ///< Up to 4 bone influences
        float boneWeights[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    };

    /**
     * @brief Output vertex produced by the skinning compute shader.
     *
     * Matches the RWStructuredBuffer<SkinningOutputVertex> layout in SkinningCS.hlsl.
     */
    struct alignas(16) SkinningOutputVertex
    {
        DirectX::XMFLOAT3 position;
        float padding0 = 0.0f;
        DirectX::XMFLOAT3 normal;
        float padding1 = 0.0f;
        DirectX::XMFLOAT2 texCoord;
        float padding2[2] = {0.0f, 0.0f};
    };

#ifdef SPARK_PLATFORM_WINDOWS

    using Microsoft::WRL::ComPtr;

    /**
     * @brief Per-mesh GPU resources for compute skinning.
     */
    struct SkinningMeshEntry
    {
        uint32_t vertexCount = 0;
        uint32_t boneCount = 0;

        ComPtr<ID3D11Buffer> sourceVertexBuffer;          ///< Structured buffer: source vertices
        ComPtr<ID3D11ShaderResourceView> sourceVertexSRV; ///< SRV for source vertex read

        ComPtr<ID3D11Buffer> boneMatrixBuffer;          ///< Structured buffer: bone matrices
        ComPtr<ID3D11ShaderResourceView> boneMatrixSRV; ///< SRV for bone matrix read

        ComPtr<ID3D11Buffer> outputVertexBuffer;           ///< Structured buffer: skinned output
        ComPtr<ID3D11UnorderedAccessView> outputVertexUAV; ///< UAV for compute shader write
        ComPtr<ID3D11ShaderResourceView> outputVertexSRV;  ///< SRV for vertex shader read

        ComPtr<ID3D11Buffer> constantBuffer; ///< Per-dispatch constants (vertex count, bone count)
    };

    /**
     * @brief GPU compute skinning manager.
     *
     * Singleton that owns the skinning compute shader and per-mesh GPU resources.
     * Call DispatchSkinning() each frame after bone matrices are evaluated on the CPU.
     */
    class GPUSkinning
    {
      public:
        static GPUSkinning& GetInstance();

        /**
         * @brief Initialize the skinning system and compile the compute shader.
         * @param device  D3D11 device for resource creation.
         * @return true on success, false if shader compilation or resource creation fails.
         */
        bool Initialize(ID3D11Device* device);

        /**
         * @brief Release all GPU resources and registered meshes.
         */
        void Shutdown();

        /**
         * @brief Register a skinned mesh for GPU skinning.
         *
         * Creates source vertex, bone matrix, and output vertex structured buffers.
         *
         * @param meshId          Unique identifier for the mesh.
         * @param sourceVertices  CPU-side source vertex data (copied to GPU).
         * @param vertexCount     Number of vertices.
         * @param maxBones        Maximum number of bones this mesh uses.
         * @return true on success.
         */
        bool RegisterMesh(uint32_t meshId, const SkinningSourceVertex* sourceVertices, uint32_t vertexCount,
                          uint32_t maxBones);

        /**
         * @brief Unregister a mesh and release its GPU resources.
         * @param meshId  Mesh to remove.
         */
        void UnregisterMesh(uint32_t meshId);

        /**
         * @brief Upload bone matrices and dispatch the skinning compute shader.
         *
         * @param context       D3D11 device context.
         * @param meshId        Mesh to skin.
         * @param boneMatrices  Array of bone matrices (row-major 4x4).
         * @param boneCount     Number of bone matrices in the array.
         */
        void DispatchSkinning(ID3D11DeviceContext* context, uint32_t meshId, const DirectX::XMFLOAT4X4* boneMatrices,
                              uint32_t boneCount);

        /**
         * @brief Get the SRV for the skinned output buffer of a registered mesh.
         *
         * Bind this as a vertex SRV in the vertex shader to read skinned positions/normals.
         *
         * @param meshId  Mesh to query.
         * @return SRV pointer, or nullptr if the mesh is not registered.
         */
        ID3D11ShaderResourceView* GetSkinnedBuffer(uint32_t meshId) const;

        /**
         * @brief Check whether a mesh is registered for GPU skinning.
         * @param meshId  Mesh to check.
         * @return true if registered.
         */
        bool IsMeshRegistered(uint32_t meshId) const;

        /**
         * @brief Check whether the skinning system is initialized.
         */
        bool IsInitialized() const { return m_initialized; }

        /**
         * @brief Get the number of currently registered meshes.
         */
        uint32_t GetRegisteredMeshCount() const { return static_cast<uint32_t>(m_meshEntries.size()); }

        /**
         * @brief Return a console-friendly status string.
         */
        std::string Console_GetStatus() const;

      private:
        GPUSkinning() = default;
        ~GPUSkinning() = default;
        GPUSkinning(const GPUSkinning&) = delete;
        GPUSkinning& operator=(const GPUSkinning&) = delete;

        bool CompileComputeShader(ID3D11Device* device);
        bool CreateConstantBuffer(ID3D11Device* device, SkinningMeshEntry& entry);

        ID3D11Device* m_device = nullptr;
        ComPtr<ID3D11ComputeShader> m_skinningShader;

        std::unordered_map<uint32_t, SkinningMeshEntry> m_meshEntries;

        bool m_initialized = false;
        uint32_t m_totalDispatches = 0;     ///< Cumulative dispatch count for metrics
        uint32_t m_dispatchesThisFrame = 0; ///< Dispatches in the current frame
    };

#endif // SPARK_PLATFORM_WINDOWS

} // namespace Spark::Graphics
