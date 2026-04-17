/**
 * @file ShaderServiceClient.cpp
 * @brief Implementation of the typed Shader service client wrapper.
 */

#include "ShaderServiceClient.h"

namespace Spark::Daemon
{

    namespace
    {
        Unexpected<std::string> WrapError(std::string phase, const std::string& inner)
        {
            return Unexpected<std::string>("ShaderServiceClient::" + phase + ": " + inner);
        }
    } // namespace

    Expected<GetCacheEntryResponse, std::string> ShaderServiceClient::GetCacheEntry(uint64_t sourceHash, uint8_t target,
                                                                                    uint8_t stage)
    {
        GetCacheEntryRequest req;
        req.key.sourceHash = sourceHash;
        req.key.target = target;
        req.key.stage = stage;

        auto response = m_client.Request(ServiceId::Shader, static_cast<uint16_t>(ShaderMessage::GetCacheEntryRequest),
                                         EncodeGetCacheEntryRequest(req));
        if (!response)
            return WrapError("GetCacheEntry", response.error());

        if (response->messageType != static_cast<uint16_t>(ShaderMessage::GetCacheEntryResponse))
            return WrapError("GetCacheEntry", "unexpected response message type");

        GetCacheEntryResponse decoded;
        if (!DecodeGetCacheEntryResponse(response->payload, decoded))
            return WrapError("GetCacheEntry", "malformed response payload");

        return decoded;
    }

    Expected<void, std::string> ShaderServiceClient::PutCacheEntry(uint64_t sourceHash, uint8_t target, uint8_t stage,
                                                                   const std::vector<uint8_t>& blob)
    {
        PutCacheEntryRequest req;
        req.key.sourceHash = sourceHash;
        req.key.target = target;
        req.key.stage = stage;
        req.blob = blob;

        auto response = m_client.Request(ServiceId::Shader, static_cast<uint16_t>(ShaderMessage::PutCacheEntryRequest),
                                         EncodePutCacheEntryRequest(req));
        if (!response)
            return WrapError("PutCacheEntry", response.error());
        if (response->messageType != static_cast<uint16_t>(ShaderMessage::PutCacheEntryResponse))
            return WrapError("PutCacheEntry", "unexpected response message type");
        return {};
    }

    Expected<void, std::string> ShaderServiceClient::ClearCache()
    {
        auto response = m_client.Request(ServiceId::Shader, static_cast<uint16_t>(ShaderMessage::ClearCacheRequest),
                                         /*payload*/ {});
        if (!response)
            return WrapError("ClearCache", response.error());
        if (response->messageType != static_cast<uint16_t>(ShaderMessage::ClearCacheResponse))
            return WrapError("ClearCache", "unexpected response message type");
        return {};
    }

    Expected<ShaderCacheStats, std::string> ShaderServiceClient::GetCacheStats()
    {
        auto response = m_client.Request(ServiceId::Shader, static_cast<uint16_t>(ShaderMessage::GetCacheStatsRequest),
                                         /*payload*/ {});
        if (!response)
            return WrapError("GetCacheStats", response.error());
        if (response->messageType != static_cast<uint16_t>(ShaderMessage::GetCacheStatsResponse))
            return WrapError("GetCacheStats", "unexpected response message type");

        ShaderCacheStats stats;
        if (!DecodeShaderCacheStats(response->payload, stats))
            return WrapError("GetCacheStats", "malformed response payload");
        return stats;
    }

} // namespace Spark::Daemon
