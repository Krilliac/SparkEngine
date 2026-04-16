/**
 * @file ShaderService.h
 * @brief Daemon-side shader blob cache (Phase 2a).
 *
 * In-memory cache keyed by `(sourceHash, target, stage)`. Persistent on-disk
 * storage and cross-instance restart survival are deferred to a later phase;
 * for now the cache is warm only while the daemon is running.
 *
 * Thread-safe: `HandleMessage` is called from per-connection worker threads,
 * so lookups and mutations are protected by an internal mutex. Hit/miss
 * counters use relaxed atomics.
 */

#pragma once

#include "ServiceBase.h"
#include "Utils/ShaderServiceProtocol.h"

#include <atomic>
#include <cstdint>
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

        mutable std::mutex m_mutex;
        std::unordered_map<Key, std::vector<uint8_t>, KeyHash> m_entries;
        uint64_t m_totalBytes = 0;
        std::atomic<uint64_t> m_hitCount{0};
        std::atomic<uint64_t> m_missCount{0};
    };

} // namespace Spark::Daemon
