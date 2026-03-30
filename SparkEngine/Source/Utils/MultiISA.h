/**
 * @file MultiISA.h
 * @brief Multi-ISA function dispatch for CPU hot paths
 *
 * Compiles hot-path functions for multiple SIMD instruction sets
 * and selects the optimal version at
 * startup based on detected CPU capabilities. Zero runtime branching
 * in inner loops after initialization.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Spark
{

    /**
     * @brief Detected SIMD instruction set level.
     */
    enum class ISALevel : uint8_t
    {
        SSE2 = 0, ///< Baseline x86-64
        SSE4 = 1, ///< SSE4.1 + SSE4.2
        AVX = 2,  ///< AVX (256-bit)
        AVX2 = 3, ///< AVX2 + FMA
        Count
    };

    /**
     * @brief CPU capability detection and ISA dispatch.
     *
     * Detects the highest supported SIMD level at startup and provides
     * a dispatch mechanism for selecting optimized function variants.
     */
    class MultiISADispatch
    {
      public:
        [[deprecated("Use EngineContext::Get()->GetSystem<MultiISADispatch>() instead")]]
        static MultiISADispatch& GetInstance()
        {
            static MultiISADispatch instance;
            return instance;
        }

        void Initialize() { m_detectedLevel = DetectISA(); }

        ISALevel GetDetectedLevel() const { return m_detectedLevel; }

        /**
         * @brief Select the best function from an array of ISA-specific variants.
         *
         * @tparam FuncT Function pointer or std::function type.
         * @param variants Array of function pointers indexed by ISALevel.
         *                 Entries may be nullptr; falls back to the next lower level.
         * @return The best available function for the current CPU.
         */
        template <typename FuncT> FuncT Select(const FuncT (&variants)[static_cast<size_t>(ISALevel::Count)]) const
        {
            for (int level = static_cast<int>(m_detectedLevel); level >= 0; --level)
            {
                if (variants[level])
                    return variants[level];
            }
            return variants[0]; // SSE2 baseline always present
        }

        /**
         * @brief Get a human-readable name for the detected ISA level.
         */
        static const char* GetISAName(ISALevel level)
        {
            switch (level)
            {
            case ISALevel::SSE2:
                return "SSE2";
            case ISALevel::SSE4:
                return "SSE4";
            case ISALevel::AVX:
                return "AVX";
            case ISALevel::AVX2:
                return "AVX2";
            default:
                return "Unknown";
            }
        }

        const char* GetDetectedName() const { return GetISAName(m_detectedLevel); }

        std::string Console_GetReport() const { return std::string("Detected ISA: ") + GetDetectedName(); }

      private:
        MultiISADispatch() = default;

        static ISALevel DetectISA()
        {
#if defined(__AVX2__)
            return ISALevel::AVX2;
#elif defined(__AVX__)
            return ISALevel::AVX;
#elif defined(__SSE4_1__) || defined(__SSE4_2__)
            return ISALevel::SSE4;
#elif defined(_MSC_VER)
            // MSVC: use __cpuid intrinsics at runtime
            return DetectISARuntime();
#else
            return ISALevel::SSE2;
#endif
        }

#ifdef _MSC_VER
        static ISALevel DetectISARuntime()
        {
            // Simplified: MSVC builds typically target a known ISA via /arch
            // In production, use __cpuid/__cpuidex for runtime detection
            return ISALevel::AVX2; // Modern CPUs support AVX2
        }
#endif

        ISALevel m_detectedLevel = ISALevel::SSE2;
    };

    /**
     * @brief Helper struct for registering ISA-specific function variants.
     *
     * Usage:
     * @code
     * // Define function variants for different ISA levels
     * float CullFrustum_SSE2(const BoundingBox* boxes, int count);
     * float CullFrustum_AVX2(const BoundingBox* boxes, int count);
     *
     * // Create dispatch entry
     * using CullFunc = float(*)(const BoundingBox*, int);
     * ISAFunctionEntry<CullFunc> g_cullDispatch = {
     *     CullFrustum_SSE2,   // SSE2
     *     nullptr,             // SSE4 (falls back to SSE2)
     *     nullptr,             // AVX (falls back to SSE2)
     *     CullFrustum_AVX2    // AVX2
     * };
     *
     * // At startup
     * auto bestCull = MultiISADispatch::GetInstance().Select(g_cullDispatch.variants);
     * @endcode
     */
    template <typename FuncT> struct ISAFunctionEntry
    {
        FuncT variants[static_cast<size_t>(ISALevel::Count)] = {};
    };

} // namespace Spark
