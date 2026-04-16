/**
 * @file TestDaemonLRU.cpp
 * @brief LRU eviction tests for the shader and asset cache services.
 *
 * Covers the Phase 5 invariants:
 *   - A Put that overflows the byte budget evicts oldest entries first.
 *   - A Get promotes the entry to most-recently-used so the next eviction
 *     targets something else.
 *   - Disk-backed caches remove evicted entries' files too.
 *   - A bigger-than-budget single Put evicts itself (cache stays clean).
 *   - SetMaxBytes called after Initialize trims the existing cache.
 *   - Stats counters are bumped per eviction.
 */

#include "TestFramework.h"

#if defined(__linux__) || defined(__APPLE__)

#include "../SparkDaemon/src/AssetService.h"
#include "../SparkDaemon/src/ShaderService.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace
{
    using namespace Spark::Daemon;

    std::filesystem::path UniqueLruCacheDir(const char* tag)
    {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "/tmp/spark-lru-%s-%d", tag, static_cast<int>(::getpid()));
        std::filesystem::path p(buf);
        std::error_code ec;
        std::filesystem::remove_all(p, ec);
        return p;
    }

    std::vector<uint8_t> Blob(size_t sizeBytes, uint8_t fill)
    {
        return std::vector<uint8_t>(sizeBytes, fill);
    }

    /// Drive the service directly (no socket / server). LRU is a pure data-
    /// structure invariant; going through the wire would just add 500ms per
    /// test for no extra coverage.
    GetCacheEntryResponse ShaderGet(ShaderService& svc, uint64_t hash, uint8_t target, uint8_t stage)
    {
        GetCacheEntryRequest req;
        req.key.sourceHash = hash;
        req.key.target = target;
        req.key.stage = stage;
        auto resp = svc.HandleMessage(static_cast<uint16_t>(ShaderMessage::GetCacheEntryRequest),
                                      EncodeGetCacheEntryRequest(req));
        GetCacheEntryResponse out;
        if (resp)
            (void)DecodeGetCacheEntryResponse(resp->payload, out);
        return out;
    }

    void ShaderPut(ShaderService& svc, uint64_t hash, uint8_t target, uint8_t stage, std::vector<uint8_t> blob)
    {
        PutCacheEntryRequest req;
        req.key.sourceHash = hash;
        req.key.target = target;
        req.key.stage = stage;
        req.blob = std::move(blob);
        svc.HandleMessage(static_cast<uint16_t>(ShaderMessage::PutCacheEntryRequest), EncodePutCacheEntryRequest(req));
    }

    ShaderCacheStats ShaderStats(ShaderService& svc)
    {
        auto resp = svc.HandleMessage(static_cast<uint16_t>(ShaderMessage::GetCacheStatsRequest), {});
        ShaderCacheStats out;
        if (resp)
            (void)DecodeShaderCacheStats(resp->payload, out);
        return out;
    }

    GetAssetResponse AssetGet(AssetService& svc, const std::string& path, uint8_t platform)
    {
        GetAssetRequest req;
        req.key.path = path;
        req.key.platform = platform;
        auto resp = svc.HandleMessage(static_cast<uint16_t>(AssetMessage::GetAssetRequest), EncodeGetAssetRequest(req));
        GetAssetResponse out;
        if (resp)
            (void)DecodeGetAssetResponse(resp->payload, out);
        return out;
    }

    void AssetPut(AssetService& svc, const std::string& path, uint8_t platform, std::vector<uint8_t> blob)
    {
        PutAssetRequest req;
        req.key.path = path;
        req.key.platform = platform;
        req.blob = std::move(blob);
        svc.HandleMessage(static_cast<uint16_t>(AssetMessage::PutAssetRequest), EncodePutAssetRequest(req));
    }

    AssetCacheStats AssetStats(AssetService& svc)
    {
        auto resp = svc.HandleMessage(static_cast<uint16_t>(AssetMessage::GetCacheStatsRequest), {});
        AssetCacheStats out;
        if (resp)
            (void)DecodeAssetCacheStats(resp->payload, out);
        return out;
    }

    size_t CountFilesWithExt(const std::filesystem::path& dir, const std::string& ext)
    {
        size_t count = 0;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
            if (entry.path().extension() == ext)
                ++count;
        return count;
    }
} // namespace

// =========================================================================
// ShaderService LRU
// =========================================================================

