/**
 * @file NonNull.h
 * @brief Lightweight non-null pointer wrapper with debug-mode enforcement
 *
 * Spark::NonNull<T*> documents and enforces non-null intent at the type level.
 * In debug builds, construction from nullptr triggers an assertion. In release
 * builds, the wrapper compiles down to a raw pointer with zero overhead.
 *
 * This is a non-owning wrapper — use it with raw observation pointers per
 * project convention (std::unique_ptr for ownership).
 *
 * Usage:
 *   void ProcessEntity(Spark::NonNull<Entity*> entity)
 *   {
 *       entity->Update();  // guaranteed non-null in debug
 *   }
 */

#pragma once

#include "Utils/Assert.h"

#include <cstddef>
#include <type_traits>

namespace Spark
{

    /**
 * @brief Non-null pointer wrapper with debug-mode null checking
 *
 * @tparam T Pointed-to type (NonNull<T*> wraps a T*)
 *
 * Implicitly convertible to/from raw pointers for zero-friction adoption.
 * Construction from nullptr_t is a compile error. Construction from a raw
 * pointer that happens to be null fires a debug assertion.
 */
    template <typename T> class NonNull
    {
      public:
        static_assert(std::is_pointer_v<T>, "NonNull<T> requires T to be a pointer type (e.g. NonNull<Foo*>)");

        using element_type = std::remove_pointer_t<T>;

        /// Construct from a raw pointer. Debug-asserts non-null.
        constexpr NonNull(T ptr) : m_ptr(ptr) // NOLINT(google-explicit-constructor)
        {
            ASSERT_MSG(m_ptr != nullptr, "NonNull constructed with nullptr");
        }

        /// Deleted: constructing from nullptr_t is a compile error.
        NonNull(std::nullptr_t) = delete;

        /// Implicit conversion to raw pointer.
        constexpr operator T() const { return m_ptr; } // NOLINT(google-explicit-constructor)

        /// Dereference.
        constexpr element_type& operator*() const { return *m_ptr; }

        /// Member access.
        constexpr T operator->() const { return m_ptr; }

        /// Explicit accessor.
        constexpr T Get() const { return m_ptr; }

      private:
        T m_ptr;
    };

    /// Deduction guide: NonNull(Foo*) → NonNull<Foo*>
    template <typename T> NonNull(T) -> NonNull<T>;

} // namespace Spark
