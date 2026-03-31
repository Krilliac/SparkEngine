// TestLocalFileCache.cpp - Tests for the in-memory local file cache
// Tests read, write, create, delete, LRU eviction, and metrics

#include "TestFramework.h"
#include "Utils/LocalFileCache.h"
#include <cstdio>
#include <filesystem>

static std::string GetCacheTestDir()
{
    return (std::filesystem::temp_directory_path() / "spark_cache_test").string();
}

static void CleanupCacheTestDir()
{
    std::error_code ec;
    std::filesystem::remove_all(GetCacheTestDir(), ec);
}

static void SetupCacheTestDir()
{
    CleanupCacheTestDir();
    Spark::FileUtils::CreateDirectories(GetCacheTestDir());
}

// =============================================================================
// Read Tests
// =============================================================================

TEST(LocalFileCache_ReadTextMissAndHit)
{
    SetupCacheTestDir();
    std::string path = GetCacheTestDir() + "/read_text.txt";
    Spark::FileUtils::WriteTextFile(path, "Hello Cache");

    Spark::LocalFileCache cache;

    // First read: miss
    auto r1 = cache.ReadText(path);
    EXPECT_TRUE(r1.IsOk());
    EXPECT_EQ(r1.Value(), std::string("Hello Cache"));

    auto metrics = cache.GetMetrics();
    EXPECT_EQ(metrics.misses, (uint64_t)1);
    EXPECT_EQ(metrics.hits, (uint64_t)0);

    // Second read: hit
    auto r2 = cache.ReadText(path);
    EXPECT_TRUE(r2.IsOk());
    EXPECT_EQ(r2.Value(), std::string("Hello Cache"));

    metrics = cache.GetMetrics();
    EXPECT_EQ(metrics.hits, (uint64_t)1);
    EXPECT_EQ(metrics.misses, (uint64_t)1);

    CleanupCacheTestDir();
}

TEST(LocalFileCache_ReadBinaryMissAndHit)
{
    SetupCacheTestDir();
    std::string path = GetCacheTestDir() + "/read_binary.bin";
    std::vector<uint8_t> data = {0xDE, 0xAD, 0xBE, 0xEF};
    Spark::FileUtils::WriteBinaryFile(path, data);

    Spark::LocalFileCache cache;

    auto r1 = cache.ReadBinary(path);
    EXPECT_TRUE(r1.IsOk());
    EXPECT_EQ((int)r1.Value().size(), 4);
    EXPECT_EQ(r1.Value()[0], (uint8_t)0xDE);
    EXPECT_EQ(r1.Value()[3], (uint8_t)0xEF);

    auto r2 = cache.ReadBinary(path);
    EXPECT_TRUE(r2.IsOk());
    EXPECT_EQ(cache.GetMetrics().hits, (uint64_t)1);

    CleanupCacheTestDir();
}

TEST(LocalFileCache_ReadNonexistentReturnsError)
{
    Spark::LocalFileCache cache;
    auto result = cache.ReadText("/tmp/spark_nonexistent_cache_file_xyz.txt");
    EXPECT_TRUE(result.IsErr());
}

// =============================================================================
// Write Tests
// =============================================================================

TEST(LocalFileCache_WriteTextUpdatesCache)
{
    SetupCacheTestDir();
    std::string path = GetCacheTestDir() + "/write_text.txt";

    Spark::LocalFileCache cache;
    auto w = cache.WriteText(path, "Written content");
    EXPECT_TRUE(w.IsOk());

    // Subsequent read should be a cache hit
    auto r = cache.ReadText(path);
    EXPECT_TRUE(r.IsOk());
    EXPECT_EQ(r.Value(), std::string("Written content"));
    EXPECT_EQ(cache.GetMetrics().hits, (uint64_t)1);
    EXPECT_EQ(cache.GetMetrics().misses, (uint64_t)0);

    // Verify file is actually on disk
    auto disk = Spark::FileUtils::ReadTextFile(path);
    EXPECT_TRUE(disk.has_value());
    EXPECT_EQ(disk.value(), std::string("Written content"));

    CleanupCacheTestDir();
}

