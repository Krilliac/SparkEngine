/**
 * @file ConstantBufferRing.h
 * @brief Ring-buffer allocator for dynamic constant buffers
 * @author Spark Engine Team
 * @date 2026
 *
 * Eliminates per-draw Map/Unmap stalls by sub-allocating from a single large
 * ID3D11Buffer. Uses MAP_WRITE_DISCARD once at frame start, then bump-allocates
 * linearly within the frame. Requires DX11.1 offset constant buffer binding
 * (VSSetConstantBuffers1 / PSSetConstantBuffers1).
 *
 * ## Usage
 * @code
 *   ConstantBufferRing ring;
 *   ring.Initialize(device, 2 * 1024 * 1024); // 2 MB
 *
 *   // Each frame:
 *   ring.BeginFrame(context);
 *
 *   auto alloc = ring.Allocate(sizeof(PerObjectCB));
 *   memcpy(alloc.cpuAddress, &objectCB, sizeof(PerObjectCB));
 *   ring.BindVS(context, 0, alloc);
 *   ring.BindPS(context, 0, alloc);
 *
 *   ring.EndFrame();
 * @endcode
 *
 * @see GraphicsEngine, PipelineStateCache
 */

#pragma once

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d11_1.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>

using Microsoft::WRL::ComPtr;

namespace Spark::Graphics
{

/**
 * @brief Result of a constant buffer sub-allocation
 */
struct CBAllocation
{
    uint32_t offset = 0;        ///< Byte offset into the ring buffer
    uint32_t size = 0;          ///< Size of the allocation in bytes
    void* cpuAddress = nullptr; ///< CPU-visible pointer for writing data

    /** @brief Check if this allocation is valid */
    bool IsValid() const { return cpuAddress != nullptr; }
};

/**
 * @brief Metrics for constant buffer ring usage
 */
struct CBRingMetrics
{
    uint32_t capacityBytes = 0;       ///< Total ring buffer capacity
    uint32_t usedThisFrame = 0;       ///< Bytes allocated this frame
    uint32_t allocationsThisFrame = 0; ///< Number of allocations this frame
    uint32_t peakUsageBytes = 0;      ///< High water mark across all frames
};

/**
 * @brief Ring-buffer allocator for dynamic constant buffers
 *
 * Creates a single large dynamic constant buffer and sub-allocates from it
 * each frame using a bump allocator. This avoids the overhead of individual
 * Map/Unmap calls per draw and reduces driver overhead significantly.
 *
 * DX11.1's offset constant buffer binding (VSSetConstantBuffers1) allows
 * binding a sub-range of the buffer to a shader slot without rebinding
 * the entire buffer.
 */
class ConstantBufferRing
{
public:
    ConstantBufferRing() = default;
    ~ConstantBufferRing() { Shutdown(); }

    // Non-copyable, non-movable
    ConstantBufferRing(const ConstantBufferRing&) = delete;
    ConstantBufferRing& operator=(const ConstantBufferRing&) = delete;
    ConstantBufferRing(ConstantBufferRing&&) = delete;
    ConstantBufferRing& operator=(ConstantBufferRing&&) = delete;

    /**
     * @brief Initialize the ring buffer
     * @param device D3D11 device to create the buffer on
     * @param capacityBytes Total buffer size in bytes (default 2 MB)
     * @return true on success
     */
    bool Initialize(ID3D11Device* device, uint32_t capacityBytes = 2 * 1024 * 1024)
    {
        if (!device || capacityBytes == 0)
        {
            return false;
        }

        // Capacity must be a multiple of 256 (D3D11 constant buffer alignment)
        m_capacity = (capacityBytes + 255) & ~255u;

        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = m_capacity;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        HRESULT hr = device->CreateBuffer(&desc, nullptr, m_buffer.ReleaseAndGetAddressOf());
        if (FAILED(hr))
        {
            return false;
        }

        m_initialized = true;
        m_metrics.capacityBytes = m_capacity;
        return true;
    }

    /**
     * @brief Release all resources
     */
    void Shutdown()
    {
        m_buffer.Reset();
        m_mappedPtr = nullptr;
        m_currentOffset = 0;
        m_initialized = false;
        m_frameMapped = false;
        m_metrics = {};
    }

    /**
     * @brief Begin a new frame — maps the buffer with DISCARD
     * @param context D3D11 device context
     * @return true if mapping succeeded
     */
    bool BeginFrame(ID3D11DeviceContext* context)
    {
        if (!m_initialized || !context || m_frameMapped)
        {
            return false;
        }

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT hr = context->Map(m_buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr))
        {
            return false;
        }

        m_mappedPtr = static_cast<uint8_t*>(mapped.pData);
        m_currentOffset = 0;
        m_frameMapped = true;
        m_metrics.usedThisFrame = 0;
        m_metrics.allocationsThisFrame = 0;
        m_context = context;
        return true;
    }

    /**
     * @brief End the current frame — unmaps the buffer
     */
    void EndFrame()
    {
        if (!m_frameMapped || !m_context)
        {
            return;
        }

        m_context->Unmap(m_buffer.Get(), 0);
        m_mappedPtr = nullptr;
        m_frameMapped = false;

        // Update peak usage
        if (m_metrics.usedThisFrame > m_metrics.peakUsageBytes)
        {
            m_metrics.peakUsageBytes = m_metrics.usedThisFrame;
        }

        m_context = nullptr;
    }

