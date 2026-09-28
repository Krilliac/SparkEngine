/**
 * @file FuzzShaderServiceProtocolProduction.cpp
 * @brief libc++-compiled production adapter for the shader daemon message libFuzzer harness.
 *
 * Input byte 0 selects one of the four shipped decoders in
 * Utils/ShaderServiceProtocol.h (the daemon decodes client requests with them,
 * the engine decodes daemon responses); the remaining bytes are the message
 * payload. A violated contract aborts so libFuzzer records a crash rather than
 * a silent pass:
 *  - an accepted message re-encodes through its Encode* counterpart to exactly
 *    the payload prefix the decoder consumed (a found flag normalised to 0/1),
 *  - a decoded blob is never longer than the payload that carried it,
 *  - the publish-on-success decoders (GetCacheEntryResponse,
 *    PutCacheEntryRequest) leave the caller's output untouched on rejection.
 */

#include "FuzzShaderServiceProtocolProduction.h"

#include "Utils/ShaderServiceProtocol.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace
{
    constexpr std::size_t kMaxInputBytes = 64u * 1024u;

    // Fixed field widths of the wire format, from the Encode* functions.
    constexpr std::size_t kCacheKeyBytes = sizeof(std::uint64_t) + 2 * sizeof(std::uint8_t);
    constexpr std::size_t kLegacyStatsBytes = 4 * sizeof(std::uint64_t);
    constexpr std::size_t kStatsBytes = 5 * sizeof(std::uint64_t);

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzShaderServiceProtocol: decoder violated: %s\n", what);
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

    constexpr std::size_t kNoNormalizedByte = static_cast<std::size_t>(-1);

    void FuzzGetCacheEntryRequest(const std::vector<std::uint8_t>& payload)
    {
        Spark::Daemon::GetCacheEntryRequest request;
        const bool accepted = Spark::Daemon::DecodeGetCacheEntryRequest(payload, request);
        if (accepted != (payload.size() >= kCacheKeyBytes))
            InvariantFailure("GetCacheEntryRequest acceptance does not match its fixed length");
        if (accepted)
            RequireConsumedPrefix(payload, Spark::Daemon::EncodeGetCacheEntryRequest(request), kNoNormalizedByte);
    }

    void FuzzGetCacheEntryResponse(const std::vector<std::uint8_t>& payload)
    {
        Spark::Daemon::GetCacheEntryResponse response;
        response.found = true;
        response.blob = {0xA5};
        if (!Spark::Daemon::DecodeGetCacheEntryResponse(payload, response))
        {
            if (!response.found || response.blob != std::vector<std::uint8_t>{0xA5})
                InvariantFailure("a rejected GetCacheEntryResponse modified the caller's output");
            return;
        }
        if (response.blob.size() + sizeof(std::uint8_t) + sizeof(std::uint32_t) > payload.size())
            InvariantFailure("GetCacheEntryResponse blob is longer than its payload");
        RequireConsumedPrefix(payload, Spark::Daemon::EncodeGetCacheEntryResponse(response), 0);
    }

    void FuzzPutCacheEntryRequest(const std::vector<std::uint8_t>& payload)
    {
        Spark::Daemon::PutCacheEntryRequest request;
        request.key.sourceHash = 0xDEADBEEFDEADBEEFull;
        request.blob = {0xA5};
        if (!Spark::Daemon::DecodePutCacheEntryRequest(payload, request))
        {
            if (request.key.sourceHash != 0xDEADBEEFDEADBEEFull || request.blob != std::vector<std::uint8_t>{0xA5})
                InvariantFailure("a rejected PutCacheEntryRequest modified the caller's output");
            return;
        }
        if (request.blob.size() + kCacheKeyBytes + sizeof(std::uint32_t) > payload.size())
            InvariantFailure("PutCacheEntryRequest blob is longer than its payload");
        RequireConsumedPrefix(payload, Spark::Daemon::EncodePutCacheEntryRequest(request), kNoNormalizedByte);
    }

    void FuzzShaderCacheStats(const std::vector<std::uint8_t>& payload)
    {
        Spark::Daemon::ShaderCacheStats stats;
        const bool accepted = Spark::Daemon::DecodeShaderCacheStats(payload, stats);
        if (accepted != (payload.size() >= kLegacyStatsBytes))
            InvariantFailure("ShaderCacheStats acceptance does not match its fixed length");
        if (!accepted)
            return;
        std::vector<std::uint8_t> reencoded = Spark::Daemon::EncodeShaderCacheStats(stats);
        // A 32-byte legacy payload predates evictionCount, which must stay zero.
        if (payload.size() < kStatsBytes)
        {
            if (stats.evictionCount != 0)
                InvariantFailure("legacy ShaderCacheStats invented an eviction count");
            reencoded.resize(kLegacyStatsBytes);
        }
        RequireConsumedPrefix(payload, reencoded, kNoNormalizedByte);
    }
} // namespace

// This C ABI is the only boundary between the libstdc++ libFuzzer executable
// and the libc++-compiled production decoders.
extern "C" int SparkFuzzDecodeShaderServiceMessage(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes)
        return 0;
    if (data == nullptr || size == 0)
        return 0;

    const std::vector<std::uint8_t> payload(data + 1, data + size);
    switch (data[0] % 4u)
    {
    case 0:
        FuzzGetCacheEntryRequest(payload);
        break;
    case 1:
        FuzzGetCacheEntryResponse(payload);
        break;
    case 2:
        FuzzPutCacheEntryRequest(payload);
        break;
    default:
        FuzzShaderCacheStats(payload);
        break;
    }
    return 0;
}