TEST(LocalFileCache_WriteBinaryUpdatesCache)
{
    SetupCacheTestDir();
    std::string path = GetCacheTestDir() + "/write_binary.bin";
    std::vector<uint8_t> data = {0x01, 0x02, 0x03};

    Spark::LocalFileCache cache;
    auto w = cache.WriteBinary(path, data);
    EXPECT_TRUE(w.IsOk());

    auto r = cache.ReadBinary(path);
    EXPECT_TRUE(r.IsOk());
    EXPECT_EQ((int)r.Value().size(), 3);
    EXPECT_EQ(cache.GetMetrics().hits, (uint64_t)1);

    CleanupCacheTestDir();
}

// =============================================================================
// Create Tests
// =============================================================================

TEST(LocalFileCache_CreateNewFile)
{
    SetupCacheTestDir();
    std::string path = GetCacheTestDir() + "/created.txt";

    Spark::LocalFileCache cache;
    auto c = cache.Create(path, "Initial content");
    EXPECT_TRUE(c.IsOk());

    // Should be cached
    auto r = cache.ReadText(path);
    EXPECT_TRUE(r.IsOk());
    EXPECT_EQ(r.Value(), std::string("Initial content"));
    EXPECT_EQ(cache.GetMetrics().hits, (uint64_t)1);

    CleanupCacheTestDir();
}

TEST(LocalFileCache_CreateFailsIfExists)
{
    SetupCacheTestDir();
    std::string path = GetCacheTestDir() + "/existing.txt";
    Spark::FileUtils::WriteTextFile(path, "existing");

    Spark::LocalFileCache cache;
    auto c = cache.Create(path, "new content");
    EXPECT_TRUE(c.IsErr());

    CleanupCacheTestDir();
}

TEST(LocalFileCache_CreateEmptyFile)
{
    SetupCacheTestDir();
    std::string path = GetCacheTestDir() + "/empty.txt";

    Spark::LocalFileCache cache;
    auto c = cache.Create(path);
    EXPECT_TRUE(c.IsOk());
    EXPECT_TRUE(Spark::FileUtils::FileExists(path));

    CleanupCacheTestDir();
}

// =============================================================================
// Delete Tests
// =============================================================================

TEST(LocalFileCache_DeleteRemovesFromDiskAndCache)
{
    SetupCacheTestDir();
    std::string path = GetCacheTestDir() + "/to_delete.txt";
    Spark::FileUtils::WriteTextFile(path, "delete me");

    Spark::LocalFileCache cache;
    // Read to populate cache
    cache.ReadText(path);
    EXPECT_TRUE(cache.Contains(path));

    auto d = cache.Delete(path);
    EXPECT_TRUE(d.IsOk());
    EXPECT_FALSE(cache.Contains(path));
    EXPECT_FALSE(Spark::FileUtils::FileExists(path));

    CleanupCacheTestDir();
}

TEST(LocalFileCache_DeleteNonexistentReturnsError)
{
    Spark::LocalFileCache cache;
    auto d = cache.Delete("/tmp/spark_nonexistent_delete_xyz.txt");
    EXPECT_TRUE(d.IsErr());
}

// =============================================================================
// Invalidate and Clear Tests
// =============================================================================

TEST(LocalFileCache_InvalidateEvictsButKeepsFile)
{
    SetupCacheTestDir();
    std::string path = GetCacheTestDir() + "/invalidate.txt";
    Spark::FileUtils::WriteTextFile(path, "keep on disk");

    Spark::LocalFileCache cache;
    cache.ReadText(path);
    EXPECT_TRUE(cache.Contains(path));

    cache.Invalidate(path);
    EXPECT_FALSE(cache.Contains(path));
    EXPECT_TRUE(Spark::FileUtils::FileExists(path));

    CleanupCacheTestDir();
}

