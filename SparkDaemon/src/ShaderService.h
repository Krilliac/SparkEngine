/**
 * @file ShaderService.h
 * @brief Daemon-side shader blob cache (Phase 2a/2b + Phase 5 LRU eviction).
 *
 * In-memory cache keyed by `(sourceHash, target, stage)`, optionally backed
 * by an on-disk directory so entries survive daemon restarts. An optional
 * byte-budget (`SetMaxBytes`) evicts oldest-used entries (in memory AND on
 * disk) when a Put would push total usage over the limit.
 *
 * Disk layout (one file per entry, raw blob bytes):
 * ```
 * <cacheDir>/<hash16>_<target>_<stage>.blob
 * ```
 *
 * On `Initialize(cacheDir)` the service scans the directory, loads every
 * matching file into the in-memory map, and retains the path for subsequent
 * writes. Without `Initialize`, the cache is in-memory only.
 *
 * Thread-safe: `HandleMessage` runs on per-connection worker threads, so
 * lookups and mutations are protected by an internal mutex. Hit/miss
 * counters use relaxed atomics.
 *
 * ## LRU semantics
 *
 * Entries live in an `std::list` ordered by recency — most-recently-accessed
 * at the front, oldest at the back. A side `unordered_map` indexes keys to
 * list iterators for O(1) lookup. `Get` promotes the hit to the front;
 * `Put` inserts or promotes. When `m_maxBytes > 0` and a Put would push
 * `m_totalBytes` over the limit, entries are evicted from the back until
 * under budget.
 */

#pragma once

#include "ServiceBase.h"
#include "Utils/ShaderServiceProtocol.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <list>
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

        /**
         * @brief Cap total cached bytes; oldest entries are evicted on overflow.
         *
         * Zero = unbounded (default). Must be called before the server
         * accepts connections — changing the limit under concurrent load
         * is not currently supported.
         *
         * If `Initialize` loaded more than @p maxBytes from disk, the
         * next call to SetMaxBytes trims the cache down to the new limit
         * right away (both memory and disk).
         */
        void SetMaxBytes(uint64_t maxBytes);

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

        /// Entry stored in the LRU list: front = most-recently-accessed.
        struct Entry
        {
            Key key;
            std::vector<uint8_t> blob;
        };
        using EntryList = std::list<Entry>;
        using EntryIter = EntryList::iterator;

        ServiceResponse HandleGetCacheEntry(const std::vector<uint8_t>& payload);
        ServiceResponse HandlePutCacheEntry(const std::vector<uint8_t>& payload);
        ServiceResponse HandleClearCache();
        ServiceResponse HandleGetCacheStats();

        ServiceResponse MakeError(const std::string& message) const;

        // Requires m_mutex held.
        void EvictUntilUnderBudget();
        void InsertOrReplace(const Key& key, std::vector<uint8_t> blob);

        [[nodiscard]] std::filesystem::path BlobPath(const Key& key) const;
        static std::optional<Key> ParseBlobFilename(const std::string& stem);
        bool WriteBlobFile(const Key& key, const std::vector<uint8_t>& blob);
        void DeleteBlobFile(const Key& key);
        void DeleteAllBlobFiles();

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
