/**
 * @file TransientBufferAllocator.h
 * @brief Per-frame bump allocator for transient vertex/index data
 *
 * Provides fast, linear allocation of vertex and index buffer space
 * for per-frame dynamic geometry (particles, debug lines, UI, decals).
 * The entire buffer is reset each frame with a single Map/Discard.
 *
 * Inspired by bgfx's transient buffer system.
 *
 * Usage:
 * @code
 *   allocator.BeginFrame(device);
 *   auto alloc = allocator.AllocateVertices(vertexCount, sizeof(Vertex));
 *   memcpy(alloc.data, vertices, vertexCount * sizeof(Vertex));
 *   // ... bind alloc.buffer at alloc.offsetBytes for drawing ...
 *   allocator.EndFrame(device);
 * @endcode
 *
 * @see IRHIDevice, IRHIBuffer
 */

#pragma once

#include "RHIDevice.h"
#include <cstdint>
#include <cstring>
#include <memory>

namespace Spark::RHI
{

    /**
     * @brief Result of a transient buffer allocation.
     */
    struct TransientAllocation
    {
        IRHIBuffer* buffer = nullptr; ///< The underlying GPU buffer
        void* data = nullptr;         ///< CPU-writable pointer (valid until EndFrame)
        uint32_t offsetBytes = 0;     ///< Byte offset within the buffer
        uint32_t sizeBytes = 0;       ///< Byte size of this allocation
        bool valid = false;           ///< True if allocation succeeded
    };

    /**
     * @brief Per-frame linear allocator for transient vertex and index buffers.
     *
     * Maintains one large Dynamic buffer for vertices and one for indices.
     * Each frame the buffers are mapped with DISCARD, and allocations bump
     * a pointer forward. At EndFrame the buffers are unmapped.
     */
    class TransientBufferAllocator
    {
      public:
        /**
         * @param vertexBudget  Maximum vertex buffer bytes per frame (default 4 MB).
         * @param indexBudget   Maximum index buffer bytes per frame (default 2 MB).
         */
        explicit TransientBufferAllocator(uint32_t vertexBudget = 4 * 1024 * 1024,
                                          uint32_t indexBudget = 2 * 1024 * 1024)
            : m_vertexBudget(vertexBudget), m_indexBudget(indexBudget)
        {
        }

        ~TransientBufferAllocator() { Shutdown(nullptr); }

        /**
         * @brief Create the underlying GPU buffers. Call once at startup.
         * @return true on success.
         */
        bool Initialize(IRHIDevice* device)
        {
            if (!device)
                return false;

            RHIBufferDesc vbDesc;
            vbDesc.size = m_vertexBudget;
            vbDesc.stride = 0; // Stride set per-draw
            vbDesc.usage = RHIBufferUsage::Vertex;
            vbDesc.access = RHIBufferAccess::Dynamic;
            vbDesc.debugName = "TransientVertexBuffer";
            m_vertexBuffer = device->CreateBuffer(vbDesc);

            RHIBufferDesc ibDesc;
            ibDesc.size = m_indexBudget;
            ibDesc.stride = sizeof(uint32_t);
            ibDesc.usage = RHIBufferUsage::Index;
            ibDesc.access = RHIBufferAccess::Dynamic;
            ibDesc.debugName = "TransientIndexBuffer";
            m_indexBuffer = device->CreateBuffer(ibDesc);

            return m_vertexBuffer != nullptr && m_indexBuffer != nullptr;
        }

        /**
         * @brief Destroy GPU buffers. Call at shutdown.
         */
        // Intentional: device reserved for GPU resource cleanup in backends that need it
        void Shutdown([[maybe_unused]] IRHIDevice* device)
        {
            m_vertexBuffer.reset();
            m_indexBuffer.reset();
        }

        /**
         * @brief Begin a new frame. Maps buffers for writing. Call before any allocations.
         */
        void BeginFrame(IRHIDevice* device)
        {
            m_vertexOffset = 0;
            m_indexOffset = 0;

            if (device && m_vertexBuffer)
                m_vertexMapped = static_cast<uint8_t*>(device->MapBuffer(m_vertexBuffer.get()));
            if (device && m_indexBuffer)
                m_indexMapped = static_cast<uint8_t*>(device->MapBuffer(m_indexBuffer.get()));
        }