TEST(LocalFileCache_ClearEvictsAll)
{
    SetupCacheTestDir();
    Spark::FileUtils::WriteTextFile(GetCacheTestDir() + "/a.txt", "a");
    Spark::FileUtils::WriteTextFile(GetCacheTestDir() + "/b.txt", "b");

    Spark::LocalFileCache cache;
    cache.ReadText(GetCacheTestDir() + "/a.txt");
    cache.ReadText(GetCacheTestDir() + "/b.txt");
    EXPECT_EQ(cache.GetEntryCount(), (size_t)2);

    cache.Clear();
    EXPECT_EQ(cache.GetEntryCount(), (size_t)0);
    EXPECT_EQ(cache.GetCurrentSize(), (size_t)0);

    CleanupCacheTestDir();
}

// =============================================================================
// LRU Eviction Tests
// =============================================================================

TEST(LocalFileCache_LRUEviction)
{
    SetupCacheTestDir();

    // Create files with known sizes
    std::string pathA = GetCacheTestDir() + "/lru_a.txt";
    std::string pathB = GetCacheTestDir() + "/lru_b.txt";
    std::string pathC = GetCacheTestDir() + "/lru_c.txt";

    std::string contentA(100, 'A'); // 100 bytes
    std::string contentB(100, 'B'); // 100 bytes
    std::string contentC(100, 'C'); // 100 bytes

    Spark::FileUtils::WriteTextFile(pathA, contentA);
    Spark::FileUtils::WriteTextFile(pathB, contentB);
    Spark::FileUtils::WriteTextFile(pathC, contentC);

    // Cache with 250 byte limit (fits 2 entries but not 3)
    Spark::LocalFileCache cache(250);

    cache.ReadText(pathA); // A in cache
    cache.ReadText(pathB); // A, B in cache

    EXPECT_EQ(cache.GetEntryCount(), (size_t)2);

    cache.ReadText(pathC); // Evicts A (LRU), B and C remain
    EXPECT_EQ(cache.GetEntryCount(), (size_t)2);
    EXPECT_GT(cache.GetMetrics().evictions, (uint64_t)0);

    // A should have been evicted; reading it again is a miss
    auto r = cache.ReadText(pathA);
    EXPECT_TRUE(r.IsOk());
    EXPECT_EQ(cache.GetMetrics().misses, (uint64_t)4); // A, B, C misses + A re-read

    CleanupCacheTestDir();
}

TEST(LocalFileCache_SetMaxSizeTriggersEviction)
{
    SetupCacheTestDir();

    std::string pathA = GetCacheTestDir() + "/resize_a.txt";
    std::string pathB = GetCacheTestDir() + "/resize_b.txt";

    Spark::FileUtils::WriteTextFile(pathA, std::string(100, 'A'));
    Spark::FileUtils::WriteTextFile(pathB, std::string(100, 'B'));

    Spark::LocalFileCache cache(1024);
    cache.ReadText(pathA);
    cache.ReadText(pathB);
    EXPECT_EQ(cache.GetEntryCount(), (size_t)2);

    // Shrink below current usage
    cache.SetMaxSize(100);
    EXPECT_LE(cache.GetCurrentSize(), (size_t)100);
    EXPECT_GT(cache.GetMetrics().evictions, (uint64_t)0);

    CleanupCacheTestDir();
}

// =============================================================================
// Metrics Tests
// =============================================================================

TEST(LocalFileCache_MetricsTotalBytesServed)
{
    SetupCacheTestDir();
    std::string path = GetCacheTestDir() + "/metrics.txt";
    std::string content = "12345"; // 5 bytes
    Spark::FileUtils::WriteTextFile(path, content);

    Spark::LocalFileCache cache;
    cache.ReadText(path); // miss, 5 bytes served
    cache.ReadText(path); // hit, 5 bytes served

    auto metrics = cache.GetMetrics();
    EXPECT_EQ(metrics.totalBytesServed, (uint64_t)10);

    CleanupCacheTestDir();
}

// =============================================================================
// Contains
// =============================================================================

TEST(LocalFileCache_ContainsAfterWrite)
{
    SetupCacheTestDir();
    std::string path = GetCacheTestDir() + "/contains.txt";

    Spark::LocalFileCache cache;
    EXPECT_FALSE(cache.Contains(path));

    cache.WriteText(path, "content");
    EXPECT_TRUE(cache.Contains(path));

    CleanupCacheTestDir();
}

// =============================================================================
// GetCurrentSize and GetEntryCount
// =============================================================================

