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

#include <atomic>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace Spark::Daemon
{
    class ShaderServiceClient;
}

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

        /**
         * @brief Attach an optional daemon client for cross-process cache sharing.
         *
         * When set, `Lookup` consults the daemon first and falls through to the
         * local disk on miss or transport error. `Store` always writes locally
         * and, on success, also pushes the blob to the daemon (best-effort —
         * transport errors are swallowed so the local cache always stays authoritative).
         *
         * Ownership remains with the caller — the pointer must outlive this
         * `ShaderDiskCache` instance or be cleared via `SetDaemonClient(nullptr)`
         * before the client is destroyed.
         */
        void SetDaemonClient(Spark::Daemon::ShaderServiceClient* client);

        /// @brief Count of daemon hits since initialization (test/diagnostic).
        uint64_t GetDaemonHits() const { return m_daemonHits.load(std::memory_order_relaxed); }

        /// @brief Count of daemon misses/errors since initialization (test/diagnostic).
        uint64_t GetDaemonMisses() const { return m_daemonMisses.load(std::memory_order_relaxed); }

      private:
        std::filesystem::path GetBlobPath(uint64_t hash, ShaderTarget target, ShaderStage stage) const;
        static uint64_t HashSourceForDisk(const ShaderSource& source, ShaderTarget target);
        static std::string TargetName(ShaderTarget target);
        static std::string StageName(ShaderStage stage);

        std::filesystem::path m_cacheDir;
        mutable std::mutex m_mutex;
        bool m_initialized = false;

        Spark::Daemon::ShaderServiceClient* m_daemonClient = nullptr;
        mutable std::atomic<uint64_t> m_daemonHits{0};
        mutable std::atomic<uint64_t> m_daemonMisses{0};
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

    /**
     * @brief Process-wide singleton accessor for the shader disk cache.
     *
     * Phase V activation helper. Returns a Meyers singleton so every
     * translation unit that includes this header talks to the same
     * `ShaderDiskCache` instance. `Shader::Initialize` calls
     * `GetShaderDiskCache().Initialize(path)` once per process; subsequent
     * shader compiles consult the same cache for lookups and stores.
     */
    inline ShaderDiskCache& GetShaderDiskCache()
    {
        static ShaderDiskCache s_cache;
        return s_cache;
    }

} // namespace Spark::Graphics