TEST(ShaderLRU_PutOverflowEvictsOldest)
{
    ShaderService svc;
    svc.SetMaxBytes(300); // holds 3 x 100-byte entries

    ShaderPut(svc, 1, 0, 0, Blob(100, 0x01));
    ShaderPut(svc, 2, 0, 0, Blob(100, 0x02));
    ShaderPut(svc, 3, 0, 0, Blob(100, 0x03));
    EXPECT_EQ(svc.GetEntryCount(), 3u);

    // Fourth put overflows — key=1 (oldest) is evicted.
    ShaderPut(svc, 4, 0, 0, Blob(100, 0x04));
    EXPECT_EQ(svc.GetEntryCount(), 3u);
    EXPECT_FALSE(ShaderGet(svc, 1, 0, 0).found);
    EXPECT_TRUE(ShaderGet(svc, 2, 0, 0).found);
    EXPECT_TRUE(ShaderGet(svc, 3, 0, 0).found);
    EXPECT_TRUE(ShaderGet(svc, 4, 0, 0).found);

    auto stats = ShaderStats(svc);
    EXPECT_EQ(stats.entryCount, 3u);
    EXPECT_EQ(stats.totalBytes, 300u);
}

TEST(ShaderLRU_GetPromotesRecency)
{
    ShaderService svc;
    svc.SetMaxBytes(300);

    ShaderPut(svc, 1, 0, 0, Blob(100, 0x01));
    ShaderPut(svc, 2, 0, 0, Blob(100, 0x02));
    ShaderPut(svc, 3, 0, 0, Blob(100, 0x03));

    // Touch key=1 so it's now most-recently-used; key=2 is oldest.
    EXPECT_TRUE(ShaderGet(svc, 1, 0, 0).found);

    // Overflow — key=2 should be evicted, NOT key=1.
    ShaderPut(svc, 4, 0, 0, Blob(100, 0x04));
    EXPECT_TRUE(ShaderGet(svc, 1, 0, 0).found);
    EXPECT_FALSE(ShaderGet(svc, 2, 0, 0).found);
    EXPECT_TRUE(ShaderGet(svc, 3, 0, 0).found);
    EXPECT_TRUE(ShaderGet(svc, 4, 0, 0).found);
}

TEST(ShaderLRU_OverBudgetSinglePutEvictsItself)
{
    ShaderService svc;
    svc.SetMaxBytes(100);

    ShaderPut(svc, 1, 0, 0, Blob(50, 0x01));
    EXPECT_EQ(svc.GetEntryCount(), 1u);

    // Single put bigger than the budget — the ONLY entry now is this one,
    // which exceeds budget, so it gets evicted itself. Cache ends empty.
    ShaderPut(svc, 2, 0, 0, Blob(200, 0x02));
    EXPECT_EQ(svc.GetEntryCount(), 0u);
    EXPECT_EQ(ShaderStats(svc).totalBytes, 0u);
}

TEST(ShaderLRU_SetMaxBytesTrimsExistingCache)
{
    ShaderService svc;
    // No budget yet — three entries accumulate.
    ShaderPut(svc, 1, 0, 0, Blob(100, 0x01));
    ShaderPut(svc, 2, 0, 0, Blob(100, 0x02));
    ShaderPut(svc, 3, 0, 0, Blob(100, 0x03));
    EXPECT_EQ(svc.GetEntryCount(), 3u);

    // Shrinking the budget after the fact must drop entries down to the new cap.
    svc.SetMaxBytes(150);
    EXPECT_TRUE(svc.GetEntryCount() <= 2u);
    EXPECT_TRUE(ShaderStats(svc).totalBytes <= 150u);
}

TEST(ShaderLRU_DiskFilesRemovedOnEviction)
{
    auto cacheDir = UniqueLruCacheDir("shader-disk");
    ShaderService svc;
    EXPECT_TRUE(svc.Initialize(cacheDir).has_value());
    svc.SetMaxBytes(300);

    ShaderPut(svc, 1, 0, 0, Blob(100, 0x01));
    ShaderPut(svc, 2, 0, 0, Blob(100, 0x02));
    ShaderPut(svc, 3, 0, 0, Blob(100, 0x03));
    EXPECT_EQ(CountFilesWithExt(cacheDir, ".blob"), 3u);

    ShaderPut(svc, 4, 0, 0, Blob(100, 0x04)); // evicts key=1
    EXPECT_EQ(CountFilesWithExt(cacheDir, ".blob"), 3u);
    EXPECT_EQ(svc.GetEntryCount(), 3u);

    std::error_code ec;
    std::filesystem::remove_all(cacheDir, ec);
}

// =========================================================================
// AssetService LRU
// =========================================================================

TEST(AssetLRU_PutOverflowEvictsOldest)
{
    AssetService svc;
    svc.SetMaxBytes(300);

    AssetPut(svc, "a.png", 0, Blob(100, 0x01));
    AssetPut(svc, "b.png", 0, Blob(100, 0x02));
    AssetPut(svc, "c.png", 0, Blob(100, 0x03));
    EXPECT_EQ(svc.GetEntryCount(), 3u);

    AssetPut(svc, "d.png", 0, Blob(100, 0x04)); // evicts a.png
    EXPECT_EQ(svc.GetEntryCount(), 3u);
    EXPECT_FALSE(AssetGet(svc, "a.png", 0).found);
    EXPECT_TRUE(AssetGet(svc, "b.png", 0).found);
    EXPECT_TRUE(AssetGet(svc, "c.png", 0).found);
    EXPECT_TRUE(AssetGet(svc, "d.png", 0).found);
}

