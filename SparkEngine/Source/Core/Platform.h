/**
 * @file Platform.h
 * @brief Cross-platform type definitions and compatibility layer
 * @author Spark Engine Team
 * @date 2026
 *
 * This header provides platform-agnostic type definitions so that
 * engine code can compile on Windows (MSVC), Linux (GCC/Clang), and macOS.
 * On Windows, these map directly to the native Win32/COM types.
 * On other platforms, lightweight stubs are provided via sub-headers:
 *   - PlatformTypes.h          — Win32 type aliases and macros
 *   - PlatformAudioStubs.h     — XAudio2 and XInput stubs
 *   - PlatformDirectXMathStubs.h — DirectXMath vector/matrix stubs
 *   - PlatformD3DStubs.h       — D3D11/DXGI type stubs and ComPtr
 */

#pragma once

// ============================================================================
// Compiler Detection
// Defines SPARK_COMPILER_* macros before any platform-specific includes.
// ============================================================================

// --- MSVC (Visual Studio) ---
#if defined(_MSC_VER)
#define SPARK_COMPILER_MSVC 1
#define SPARK_COMPILER_VERSION _MSC_VER
// Visual Studio release year mapping (_MSC_VER ranges):
//   VS 2017 (v141) : 1910-1916
//   VS 2019 (v142) : 1920-1929
//   VS 2022 (v143) : 1930-1939
//   VS 2026 (v144) : 1940+
#if _MSC_VER >= 1950
#define SPARK_COMPILER_MSVC_2026_PLUS 1
#elif _MSC_VER >= 1940
#define SPARK_COMPILER_MSVC_2026 1
#elif _MSC_VER >= 1930
#define SPARK_COMPILER_MSVC_2022 1
#elif _MSC_VER >= 1920
#define SPARK_COMPILER_MSVC_2019 1
#elif _MSC_VER >= 1910
#define SPARK_COMPILER_MSVC_2017 1
#endif

// --- Apple Clang (must be checked before generic Clang) ---
#elif defined(__clang__) && defined(__apple_build_version__)
#define SPARK_COMPILER_APPLE_CLANG 1
#define SPARK_COMPILER_CLANG 1
#define SPARK_COMPILER_VERSION (__clang_major__ * 10000 + __clang_minor__ * 100 + __clang_patchlevel__)

// --- Intel oneAPI DPC++/C++ (ICPX) - Clang-compatible ---
#elif defined(__INTEL_LLVM_COMPILER)
#define SPARK_COMPILER_INTEL_LLVM 1
#define SPARK_COMPILER_CLANG 1 // ICPX is Clang-compatible
#define SPARK_COMPILER_VERSION __INTEL_LLVM_COMPILER

// --- Intel Classic C++ Compiler (ICC) ---
#elif defined(__INTEL_COMPILER)
#define SPARK_COMPILER_INTEL_CLASSIC 1
#define SPARK_COMPILER_VERSION __INTEL_COMPILER

// --- Generic LLVM Clang ---
#elif defined(__clang__)
#define SPARK_COMPILER_CLANG 1
#define SPARK_COMPILER_VERSION (__clang_major__ * 10000 + __clang_minor__ * 100 + __clang_patchlevel__)

// --- GCC ---
#elif defined(__GNUC__)
#define SPARK_COMPILER_GCC 1
#define SPARK_COMPILER_VERSION (__GNUC__ * 10000 + __GNUC_MINOR__ * 100 + __GNUC_PATCHLEVEL__)

#else
#define SPARK_COMPILER_UNKNOWN 1
#define SPARK_COMPILER_VERSION 0
#endif

// Convenience: true if any Clang-family compiler (Clang, Apple Clang, Intel LLVM)
#if defined(SPARK_COMPILER_CLANG) || defined(SPARK_COMPILER_APPLE_CLANG) || defined(SPARK_COMPILER_INTEL_LLVM)
#define SPARK_COMPILER_CLANG_FAMILY 1
#endif

// ============================================================================
// C++ Standard Detection
// Detects the active C++ standard level and defines SPARK_CPP_* macros.
// ============================================================================

