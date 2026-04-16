/**
 * @file ShaderService.h
 * @brief Daemon-side shader blob cache (Phase 2a + 2b persistence).
 *
 * In-memory cache keyed by `(sourceHash, target, stage)`, optionally backed
 * by an on-disk directory so entries survive daemon restarts.
 *
 * Disk layout (one file per entry, raw blob bytes):
 * ```
 * <cacheDir>/<hash16>_<target>_<stage>.blob
 * ```
 *
 * On `Initialize(cacheDir)` the service scans the directory, loads every
 * matching file into the in-memory map, and retains the path for subsequent
 * writes. Without `Initialize`, the cache is in-memory only (matches the
 * original Phase 2a behaviour — useful for tests).
 *
 * Thread-safe: `HandleMessage` runs on per-connection worker threads, so
 * lookups and mutations are protected by an internal mutex. Hit/miss
 * counters use relaxed atomics.
 */

#pragma once

#include "ServiceBase.h"
#include "Utils/ShaderServiceProtocol.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace Spark::Daemon
{

    class ShaderService final : public ServiceBase
    {
      public:
        ShaderService() = default;

        [[nodiscard]] ServiceId GetServiceId() const noexcept override { return ServiceId::Shader; }
        [[nodiscard]] const char* GetName() const noexcept override { return "shader"; }

        /**
         * @brief Enable on-disk persistence.
         *
         * Creates the directory if missing, then scans for existing
         * `<hash>_<target>_<stage>.blob` files and loads them into the
         * in-memory map. Safe to call once, before the server starts
         * accepting connections.
         *
         * @return Number of entries loaded from disk, or `std::nullopt` on
         *         I/O error (e.g. path exists but is not a directory).
         */
        std::optional<size_t> Initialize(const std::filesystem::path& cacheDir);

        std::optional<ServiceResponse> HandleMessage(uint16_t messageType,
                                                     const std::vector<uint8_t>& payload) override;

        /// Current entry count. Used by tests; runtime callers should use the RPC.
        [[nodiscard]] size_t GetEntryCount() const;

      private:
        struct Key
        {
            uint64_t sourceHash = 0;
            uint8_t target = 0;
            uint8_t stage = 0;

            bool operator==(const Key& other) const noexcept
            {
                return sourceHash == other.sourceHash && target == other.target && stage == other.stage;
            }
        };

        struct KeyHash
        {
            size_t operator()(const Key& k) const noexcept
            {
                size_t h = std::hash<uint64_t>{}(k.sourceHash);
                h ^= static_cast<size_t>(k.target) << 1;
                h ^= static_cast<size_t>(k.stage) << 9;
                return h;
            }
        };

        ServiceResponse HandleGetCacheEntry(const std::vector<uint8_t>& payload);
        ServiceResponse HandlePutCacheEntry(const std::vector<uint8_t>& payload);
        ServiceResponse HandleClearCache();
        ServiceResponse HandleGetCacheStats();

        ServiceResponse MakeError(const std::string& message) const;

        [[nodiscard]] std::filesystem::path BlobPath(const Key& key) const;
        static std::optional<Key> ParseBlobFilename(const std::string& stem);
        bool WriteBlobFile(const Key& key, const std::vector<uint8_t>& blob);
        void DeleteAllBlobFiles();

        mutable std::mutex m_mutex;
        std::unordered_map<Key, std::vector<uint8_t>, KeyHash> m_entries;
        uint64_t m_totalBytes = 0;
        std::atomic<uint64_t> m_hitCount{0};
        std::atomic<uint64_t> m_missCount{0};

        std::filesystem::path m_cacheDir;
        bool m_diskBacked = false;
    };

} // namespace Spark::Daemon
