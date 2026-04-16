/**
 * @file ShaderService.cpp
 * @brief Shader blob cache service (Phase 2a in-memory + Phase 2b disk persistence).
 */

#include "ShaderService.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <system_error>

namespace Spark::Daemon
{

    std::optional<size_t> ShaderService::Initialize(const std::filesystem::path& cacheDir)
    {
        std::error_code ec;
        std::filesystem::create_directories(cacheDir, ec);
        if (ec && !std::filesystem::is_directory(cacheDir, ec))
            return std::nullopt;
        if (!std::filesystem::is_directory(cacheDir, ec))
            return std::nullopt;

        std::lock_guard lock(m_mutex);
        m_cacheDir = cacheDir;
        m_diskBacked = true;

        size_t loaded = 0;
        for (const auto& entry : std::filesystem::directory_iterator(cacheDir, ec))
        {
            if (ec)
                break;
            if (!entry.is_regular_file())
                continue;
            if (entry.path().extension() != ".blob")
                continue;

            auto parsed = ParseBlobFilename(entry.path().stem().string());
            if (!parsed)
                continue;

            std::ifstream in(entry.path(), std::ios::binary);
            if (!in)
                continue;
            in.seekg(0, std::ios::end);
            auto size = static_cast<size_t>(in.tellg());
            in.seekg(0, std::ios::beg);

            std::vector<uint8_t> blob(size);
            if (size > 0)
                in.read(reinterpret_cast<char*>(blob.data()), static_cast<std::streamsize>(size));
            if (!in && size > 0)
                continue;

            m_totalBytes += blob.size();
            m_entries.emplace(*parsed, std::move(blob));
            ++loaded;
        }
        return loaded;
    }

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
        std::vector<uint8_t> blobCopyForDisk;
        {
            std::lock_guard lock(m_mutex);
            auto [it, inserted] = m_entries.try_emplace(key, std::move(req.blob));
            if (!inserted)
            {
                m_totalBytes -= it->second.size();
                it->second = std::move(req.blob);
            }
            m_totalBytes += it->second.size();
            if (m_diskBacked)
                blobCopyForDisk = it->second;
        }

        if (m_diskBacked)
            WriteBlobFile(key, blobCopyForDisk);

        ServiceResponse out;
        out.messageType = static_cast<uint16_t>(ShaderMessage::PutCacheEntryResponse);
        return out;
    }

    ServiceResponse ShaderService::HandleClearCache()
    {
        bool deleteFromDisk = false;
        {
            std::lock_guard lock(m_mutex);
            m_entries.clear();
            m_totalBytes = 0;
            deleteFromDisk = m_diskBacked;
        }
        m_hitCount.store(0, std::memory_order_relaxed);
        m_missCount.store(0, std::memory_order_relaxed);

        if (deleteFromDisk)
            DeleteAllBlobFiles();

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

    std::filesystem::path ShaderService::BlobPath(const Key& key) const
    {
        // Format: <16 hex digits>_<3 digit target>_<3 digit stage>.blob
        // Zero-padded numeric fields keep filenames sortable and unambiguous
        // to parse regardless of platform locale.
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%016llx_%03u_%03u.blob", static_cast<unsigned long long>(key.sourceHash),
                      static_cast<unsigned>(key.target), static_cast<unsigned>(key.stage));
        return m_cacheDir / buf;
    }

    std::optional<ShaderService::Key> ShaderService::ParseBlobFilename(const std::string& stem)
    {
        // stem = "<16 hex digits>_<3 digits>_<3 digits>"
        if (stem.size() != 16 + 1 + 3 + 1 + 3)
            return std::nullopt;
        if (stem[16] != '_' || stem[20] != '_')
            return std::nullopt;

        Key key;
        try
        {
            key.sourceHash = std::stoull(stem.substr(0, 16), nullptr, 16);
            unsigned target = std::stoul(stem.substr(17, 3));
            unsigned stage = std::stoul(stem.substr(21, 3));
            if (target > 0xFFu || stage > 0xFFu)
                return std::nullopt;
            key.target = static_cast<uint8_t>(target);
            key.stage = static_cast<uint8_t>(stage);
        }
        catch (...)
        {
            return std::nullopt;
        }
        return key;
    }

    bool ShaderService::WriteBlobFile(const Key& key, const std::vector<uint8_t>& blob)
    {
        // Atomic-ish write: stage to a tmp file, then rename over the target.
        // Avoids leaving a half-written blob visible to concurrent readers if
        // the daemon crashes mid-write.
        auto finalPath = BlobPath(key);
        auto tmpPath = finalPath;
        tmpPath += ".tmp";

        {
            std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
            if (!out)
                return false;
            if (!blob.empty())
                out.write(reinterpret_cast<const char*>(blob.data()), static_cast<std::streamsize>(blob.size()));
            if (!out)
                return false;
        }

        std::error_code ec;
        std::filesystem::rename(tmpPath, finalPath, ec);
        if (ec)
        {
            std::filesystem::remove(tmpPath, ec);
            return false;
        }
        return true;
    }

    void ShaderService::DeleteAllBlobFiles()
    {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(m_cacheDir, ec))
        {
            if (ec)
                return;
            if (!entry.is_regular_file())
                continue;
            if (entry.path().extension() != ".blob")
                continue;
            std::filesystem::remove(entry.path(), ec);
            // Ignore individual failures — best effort.
        }
    }

} // namespace Spark::Daemon