// GCC 13/14 with -std=c++23 may report __cplusplus as 202100L (draft value)
// rather than the final 202302L. We treat >= 202100L as C++23 since that
// intermediate value is only emitted when the compiler is in C++23 mode.
#if defined(__cplusplus)
#if __cplusplus >= 202612L
#define SPARK_CPP26 1
#define SPARK_CPP23 1
#define SPARK_CPP20 1
#elif __cplusplus >= 202100L
#define SPARK_CPP23 1
#define SPARK_CPP20 1
#elif __cplusplus >= 202002L
#define SPARK_CPP20 1
#endif
#endif

// MSVC reports __cplusplus correctly only with /Zc:__cplusplus (which we enable).
// Double-check via _MSVC_LANG as a fallback for third-party code that omits the flag.
#if defined(_MSVC_LANG) && !defined(SPARK_CPP23)
#if _MSVC_LANG >= 202612L
#define SPARK_CPP26 1
#define SPARK_CPP23 1
#define SPARK_CPP20 1
#elif _MSVC_LANG >= 202100L
#define SPARK_CPP23 1
#define SPARK_CPP20 1
#elif _MSVC_LANG >= 202002L && !defined(SPARK_CPP20)
#define SPARK_CPP20 1
#endif
#endif

// Minimum standard check — engine requires C++23
#if !defined(SPARK_CPP23)
#error "SparkEngine requires a C++23-capable compiler. Please use GCC 13+, Clang 17+, or MSVC 19.36+ (VS 2022 17.6+)."
#endif

// ============================================================================
// C++ Feature Detection (library features gated behind __cpp_lib_* / __has_include)
// These allow gradual adoption of C++23 library features and forward-compat
// with C++26 features that some compilers already ship.
//
// <version> must be pulled in first so that all feature-test macros are
// populated before we gate on them — otherwise the set of SPARK_HAS_* macros
// (and therefore the visibility of polyfills like std::expected below) would
// depend on which standard headers each TU happened to include earlier,
// producing ODR violations across the program under LTO.
// ============================================================================

#include <version>

// C++23 library features
#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202211L
#define SPARK_HAS_EXPECTED 1
#endif

#if defined(__cpp_lib_print) && __cpp_lib_print >= 202207L
#define SPARK_HAS_PRINT 1
#endif

#if defined(__cpp_lib_flat_map) && __cpp_lib_flat_map >= 202207L
#define SPARK_HAS_FLAT_MAP 1
#endif

#if defined(__cpp_lib_stacktrace) && __cpp_lib_stacktrace >= 202011L
#define SPARK_HAS_STACKTRACE 1
#endif

#if defined(__cpp_lib_mdspan) && __cpp_lib_mdspan >= 202207L
#define SPARK_HAS_MDSPAN 1
#endif

#if defined(__cpp_lib_generator) && __cpp_lib_generator >= 202207L
#define SPARK_HAS_GENERATOR 1
#endif

// C++23 language features
#if defined(__cpp_if_consteval) && __cpp_if_consteval >= 202106L
#define SPARK_HAS_IF_CONSTEVAL 1
#endif

#if defined(__cpp_deducing_this) && __cpp_deducing_this >= 202110L
#define SPARK_HAS_DEDUCING_THIS 1
#endif

#if defined(__cpp_multidimensional_subscript) && __cpp_multidimensional_subscript >= 202211L
#define SPARK_HAS_MULTIDIM_SUBSCRIPT 1
#endif

#if defined(__cpp_size_t_suffix) && __cpp_size_t_suffix >= 202011L
#define SPARK_HAS_SIZE_T_SUFFIX 1
#endif

// C++26 forward-compatibility — features that some compilers already ship
#if defined(__cpp_lib_optional_range_support) && __cpp_lib_optional_range_support >= 202406L
#define SPARK_HAS_OPTIONAL_RANGE 1
#endif

#if defined(__cpp_contracts) && __cpp_contracts >= 202411L
#define SPARK_HAS_CONTRACTS 1
#endif

#if defined(__cpp_pack_indexing) && __cpp_pack_indexing >= 202311L
#define SPARK_HAS_PACK_INDEXING 1
#endif

