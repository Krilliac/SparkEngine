/**
 * @file AssetServiceProtocol.h
 * @brief Wire format for the SparkDaemon Asset service (ServiceId::Asset).
 *
 * Daemon-side asset blob cache keyed by `(logical path, platform)`. Phase 3a
 * scope: cache get / put / invalidate-by-path / stats. Actual cooking
 * (mesh compression, texture mip gen, audio transcode) and file watching
 * are deferred.
 *
 * The "logical path" is whatever the engine uses to identify the source
 * asset — typically a virtual filesystem path like `"Textures/brick.png"`.
 * Daemon-side it's opaque: just a string key.
 *
 * Platform is a `uint8_t` — meaning is the engine's concern
 * (Windows / Linux / macOS / Android / iOS / Console variants).
 *
 * Shared by `AssetServiceClient` (engine) and `AssetService` (daemon).
 * Keep free of engine-only dependencies.
 */

#pragma once

#include "DaemonProtocol.h"
#include "Serializer.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Spark::Daemon
{

    enum class AssetMessage : uint16_t
    {
        GetAssetRequest = 0x0001,
        GetAssetResponse = 0x0002,
        PutAssetRequest = 0x0003,
        PutAssetResponse = 0x0004,
        InvalidateAssetRequest = 0x0005, ///< Remove all platform variants for a given path.
        InvalidateAssetResponse = 0x0006,
        GetCacheStatsRequest = 0x0007,
        GetCacheStatsResponse = 0x0008,
        ClearCacheRequest = 0x0009, ///< Drop every entry (all paths, all platforms). Also zeros stats.
        ClearCacheResponse = 0x000A,
    };

    // =========================================================================
    // Payload structs
    // =========================================================================

    struct AssetKey
    {
        std::string path;
        uint8_t platform = 0;
    };

    struct GetAssetRequest
    {
        AssetKey key;
    };

    struct GetAssetResponse
    {
        bool found = false;
        std::vector<uint8_t> blob;
    };

    struct PutAssetRequest
    {
        AssetKey key;
        std::vector<uint8_t> blob;
    };

    struct InvalidateAssetRequest
    {
        std::string path;
    };

    struct InvalidateAssetResponse
    {
        uint32_t removedCount = 0; ///< Number of platform variants that were removed.
    };

    struct AssetCacheStats
    {
        uint64_t entryCount = 0;
        uint64_t totalBytes = 0;
        uint64_t hitCount = 0;
        uint64_t missCount = 0;
        /// Cumulative count of entries evicted by the LRU since service start
        /// (or since the last ClearCache). Appended in Phase 5 follow-up —
        /// decoders tolerate shorter payloads and leave this field zero.
        uint64_t evictionCount = 0;
    };

    // =========================================================================
    // Payload codecs — BinaryWriter/BinaryReader
    // =========================================================================

    [[nodiscard]] inline std::vector<uint8_t> EncodeGetAssetRequest(const GetAssetRequest& req)
    {
        Spark::BinaryWriter w;
        w.WriteString(req.key.path);
        w.Write<uint8_t>(req.key.platform);
        return w.TakeBuffer();
    }

    [[nodiscard]] inline bool DecodeGetAssetRequest(const std::vector<uint8_t>& bytes, GetAssetRequest& out)
    {
        Spark::BinaryReader r(bytes);
        out.key.path = r.ReadString();
        out.key.platform = r.Read<uint8_t>();
        return !r.HasError();
    }

    [[nodiscard]] inline std::vector<uint8_t> EncodeGetAssetResponse(const GetAssetResponse& resp)
    {
        Spark::BinaryWriter w;
        w.Write<uint8_t>(resp.found ? 1u : 0u);
        w.Write<uint32_t>(static_cast<uint32_t>(resp.blob.size()));
        if (!resp.blob.empty())
            w.WriteBytes(resp.blob.data(), resp.blob.size());
        return w.TakeBuffer();
    }

    [[nodiscard]] inline bool DecodeGetAssetResponse(const std::vector<uint8_t>& bytes, GetAssetResponse& out)
    {
        Spark::BinaryReader r(bytes);
        out.found = r.Read<uint8_t>() != 0;
        auto size = r.Read<uint32_t>();
        if (r.HasError())
            return false;
        out.blob.resize(size);
        if (size > 0 && !r.ReadBytes(out.blob.data(), size))
            return false;
        return !r.HasError();
    }

    [[nodiscard]] inline std::vector<uint8_t> EncodePutAssetRequest(const PutAssetRequest& req)
    {
        Spark::BinaryWriter w;
        w.WriteString(req.key.path);
        w.Write<uint8_t>(req.key.platform);
        w.Write<uint32_t>(static_cast<uint32_t>(req.blob.size()));
        if (!req.blob.empty())
            w.WriteBytes(req.blob.data(), req.blob.size());
        return w.TakeBuffer();
    }

    [[nodiscard]] inline bool DecodePutAssetRequest(const std::vector<uint8_t>& bytes, PutAssetRequest& out)
    {
        Spark::BinaryReader r(bytes);
        out.key.path = r.ReadString();
        out.key.platform = r.Read<uint8_t>();
        auto size = r.Read<uint32_t>();
        if (r.HasError())
            return false;
        out.blob.resize(size);
        if (size > 0 && !r.ReadBytes(out.blob.data(), size))
            return false;
        return !r.HasError();
    }

    [[nodiscard]] inline std::vector<uint8_t> EncodeInvalidateAssetRequest(const InvalidateAssetRequest& req)
    {
        Spark::BinaryWriter w;
        w.WriteString(req.path);
        return w.TakeBuffer();
    }

    [[nodiscard]] inline bool DecodeInvalidateAssetRequest(const std::vector<uint8_t>& bytes,
                                                           InvalidateAssetRequest& out)
    {
        Spark::BinaryReader r(bytes);
        out.path = r.ReadString();
        return !r.HasError();
    }

    [[nodiscard]] inline std::vector<uint8_t> EncodeInvalidateAssetResponse(const InvalidateAssetResponse& resp)
    {
        Spark::BinaryWriter w;
        w.Write<uint32_t>(resp.removedCount);
        return w.TakeBuffer();
    }

    [[nodiscard]] inline bool DecodeInvalidateAssetResponse(const std::vector<uint8_t>& bytes,
                                                            InvalidateAssetResponse& out)
    {
        Spark::BinaryReader r(bytes);
        out.removedCount = r.Read<uint32_t>();
        return !r.HasError();
    }

    [[nodiscard]] inline std::vector<uint8_t> EncodeAssetCacheStats(const AssetCacheStats& stats)
    {
        Spark::BinaryWriter w;
        w.Write<uint64_t>(stats.entryCount);
        w.Write<uint64_t>(stats.totalBytes);
        w.Write<uint64_t>(stats.hitCount);
        w.Write<uint64_t>(stats.missCount);
        w.Write<uint64_t>(stats.evictionCount);
        return w.TakeBuffer();
    }

    [[nodiscard]] inline bool DecodeAssetCacheStats(const std::vector<uint8_t>& bytes, AssetCacheStats& out)
    {
        Spark::BinaryReader r(bytes);
        out.entryCount = r.Read<uint64_t>();
        out.totalBytes = r.Read<uint64_t>();
        out.hitCount = r.Read<uint64_t>();
        out.missCount = r.Read<uint64_t>();
        if (r.HasError())
            return false;
        // evictionCount was appended in a follow-up — tolerate older daemons
        // that sent a 32-byte payload without it.
        if (r.Remaining() >= sizeof(uint64_t))
            out.evictionCount = r.Read<uint64_t>();
        return !r.HasError();
    }

} // namespace Spark::Daemon
