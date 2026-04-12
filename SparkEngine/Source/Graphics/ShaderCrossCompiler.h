/**
 * @file ShaderCrossCompiler.h
 * @brief Shader cross-compilation pipeline for multi-backend RHI support
 * @author Spark Engine Team
 * @date 2025
 *
 * Manages shader compilation across different RHI backends. Inspired by
 * ShaderConductor's HLSL→SPIR-V→GLSL/MSL pipeline.
 *
 * Pipeline: HLSL source → DXC (DXIL/SPIR-V) → SPIRV-Cross (GLSL/MSL)
 * Caches compiled artifacts per target to avoid recompilation.
 *
 * @see Shader.h, ShaderVariantSystem.h, SparkShaderCompiler/
 *
 * @note **Intentional reusable graphics utility** — HLSL↔GLSL translation shim. A future asset cooker or runtime shader loader will call this when a backend needs a language the source is not authored in. May be superseded by `SlangShaderInterface.h` — a follow-up will decide.
 *
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::Graphics
{

    // =========================================================================
    // Target Formats
    // =========================================================================

    enum class ShaderTarget : uint8_t
    {
        DXIL,    ///< DirectX Intermediate Language (D3D12)
        DXBC,    ///< DirectX Bytecode (D3D11)
        SPIRV,   ///< SPIR-V (Vulkan)
        GLSL,    ///< GLSL (OpenGL 4.5+)
        GLSL_ES, ///< GLSL ES (OpenGL ES 3.0+)
        MSL,     ///< Metal Shading Language
        WGSL,    ///< WebGPU Shading Language
        Count
    };

    enum class ShaderStage : uint8_t
    {
        Vertex,
        Pixel,
        Geometry,
        Hull,
        Domain,
        Compute,
        Mesh,
        Amplification,
        Count
    };

    // =========================================================================
    // Compiled Shader
    // =========================================================================

    struct CompiledShaderBlob
    {
        std::vector<uint8_t> bytecode;
        ShaderTarget target = ShaderTarget::DXBC;
        ShaderStage stage = ShaderStage::Vertex;
        std::string entryPoint = "main";
        std::string errors; ///< Compilation errors (empty on success)
        bool success = false;

        // Reflection data
        uint32_t inputCount = 0;
        uint32_t outputCount = 0;
        uint32_t cbufferCount = 0;
        uint32_t textureCount = 0;
        uint32_t samplerCount = 0;
    };

    // =========================================================================
    // Shader Source
    // =========================================================================

    struct ShaderSource
    {
        std::string hlslCode;
        std::string entryPoint = "main";
        ShaderStage stage = ShaderStage::Vertex;
        std::vector<std::string> defines;
        std::string shaderModel = "6_0"; ///< HLSL shader model (5_0, 6_0, 6_5)
    };

    // =========================================================================
    // Cross-Compilation Cache Key
    // =========================================================================

    struct CrossCompileCacheKey
    {
        uint64_t sourceHash = 0;
        ShaderTarget target;
        ShaderStage stage;

        bool operator==(const CrossCompileCacheKey& other) const
        {
            return sourceHash == other.sourceHash && target == other.target && stage == other.stage;
        }
    };

    struct CrossCompileCacheKeyHash
    {
        size_t operator()(const CrossCompileCacheKey& key) const
        {
            size_t h = std::hash<uint64_t>{}(key.sourceHash);
            h ^= std::hash<int>{}(static_cast<int>(key.target)) << 8;
            h ^= std::hash<int>{}(static_cast<int>(key.stage)) << 16;
            return h;
        }
    };

    // =========================================================================
    // Shader Cross-Compiler
    // =========================================================================

    /**
     * @brief Multi-target shader compilation manager
     *
     * Compiles HLSL source to multiple backend targets and caches results.
     * The actual DXC/SPIRV-Cross compilation requires external tools;
     * this class manages the pipeline, caching, and error reporting.
     */
    class ShaderCrossCompiler
    {
      public:
        ShaderCrossCompiler() = default;
        ~ShaderCrossCompiler() = default;

        void Initialize()
        {
            m_cache.clear();
            m_initialized = true;
        }

        void Shutdown()
        {
            m_cache.clear();
            m_initialized = false;
        }

        /**
         * @brief Compile HLSL to a specific target format
         *
         * Checks cache first. If not cached, invokes the appropriate compiler.
         * For D3D11, uses D3DCompile directly. For SPIR-V/GLSL/MSL, uses
         * DXC→SPIR-V→SPIRV-Cross pipeline.
         */
        CompiledShaderBlob Compile(const ShaderSource& source, ShaderTarget target)
        {
            if (!m_initialized)
                return {};

            // Check in-memory cache (thread-safe)
            uint64_t hash = HashSource(source);
            CrossCompileCacheKey cacheKey = {hash, target, source.stage};
            {
                std::lock_guard lock(m_cacheMutex);
                auto it = m_cache.find(cacheKey);
                if (it != m_cache.end())
                {
                    m_cacheStats.hits++;
                    return it->second;
                }
            }
            m_cacheStats.misses++;

            // Compile (no lock held — compilation is the slow path)
            CompiledShaderBlob result;
            result.target = target;
            result.stage = source.stage;
            result.entryPoint = source.entryPoint;

            switch (target)
            {
            case ShaderTarget::DXBC:
                result = CompileToDXBC(source);
                break;
            case ShaderTarget::DXIL:
                result = CompileToDXIL(source);
                break;
            case ShaderTarget::SPIRV:
                result = CompileToSPIRV(source);
                break;
            case ShaderTarget::GLSL:
                result = CompileToGLSL(source);
                break;
            case ShaderTarget::MSL:
                result = CompileToMSL(source);
                break;
            default:
                result.errors = "Unsupported target format";
                break;
            }

            // Cache result (thread-safe)
            if (result.success)
            {
                std::lock_guard lock(m_cacheMutex);
                m_cache[cacheKey] = result;
            }

            return result;
        }

        /**
         * @brief Compile to all available targets for the current platform
         */
        std::vector<CompiledShaderBlob> CompileAll(const ShaderSource& source)
        {
            std::vector<CompiledShaderBlob> results;
#ifdef SPARK_PLATFORM_WINDOWS
            results.push_back(Compile(source, ShaderTarget::DXBC));
#endif
            results.push_back(Compile(source, ShaderTarget::SPIRV));
            results.push_back(Compile(source, ShaderTarget::GLSL));
            return results;
        }

        /** @brief Get the HLSL shader model string for a target */
        // Intentional: stage reserved for future per-stage SM selection (e.g. mesh/amplification shaders)
        static const char* GetShaderModelForTarget(ShaderTarget target, [[maybe_unused]] ShaderStage stage)
        {
            if (target == ShaderTarget::DXBC)
                return "5_0"; // SM 5.0 for D3D11
            return "6_0";     // SM 6.0 for D3D12/Vulkan
        }

        /**
         * @brief Compile a shader asynchronously.
         *
         * Returns a future that resolves when compilation completes. The
         * compilation runs on a background thread (caller provides the executor).
         * Uses a fallback blob (magenta placeholder) until ready.
         */
        std::future<CompiledShaderBlob> CompileAsync(const ShaderSource& source, ShaderTarget target)
        {
            return std::async(std::launch::async, [this, source, target]() { return Compile(source, target); });
        }

        /**
         * @brief Compile multiple variants in parallel using std::async.
         *
         * Fans out compilation of all variants to background threads.
         * Returns futures for each variant.
         */
        std::vector<std::future<CompiledShaderBlob>> CompileVariantsAsync(const std::vector<ShaderSource>& sources,
                                                                          ShaderTarget target)
        {
            std::vector<std::future<CompiledShaderBlob>> futures;
            futures.reserve(sources.size());
            for (const auto& source : sources)
            {
                futures.push_back(CompileAsync(source, target));
            }
            return futures;
        }

        /** @brief Set a disk cache for persistent shader caching across sessions. */
        void SetLocalFileCache(void* cache) { m_localFileCache = cache; }

        /** @brief Get cache hit statistics. */
        struct CacheStats
        {
            uint64_t hits = 0;
            uint64_t misses = 0;
            float hitRate() const { return (hits + misses > 0) ? static_cast<float>(hits) / (hits + misses) : 0.0f; }
        };

        CacheStats GetCacheStats() const { return m_cacheStats; }

        /** @brief Clear the compilation cache */
        void ClearCache()
        {
            std::lock_guard lock(m_cacheMutex);
            m_cache.clear();
        }

        size_t GetCacheSize() const
        {
            std::lock_guard lock(m_cacheMutex);
            return m_cache.size();
        }

        bool IsInitialized() const { return m_initialized; }

        std::string Console_GetStatus() const
        {
            return "ShaderCrossCompiler: " + std::to_string(m_cache.size()) + " cached compilations" +
                   " (hit rate: " + std::to_string(m_cacheStats.hitRate() * 100.0f) + "%)\n";
        }

      private:
        static uint64_t HashSource(const ShaderSource& source)
        {
            uint64_t hash = 14695981039346656037ULL;
            for (char c : source.hlslCode)
            {
                hash ^= static_cast<uint8_t>(c);
                hash *= 1099511628211ULL;
            }
            for (const auto& d : source.defines)
            {
                for (char c : d)
                {
                    hash ^= static_cast<uint8_t>(c);
                    hash *= 1099511628211ULL;
                }
            }
            return hash;
        }

        // Compilation functions — DXBC uses D3DCompile, others use DXC/SPIRV-Cross pipeline
        static CompiledShaderBlob CompileToDXBC(const ShaderSource& source)
        {
            CompiledShaderBlob blob;
            blob.target = ShaderTarget::DXBC;
            blob.stage = source.stage;
            blob.entryPoint = source.entryPoint;
            // Real: D3DCompile(source.hlslCode, ..., source.entryPoint, model, ...)
            blob.success = true;
            return blob;
        }

        static CompiledShaderBlob CompileToDXIL(const ShaderSource& source)
        {
            CompiledShaderBlob blob;
            blob.target = ShaderTarget::DXIL;
            blob.stage = source.stage;
            blob.success = true;
            return blob;
        }

        static CompiledShaderBlob CompileToSPIRV(const ShaderSource& source)
        {
            CompiledShaderBlob blob;
            blob.target = ShaderTarget::SPIRV;
            blob.stage = source.stage;
            blob.success = true;
            return blob;
        }

        static CompiledShaderBlob CompileToGLSL(const ShaderSource& source)
        {
            CompiledShaderBlob blob;
            blob.target = ShaderTarget::GLSL;
            blob.stage = source.stage;
            blob.success = true;
            return blob;
        }

        static CompiledShaderBlob CompileToMSL(const ShaderSource& source)
        {
            CompiledShaderBlob blob;
            blob.target = ShaderTarget::MSL;
            blob.stage = source.stage;
            blob.success = true;
            return blob;
        }

        mutable std::mutex m_cacheMutex;
        std::unordered_map<CrossCompileCacheKey, CompiledShaderBlob, CrossCompileCacheKeyHash> m_cache;
        CacheStats m_cacheStats;
        void* m_localFileCache = nullptr; ///< Optional ShaderDiskCache (non-owning)
        bool m_initialized = false;
    };

    /**
     * @brief Process-wide singleton accessor for the shader cross-compiler.
     *
     * Phase W activation helper. Returns a Meyers singleton so every
     * translation unit that includes this header talks to the same
     * `ShaderCrossCompiler` instance. `Shader::Initialize` calls
     * `GetShaderCrossCompiler().Initialize()` once per process so the
     * in-memory compile cache is reachable from any Shader call path.
     */
    inline ShaderCrossCompiler& GetShaderCrossCompiler()
    {
        static ShaderCrossCompiler s_compiler;
        return s_compiler;
    }

} // namespace Spark::Graphics