#if defined(__cpp_reflection) && __cpp_reflection >= 202306L
#define SPARK_HAS_REFLECTION 1
#endif

// ============================================================================
// Platform Detection
// ============================================================================

#if defined(_WIN32) || defined(_WIN64)
#define SPARK_PLATFORM_WINDOWS 1
#elif defined(__linux__)
#define SPARK_PLATFORM_LINUX 1
#elif defined(__APPLE__)
#define SPARK_PLATFORM_MACOS 1
#endif

// ============================================================================
// Platform-Agnostic Type Aliases
// ============================================================================

namespace Spark
{

    /// Platform-agnostic window handle. Use in public APIs instead of HWND.
    using NativeWindowHandle = void*; // compatible with HWND on Windows

} // namespace Spark

// ============================================================================
// std::expected polyfill (for compilers where __cpp_lib_expected is absent)
// ============================================================================

#ifdef SPARK_HAS_EXPECTED
#include <expected>
#else

namespace std
{
    template <typename E> class unexpected
    {
      public:
        explicit unexpected(E e) : m_error(static_cast<E&&>(e)) {}
        const E& error() const& { return m_error; }
        E& error() & { return m_error; }

      private:
        E m_error;
    };

    template <typename T, typename E> class expected
    {
      public:
        expected(T val) : m_hasValue(true) { new (&m_val) T(static_cast<T&&>(val)); }                  // NOLINT
        expected(unexpected<E> u) : m_hasValue(false) { new (&m_err) E(static_cast<E&&>(u.error())); } // NOLINT
        ~expected()
        {
            if (m_hasValue)
                m_val.~T();
            else
                m_err.~E();
        }
        expected(const expected& o) : m_hasValue(o.m_hasValue)
        {
            if (m_hasValue)
                new (&m_val) T(o.m_val);
            else
                new (&m_err) E(o.m_err);
        }
        expected(expected&& o) noexcept : m_hasValue(o.m_hasValue)
        {
            if (m_hasValue)
                new (&m_val) T(static_cast<T&&>(o.m_val));
            else
                new (&m_err) E(static_cast<E&&>(o.m_err));
        }
        expected& operator=(const expected&) = delete;
        expected& operator=(expected&&) = delete;

        explicit operator bool() const { return m_hasValue; }
        bool has_value() const { return m_hasValue; }

        T& value() { return m_val; }
        const T& value() const { return m_val; }
        T& operator*() { return m_val; }
        const T& operator*() const { return m_val; }
        T* operator->() { return &m_val; }
        const T* operator->() const { return &m_val; }

        const E& error() const { return m_err; }

      private:
        bool m_hasValue;
        union
        {
            T m_val;
            E m_err;
        };
    };

    // Specialization for expected<void, E>
    template <typename E> class expected<void, E>
    {
      public:
        expected() : m_hasError(false) {}                                                   // success
        expected(unexpected<E> u) : m_hasError(true), m_err(static_cast<E&&>(u.error())) {} // NOLINT

        explicit operator bool() const { return !m_hasError; }
        bool has_value() const { return !m_hasError; }
        void value() const {}
        const E& error() const { return m_err; }

      private:
        bool m_hasError;
        E m_err{};
    };
} // namespace std
#endif // SPARK_HAS_EXPECTED

// ============================================================================
// Windows: Use native types
// ============================================================================

#ifdef SPARK_PLATFORM_WINDOWS

#include <windows.h>
#include <DirectXMath.h>
using namespace DirectX;

#else // !SPARK_PLATFORM_WINDOWS

// ============================================================================
// Non-Windows: Provide compatible type stubs via sub-headers
// ============================================================================

#include "PlatformTypes.h"
#include "PlatformAudioStubs.h"
#include "PlatformDirectXMathStubs.h"
#include "PlatformD3DStubs.h"

// Bring DirectX stub types into global scope so existing code using bare
// XMFLOAT3, XMVECTOR, etc. compiles without DirectX:: qualification.
using namespace DirectX;

#endif // !SPARK_PLATFORM_WINDOWS