        /**
         * @brief End the frame. Unmaps buffers. Allocations from this frame become invalid.
         */
        void EndFrame(IRHIDevice* device)
        {
            if (device && m_vertexBuffer && m_vertexMapped)
                device->UnmapBuffer(m_vertexBuffer.get());
            if (device && m_indexBuffer && m_indexMapped)
                device->UnmapBuffer(m_indexBuffer.get());

            m_vertexMapped = nullptr;
            m_indexMapped = nullptr;
        }

        /**
         * @brief Allocate transient vertex buffer space.
         *
         * @param vertexCount Number of vertices.
         * @param vertexStride Byte size of one vertex.
         * @return Allocation with CPU-writable pointer. Check `valid` before use.
         */
        TransientAllocation AllocateVertices(uint32_t vertexCount, uint32_t vertexStride)
        {
            uint32_t sizeBytes = vertexCount * vertexStride;
            // Align to 16 bytes for SIMD friendliness
            uint32_t alignedOffset = (m_vertexOffset + 15) & ~15u;

            if (!m_vertexMapped || alignedOffset + sizeBytes > m_vertexBudget)
                return {}; // Over budget

            TransientAllocation alloc;
            alloc.buffer = m_vertexBuffer.get();
            alloc.data = m_vertexMapped + alignedOffset;
            alloc.offsetBytes = alignedOffset;
            alloc.sizeBytes = sizeBytes;
            alloc.valid = true;

            m_vertexOffset = alignedOffset + sizeBytes;
            return alloc;
        }

        /**
         * @brief Allocate transient index buffer space.
         *
         * @param indexCount Number of indices.
         * @param indexStride Byte size of one index (2 for uint16, 4 for uint32).
         * @return Allocation with CPU-writable pointer. Check `valid` before use.
         */
        TransientAllocation AllocateIndices(uint32_t indexCount, uint32_t indexStride = sizeof(uint32_t))
        {
            uint32_t sizeBytes = indexCount * indexStride;
            uint32_t alignedOffset = (m_indexOffset + 15) & ~15u;

            if (!m_indexMapped || alignedOffset + sizeBytes > m_indexBudget)
                return {};

            TransientAllocation alloc;
            alloc.buffer = m_indexBuffer.get();
            alloc.data = m_indexMapped + alignedOffset;
            alloc.offsetBytes = alignedOffset;
            alloc.sizeBytes = sizeBytes;
            alloc.valid = true;

            m_indexOffset = alignedOffset + sizeBytes;
            return alloc;
        }

        /**
         * @brief Check if there's room for a vertex allocation without actually allocating.
         */
        bool HasVertexSpace(uint32_t vertexCount, uint32_t vertexStride) const
        {
            uint32_t sizeBytes = vertexCount * vertexStride;
            uint32_t alignedOffset = (m_vertexOffset + 15) & ~15u;
            return m_vertexMapped && (alignedOffset + sizeBytes <= m_vertexBudget);
        }

        /**
         * @brief Check if there's room for an index allocation without actually allocating.
         */
        bool HasIndexSpace(uint32_t indexCount, uint32_t indexStride = sizeof(uint32_t)) const
        {
            uint32_t sizeBytes = indexCount * indexStride;
            uint32_t alignedOffset = (m_indexOffset + 15) & ~15u;
            return m_indexMapped && (alignedOffset + sizeBytes <= m_indexBudget);
        }

        /** @brief Bytes used so far this frame for vertices. */
        uint32_t GetVertexBytesUsed() const { return m_vertexOffset; }

        /** @brief Bytes used so far this frame for indices. */
        uint32_t GetIndexBytesUsed() const { return m_indexOffset; }

        /** @brief Total vertex buffer budget in bytes. */
        uint32_t GetVertexBudget() const { return m_vertexBudget; }

        /** @brief Total index buffer budget in bytes. */
        uint32_t GetIndexBudget() const { return m_indexBudget; }

      private:
        std::unique_ptr<IRHIBuffer> m_vertexBuffer;
        std::unique_ptr<IRHIBuffer> m_indexBuffer;

        uint8_t* m_vertexMapped = nullptr;
        uint8_t* m_indexMapped = nullptr;

        uint32_t m_vertexOffset = 0;
        uint32_t m_indexOffset = 0;

        uint32_t m_vertexBudget = 0;
        uint32_t m_indexBudget = 0;
    };

} // namespace Spark::RHI
