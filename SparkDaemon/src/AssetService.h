/**
 * @file AssetService.h
 * @brief Daemon-side asset blob cache (Phase 3a).
 *
 * In-memory cache keyed by `(logical path, platform)`, optionally backed by
 * an on-disk directory. Follows the same shape as `ShaderService` but with:
 *   - path-string keys instead of `u64` content hashes
 *   - an `Invalidate(path)` operation that drops every platform variant
 *     for a source path in one shot (replacing the engine's per-process
 *     timestamp polling on asset reloads)
 *
 * Disk layout (one file per entry):
 * ```
 *   <cacheDir>/<pathHash16>_<platform3>.asset
 *   file contents: [u32 pathLen][pathBytes][blobBytes]
 * ```
 *
 * The path is stored inside the file so on restart we can rebuild the
 * `(path, platform) -> blob` map without needing a separate index.
 * `pathHash` is FNV-1a of the path bytes — hash collisions are possible
 * in principle but astronomically unlikely in practice; on collision,
 * whichever put ran last wins on disk (the in-memory map stays correct
 * either way because it's keyed by the full path string).
 *
 * Thread-safe: `HandleMessage` is called from per-connection worker threads.
 */

#pragma once

#include "ServiceBase.h"
#include "Utils/AssetServiceProtocol.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::Daemon
{

    class AssetService final : public ServiceBase
    {
      public:
        AssetService() = default;

        [[nodiscard]] ServiceId GetServiceId() const noexcept override { return ServiceId::Asset; }
        [[nodiscard]] const char* GetName() const noexcept override { return "asset"; }

        /**
         * @brief Enable on-disk persistence.
         *
         * Creates the directory if missing, scans for existing `.asset`
         * files, loads them into the in-memory map. Safe to call once
         * before the server accepts connections.
         *
         * @return Number of entries loaded, or `nullopt` on I/O error.
         */
        std::optional<size_t> Initialize(const std::filesystem::path& cacheDir);

        /**
         * @brief Cap total cached bytes; oldest entries are evicted on overflow.
         *
         * Zero = unbounded (default). Mirrors `ShaderService::SetMaxBytes`.
         */
        void SetMaxBytes(uint64_t maxBytes);

        std::optional<ServiceResponse> HandleMessage(uint16_t messageType,
                                                     const std::vector<uint8_t>& payload) override;

        /// Current entry count. For tests; runtime callers should use the RPC.
        [[nodiscard]] size_t GetEntryCount() const;

      private:
        struct Key
        {
            std::string path;
            uint8_t platform = 0;

            bool operator==(const Key& other) const noexcept
            {
                return platform == other.platform && path == other.path;
            }
        };

        struct KeyHash
        {
            size_t operator()(const Key& k) const noexcept
            {
                return std::hash<std::string>{}(k.path) ^ (static_cast<size_t>(k.platform) << 1);
            }
        };

        struct Entry
        {
            Key key;
            std::vector<uint8_t> blob;
        };
        using EntryList = std::list<Entry>;
        using EntryIter = EntryList::iterator;

        ServiceResponse HandleGetAsset(const std::vector<uint8_t>& payload);
        ServiceResponse HandlePutAsset(const std::vector<uint8_t>& payload);
        ServiceResponse HandleInvalidateAsset(const std::vector<uint8_t>& payload);
        ServiceResponse HandleGetCacheStats();

        ServiceResponse MakeError(const std::string& message) const;

        // Requires m_mutex held.
        void EvictUntilUnderBudget();
        void InsertOrReplace(Key key, std::vector<uint8_t> blob);
        void EraseByIterator(EntryIter it);

        [[nodiscard]] std::filesystem::path BlobPath(const Key& key) const;
        static uint64_t HashPath(const std::string& path) noexcept;
        bool WriteBlobFile(const Key& key, const std::vector<uint8_t>& blob);
        bool ReadBlobFile(const std::filesystem::path& file, Key& outKey, std::vector<uint8_t>& outBlob);
        void DeleteBlobFile(const Key& key);

        mutable std::mutex m_mutex;
        EntryList m_lruList;
        std::unordered_map<Key, EntryIter, KeyHash> m_index;
        uint64_t m_totalBytes = 0;
        uint64_t m_maxBytes = 0; ///< 0 = unbounded.
        std::atomic<uint64_t> m_hitCount{0};
        std::atomic<uint64_t> m_missCount{0};
        std::atomic<uint64_t> m_evictionCount{0};

        std::filesystem::path m_cacheDir;
        bool m_diskBacked = false;
    };

} // namespace Spark::Daemon