TEST(LocalFileCache_CurrentSizeTracking)
{
    SetupCacheTestDir();
    std::string pathA = GetCacheTestDir() + "/size_a.txt";
    std::string pathB = GetCacheTestDir() + "/size_b.txt";

    Spark::LocalFileCache cache;
    cache.WriteText(pathA, "AAAA"); // 4 bytes
    cache.WriteText(pathB, "BB");   // 2 bytes

    EXPECT_EQ(cache.GetCurrentSize(), (size_t)6);
    EXPECT_EQ(cache.GetEntryCount(), (size_t)2);

    cache.Invalidate(pathA);
    EXPECT_EQ(cache.GetCurrentSize(), (size_t)2);
    EXPECT_EQ(cache.GetEntryCount(), (size_t)1);

    CleanupCacheTestDir();
}

// =============================================================================
// Write updates existing cache entry
// =============================================================================

TEST(LocalFileCache_WriteUpdatesExistingEntry)
{
    SetupCacheTestDir();
    std::string path = GetCacheTestDir() + "/update.txt";

    Spark::LocalFileCache cache;
    cache.WriteText(path, "original");

    auto r1 = cache.ReadText(path);
    EXPECT_TRUE(r1.IsOk());
    EXPECT_EQ(r1.Value(), std::string("original"));

    // Overwrite
    cache.WriteText(path, "updated");

    auto r2 = cache.ReadText(path);
    EXPECT_TRUE(r2.IsOk());
    EXPECT_EQ(r2.Value(), std::string("updated"));

    // Both reads should be cache hits
    EXPECT_EQ(cache.GetMetrics().hits, (uint64_t)2);

    CleanupCacheTestDir();
}

// =============================================================================
// ReadBinary non-existent
// =============================================================================

TEST(LocalFileCache_ReadBinaryNonexistent)
{
    Spark::LocalFileCache cache;
    auto result = cache.ReadBinary("/tmp/spark_nonexistent_binary_xyz.bin");
    EXPECT_TRUE(result.IsErr());
}

// =============================================================================
// Create with subdirectory
// =============================================================================

TEST(LocalFileCache_CreateInSubdir)
{
    SetupCacheTestDir();
    std::string path = GetCacheTestDir() + "/subdir/nested.txt";

    Spark::LocalFileCache cache;
    auto c = cache.Create(path, "nested content");
    EXPECT_TRUE(c.IsOk());
    EXPECT_TRUE(cache.Contains(path));

    auto r = cache.ReadText(path);
    EXPECT_TRUE(r.IsOk());
    EXPECT_EQ(r.Value(), std::string("nested content"));

    CleanupCacheTestDir();
}

// =============================================================================
// LRU touch order
// =============================================================================

TEST(LocalFileCache_LRUTouchOrder)
{
    SetupCacheTestDir();

    std::string pathA = GetCacheTestDir() + "/touch_a.txt";
    std::string pathB = GetCacheTestDir() + "/touch_b.txt";
    std::string pathC = GetCacheTestDir() + "/touch_c.txt";

    Spark::FileUtils::WriteTextFile(pathA, std::string(100, 'A'));
    Spark::FileUtils::WriteTextFile(pathB, std::string(100, 'B'));
    Spark::FileUtils::WriteTextFile(pathC, std::string(100, 'C'));

    // Cache with 250 byte limit
    Spark::LocalFileCache cache(250);

    cache.ReadText(pathA); // A
    cache.ReadText(pathB); // A, B

    // Touch A again to make B the LRU
    cache.ReadText(pathA); // B, A (A is now MRU)

    // Adding C should evict B (LRU), not A
    cache.ReadText(pathC); // A, C

    EXPECT_TRUE(cache.Contains(pathA));
    EXPECT_FALSE(cache.Contains(pathB));
    EXPECT_TRUE(cache.Contains(pathC));

    CleanupCacheTestDir();
}

// =============================================================================
// Default max size constant
// =============================================================================

TEST(LocalFileCache_DefaultMaxSize)
{
    EXPECT_EQ(Spark::LocalFileCache::DEFAULT_MAX_SIZE, static_cast<size_t>(64 * 1024 * 1024));
}
