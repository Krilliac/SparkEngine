/**
 * @file AssetServiceClient.h
 * @brief Engine-side wrapper for the SparkDaemon Asset service.
 *
 * Typed facade over `DaemonClient` for the asset-cache RPCs:
 * `GetAsset`, `PutAsset`, `InvalidateAsset`, `GetCacheStats`.
 *
 * Phase 3a — cache operations only. Cooking pipeline (compression,
 * transcoding, mip generation) and file watching land in later phases.
 *
 * See `ShaderServiceClient` for the established pattern — both are thin
 * typed facades over the same `DaemonClient`.
 */

#pragma once

#include "AssetServiceProtocol.h"
#include "DaemonClient.h"

#include <cstdint>
#include "Expected.h"
#include <string>
#include <vector>

namespace Spark::Daemon
{

    class AssetServiceClient
    {
      public:
        explicit AssetServiceClient(DaemonClient& client) noexcept : m_client(client) {}

        /**
         * @brief Look up a compiled asset blob by logical path + platform.
         *
         * @return Response with `found=false, blob={}` on miss, or the
         *         stored blob on hit. Transport failures surface as
         *         `unexpected()`.
         */
        Expected<GetAssetResponse, std::string> GetAsset(const std::string& path, uint8_t platform);

        /**
         * @brief Store a compiled asset blob under the given path + platform.
         *
         * Overwrites any existing entry with the same key. Blob size is
         * bounded by `Spark::Daemon::kMaxPayloadSize` (16 MiB) minus the
         * path-length header.
         */
        Expected<void, std::string> PutAsset(const std::string& path, uint8_t platform,
                                             const std::vector<uint8_t>& blob);

        /**
         * @brief Drop every platform variant for a given path.
         *
         * Used when a source asset changes on disk — one `InvalidateAsset`
         * call removes the `.png`'s Windows, Linux, and mobile variants in
         * a single round-trip.
         *
         * @return Number of platform variants removed.
         */
        Expected<uint32_t, std::string> InvalidateAsset(const std::string& path);

        /// Fetch entry count, total bytes, hit and miss counters.
        Expected<AssetCacheStats, std::string> GetCacheStats();

        /// Drop every entry in the daemon's asset cache (and its on-disk files).
        /// Zeros hit/miss/eviction counters too.
        Expected<void, std::string> ClearCache();

      private:
        DaemonClient& m_client;
    };

} // namespace Spark::Daemon
