/**
 * @file ShaderServiceClient.h
 * @brief Engine-side wrapper for the SparkDaemon Shader service.
 *
 * Typed facade over `DaemonClient` for the shader-cache RPCs:
 * `GetCacheEntry`, `PutCacheEntry`, `ClearCache`, `GetCacheStats`.
 *
 * Phase 2a — cache operations only. Compilation, file watching, and
 * cross-compilation batching are added in later phases and get their own
 * methods here.
 *
 * ## Ownership
 *
 * The wrapper does not own the underlying `DaemonClient`. Callers pass a
 * reference — typically a process-wide client shared with other service
 * wrappers (asset, collab, build) that talk to the same daemon.
 *
 * ## Thread safety
 *
 * All calls are thread-safe: `DaemonClient::Request` serialises send+recv
 * under its internal mutex, and this wrapper adds no mutable state.
 */

#pragma once

#include "DaemonClient.h"
#include "ShaderServiceProtocol.h"

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace Spark::Daemon
{

    class ShaderServiceClient
    {
      public:
        explicit ShaderServiceClient(DaemonClient& client) noexcept : m_client(client) {}

        /**
         * @brief Look up a compiled shader blob by content hash + target/stage.
         *
         * @return Response with `found=false` (cache miss, blob empty) or
         *         `found=true` with the stored bytes. Transport failures are
         *         returned as `unexpected()`.
         */
        std::expected<GetCacheEntryResponse, std::string> GetCacheEntry(uint64_t sourceHash, uint8_t target,
                                                                        uint8_t stage);

        /**
         * @brief Store a compiled shader blob under the given key.
         *
         * Overwrites any existing entry with the same key. Blob size is
         * bounded by `Spark::Daemon::kMaxPayloadSize` (16 MiB) minus the
         * 14-byte key header.
         */
        std::expected<void, std::string> PutCacheEntry(uint64_t sourceHash, uint8_t target, uint8_t stage,
                                                       const std::vector<uint8_t>& blob);

        /// Drop every entry in the daemon's cache.
        std::expected<void, std::string> ClearCache();

        /// Fetch entry count, total bytes, hit and miss counters.
        std::expected<ShaderCacheStats, std::string> GetCacheStats();

      private:
        DaemonClient& m_client;
    };

} // namespace Spark::Daemon
