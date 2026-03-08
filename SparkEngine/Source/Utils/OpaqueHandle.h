/**
 * @file OpaqueHandle.h
 * @brief Type-safe opaque handle wrapper for subsystem-managed pointers
 * @author Spark Engine Team
 * @date 2025
 *
 * Replaces raw `void*` handles in ECS components with compile-time type safety.
 * Each handle type uses a unique tag so that, e.g., a PhysicsHandle cannot be
 * accidentally assigned to an AudioHandle field.
 *
 * @code
 *   PhysicsHandle h(rawPtr);
 *   auto* body = h.As<PhysicsBody>();  // type-safe cast
 *   if (h) { ... }                      // validity check
 * @endcode
 */

#pragma once
#include <cstddef>

namespace Spark
{

    /**
 * @brief Type-safe wrapper around a void* managed by a specific subsystem.
 *
 * @tparam Tag  A unique empty struct that distinguishes this handle type from
 *              others at compile time. Two OpaqueHandle instantiations with
 *              different Tag types are incompatible.
 */
    template <typename Tag> class OpaqueHandle
    {
      public:
        OpaqueHandle() = default;
        explicit OpaqueHandle(void* ptr) : m_ptr(ptr) {}

        /** @brief Returns true if the handle points to a valid object. */
        bool IsValid() const { return m_ptr != nullptr; }

        /** @brief Releases the handle (sets to null). Does NOT free memory. */
        void Reset() { m_ptr = nullptr; }

        /**
     * @brief Cast the opaque pointer to the expected concrete type.
     *
     * Only the owning subsystem should call this. The cast is unchecked
     * (equivalent to static_cast<T*>(void*)).
     */
        template <typename T> T* As() const { return static_cast<T*>(m_ptr); }

        /** @brief Access the raw void* pointer. Prefer As<T>() when the type is known. */
        void* Raw() const { return m_ptr; }

        /** @brief Boolean conversion — true if the handle is valid. */
        explicit operator bool() const { return IsValid(); }

        /** @brief Equality comparison. */
        bool operator==(const OpaqueHandle& other) const { return m_ptr == other.m_ptr; }
        bool operator!=(const OpaqueHandle& other) const { return m_ptr != other.m_ptr; }

        /** @brief Allow assigning nullptr to reset the handle. */
        OpaqueHandle& operator=(std::nullptr_t)
        {
            m_ptr = nullptr;
            return *this;
        }

      private:
        void* m_ptr = nullptr;
    };

    // ============================================================================
    // Tag types — one per subsystem that manages opaque handles
    // ============================================================================

    struct PhysicsHandleTag
    {
    };
    struct AudioHandleTag
    {
    };
    struct ParticleHandleTag
    {
    };
    struct AnimationHandleTag
    {
    };
    struct BehaviorTreeHandleTag
    {
    };
    struct NavQueryHandleTag
    {
    };

    // ============================================================================
    // Concrete handle type aliases used in ECS components
    // ============================================================================

    /** @brief Handle to a PhysicsBody managed by the PhysicsSystem. */
    using PhysicsHandle = OpaqueHandle<PhysicsHandleTag>;

    /** @brief Handle to an AudioSource managed by the AudioEngine. */
    using AudioHandle = OpaqueHandle<AudioHandleTag>;

    /** @brief Handle to a ParticleEmitter managed by the ParticleSystem. */
    using ParticleHandle = OpaqueHandle<ParticleHandleTag>;

    /** @brief Handle to an AnimationInstance managed by the AnimationUpdateSystem. */
    using AnimationHandle = OpaqueHandle<AnimationHandleTag>;

    /** @brief Handle to a BehaviorTree instance managed by the AISystem. */
    using BehaviorTreeHandle = OpaqueHandle<BehaviorTreeHandleTag>;

    /** @brief Handle to a NavMeshQuery managed by the AISystem. */
    using NavQueryHandle = OpaqueHandle<NavQueryHandleTag>;

} // namespace Spark
