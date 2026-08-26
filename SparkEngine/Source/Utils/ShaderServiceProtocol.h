/**
 * @file ShaderServiceProtocol.h
 * @brief Wire format for the SparkDaemon Shader service (ServiceId::Shader).
 *
 * Defines the request/response message numbers and payload codecs for a
 * blob-cache service. The daemon is domain-agnostic — it stores opaque
 * byte blobs keyed by `(sourceHash, target, stage)`. Semantic meaning of
 * target and stage (DXIL / DXBC / SPIR-V / ... ; Vertex / Pixel / ...) is
 * the engine's concern; on the wire they are just `uint8_t`.
 *
 * Phase 2a scope: cache get / put / clear / stats. Actual compilation,
 * file watching, and batch compilation land in later phases.
 *
 * Shared by `ShaderServiceClient` (engine side) and `ShaderService`
 * (daemon side). The daemon does not link `SparkEngineLib`, so this
 * header must remain free of engine-only dependencies.
 */

#pragma once

#include "DaemonProtocol.h"
#include "Serializer.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace Spark::Daemon
{

    /// Messages handled by the Shader service.
    enum class ShaderMessage : uint16_t
    {
        GetCacheEntryRequest = 0x0001,
        GetCacheEntryResponse = 0x0002,
        PutCacheEntryRequest = 0x0003,
        PutCacheEntryResponse = 0x0004,
        ClearCacheRequest = 0x0005,
        ClearCacheResponse = 0x0006,
        GetCacheStatsRequest = 0x0007,
        GetCacheStatsResponse = 0x0008,
    };

    // =========================================================================
    // Payload structs
    // =========================================================================

    /// Cache entry key. Hash is opaque to the daemon — callers pick the
    /// hashing function (typically a stable content hash of HLSL source +
    /// defines + entry point).
    struct ShaderCacheKey
    {
        uint64_t sourceHash = 0;
        uint8_t target = 0;
        uint8_t stage = 0;
    };

    struct GetCacheEntryRequest
    {
        ShaderCacheKey key;
    };

    struct GetCacheEntryResponse
    {
        bool found = false;
        std::vector<uint8_t> blob;
    };

    struct PutCacheEntryRequest
    {
        ShaderCacheKey key;
        std::vector<uint8_t> blob;
    };

    struct ShaderCacheStats
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
    // Payload codecs — hand-rolled with Spark::BinaryWriter/Reader
    // =========================================================================

    [[nodiscard]] inline std::vector<uint8_t> EncodeGetCacheEntryRequest(const GetCacheEntryRequest& req)
    {
        Spark::BinaryWriter w;
        w.Write<uint64_t>(req.key.sourceHash);
        w.Write<uint8_t>(req.key.target);
        w.Write<uint8_t>(req.key.stage);
        return w.TakeBuffer();
    }

    [[nodiscard]] inline bool DecodeGetCacheEntryRequest(const std::vector<uint8_t>& bytes, GetCacheEntryRequest& out)
    {
        Spark::BinaryReader r(bytes);
        out.key.sourceHash = r.Read<uint64_t>();
        out.key.target = r.Read<uint8_t>();
        out.key.stage = r.Read<uint8_t>();
        return !r.HasError();
    }

    [[nodiscard]] inline std::vector<uint8_t> EncodeGetCacheEntryResponse(const GetCacheEntryResponse& resp)
    {
        Spark::BinaryWriter w;
        w.Write<uint8_t>(resp.found ? 1u : 0u);
        w.Write<uint32_t>(static_cast<uint32_t>(resp.blob.size()));
        if (!resp.blob.empty())
            w.WriteBytes(resp.blob.data(), resp.blob.size());
        return w.TakeBuffer();
    }

    [[nodiscard]] inline bool DecodeGetCacheEntryResponse(const std::vector<uint8_t>& bytes, GetCacheEntryResponse& out)
    {
        Spark::BinaryReader r(bytes);
        GetCacheEntryResponse decoded;
        decoded.found = r.Read<uint8_t>() != 0;
        auto size = r.Read<uint32_t>();
        if (r.HasError() || static_cast<size_t>(size) > r.Remaining())
            return false;
        decoded.blob.resize(size);
        if (size > 0 && !r.ReadBytes(decoded.blob.data(), size))
            return false;
        if (r.HasError())
            return false;
        out = std::move(decoded);
        return true;
    }

    [[nodiscard]] inline std::vector<uint8_t> EncodePutCacheEntryRequest(const PutCacheEntryRequest& req)
    {
        Spark::BinaryWriter w;
        w.Write<uint64_t>(req.key.sourceHash);
        w.Write<uint8_t>(req.key.target);
        w.Write<uint8_t>(req.key.stage);
        w.Write<uint32_t>(static_cast<uint32_t>(req.blob.size()));
        if (!req.blob.empty())
            w.WriteBytes(req.blob.data(), req.blob.size());
        return w.TakeBuffer();
    }

    [[nodiscard]] inline bool DecodePutCacheEntryRequest(const std::vector<uint8_t>& bytes, PutCacheEntryRequest& out)
    {
        Spark::BinaryReader r(bytes);
        PutCacheEntryRequest decoded;
        decoded.key.sourceHash = r.Read<uint64_t>();
        decoded.key.target = r.Read<uint8_t>();
        decoded.key.stage = r.Read<uint8_t>();
        auto size = r.Read<uint32_t>();
        if (r.HasError() || static_cast<size_t>(size) > r.Remaining())
            return false;
        decoded.blob.resize(size);
        if (size > 0 && !r.ReadBytes(decoded.blob.data(), size))
            return false;
        if (r.HasError())
            return false;
        out = std::move(decoded);
        return true;
    }

    [[nodiscard]] inline std::vector<uint8_t> EncodeShaderCacheStats(const ShaderCacheStats& stats)
    {
        Spark::BinaryWriter w;
        w.Write<uint64_t>(stats.entryCount);
        w.Write<uint64_t>(stats.totalBytes);
        w.Write<uint64_t>(stats.hitCount);
        w.Write<uint64_t>(stats.missCount);
        w.Write<uint64_t>(stats.evictionCount);
        return w.TakeBuffer();
    }

    [[nodiscard]] inline bool DecodeShaderCacheStats(const std::vector<uint8_t>& bytes, ShaderCacheStats& out)
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
