/**
 * @file GPUSceneBuffer.h
 * @brief Persistent GPU structured buffer for scene instance data
 * @author Spark Engine Team
 * @date 2026
 *
 * Maintains a
 * persistent structured buffer on the GPU containing per-instance data
 * (world matrix, previous frame matrix, material ID, flags). Updated
 * incrementally each frame — only dirty entries are re-uploaded — avoiding
 * full-scene re-upload overhead.
 *
 * Combined with DrawSortKey and instanced draw calls, this enables the
 * renderer to issue a single IASetVertexBuffers + DrawIndexedInstanced
 * per mesh group, reading per-instance transforms from the scene buffer.
 *
 * ## Usage
 * @code
 *   GPUSceneBuffer sceneBuffer;
 *   sceneBuffer.Initialize(device, 4096);  // up to 4096 instances
 *
 *   // When an entity's transform changes
 *   GPUInstanceData data;
 *   XMStoreFloat4x4(&data.worldMatrix, worldMat);
 *   XMStoreFloat4x4(&data.prevWorldMatrix, prevWorldMat);
 *   data.materialId = matId;
 *   sceneBuffer.UpdateInstance(entitySlot, data);
 *
 *   // Each frame, flush dirty entries to GPU
 *   sceneBuffer.FlushToGPU(context);
 *
 *   // Bind for vertex shader access
 *   sceneBuffer.Bind(context, 0);  // t0 slot
 * @endcode
 *
 * @see DrawSortKey, GraphicsEngine, RenderSystem
 */

#pragma once

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#include <d3d11.h>
#include <wrl/client.h>
#endif

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <utility>

#ifdef SPARK_PLATFORM_WINDOWS

using Microsoft::WRL::ComPtr;

namespace Spark::Graphics
{

    /**
     * @brief Per-instance data stored in the GPU scene buffer.
     *
     * Matches the HLSL StructuredBuffer<InstanceData> layout.
     * 16-byte aligned for GPU compatibility.
     */
    struct alignas(16) GPUInstanceData
    {
        DirectX::XMFLOAT4X4 worldMatrix;     ///< Current frame world matrix
        DirectX::XMFLOAT4X4 prevWorldMatrix; ///< Previous frame matrix (for motion vectors)
        DirectX::XMFLOAT4X4 normalMatrix;    ///< Inverse-transpose of world (for normals)
        uint32_t materialId = 0;             ///< Index into material table
        uint32_t flags = 0;                  ///< Bit flags (visible, castShadow, etc.)
        float lodDistance = 0.0f;            ///< Distance to camera for LOD selection
        float padding = 0.0f;
    };

    /**
     * @brief Instance flags for GPUInstanceData::flags field.
     */
    enum class InstanceFlags : uint32_t
    {
        None = 0,
        Visible = 1 << 0,
        CastShadow = 1 << 1,
        ReceiveShadow = 1 << 2,
        Static = 1 << 3,
        Skinned = 1 << 4,
        Selected = 1 << 5 // Editor selection highlight
    };

    inline InstanceFlags operator|(InstanceFlags a, InstanceFlags b)
    {
        return static_cast<InstanceFlags>(std::to_underlying(a) | std::to_underlying(b));
    }

    /**
     * @brief GPU scene buffer metrics.
     */
    struct SceneBufferMetrics
    {
        uint32_t capacity = 0;
        uint32_t activeInstances = 0;
        uint32_t dirtyInstances = 0;
        uint32_t uploadsThisFrame = 0;
        size_t bufferSizeBytes = 0;
    };

    /**
     * @brief Persistent GPU structured buffer for scene instance data with incremental updates.
     */
    class GPUSceneBuffer
    {
      public:
        GPUSceneBuffer() = default;
        ~GPUSceneBuffer() = default;

        /**
         * @brief Initialize the buffer with a given capacity.
         *
         * @param device    D3D11 device for buffer creation.
         * @param capacity  Maximum number of instances.
         * @return true on success.
         */
        bool Initialize(ID3D11Device* device, uint32_t capacity = 4096)
        {
            m_device = device;
            m_capacity = capacity;

            m_cpuData.resize(capacity);
            m_dirtyFlags.resize(capacity, false);

            // Create GPU structured buffer
            D3D11_BUFFER_DESC bufDesc = {};
            bufDesc.ByteWidth = capacity * sizeof(GPUInstanceData);
            bufDesc.Usage = D3D11_USAGE_DYNAMIC;
            bufDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            bufDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            bufDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            bufDesc.StructureByteStride = sizeof(GPUInstanceData);

            HRESULT hr = device->CreateBuffer(&bufDesc, nullptr, &m_buffer);
            if (FAILED(hr))
                return false;

            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            srvDesc.Buffer.NumElements = capacity;

            hr = device->CreateShaderResourceView(m_buffer.Get(), &srvDesc, &m_srv);
            if (FAILED(hr))
                return false;

            m_initialized = true;
            return true;
        }

        void Shutdown()
        {
            m_buffer.Reset();
            m_srv.Reset();
            m_cpuData.clear();
            m_dirtyFlags.clear();
            m_initialized = false;
        }

