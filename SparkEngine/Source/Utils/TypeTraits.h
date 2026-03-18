/**
 * @file TypeTraits.h
 * @brief C++23 concept shorthands and compile-time type utilities
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides reusable C++23 concept aliases and TypeList utilities that reduce
 * boilerplate across the engine. Concepts enforce template constraints at the
 * call site, improving error messages and documentation. TypeList enables
 * compile-time type enumeration for variant dispatch and component indexing.
 *
 * ## Usage
 * @code
 *   // Constrain a template to numeric types
 *   template <Spark::Arithmetic T>
 *   T Clamp(T value, T lo, T hi) { return std::clamp(value, lo, hi); }
 *
 *   // Compile-time index of a type in a list
 *   using Components = Spark::TypeList<Transform, Velocity, Health>;
 *   static_assert(Spark::TypeIndex<Velocity, Components>::value == 1);
 *
 *   // AlwaysFalse for static_assert in constexpr-if branches
 *   if constexpr (std::is_same_v<T, SomeType>) { ... }
 *   else { static_assert(Spark::AlwaysFalse<T>, "Unsupported type"); }
 * @endcode
 */

#pragma once

#include <concepts>
#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>

namespace Spark
{

    // =========================================================================
    // Core concepts
    // =========================================================================

    /**
     * @brief Satisfied by any arithmetic type (integral or floating-point).
     *
     * Equivalent to `std::is_arithmetic_v<T>` but usable as a concept constraint
     * directly in template parameter lists and `requires` clauses.
     *
     * @tparam T  Type to test.
     */
    template <typename T>
    concept Arithmetic = std::is_arithmetic_v<T>;

    /**
     * @brief Satisfied by any scoped or unscoped enum type.
     *
     * @tparam T  Type to test.
     */
    template <typename T>
    concept Enum = std::is_enum_v<T>;

    /**
     * @brief Satisfied by `std::string`, `std::string_view`, and `const char*`.
     *
     * @tparam T  Type to test.
     */
    template <typename T>
    concept StringLike =
        std::is_same_v<std::decay_t<T>, std::string> || std::is_same_v<std::decay_t<T>, std::string_view> ||
        std::is_same_v<std::decay_t<T>, char*> || std::is_same_v<std::decay_t<T>, const char*>;

    /**
     * @brief Satisfied when `F` is callable with `Args...` and returns `Ret`.
     *
     * Use `Callable<F>` (omitting `Ret`) when the return type does not matter;
     * use `Callable<F, Ret, Args...>` for strict return-type checking.
     *
     * @tparam F     Callable type (function pointer, lambda, functor).
     * @tparam Ret   Expected return type (default: void — any return type allowed).
     * @tparam Args  Argument types.
     */
    template <typename F, typename Ret = void, typename... Args>
    concept Callable = requires(F f, Args... args) {
        { f(args...) } -> std::convertible_to<Ret>;
    };

    /**
     * @brief Satisfied when `F` is callable with `Args...` (any return type).
     *
     * Convenience alias that does not constrain the return type.
     *
     * @tparam F     Callable type.
     * @tparam Args  Argument types.
     */
    template <typename F, typename... Args>
    concept Invocable = std::invocable<F, Args...>;

    /**
     * @brief Satisfied by types that can be trivially copied (plain-old-data-like).
     *
     * Useful for serialization utilities that use `memcpy` for fast byte-level copying.
     *
     * @tparam T  Type to test.
     */
    template <typename T>
    concept TriviallyCopyable = std::is_trivially_copyable_v<T>;

    /**
     * @brief Satisfied by types that support equality comparison via `==`.
     *
     * @tparam T  Type to test.
     */
    template <typename T>
    concept EqualityComparable = std::equality_comparable<T>;

    /**
     * @brief Satisfied by types that support `<`, `>`, `<=`, `>=` comparisons.
     *
     * @tparam T  Type to test.
     */
    template <typename T>
    concept Comparable = std::totally_ordered<T>;

    /**
     * @brief Satisfied by pointer types (raw pointers, not smart pointers).
     *
     * @tparam T  Type to test.
     */
    template <typename T>
    concept Pointer = std::is_pointer_v<T>;

    /**
     * @brief Satisfied when T is default-constructible.
     *
     * @tparam T  Type to test.
     */
    template <typename T>
    concept DefaultConstructible = std::default_initializable<T>;

    // =========================================================================
    // TypeList — compile-time list of types
    // =========================================================================