TEST(AssetLRU_GetPromotesRecency)
{
    AssetService svc;
    svc.SetMaxBytes(300);

    AssetPut(svc, "a.png", 0, Blob(100, 0xAA));
    AssetPut(svc, "b.png", 0, Blob(100, 0xBB));
    AssetPut(svc, "c.png", 0, Blob(100, 0xCC));

    EXPECT_TRUE(AssetGet(svc, "a.png", 0).found); // promote a

    AssetPut(svc, "d.png", 0, Blob(100, 0xDD)); // evicts b (now oldest)
    EXPECT_TRUE(AssetGet(svc, "a.png", 0).found);
    EXPECT_FALSE(AssetGet(svc, "b.png", 0).found);
    EXPECT_TRUE(AssetGet(svc, "c.png", 0).found);
    EXPECT_TRUE(AssetGet(svc, "d.png", 0).found);
}

TEST(AssetLRU_InvalidateCoexistsWithEviction)
{
    // Invalidate-by-path should still work correctly after some eviction
    // pressure has rearranged the underlying list.
    AssetService svc;
    svc.SetMaxBytes(400);

    AssetPut(svc, "shared.png", 1, Blob(100, 0x11));
    AssetPut(svc, "shared.png", 2, Blob(100, 0x22));
    AssetPut(svc, "other.png", 1, Blob(100, 0x33));
    AssetPut(svc, "other.png", 2, Blob(100, 0x44));
    EXPECT_EQ(svc.GetEntryCount(), 4u);

    // Fifth put → evicts "shared.png"/platform 1 (oldest).
    AssetPut(svc, "new.png", 0, Blob(100, 0x55));
    EXPECT_EQ(svc.GetEntryCount(), 4u);

    // Invalidate "other.png" — both variants should go.
    InvalidateAssetRequest req;
    req.path = "other.png";
    auto resp = svc.HandleMessage(static_cast<uint16_t>(AssetMessage::InvalidateAssetRequest),
                                  EncodeInvalidateAssetRequest(req));
    EXPECT_TRUE(resp.has_value());
    InvalidateAssetResponse decoded;
    EXPECT_TRUE(DecodeInvalidateAssetResponse(resp->payload, decoded));
    EXPECT_EQ(decoded.removedCount, 2u);
    EXPECT_EQ(svc.GetEntryCount(), 2u);
}

TEST(AssetLRU_DiskFilesRemovedOnEviction)
{
    auto cacheDir = UniqueLruCacheDir("asset-disk");
    AssetService svc;
    EXPECT_TRUE(svc.Initialize(cacheDir).has_value());
    svc.SetMaxBytes(300);

    AssetPut(svc, "a.png", 0, Blob(100, 0x01));
    AssetPut(svc, "b.png", 0, Blob(100, 0x02));
    AssetPut(svc, "c.png", 0, Blob(100, 0x03));
    EXPECT_EQ(CountFilesWithExt(cacheDir, ".asset"), 3u);

    AssetPut(svc, "d.png", 0, Blob(100, 0x04));
    EXPECT_EQ(CountFilesWithExt(cacheDir, ".asset"), 3u);

    std::error_code ec;
    std::filesystem::remove_all(cacheDir, ec);
}

TEST(AssetLRU_ReloadAfterRestartRespectsLimit)
{
    // Persist past the limit, then reload with a limit — the in-memory
    // count should shrink to fit after SetMaxBytes.
    auto cacheDir = UniqueLruCacheDir("asset-reload-limit");
    {
        AssetService svc;
        EXPECT_TRUE(svc.Initialize(cacheDir).has_value());
        // No limit yet — everything fits on disk.
        AssetPut(svc, "1.png", 0, Blob(100, 0x11));
        AssetPut(svc, "2.png", 0, Blob(100, 0x22));
        AssetPut(svc, "3.png", 0, Blob(100, 0x33));
        AssetPut(svc, "4.png", 0, Blob(100, 0x44));
        EXPECT_EQ(CountFilesWithExt(cacheDir, ".asset"), 4u);
    }

    // Reload: everything comes back into memory...
    AssetService svc;
    auto loaded = svc.Initialize(cacheDir);
    EXPECT_TRUE(loaded.has_value());
    EXPECT_EQ(*loaded, 4u);

    // ...then the budget is applied and trims down.
    svc.SetMaxBytes(250);
    EXPECT_TRUE(svc.GetEntryCount() <= 2u);
    EXPECT_TRUE(AssetStats(svc).totalBytes <= 250u);

    // Trimmed entries also got their disk files removed.
    EXPECT_TRUE(CountFilesWithExt(cacheDir, ".asset") <= 2u);

    std::error_code ec;
    std::filesystem::remove_all(cacheDir, ec);
}

#endif // POSIX