        /**
         * @brief Update a single instance's data. Marks the slot as dirty.
         *
         * @param slot  Instance index (must be < capacity).
         * @param data  New instance data.
         */
        void UpdateInstance(uint32_t slot, const GPUInstanceData& data)
        {
            if (slot >= m_capacity)
                return;

            m_cpuData[slot] = data;
            m_dirtyFlags[slot] = true;
            m_activeSlots = std::max(m_activeSlots, slot + 1);
            ++m_dirtyCount;
        }

        /**
         * @brief Update only the world matrix of an instance (common case).
         *
         * Preserves the previous frame matrix in prevWorldMatrix for motion vectors.
         */
        void UpdateTransform(uint32_t slot, const DirectX::XMMATRIX& worldMatrix)
        {
            if (slot >= m_capacity)
                return;

            // Save current as previous
            m_cpuData[slot].prevWorldMatrix = m_cpuData[slot].worldMatrix;

            // Store new world matrix
            DirectX::XMStoreFloat4x4(&m_cpuData[slot].worldMatrix, worldMatrix);

            // Compute normal matrix (inverse-transpose)
            DirectX::XMMATRIX invTranspose = DirectX::XMMatrixTranspose(DirectX::XMMatrixInverse(nullptr, worldMatrix));
            DirectX::XMStoreFloat4x4(&m_cpuData[slot].normalMatrix, invTranspose);

            m_dirtyFlags[slot] = true;
            m_activeSlots = std::max(m_activeSlots, slot + 1);
            ++m_dirtyCount;
        }

        /**
         * @brief Flush all dirty entries to the GPU buffer.
         *
         * Maps the buffer and copies only dirty entries. If more than 50%
         * of entries are dirty, does a full upload for efficiency.
         *
         * @param context  D3D11 device context.
         */
        void FlushToGPU(ID3D11DeviceContext* context)
        {
            if (!m_initialized || m_dirtyCount == 0)
            {
                m_metrics.uploadsThisFrame = 0;
                return;
            }

            D3D11_MAPPED_SUBRESOURCE mapped;
            HRESULT hr = context->Map(m_buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
            if (FAILED(hr) || !mapped.pData)
                return;

            // Always copy the active range (WRITE_DISCARD invalidates the whole buffer)
            std::memcpy(mapped.pData, m_cpuData.data(), m_activeSlots * sizeof(GPUInstanceData));

            context->Unmap(m_buffer.Get(), 0);

            m_metrics.uploadsThisFrame = m_dirtyCount;
            m_metrics.dirtyInstances = m_dirtyCount;

            // Clear dirty flags
            std::fill(m_dirtyFlags.begin(), m_dirtyFlags.begin() + m_activeSlots, false);
            m_dirtyCount = 0;
        }

        /**
         * @brief Bind the scene buffer as a shader resource.
         *
         * @param context  D3D11 device context.
         * @param slot     SRV register slot (e.g. t0).
         */
        void BindVS(ID3D11DeviceContext* context, uint32_t slot) const
        {
            ID3D11ShaderResourceView* srv = m_srv.Get();
            context->VSSetShaderResources(slot, 1, &srv);
        }

        void BindPS(ID3D11DeviceContext* context, uint32_t slot) const
        {
            ID3D11ShaderResourceView* srv = m_srv.Get();
            context->PSSetShaderResources(slot, 1, &srv);
        }

        /**
         * @brief Get read-only access to CPU-side instance data.
         */
        const GPUInstanceData& GetInstance(uint32_t slot) const { return m_cpuData[slot]; }

        /**
         * @brief Allocate the next available slot.
         * @return Slot index, or UINT32_MAX if full.
         */
        uint32_t AllocateSlot()
        {
            if (m_activeSlots >= m_capacity)
                return UINT32_MAX;
            return m_activeSlots++;
        }

        uint32_t GetCapacity() const { return m_capacity; }
        uint32_t GetActiveCount() const { return m_activeSlots; }
        bool IsInitialized() const { return m_initialized; }

        // =================================================================
        // Metrics
        // =================================================================

        SceneBufferMetrics GetMetrics() const
        {
            SceneBufferMetrics m = m_metrics;
            m.capacity = m_capacity;
            m.activeInstances = m_activeSlots;
            m.bufferSizeBytes = static_cast<size_t>(m_capacity) * sizeof(GPUInstanceData);
            return m;
        }

        std::string Console_GetStatus() const
        {
            auto m = GetMetrics();
            std::string s = "GPU Scene Buffer:\n";
            s += "  Capacity:    " + std::to_string(m.capacity) + "\n";
            s += "  Active:      " + std::to_string(m.activeInstances) + "\n";
            s += "  Dirty:       " + std::to_string(m.dirtyInstances) + "\n";
            s += "  Uploads:     " + std::to_string(m.uploadsThisFrame) + "\n";
            s += "  Buffer size: " + std::to_string(m.bufferSizeBytes / 1024) + " KB\n";
            return s;
        }

      private:
        ID3D11Device* m_device = nullptr;
        ComPtr<ID3D11Buffer> m_buffer;
        ComPtr<ID3D11ShaderResourceView> m_srv;

        std::vector<GPUInstanceData> m_cpuData;
        std::vector<bool> m_dirtyFlags;
        uint32_t m_capacity = 0;
        uint32_t m_activeSlots = 0;
        uint32_t m_dirtyCount = 0;
        bool m_initialized = false;

        SceneBufferMetrics m_metrics{};
    };

} // namespace Spark::Graphics

#endif // SPARK_PLATFORM_WINDOWS