    /**
     * @brief Sub-allocate from the ring buffer
     * @param sizeBytes Number of bytes to allocate
     * @param alignment Alignment requirement (default 256, D3D11 CB minimum)
     * @return CBAllocation with offset and CPU pointer, or invalid if full
     */
    CBAllocation Allocate(uint32_t sizeBytes, uint32_t alignment = 256)
    {
        CBAllocation result;

        if (!m_frameMapped || sizeBytes == 0)
        {
            return result;
        }

        // Align the current offset
        uint32_t alignedOffset = (m_currentOffset + alignment - 1) & ~(alignment - 1);

        // Align the size to 256 bytes (D3D11 constant buffer size requirement)
        uint32_t alignedSize = (sizeBytes + 255) & ~255u;

        // Check for overflow
        if (alignedOffset + alignedSize > m_capacity)
        {
            return result; // Buffer full
        }

        result.offset = alignedOffset;
        result.size = alignedSize;
        result.cpuAddress = m_mappedPtr + alignedOffset;

        m_currentOffset = alignedOffset + alignedSize;
        m_metrics.usedThisFrame = m_currentOffset;
        m_metrics.allocationsThisFrame++;

        return result;
    }

    /**
     * @brief Bind a sub-range to a vertex shader constant buffer slot
     * @param context D3D11.1 device context
     * @param slot Shader register slot (b0, b1, etc.)
     * @param alloc The allocation to bind
     */
    void BindVS(ID3D11DeviceContext1* context, uint32_t slot, const CBAllocation& alloc) const
    {
        if (!context || !alloc.IsValid())
        {
            return;
        }

        ID3D11Buffer* buffers[] = {m_buffer.Get()};
        // Offsets and counts are in units of 16-byte constants
        UINT firstConstant = alloc.offset / 16;
        UINT numConstants = alloc.size / 16;
        context->VSSetConstantBuffers1(slot, 1, buffers, &firstConstant, &numConstants);
    }

    /**
     * @brief Bind a sub-range to a pixel shader constant buffer slot
     * @param context D3D11.1 device context
     * @param slot Shader register slot (b0, b1, etc.)
     * @param alloc The allocation to bind
     */
    void BindPS(ID3D11DeviceContext1* context, uint32_t slot, const CBAllocation& alloc) const
    {
        if (!context || !alloc.IsValid())
        {
            return;
        }

        ID3D11Buffer* buffers[] = {m_buffer.Get()};
        UINT firstConstant = alloc.offset / 16;
        UINT numConstants = alloc.size / 16;
        context->PSSetConstantBuffers1(slot, 1, buffers, &firstConstant, &numConstants);
    }

    /**
     * @brief Bind a sub-range to a geometry shader constant buffer slot
     */
    void BindGS(ID3D11DeviceContext1* context, uint32_t slot, const CBAllocation& alloc) const
    {
        if (!context || !alloc.IsValid())
        {
            return;
        }

        ID3D11Buffer* buffers[] = {m_buffer.Get()};
        UINT firstConstant = alloc.offset / 16;
        UINT numConstants = alloc.size / 16;
        context->GSSetConstantBuffers1(slot, 1, buffers, &firstConstant, &numConstants);
    }

    /**
     * @brief Bind a sub-range to a compute shader constant buffer slot
     */
    void BindCS(ID3D11DeviceContext1* context, uint32_t slot, const CBAllocation& alloc) const
    {
        if (!context || !alloc.IsValid())
        {
            return;
        }

        ID3D11Buffer* buffers[] = {m_buffer.Get()};
        UINT firstConstant = alloc.offset / 16;
        UINT numConstants = alloc.size / 16;
        context->CSSetConstantBuffers1(slot, 1, buffers, &firstConstant, &numConstants);
    }

    /** @brief Get the underlying D3D11 buffer */
    ID3D11Buffer* GetBuffer() const { return m_buffer.Get(); }

    /** @brief Check if the ring is initialized */
    bool IsInitialized() const { return m_initialized; }

    /** @brief Check if currently mapped for a frame */
    bool IsFrameActive() const { return m_frameMapped; }

    /** @brief Get current usage metrics */
    const CBRingMetrics& GetMetrics() const { return m_metrics; }

    /**
     * @brief Get a console-friendly status string
     * @return Formatted status with usage statistics
     */
    std::wstring Console_GetStatus() const
    {
        if (!m_initialized)
        {
            return L"[ConstantBufferRing] Not initialized";
        }

        float usagePercent = m_capacity > 0
            ? (static_cast<float>(m_metrics.usedThisFrame) / static_cast<float>(m_capacity)) * 100.0f
            : 0.0f;
        float peakPercent = m_capacity > 0
            ? (static_cast<float>(m_metrics.peakUsageBytes) / static_cast<float>(m_capacity)) * 100.0f
            : 0.0f;

        wchar_t buf[256];
        swprintf(buf, 256,
                 L"[ConstantBufferRing] Capacity: %u KB | Used: %u KB (%.1f%%) | "
                 L"Allocs: %u | Peak: %u KB (%.1f%%)",
                 m_capacity / 1024, m_metrics.usedThisFrame / 1024, usagePercent,
                 m_metrics.allocationsThisFrame, m_metrics.peakUsageBytes / 1024, peakPercent);
        return std::wstring(buf);
    }

private:
    ComPtr<ID3D11Buffer> m_buffer;
    ID3D11DeviceContext* m_context = nullptr; ///< Non-owning, valid only during frame
    uint8_t* m_mappedPtr = nullptr;
    uint32_t m_currentOffset = 0;
    uint32_t m_capacity = 0;
    bool m_initialized = false;
    bool m_frameMapped = false;
    CBRingMetrics m_metrics = {};
};

} // namespace Spark::Graphics

#endif // SPARK_PLATFORM_WINDOWS
