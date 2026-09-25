/**
 * @file MultiISA.h
 * @brief Multi-ISA function dispatch for CPU hot paths
 *
 * Compiles hot-path functions for multiple SIMD instruction sets
 * and selects the optimal version at
 * startup based on detected CPU capabilities. Zero runtime branching
 * in inner loops after initialization.
 *
 * Also owns the stable-v1 CPU floor check (BLD-100, owner decision OD-04):
 * distributed x86-64 builds target x86-64 with SSE4.2 and POPCNT
 * (x86-64-v2), cmake/SparkCpuFloor.cmake keeps above-floor code out of the
 * build, and the engine and editor entry points call
 * DescribeStableCpuFloorFailure() before any other initialization so an
 * unsupported CPU gets a clear message instead of an illegal-instruction
 * crash.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define SPARK_MULTIISA_X86 1
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#else
#define SPARK_MULTIISA_X86 0
#endif

namespace Spark
{

    /**
     * @brief x86 instruction-set features reported by CPUID and enabled by the OS.
     *
     * AVX, AVX2, FMA and F16C are reported usable only when the OS also saves
     * the YMM state (OSXSAVE + XCR0 bits 1 and 2); a CPU that has them but runs
     * under an OS that does not enable them faults on the first VEX instruction.
     */
    struct CpuFeatures
    {
        bool isX86 = false; ///< False on non-x86 hosts, where the x86 floor does not apply
        bool sse2 = false;
        bool sse3 = false;
        bool ssse3 = false;
        bool sse41 = false;
        bool sse42 = false;
        bool popcnt = false;
        bool avx = false;
        bool avx2 = false;
        bool fma = false;
        bool f16c = false;
        bool bmi1 = false;
        bool bmi2 = false;
        bool lzcnt = false;
    };

#if SPARK_MULTIISA_X86
    namespace Detail
    {
        /// Runs CPUID for @p leaf / @p subleaf; returns false when the leaf is above the CPU's maximum.
        inline bool QueryCpuid(uint32_t leaf, uint32_t subleaf, uint32_t (&regs)[4]) noexcept
        {
#if defined(_MSC_VER) && !defined(__clang__)
            int info[4] = {};
            __cpuid(info, static_cast<int>(leaf & 0x80000000u));
            if (static_cast<uint32_t>(info[0]) < leaf)
            {
                return false;
            }
            __cpuidex(info, static_cast<int>(leaf), static_cast<int>(subleaf));
            for (int i = 0; i < 4; ++i)
            {
                regs[i] = static_cast<uint32_t>(info[i]);
            }
            return true;
#else
            return __get_cpuid_count(leaf, subleaf, &regs[0], &regs[1], &regs[2], &regs[3]) != 0;
#endif
        }

        /// Reads XCR0. Only valid when CPUID reports OSXSAVE.
        inline uint64_t ReadXcr0() noexcept
        {
#if defined(_MSC_VER) && !defined(__clang__)
            return _xgetbv(0);
#else
            // Inline asm rather than _xgetbv(): the intrinsic requires compiling
            // the caller with -mxsave, which is not part of the SSE4.2 floor.
            uint32_t low = 0;
            uint32_t high = 0;
            __asm__ volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
            return (static_cast<uint64_t>(high) << 32) | low;
#endif
        }
    } // namespace Detail
