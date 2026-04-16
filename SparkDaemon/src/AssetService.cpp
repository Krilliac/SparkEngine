/**
 * @file AssetService.cpp
 * @brief Asset blob cache service (Phase 3a in-memory + 3a disk + Phase 5 LRU).
 */

#include "AssetService.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <system_error>

namespace Spark::Daemon
{

    std::optional<size_t> AssetService::Initialize(const std::filesystem::path& cacheDir)
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
            if (entry.path().extension() != ".asset")
                continue;

            Key key;
            std::vector<uint8_t> blob;
            if (!ReadBlobFile(entry.path(), key, blob))
                continue;

            m_totalBytes += blob.size();
            m_lruList.push_front(Entry{std::move(key), std::move(blob)});
            m_index.emplace(m_lruList.front().key, m_lruList.begin());
            ++loaded;
        }
        return loaded;
    }

    void AssetService::SetMaxBytes(uint64_t maxBytes)
    {
        std::lock_guard lock(m_mutex);
        m_maxBytes = maxBytes;
        EvictUntilUnderBudget();
    }

    std::optional<ServiceResponse> AssetService::HandleMessage(uint16_t messageType,
                                                               const std::vector<uint8_t>& payload)
    {
        switch (static_cast<AssetMessage>(messageType))
        {
        case AssetMessage::GetAssetRequest:
            return HandleGetAsset(payload);
        case AssetMessage::PutAssetRequest:
            return HandlePutAsset(payload);
        case AssetMessage::InvalidateAssetRequest:
            return HandleInvalidateAsset(payload);
        case AssetMessage::GetCacheStatsRequest:
            return HandleGetCacheStats();
        default:
            return MakeError("unsupported asset message");
        }
    }

    size_t AssetService::GetEntryCount() const
    {
        std::lock_guard lock(m_mutex);
        return m_lruList.size();
    }

    ServiceResponse AssetService::HandleGetAsset(const std::vector<uint8_t>& payload)
    {
        GetAssetRequest req;
        if (!DecodeGetAssetRequest(payload, req))
            return MakeError("malformed GetAsset request");

        Key key{req.key.path, req.key.platform};
        GetAssetResponse resp;
        {
            std::lock_guard lock(m_mutex);
            auto it = m_index.find(key);
            if (it != m_index.end())
            {
                resp.found = true;
                resp.blob = it->second->blob;
                m_lruList.splice(m_lruList.begin(), m_lruList, it->second);
            }
        }
        if (resp.found)
            m_hitCount.fetch_add(1, std::memory_order_relaxed);
        else
            m_missCount.fetch_add(1, std::memory_order_relaxed);

        ServiceResponse out;
        out.messageType = static_cast<uint16_t>(AssetMessage::GetAssetResponse);
        out.payload = EncodeGetAssetResponse(resp);
        return out;
    }

    ServiceResponse AssetService::HandlePutAsset(const std::vector<uint8_t>& payload)
    {
        PutAssetRequest req;
        if (!DecodePutAssetRequest(payload, req))
            return MakeError("malformed PutAsset request");

        Key key{req.key.path, req.key.platform};
        std::vector<uint8_t> blobCopyForDisk;
        bool entrySurvived = false;
        {
            std::lock_guard lock(m_mutex);
            InsertOrReplace(key, std::move(req.blob));
            EvictUntilUnderBudget();
            auto it = m_index.find(key);
            entrySurvived = (it != m_index.end());
            if (m_diskBacked && entrySurvived)
                blobCopyForDisk = it->second->blob;
        }

        if (m_diskBacked)
        {
            if (entrySurvived)
                WriteBlobFile(key, blobCopyForDisk);
            else
                DeleteBlobFile(key);
        }

        ServiceResponse out;
        out.messageType = static_cast<uint16_t>(AssetMessage::PutAssetResponse);
        return out;
    }

    ServiceResponse AssetService::HandleInvalidateAsset(const std::vector<uint8_t>& payload)
    {
        InvalidateAssetRequest req;
        if (!DecodeInvalidateAssetRequest(payload, req))
            return MakeError("malformed InvalidateAsset request");

        std::vector<Key> removedKeys;
        {
            std::lock_guard lock(m_mutex);
            for (auto it = m_lruList.begin(); it != m_lruList.end();)
            {
                if (it->key.path == req.path)
                {
                    m_totalBytes -= it->blob.size();
                    removedKeys.push_back(it->key);
                    m_index.erase(it->key);
                    it = m_lruList.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        if (m_diskBacked)
        {
            for (const auto& k : removedKeys)
                DeleteBlobFile(k);
        }

        InvalidateAssetResponse resp;
        resp.removedCount = static_cast<uint32_t>(removedKeys.size());

        ServiceResponse out;
        out.messageType = static_cast<uint16_t>(AssetMessage::InvalidateAssetResponse);
        out.payload = EncodeInvalidateAssetResponse(resp);
        return out;
    }

    ServiceResponse AssetService::HandleGetCacheStats()
    {
        AssetCacheStats stats;
        {
            std::lock_guard lock(m_mutex);
            stats.entryCount = m_lruList.size();
            stats.totalBytes = m_totalBytes;
        }
        stats.hitCount = m_hitCount.load(std::memory_order_relaxed);
        stats.missCount = m_missCount.load(std::memory_order_relaxed);

        ServiceResponse out;
        out.messageType = static_cast<uint16_t>(AssetMessage::GetCacheStatsResponse);
        out.payload = EncodeAssetCacheStats(stats);
        return out;
    }

    void AssetService::InsertOrReplace(Key key, std::vector<uint8_t> blob)
    {
        auto it = m_index.find(key);
        if (it != m_index.end())
        {
            m_totalBytes -= it->second->blob.size();
            it->second->blob = std::move(blob);
            m_totalBytes += it->second->blob.size();
            m_lruList.splice(m_lruList.begin(), m_lruList, it->second);
            return;
        }
        m_lruList.push_front(Entry{std::move(key), std::move(blob)});
        m_index.emplace(m_lruList.front().key, m_lruList.begin());
        m_totalBytes += m_lruList.front().blob.size();
    }

    void AssetService::EraseByIterator(EntryIter it)
    {
        m_totalBytes -= it->blob.size();
        m_index.erase(it->key);
        m_lruList.erase(it);
    }

    void AssetService::EvictUntilUnderBudget()
    {
        if (m_maxBytes == 0)
            return;
        while (m_totalBytes > m_maxBytes && !m_lruList.empty())
        {
            auto victimIter = std::prev(m_lruList.end());
            Key evictedKey = victimIter->key;
            EraseByIterator(victimIter);
            m_evictionCount.fetch_add(1, std::memory_order_relaxed);
            if (m_diskBacked)
                DeleteBlobFile(evictedKey);
        }
    }

    ServiceResponse AssetService::MakeError(const std::string& message) const
    {
        ServiceResponse r;
        r.messageType = static_cast<uint16_t>(ControlMessage::ErrorResponse);
        r.payload.assign(message.begin(), message.end());
        return r;
    }

    uint64_t AssetService::HashPath(const std::string& path) noexcept
    {
        // FNV-1a 64-bit. Simple, no external dep, good enough distribution for
        // a cache. Real collisions would only cost us a disk-file overwrite —
        // the in-memory map is keyed by full path so correctness is preserved.
        uint64_t h = 0xCBF29CE484222325ull;
        for (unsigned char c : path)
        {
            h ^= static_cast<uint64_t>(c);
            h *= 0x100000001B3ull;
        }
        return h;
    }

    std::filesystem::path AssetService::BlobPath(const Key& key) const
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%016llx_%03u.asset", static_cast<unsigned long long>(HashPath(key.path)),
                      static_cast<unsigned>(key.platform));
        return m_cacheDir / buf;
    }

    bool AssetService::WriteBlobFile(const Key& key, const std::vector<uint8_t>& blob)
    {
        auto finalPath = BlobPath(key);
        auto tmpPath = finalPath;
        tmpPath += ".tmp";

        {
            std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
            if (!out)
                return false;

            // Header: [u32 pathLen LE][pathBytes]
            uint32_t pathLen = static_cast<uint32_t>(key.path.size());
            uint8_t lenBytes[4] = {
                static_cast<uint8_t>(pathLen & 0xFFu),
                static_cast<uint8_t>((pathLen >> 8) & 0xFFu),
                static_cast<uint8_t>((pathLen >> 16) & 0xFFu),
                static_cast<uint8_t>((pathLen >> 24) & 0xFFu),
            };
            out.write(reinterpret_cast<const char*>(lenBytes), sizeof(lenBytes));
            if (!key.path.empty())
                out.write(key.path.data(), static_cast<std::streamsize>(key.path.size()));
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

    bool AssetService::ReadBlobFile(const std::filesystem::path& file, Key& outKey, std::vector<uint8_t>& outBlob)
    {
        std::ifstream in(file, std::ios::binary);
        if (!in)
            return false;

        // Filename carries the platform — parse it out first so we know which
        // bucket this entry belongs to even before reading the header.
        //
        // Strict grammar: "<16 hex digits>_<3 decimal digits>" (no trailing
        // extension — the ".asset" suffix was already stripped by stem()).
        // Each character is validated explicitly; std::stoul silently accepts
        // "1a2" as 1, so a handwritten check is the only way to reject
        // malformed filenames the Initialize-scan might encounter.
        auto stem = file.stem().string();
        if (stem.size() != 16 + 1 + 3 || stem[16] != '_')
            return false;
        auto isHex = [](char c) -> bool
        { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); };
        auto isDec = [](char c) -> bool { return c >= '0' && c <= '9'; };
        for (size_t i = 0; i < 16; ++i)
            if (!isHex(stem[i]))
                return false;
        for (size_t i = 17; i < 20; ++i)
            if (!isDec(stem[i]))
                return false;
        try
        {
            unsigned platform = std::stoul(stem.substr(17, 3));
            if (platform > 0xFFu)
                return false;
            outKey.platform = static_cast<uint8_t>(platform);
        }
        catch (...)
        {
            return false;
        }

        uint8_t lenBytes[4];
        in.read(reinterpret_cast<char*>(lenBytes), sizeof(lenBytes));
        if (!in)
            return false;
        uint32_t pathLen = static_cast<uint32_t>(lenBytes[0]) | (static_cast<uint32_t>(lenBytes[1]) << 8) |
                           (static_cast<uint32_t>(lenBytes[2]) << 16) | (static_cast<uint32_t>(lenBytes[3]) << 24);

        outKey.path.resize(pathLen);
        if (pathLen > 0)
            in.read(outKey.path.data(), static_cast<std::streamsize>(pathLen));
        if (!in && pathLen > 0)
            return false;

        // Remaining bytes = blob.
        in.seekg(0, std::ios::end);
        auto total = static_cast<size_t>(in.tellg());
        size_t header = 4 + pathLen;
        if (total < header)
            return false;
        size_t blobSize = total - header;

        in.seekg(static_cast<std::streamoff>(header), std::ios::beg);
        outBlob.resize(blobSize);
        if (blobSize > 0)
            in.read(reinterpret_cast<char*>(outBlob.data()), static_cast<std::streamsize>(blobSize));
        return !(!in && blobSize > 0);
    }

    void AssetService::DeleteBlobFile(const Key& key)
    {
        std::error_code ec;
        std::filesystem::remove(BlobPath(key), ec);
    }

} // namespace Spark::Daemon
