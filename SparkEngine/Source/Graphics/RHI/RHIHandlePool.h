/**
 * @file RHIHandlePool.h
 * @brief Typed handle pool with generation counters for RHI resources (Filament/bgfx-inspired)
 *
 * Replaces raw pointers for GPU resource management with typed handles that encode
 * a pool index + generation counter. When a resource is destroyed and its slot reused,
 * the generation counter increments, making stale handles detectable at O(1) cost.
 *
 * ## Usage
 * @code
 *   HandlePool<BufferResource, 4096> bufferPool;
 *   auto handle = bufferPool.Allocate(myBuffer);
 *
 *   // Lookup
 *   if (auto* buf = bufferPool.Get(handle)) { use(buf); }
 *
 *   // Free (increments generation — stale handles are now invalid)
 *   bufferPool.Free(handle);
 *
 *   // Stale handle detection
 *   assert(bufferPool.Get(handle) == nullptr);
 * @endcode
 *
 * @see RHITypes.h for handle type aliases
 *
 * @note **Intentional generic utility template** — pure C++ with no GPU
 *       dependency. There is no singleton to wire into a lifecycle; RHI
 *       device implementations (D3D11, Vulkan, ...) instantiate one per
 *       resource type as they are added. Exercised by
 *       `Tests/TestRHIHandlePool.cpp`.
 */

#pragma once

#include <cassert>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <vector>

namespace Spark::RHI
{

    /**
     * @brief A typed resource handle encoding pool index + generation counter.
     *
     * Layout (64-bit): [generation:24][index:24][tag:16]
     *   - index: Slot in the pool (up to 16M entries)
     *   - generation: Increments on reuse (stale handle detection)
     *   - tag: Compile-time resource type discriminator
     *
     * The Tag template parameter ensures handles for different resource types
     * are distinct types at compile time (e.g., BufferHandle vs TextureHandle).
     */
    template <typename Tag> struct TypedHandle
    {
        uint64_t value = 0;

        constexpr TypedHandle() = default;
        constexpr explicit TypedHandle(uint64_t v) : value(v) {}

        [[nodiscard]] constexpr bool IsValid() const { return value != 0; }
        [[nodiscard]] constexpr uint32_t GetIndex() const { return static_cast<uint32_t>((value >> 16) & 0xFFFFFF); }
        [[nodiscard]] constexpr uint32_t GetGeneration() const
        {
            return static_cast<uint32_t>((value >> 40) & 0xFFFFFF);
        }
        [[nodiscard]] constexpr uint16_t GetTag() const { return static_cast<uint16_t>(value & 0xFFFF); }

        constexpr bool operator==(const TypedHandle& other) const { return value == other.value; }
        constexpr bool operator!=(const TypedHandle& other) const { return value != other.value; }

        static constexpr TypedHandle Make(uint32_t index, uint32_t generation, uint16_t tag)
        {
            return TypedHandle{(static_cast<uint64_t>(generation & 0xFFFFFF) << 40) |
                               (static_cast<uint64_t>(index & 0xFFFFFF) << 16) | static_cast<uint64_t>(tag)};
        }
    };

    // Resource type tags for compile-time type safety
    struct BufferTag
    {
    };
    struct TextureTag
    {
    };
    struct ShaderTag
    {
    };
    struct SamplerTag
    {
    };
    struct PipelineTag
    {
    };

    using BufferHandle = TypedHandle<BufferTag>;
    using TextureHandle = TypedHandle<TextureTag>;
    using ShaderHandle = TypedHandle<ShaderTag>;
    using SamplerHandle = TypedHandle<SamplerTag>;
    using PipelineHandle = TypedHandle<PipelineTag>;

    /**
     * @brief Fixed-capacity pool that maps typed handles to resource pointers.
     *
     * Maintains a freelist of available slots. Each slot tracks its current
     * generation to detect stale handles after Free().
     *
     * @tparam T        Resource type stored in the pool
     * @tparam Tag      Handle tag type for compile-time type safety
     * @tparam Capacity Maximum number of simultaneous live entries
     */
    template <typename T, typename Tag, uint32_t Capacity = 4096> class HandlePool
    {
      public:
        using Handle = TypedHandle<Tag>;

        HandlePool() { Clear(); }

        /**
         * @brief Allocate a handle for a resource.
         * @param resource  Pointer to the resource (pool does NOT own it)
         * @return Valid handle, or invalid handle if pool is full
         */
        Handle Allocate(T* resource)
        {
            if (m_freeList.empty())
            {
                return Handle{}; // Pool full
            }

            uint32_t index = m_freeList.back();
            m_freeList.pop_back();

            auto& slot = m_slots[index];
            slot.resource = resource;
            slot.alive = true;

            ++m_count;

            return Handle::Make(index, slot.generation, TAG_VALUE);
        }

        /**
         * @brief Look up a resource by handle.
         * @return Pointer to resource, or nullptr if handle is invalid/stale
         */
        T* Get(Handle handle) const
        {
            if (!handle.IsValid())
                return nullptr;

            uint32_t index = handle.GetIndex();
            if (index >= Capacity)
                return nullptr;

            const auto& slot = m_slots[index];
            if (!slot.alive || slot.generation != handle.GetGeneration())
                return nullptr;

            return slot.resource;
        }

        /**
         * @brief Free a handle, incrementing the generation counter.
         *
         * After this call, any existing copies of the handle become stale
         * and Get() will return nullptr for them.
         *
         * @return true if the handle was valid and freed
         */
        bool Free(Handle handle)
        {
            if (!handle.IsValid())
                return false;

            uint32_t index = handle.GetIndex();
            if (index >= Capacity)
                return false;

            auto& slot = m_slots[index];
            if (!slot.alive || slot.generation != handle.GetGeneration())
                return false;

            slot.resource = nullptr;
            slot.alive = false;
            slot.generation++; // Stale handle detection

            m_freeList.push_back(index);
            --m_count;

            return true;
        }

        /// @brief Check if a handle is valid and points to a live resource.
        [[nodiscard]] bool IsValid(Handle handle) const { return Get(handle) != nullptr; }

        /// @brief Number of currently allocated entries.
        [[nodiscard]] uint32_t Count() const { return m_count; }

        /// @brief Maximum capacity.
        [[nodiscard]] constexpr uint32_t GetCapacity() const { return Capacity; }

        /// @brief Reset the pool, invalidating all outstanding handles.
        void Clear()
        {
            m_freeList.clear();
            m_freeList.reserve(Capacity);
            m_count = 0;

            for (uint32_t i = 0; i < Capacity; ++i)
            {
                m_slots[i].resource = nullptr;
                m_slots[i].generation = 1; // Start at 1 so generation 0 = invalid
                m_slots[i].alive = false;
                m_freeList.push_back(Capacity - 1 - i); // Reverse so pop_back gives lowest first
            }
        }

      private:
        static constexpr uint16_t TAG_VALUE = static_cast<uint16_t>(std::is_same_v<Tag, BufferTag>    ? 1
                                                                    : std::is_same_v<Tag, TextureTag> ? 2
                                                                    : std::is_same_v<Tag, ShaderTag>  ? 3
                                                                    : std::is_same_v<Tag, SamplerTag> ? 4
                                                                                                      : 5);

        struct Slot
        {
            T* resource = nullptr;
            uint32_t generation = 1;
            bool alive = false;
        };

        Slot m_slots[Capacity];
        std::vector<uint32_t> m_freeList;
        uint32_t m_count = 0;
    };

} // namespace Spark::RHI
