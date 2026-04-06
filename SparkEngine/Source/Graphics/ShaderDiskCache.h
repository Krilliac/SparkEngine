/**
 * @file ShaderDiskCache.h
 * @brief Persistent on-disk shader compilation cache
 *
 * Caches compiled shader bytecode to disk, keyed by a content hash of the
 * source code + defines + target. Eliminates redundant recompilation across
 * engine sessions. Inspired by RPCS3's granular shader caching system.
 *
 * @see ShaderCrossCompiler.h, ShaderVariantSystem.h
 */

#pragma once

#include "ShaderCrossCompiler.h"

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace Spark::Graphics
{

    /**
     * @brief Persistent shader cache backed by disk storage.
     *
     * Cache directory layout:
     * ```
     * ShaderCache/
     *   <hash_hex>_<target>_<stage>.blob   — compiled bytecode
     * ```
     *
     * Thread-safe: multiple threads may query the cache concurrently.
     */
    class ShaderDiskCache
    {
      public:
        ShaderDiskCache() = default;
        ~ShaderDiskCache() = default;

        /// @brief Initialize the cache with a directory path.
        /// @param cacheDir  Directory for cached shader blobs (created if absent).
        void Initialize(const std::filesystem::path& cacheDir);

        /// @brief Shutdown and flush any pending writes.
        void Shutdown();

        /// @brief Look up a cached blob. Returns nullopt if not found.
        std::optional<CompiledShaderBlob> Lookup(const ShaderSource& source, ShaderTarget target) const;

        /// @brief Store a compiled blob in the cache.
        void Store(const ShaderSource& source, ShaderTarget target, const CompiledShaderBlob& blob);

        /// @brief Remove all cached entries.
        void Clear();

        /// @brief Number of entries currently on disk.
        size_t GetEntryCount() const;

        /// @brief Total size of cached data on disk (bytes).
        size_t GetDiskUsage() const;

        bool IsInitialized() const { return m_initialized; }

      private:
        std::filesystem::path GetBlobPath(uint64_t hash, ShaderTarget target, ShaderStage stage) const;
        static uint64_t HashSourceForDisk(const ShaderSource& source, ShaderTarget target);
        static std::string TargetName(ShaderTarget target);
        static std::string StageName(ShaderStage stage);

        std::filesystem::path m_cacheDir;
        mutable std::mutex m_mutex;
        bool m_initialized = false;
    };

    /**
     * @brief Async shader compilation result token.
     *
     * Returned by CompileAsync(). Poll IsReady() or call Wait() to block.
     */
    class ShaderCompileFuture
    {
      public:
        ShaderCompileFuture() = default;

        /// @brief Check if compilation has completed.
        bool IsReady() const { return m_ready.load(std::memory_order_acquire); }

        /// @brief Get the result (blocks if not ready).
        const CompiledShaderBlob& GetResult() const
        {
            while (!IsReady())
            {
            }
            return m_result;
        }

        // Internal — set by the compiler worker
        void SetResult(CompiledShaderBlob result)
        {
            m_result = std::move(result);
            m_ready.store(true, std::memory_order_release);
        }

      private:
        CompiledShaderBlob m_result;
        std::atomic<bool> m_ready{false};
    };

} // namespace Spark::Graphics
