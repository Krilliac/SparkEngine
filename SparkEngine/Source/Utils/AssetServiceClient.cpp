/**
 * @file AssetServiceClient.cpp
 * @brief Implementation of the typed Asset service client wrapper.
 */

#include "AssetServiceClient.h"

namespace Spark::Daemon
{

    namespace
    {
        std::unexpected<std::string> WrapError(std::string phase, const std::string& inner)
        {
            return std::unexpected<std::string>("AssetServiceClient::" + phase + ": " + inner);
        }
    } // namespace

    std::expected<GetAssetResponse, std::string> AssetServiceClient::GetAsset(const std::string& path, uint8_t platform)
    {
        GetAssetRequest req;
        req.key.path = path;
        req.key.platform = platform;

        auto response = m_client.Request(ServiceId::Asset, static_cast<uint16_t>(AssetMessage::GetAssetRequest),
                                         EncodeGetAssetRequest(req));
        if (!response)
            return WrapError("GetAsset", response.error());
        if (response->messageType != static_cast<uint16_t>(AssetMessage::GetAssetResponse))
            return WrapError("GetAsset", "unexpected response message type");

        GetAssetResponse decoded;
        if (!DecodeGetAssetResponse(response->payload, decoded))
            return WrapError("GetAsset", "malformed response payload");
        return decoded;
    }

    std::expected<void, std::string> AssetServiceClient::PutAsset(const std::string& path, uint8_t platform,
                                                                  const std::vector<uint8_t>& blob)
    {
        PutAssetRequest req;
        req.key.path = path;
        req.key.platform = platform;
        req.blob = blob;

        auto response = m_client.Request(ServiceId::Asset, static_cast<uint16_t>(AssetMessage::PutAssetRequest),
                                         EncodePutAssetRequest(req));
        if (!response)
            return WrapError("PutAsset", response.error());
        if (response->messageType != static_cast<uint16_t>(AssetMessage::PutAssetResponse))
            return WrapError("PutAsset", "unexpected response message type");
        return {};
    }

    std::expected<uint32_t, std::string> AssetServiceClient::InvalidateAsset(const std::string& path)
    {
        InvalidateAssetRequest req;
        req.path = path;

        auto response = m_client.Request(ServiceId::Asset, static_cast<uint16_t>(AssetMessage::InvalidateAssetRequest),
                                         EncodeInvalidateAssetRequest(req));
        if (!response)
            return WrapError("InvalidateAsset", response.error());
        if (response->messageType != static_cast<uint16_t>(AssetMessage::InvalidateAssetResponse))
            return WrapError("InvalidateAsset", "unexpected response message type");

        InvalidateAssetResponse decoded;
        if (!DecodeInvalidateAssetResponse(response->payload, decoded))
            return WrapError("InvalidateAsset", "malformed response payload");
        return decoded.removedCount;
    }

    std::expected<AssetCacheStats, std::string> AssetServiceClient::GetCacheStats()
    {
        auto response = m_client.Request(ServiceId::Asset, static_cast<uint16_t>(AssetMessage::GetCacheStatsRequest),
                                         /*payload*/ {});
        if (!response)
            return WrapError("GetCacheStats", response.error());
        if (response->messageType != static_cast<uint16_t>(AssetMessage::GetCacheStatsResponse))
            return WrapError("GetCacheStats", "unexpected response message type");

        AssetCacheStats stats;
        if (!DecodeAssetCacheStats(response->payload, stats))
            return WrapError("GetCacheStats", "malformed response payload");
        return stats;
    }

    std::expected<void, std::string> AssetServiceClient::ClearCache()
    {
        auto response = m_client.Request(ServiceId::Asset, static_cast<uint16_t>(AssetMessage::ClearCacheRequest),
                                         /*payload*/ {});
        if (!response)
            return WrapError("ClearCache", response.error());
        if (response->messageType != static_cast<uint16_t>(AssetMessage::ClearCacheResponse))
            return WrapError("ClearCache", "unexpected response message type");
        return {};
    }

} // namespace Spark::Daemon
