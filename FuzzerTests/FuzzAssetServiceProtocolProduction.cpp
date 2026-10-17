/**
 * @file FuzzAssetServiceProtocolProduction.cpp
 * @brief libc++-compiled production adapter for the asset daemon message libFuzzer harness.
 *
 * Input byte 0 selects one of the six shipped decoders in
 * Utils/AssetServiceProtocol.h (the daemon's AssetService decodes client
 * requests with them, the engine's AssetServiceClient decodes daemon
 * responses); the remaining bytes are the message payload. A violated
 * contract aborts so libFuzzer records a crash rather than a silent pass:
 *  - every decoder is publish-on-success, so a rejected payload leaves the
 *    caller's output (pre-filled with a sentinel) untouched,
 *  - acceptance of the fixed-layout messages matches an independent length
 *    model computed from the raw bytes,
 *  - an accepted message re-encodes through its Encode* counterpart to exactly
 *    the payload prefix the decoder consumed (a found flag normalised to 0/1),
 *    and that prefix decodes again to the same fields,
 *  - a decoded path or blob is never longer than the payload that carried it.
 */

#include "FuzzAssetServiceProtocolProduction.h"

#include "Utils/AssetServiceProtocol.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
    constexpr std::size_t kMaxInputBytes = 64u * 1024u;

    // Fixed field widths of the wire format, from the Encode* functions.
    constexpr std::size_t kLengthPrefixBytes = sizeof(std::uint32_t);
    constexpr std::size_t kLegacyStatsBytes = 4 * sizeof(std::uint64_t);
    constexpr std::size_t kStatsBytes = 5 * sizeof(std::uint64_t);
    constexpr std::size_t kNoNormalizedByte = static_cast<std::size_t>(-1);

    const std::string kSentinelPath = "sentinel/path";
    const std::vector<std::uint8_t> kSentinelBlob = {0xA5};
    constexpr std::uint8_t kSentinelPlatform = 0xEE;
    constexpr std::uint32_t kSentinelCount = 0xC0FFEEu;
    constexpr std::uint64_t kSentinelStat = 0xDEADBEEFDEADBEEFull;

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzAssetServiceProtocol: decoder violated: %s\n", what);
        std::abort();
    }

    /// The re-encoding must equal the bytes the decoder consumed. @p normalizedByte
    /// names one offset whose non-zero input value encodes back as 1 (a bool).
    void RequireConsumedPrefix(const std::vector<std::uint8_t>& payload, const std::vector<std::uint8_t>& reencoded,
                               std::size_t normalizedByte)
    {
        if (reencoded.size() > payload.size())
            InvariantFailure("re-encoding is longer than the accepted payload");
        for (std::size_t index = 0; index < reencoded.size(); ++index)
        {
            std::uint8_t expected = payload[index];
            if (index == normalizedByte)
                expected = expected != 0 ? 1u : 0u;
            if (reencoded[index] != expected)
                InvariantFailure("re-encoding differs from the consumed payload prefix");
        }
    }

    /// Length of the string whose u32 prefix starts at @p offset, or nothing when
    /// the prefix or the bytes it claims are not all present (independent model).
    bool ModelString(const std::vector<std::uint8_t>& payload, std::size_t offset, std::size_t& end)
    {
        if (payload.size() < offset || payload.size() - offset < kLengthPrefixBytes)
            return false;
        const std::size_t length = static_cast<std::size_t>(payload[offset]) |
                                   (static_cast<std::size_t>(payload[offset + 1]) << 8) |
                                   (static_cast<std::size_t>(payload[offset + 2]) << 16) |
                                   (static_cast<std::size_t>(payload[offset + 3]) << 24);
        const std::size_t available = payload.size() - offset - kLengthPrefixBytes;
        if (length > available)
            return false;
        end = offset + kLengthPrefixBytes + length;
        return true;
    }

    void FuzzGetAssetRequest(const std::vector<std::uint8_t>& payload)
    {
        Spark::Daemon::GetAssetRequest request;
        request.key.path = kSentinelPath;
        request.key.platform = kSentinelPlatform;
        std::size_t pathEnd = 0;
        const bool expected = ModelString(payload, 0, pathEnd) && payload.size() > pathEnd;
        const bool accepted = Spark::Daemon::DecodeGetAssetRequest(payload, request);
        if (accepted != expected)
            InvariantFailure("GetAssetRequest acceptance does not match its length model");
        if (!accepted)
        {
            if (request.key.path != kSentinelPath || request.key.platform != kSentinelPlatform)
                InvariantFailure("a rejected GetAssetRequest modified the caller's output");
            return;
        }
        const std::vector<std::uint8_t> reencoded = Spark::Daemon::EncodeGetAssetRequest(request);
        RequireConsumedPrefix(payload, reencoded, kNoNormalizedByte);
        Spark::Daemon::GetAssetRequest again;
        if (!Spark::Daemon::DecodeGetAssetRequest(reencoded, again) || again.key.path != request.key.path ||
            again.key.platform != request.key.platform)
            InvariantFailure("GetAssetRequest does not survive a round trip");
    }

    void FuzzGetAssetResponse(const std::vector<std::uint8_t>& payload)
    {
        Spark::Daemon::GetAssetResponse response;
        response.found = true;
        response.blob = kSentinelBlob;
        if (!Spark::Daemon::DecodeGetAssetResponse(payload, response))
        {
            if (!response.found || response.blob != kSentinelBlob)
                InvariantFailure("a rejected GetAssetResponse modified the caller's output");
            return;
        }
        if (response.blob.size() + sizeof(std::uint8_t) + kLengthPrefixBytes > payload.size())
            InvariantFailure("GetAssetResponse blob is longer than its payload");
        const std::vector<std::uint8_t> reencoded = Spark::Daemon::EncodeGetAssetResponse(response);
        RequireConsumedPrefix(payload, reencoded, 0);
        Spark::Daemon::GetAssetResponse again;
        if (!Spark::Daemon::DecodeGetAssetResponse(reencoded, again) || again.found != response.found ||
            again.blob != response.blob)
            InvariantFailure("GetAssetResponse does not survive a round trip");
    }

    void FuzzPutAssetRequest(const std::vector<std::uint8_t>& payload)
    {
        Spark::Daemon::PutAssetRequest request;
        request.key.path = kSentinelPath;
        request.key.platform = kSentinelPlatform;
        request.blob = kSentinelBlob;
        if (!Spark::Daemon::DecodePutAssetRequest(payload, request))
        {
            if (request.key.path != kSentinelPath || request.key.platform != kSentinelPlatform ||
                request.blob != kSentinelBlob)
                InvariantFailure("a rejected PutAssetRequest modified the caller's output");
            return;
        }
        if (request.key.path.size() + request.blob.size() + 2 * kLengthPrefixBytes + sizeof(std::uint8_t) >
            payload.size())
            InvariantFailure("PutAssetRequest path and blob are longer than their payload");
        const std::vector<std::uint8_t> reencoded = Spark::Daemon::EncodePutAssetRequest(request);
        RequireConsumedPrefix(payload, reencoded, kNoNormalizedByte);
        Spark::Daemon::PutAssetRequest again;
        if (!Spark::Daemon::DecodePutAssetRequest(reencoded, again) || again.key.path != request.key.path ||
            again.key.platform != request.key.platform || again.blob != request.blob)
            InvariantFailure("PutAssetRequest does not survive a round trip");
    }

    void FuzzInvalidateAssetRequest(const std::vector<std::uint8_t>& payload)
    {
        Spark::Daemon::InvalidateAssetRequest request;
        request.path = kSentinelPath;
        std::size_t pathEnd = 0;
        const bool expected = ModelString(payload, 0, pathEnd);
        const bool accepted = Spark::Daemon::DecodeInvalidateAssetRequest(payload, request);
        if (accepted != expected)
            InvariantFailure("InvalidateAssetRequest acceptance does not match its length model");
        if (!accepted)
        {
            if (request.path != kSentinelPath)
                InvariantFailure("a rejected InvalidateAssetRequest modified the caller's output");
            return;
        }
        RequireConsumedPrefix(payload, Spark::Daemon::EncodeInvalidateAssetRequest(request), kNoNormalizedByte);
    }

    void FuzzInvalidateAssetResponse(const std::vector<std::uint8_t>& payload)
    {
        Spark::Daemon::InvalidateAssetResponse response;
        response.removedCount = kSentinelCount;
        const bool accepted = Spark::Daemon::DecodeInvalidateAssetResponse(payload, response);
        if (accepted != (payload.size() >= sizeof(std::uint32_t)))
            InvariantFailure("InvalidateAssetResponse acceptance does not match its fixed length");
        if (!accepted)
        {
            if (response.removedCount != kSentinelCount)
                InvariantFailure("a rejected InvalidateAssetResponse modified the caller's output");
            return;
        }
        RequireConsumedPrefix(payload, Spark::Daemon::EncodeInvalidateAssetResponse(response), kNoNormalizedByte);
    }

    void FuzzAssetCacheStats(const std::vector<std::uint8_t>& payload)
    {
        Spark::Daemon::AssetCacheStats stats;
        stats.entryCount = kSentinelStat;
        stats.totalBytes = kSentinelStat;
        stats.hitCount = kSentinelStat;
        stats.missCount = kSentinelStat;
        stats.evictionCount = kSentinelStat;
        const bool accepted = Spark::Daemon::DecodeAssetCacheStats(payload, stats);
        if (accepted != (payload.size() >= kLegacyStatsBytes))
            InvariantFailure("AssetCacheStats acceptance does not match its fixed length");
        if (!accepted)
        {
            if (stats.entryCount != kSentinelStat || stats.totalBytes != kSentinelStat ||
                stats.hitCount != kSentinelStat || stats.missCount != kSentinelStat ||
                stats.evictionCount != kSentinelStat)
                InvariantFailure("a rejected AssetCacheStats modified the caller's output");
            return;
        }
        std::vector<std::uint8_t> reencoded = Spark::Daemon::EncodeAssetCacheStats(stats);
        // A legacy payload shorter than 40 bytes predates evictionCount, which must
        // read as zero rather than keep whatever the caller held.
        if (payload.size() < kStatsBytes)
        {
            if (stats.evictionCount != 0)
                InvariantFailure("legacy AssetCacheStats kept or invented an eviction count");
            reencoded.resize(kLegacyStatsBytes);
        }
        RequireConsumedPrefix(payload, reencoded, kNoNormalizedByte);
    }
} // namespace

// This C ABI is the only boundary between the libstdc++ libFuzzer executable
// and the libc++-compiled production decoders.
extern "C" int SparkFuzzDecodeAssetServiceMessage(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes)
        return 0;
    if (data == nullptr || size == 0)
        return 0;

    const std::vector<std::uint8_t> payload(data + 1, data + size);
    switch (data[0] % 6u)
    {
    case 0:
        FuzzGetAssetRequest(payload);
        break;
    case 1:
        FuzzGetAssetResponse(payload);
        break;
    case 2:
        FuzzPutAssetRequest(payload);
        break;
    case 3:
        FuzzInvalidateAssetRequest(payload);
        break;
    case 4:
        FuzzInvalidateAssetResponse(payload);
        break;
    default:
        FuzzAssetCacheStats(payload);
        break;
    }
    return 0;
}
