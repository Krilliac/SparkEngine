/**
 * @file ShaderService.cpp
 * @brief In-memory shader blob cache service (Phase 2a).
 */

#include "ShaderService.h"

namespace Spark::Daemon
{

    std::optional<ServiceResponse> ShaderService::HandleMessage(uint16_t messageType,
                                                                const std::vector<uint8_t>& payload)
    {
        switch (static_cast<ShaderMessage>(messageType))
        {
        case ShaderMessage::GetCacheEntryRequest:
            return HandleGetCacheEntry(payload);
        case ShaderMessage::PutCacheEntryRequest:
            return HandlePutCacheEntry(payload);
        case ShaderMessage::ClearCacheRequest:
            return HandleClearCache();
        case ShaderMessage::GetCacheStatsRequest:
            return HandleGetCacheStats();
        default:
            return MakeError("unsupported shader message");
        }
    }

    size_t ShaderService::GetEntryCount() const
    {
        std::lock_guard lock(m_mutex);
        return m_entries.size();
    }

    ServiceResponse ShaderService::HandleGetCacheEntry(const std::vector<uint8_t>& payload)
    {
        GetCacheEntryRequest req;
        if (!DecodeGetCacheEntryRequest(payload, req))
            return MakeError("malformed GetCacheEntry request");

        Key key{req.key.sourceHash, req.key.target, req.key.stage};
        GetCacheEntryResponse resp;
        {
            std::lock_guard lock(m_mutex);
            auto it = m_entries.find(key);
            if (it != m_entries.end())
            {
                resp.found = true;
                resp.blob = it->second;
            }
        }
        if (resp.found)
            m_hitCount.fetch_add(1, std::memory_order_relaxed);
        else
            m_missCount.fetch_add(1, std::memory_order_relaxed);

        ServiceResponse out;
        out.messageType = static_cast<uint16_t>(ShaderMessage::GetCacheEntryResponse);
        out.payload = EncodeGetCacheEntryResponse(resp);
        return out;
    }

    ServiceResponse ShaderService::HandlePutCacheEntry(const std::vector<uint8_t>& payload)
    {
        PutCacheEntryRequest req;
        if (!DecodePutCacheEntryRequest(payload, req))
            return MakeError("malformed PutCacheEntry request");

        Key key{req.key.sourceHash, req.key.target, req.key.stage};
        {
            std::lock_guard lock(m_mutex);
            auto [it, inserted] = m_entries.try_emplace(key, std::move(req.blob));
            if (!inserted)
            {
                m_totalBytes -= it->second.size();
                it->second = std::move(req.blob);
            }
            m_totalBytes += it->second.size();
        }

        ServiceResponse out;
        out.messageType = static_cast<uint16_t>(ShaderMessage::PutCacheEntryResponse);
        return out;
    }

    ServiceResponse ShaderService::HandleClearCache()
    {
        {
            std::lock_guard lock(m_mutex);
            m_entries.clear();
            m_totalBytes = 0;
        }
        m_hitCount.store(0, std::memory_order_relaxed);
        m_missCount.store(0, std::memory_order_relaxed);

        ServiceResponse out;
        out.messageType = static_cast<uint16_t>(ShaderMessage::ClearCacheResponse);
        return out;
    }

    ServiceResponse ShaderService::HandleGetCacheStats()
    {
        ShaderCacheStats stats;
        {
            std::lock_guard lock(m_mutex);
            stats.entryCount = m_entries.size();
            stats.totalBytes = m_totalBytes;
        }
        stats.hitCount = m_hitCount.load(std::memory_order_relaxed);
        stats.missCount = m_missCount.load(std::memory_order_relaxed);

        ServiceResponse out;
        out.messageType = static_cast<uint16_t>(ShaderMessage::GetCacheStatsResponse);
        out.payload = EncodeShaderCacheStats(stats);
        return out;
    }

    ServiceResponse ShaderService::MakeError(const std::string& message) const
    {
        ServiceResponse r;
        r.messageType = static_cast<uint16_t>(ControlMessage::ErrorResponse);
        r.payload.assign(message.begin(), message.end());
        return r;
    }

} // namespace Spark::Daemon