#endif

    /**
     * @brief Detect the running CPU's instruction-set features with CPUID/XGETBV.
     * @return Detected features; all false (and isX86 false) on non-x86 hosts.
     */
    inline CpuFeatures DetectCpuFeatures() noexcept
    {
        CpuFeatures features;
#if SPARK_MULTIISA_X86
        features.isX86 = true;
        uint32_t regs[4] = {}; // EAX, EBX, ECX, EDX
        if (!Detail::QueryCpuid(1, 0, regs))
        {
            return features;
        }
        const uint32_t ecx1 = regs[2];
        const uint32_t edx1 = regs[3];
        features.sse2 = (edx1 & (1u << 26)) != 0;
        features.sse3 = (ecx1 & (1u << 0)) != 0;
        features.ssse3 = (ecx1 & (1u << 9)) != 0;
        features.sse41 = (ecx1 & (1u << 19)) != 0;
        features.sse42 = (ecx1 & (1u << 20)) != 0;
        features.popcnt = (ecx1 & (1u << 23)) != 0;

        const bool osSavesYmm = (ecx1 & (1u << 27)) != 0 && (Detail::ReadXcr0() & 0x6u) == 0x6u;
        features.avx = osSavesYmm && (ecx1 & (1u << 28)) != 0;
        features.fma = features.avx && (ecx1 & (1u << 12)) != 0;
        features.f16c = features.avx && (ecx1 & (1u << 29)) != 0;

        if (Detail::QueryCpuid(7, 0, regs))
        {
            features.bmi1 = (regs[1] & (1u << 3)) != 0;
            features.avx2 = features.avx && (regs[1] & (1u << 5)) != 0;
            features.bmi2 = (regs[1] & (1u << 8)) != 0;
        }
        if (Detail::QueryCpuid(0x80000001u, 0, regs))
        {
            features.lzcnt = (regs[2] & (1u << 5)) != 0;
        }
#endif
        return features;
    }

    /**
     * @brief Check @p features against the stable-v1 x86-64 CPU floor (OD-04).
     *
     * The floor is x86-64-v2: SSE2, SSE3, SSSE3, SSE4.1, SSE4.2 and POPCNT.
     * Non-x86 hosts always pass because the x86 floor does not apply to them.
     *
     * @param features Features from DetectCpuFeatures().
     * @return An empty string when the floor is met, otherwise a user-facing
     *         message naming the missing features.
     */
    inline std::string DescribeStableCpuFloorFailure(const CpuFeatures& features)
    {
        if (!features.isX86)
        {
            return {};
        }
        std::string missing;
        const auto require = [&missing](bool present, const char* name)
        {
            if (!present)
            {
                missing += missing.empty() ? name : std::string(", ") + name;
            }
        };
        require(features.sse2, "SSE2");
        require(features.sse3, "SSE3");
        require(features.ssse3, "SSSE3");
        require(features.sse41, "SSE4.1");
        require(features.sse42, "SSE4.2");
        require(features.popcnt, "POPCNT");
        if (missing.empty())
        {
            return {};
        }
        return "This processor is not supported. SparkEngine requires an x86-64 CPU with SSE4.2 and POPCNT "
               "(x86-64-v2, e.g. Intel Nehalem / AMD Bulldozer or newer). Missing: " +
               missing;
    }

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
            return variants[static_cast<size_t>(SelectLevel(variants))];
        }

        /**
         * @brief ISA level of the variant Select() returns for @p variants.
         *
         * This is the kernel that actually runs, which is lower than
         * GetDetectedLevel() whenever the higher variants were not compiled in
         * (floor builds do not define __AVX2__, for example).
         *
         * @param variants Array of function pointers indexed by ISALevel.
         * @return The highest populated level not above the detected level;
         *         ISALevel::SSE2 when only the baseline is populated.
         */
        template <typename FuncT>
        ISALevel SelectLevel(const FuncT (&variants)[static_cast<size_t>(ISALevel::Count)]) const
        {
            for (int level = static_cast<int>(m_detectedLevel); level >= 0; --level)
            {
                if (variants[level])
                    return static_cast<ISALevel>(level);
            }
            return ISALevel::SSE2; // SSE2 baseline always present
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

        /// Name of the CPU's detected capability level, not of any kernel in use (see SelectLevel()).
        const char* GetDetectedName() const { return GetISAName(m_detectedLevel); }

        /// Reports the CPU's capability; each dispatcher reports the kernel it selected itself.
        std::string Console_GetReport() const
        {
            return std::string("Detected CPU ISA capability: ") + GetDetectedName();
        }

      private:
        MultiISADispatch() = default;

        static ISALevel DetectISA()
        {
            // Runtime detection, not the compiler's target macros: the build
            // targets the SSE4.2 floor, and a variant above it may only be
            // selected when this CPU (and OS) actually supports it.
            const CpuFeatures features = DetectCpuFeatures();
            if (features.avx2 && features.fma)
            {
                return ISALevel::AVX2;
            }
            if (features.avx)
            {
                return ISALevel::AVX;
            }
            if (features.sse41 && features.sse42)
            {
                return ISALevel::SSE4;
            }
            return ISALevel::SSE2;
        }

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