    /**
     * @brief Compile-time ordered list of types.
     *
     * Used for component/variant dispatch tables, static type enumeration, and
     * index-based type lookup. The list itself carries no runtime data.
     *
     * @tparam Ts  Types in the list (may be empty).
     *
     * ## Usage
     * @code
     *   using MyTypes = Spark::TypeList<int, float, std::string>;
     *   constexpr size_t n = MyTypes::size; // 3
     * @endcode
     */
    template <typename... Ts> struct TypeList
    {
        /// Number of types in the list.
        static constexpr size_t size = sizeof...(Ts);
    };

    // =========================================================================
    // TypeIndex — compile-time index lookup
    // =========================================================================

    namespace Detail
    {
        template <typename Needle, typename List, size_t I> struct TypeIndexImpl;

        template <typename Needle, typename Head, typename... Tail, size_t I>
        struct TypeIndexImpl<Needle, TypeList<Head, Tail...>, I>
            : std::conditional_t<std::is_same_v<Needle, Head>, std::integral_constant<size_t, I>,
                                 TypeIndexImpl<Needle, TypeList<Tail...>, I + 1>>
        {
        };

        // Not found — intentionally left incomplete to produce a clear linker error.
        template <typename Needle, size_t I> struct TypeIndexImpl<Needle, TypeList<>, I>;
    } // namespace Detail

    /**
     * @brief Retrieve the zero-based index of `T` within a `TypeList`.
     *
     * Produces a compile-time error if `T` is not present in `List`.
     *
     * @tparam T     Type to locate.
     * @tparam List  A `TypeList<...>` to search.
     *
     * @code
     *   using Components = Spark::TypeList<Transform, Velocity, Health>;
     *   constexpr size_t i = Spark::TypeIndex<Velocity, Components>::value; // 1
     * @endcode
     */
    template <typename T, typename List> using TypeIndex = Detail::TypeIndexImpl<T, List, 0>;

    // =========================================================================
    // TypeAt — retrieve the N-th type from a TypeList
    // =========================================================================

    namespace Detail
    {
        template <size_t N, typename List> struct TypeAtImpl;

        template <size_t N, typename Head, typename... Tail>
        struct TypeAtImpl<N, TypeList<Head, Tail...>> : TypeAtImpl<N - 1, TypeList<Tail...>>
        {
        };

        template <typename Head, typename... Tail> struct TypeAtImpl<0, TypeList<Head, Tail...>>
        {
            using type = Head;
        };
    } // namespace Detail

    /**
     * @brief Retrieve the type at index `N` from a `TypeList`.
     *
     * @tparam N     Zero-based index (must be < List::size).
     * @tparam List  A `TypeList<...>` to index into.
     *
     * @code
     *   using Components = Spark::TypeList<Transform, Velocity, Health>;
     *   using Second = Spark::TypeAt<1, Components>::type; // Velocity
     * @endcode
     */
    template <size_t N, typename List> using TypeAt = Detail::TypeAtImpl<N, List>;

    // =========================================================================
    // AlwaysFalse — helper for static_assert in constexpr-if dead branches
    // =========================================================================

    /**
     * @brief Type-dependent false constant for `static_assert` in unreachable branches.
     *
     * A plain `static_assert(false)` fires even in dead `if constexpr` branches
     * because it is not type-dependent. `AlwaysFalse<T>` defers evaluation to
     * instantiation time, which only happens when the branch is actually taken.
     *
     * @tparam T  Any type (usually the template parameter being tested).
     *
     * @code
     *   template <typename T>
     *   void Serialize(T value) {
     *       if constexpr (std::is_integral_v<T>) { WriteInt(value); }
     *       else if constexpr (std::is_floating_point_v<T>) { WriteFloat(value); }
     *       else { static_assert(Spark::AlwaysFalse<T>, "Unsupported type for Serialize"); }
     *   }
     * @endcode
     */
    template <typename T> inline constexpr bool AlwaysFalse = false;

    // =========================================================================
    // RemoveCVRef — convenience alias for std::remove_cvref_t
    // =========================================================================

    /**
     * @brief Strips const, volatile, and reference qualifiers from a type.
     *
     * Shorthand for `std::remove_cvref_t<T>`.
     *
     * @tparam T  Type to strip.
     */
    template <typename T> using RemoveCVRef = std::remove_cvref_t<T>;

    // =========================================================================
    // IsOneOf — compile-time membership test
    // =========================================================================

    /**
     * @brief True if `T` is the same type as any of `Ts...`.
     *
     * @tparam T   Type to search for.
     * @tparam Ts  Candidate types.
     *
     * @code
     *   static_assert(Spark::IsOneOf<int, float, int, double>); // passes
     * @endcode
     */
    template <typename T, typename... Ts> inline constexpr bool IsOneOf = (std::is_same_v<T, Ts> || ...);

} // namespace Spark
