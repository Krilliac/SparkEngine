/**
 * @file TestAssetServiceClient.cpp
 * @brief Loopback + persistence tests for the Phase 3a Asset service.
 *
 * Covers the in-memory path (GetAsset/PutAsset/InvalidateAsset/GetCacheStats)
 * and the disk-backed path (file written on put, reloaded on restart,
 * removed on invalidate).
 */

#include "TestFramework.h"

#if defined(__linux__) || defined(__APPLE__)

#include "Utils/AssetServiceClient.h"
#include "Utils/DaemonClient.h"

#include "../SparkDaemon/src/AssetService.h"
#include "../SparkDaemon/src/ControlService.h"
#include "../SparkDaemon/src/DaemonServer.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace
{
    std::string UniqueAssetSockPath(const char* tag)
    {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "/tmp/spark-asset-svc-%s-%d.sock", tag, static_cast<int>(::getpid()));
        return buf;
    }

    std::filesystem::path UniqueAssetCacheDir(const char* tag)
    {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "/tmp/spark-asset-cache-%s-%d", tag, static_cast<int>(::getpid()));
        std::filesystem::path p(buf);
        std::error_code ec;
        std::filesystem::remove_all(p, ec);
        return p;
    }

    bool WaitForAssetSocket(const std::string& path, std::chrono::milliseconds timeout)
    {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            struct stat st;
            if (::stat(path.c_str(), &st) == 0)
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

    /// In-memory-only fixture: no cache dir, clean state per test.
    struct InMemoryAssetFixture
    {
        std::unique_ptr<Spark::Daemon::DaemonServer> server;
        std::thread thread;
        std::string sockPath;
        Spark::Daemon::DaemonClient client;

        explicit InMemoryAssetFixture(const char* tag) : sockPath(UniqueAssetSockPath(tag))
        {
            server = std::make_unique<Spark::Daemon::DaemonServer>();
            server->AddService(std::make_unique<Spark::Daemon::ControlService>(server->GetShouldStopFlag()));
            server->AddService(std::make_unique<Spark::Daemon::AssetService>());
            thread = std::thread([this] { (void)server->Run(sockPath); });
        }

        bool Ready() { return WaitForAssetSocket(sockPath, std::chrono::milliseconds(2000)); }

        ~InMemoryAssetFixture()
        {
            client.Disconnect();
            if (server)
                server->Stop();
            if (thread.joinable())
                thread.join();
            ::unlink(sockPath.c_str());
        }
    };

    /// Disk-backed fixture — owns a cache dir, supports RestartServer() to
    /// verify persistence round-trips.
    struct DiskAssetFixture
    {
        std::string sockPath;
        std::filesystem::path cacheDir;
        std::unique_ptr<Spark::Daemon::DaemonServer> server;
        std::thread thread;

        explicit DiskAssetFixture(const char* tag)
            : sockPath(UniqueAssetSockPath(tag)), cacheDir(UniqueAssetCacheDir(tag))
        {
            StartServer();
        }

        void StartServer()
        {
            server = std::make_unique<Spark::Daemon::DaemonServer>();
            server->AddService(std::make_unique<Spark::Daemon::ControlService>(server->GetShouldStopFlag()));
            auto svc = std::make_unique<Spark::Daemon::AssetService>();
            auto loaded = svc->Initialize(cacheDir);
            EXPECT_TRUE(loaded.has_value());
            server->AddService(std::move(svc));
            thread = std::thread([this] { (void)server->Run(sockPath); });
            EXPECT_TRUE(WaitForAssetSocket(sockPath, std::chrono::milliseconds(2000)));
        }

        void RestartServer()
        {
            if (server)
                server->Stop();
            if (thread.joinable())
                thread.join();
            server.reset();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            StartServer();
        }

        ~DiskAssetFixture()
        {
            if (server)
                server->Stop();
            if (thread.joinable())
                thread.join();
            ::unlink(sockPath.c_str());
            std::error_code ec;
            std::filesystem::remove_all(cacheDir, ec);
        }
    };
} // namespace

// =========================================================================
// In-memory behaviour
// =========================================================================

TEST(AssetService_PutThenGetRoundTrip)
{
    InMemoryAssetFixture fx("rt");
    EXPECT_TRUE(fx.Ready());
    EXPECT_TRUE(fx.client.Connect(fx.sockPath).has_value());

    Spark::Daemon::AssetServiceClient asset(fx.client);

    std::vector<uint8_t> blob{'h', 'e', 'l', 'l', 'o'};
    EXPECT_TRUE(asset.PutAsset("Textures/brick.png", /*platform*/ 1, blob).has_value());

    auto get = asset.GetAsset("Textures/brick.png", 1);
    EXPECT_TRUE(get.has_value());
    EXPECT_TRUE(get->found);
    EXPECT_EQ(get->blob.size(), blob.size());
    for (size_t i = 0; i < blob.size(); ++i)
        EXPECT_EQ(get->blob[i], blob[i]);
}

TEST(AssetService_MissReturnsFoundFalse)
{
    InMemoryAssetFixture fx("miss");
    EXPECT_TRUE(fx.Ready());
    EXPECT_TRUE(fx.client.Connect(fx.sockPath).has_value());

    Spark::Daemon::AssetServiceClient asset(fx.client);
    auto get = asset.GetAsset("does/not/exist.png", 0);
    EXPECT_TRUE(get.has_value());
    EXPECT_FALSE(get->found);
    EXPECT_EQ(get->blob.size(), 0u);
}

TEST(AssetService_DifferentPlatformsAreDistinct)
{
    InMemoryAssetFixture fx("plats");
    EXPECT_TRUE(fx.Ready());
    EXPECT_TRUE(fx.client.Connect(fx.sockPath).has_value());

    Spark::Daemon::AssetServiceClient asset(fx.client);

    EXPECT_TRUE(asset.PutAsset("mesh.obj", /*Windows*/ 1, {0xAA}).has_value());
    EXPECT_TRUE(asset.PutAsset("mesh.obj", /*Linux*/ 2, {0xBB, 0xBB}).has_value());
    EXPECT_TRUE(asset.PutAsset("mesh.obj", /*Android*/ 3, {0xCC, 0xCC, 0xCC}).has_value());

    auto win = asset.GetAsset("mesh.obj", 1);
    auto lin = asset.GetAsset("mesh.obj", 2);
    auto and_ = asset.GetAsset("mesh.obj", 3);
    EXPECT_TRUE(win && win->found && win->blob.size() == 1u);
    EXPECT_TRUE(lin && lin->found && lin->blob.size() == 2u);
    EXPECT_TRUE(and_ && and_->found && and_->blob.size() == 3u);

    auto stats = asset.GetCacheStats();
    EXPECT_TRUE(stats.has_value());
    EXPECT_EQ(stats->entryCount, 3u);
}

TEST(AssetService_InvalidateDropsAllPlatformVariants)
{
    InMemoryAssetFixture fx("invalidate");
    EXPECT_TRUE(fx.Ready());
    EXPECT_TRUE(fx.client.Connect(fx.sockPath).has_value());

    Spark::Daemon::AssetServiceClient asset(fx.client);

    EXPECT_TRUE(asset.PutAsset("sound.wav", 1, {1}).has_value());
    EXPECT_TRUE(asset.PutAsset("sound.wav", 2, {2}).has_value());
    EXPECT_TRUE(asset.PutAsset("sound.wav", 3, {3}).has_value());
    EXPECT_TRUE(asset.PutAsset("other.wav", 1, {0xFF}).has_value()); // unrelated, must survive

    auto removed = asset.InvalidateAsset("sound.wav");
    EXPECT_TRUE(removed.has_value());
    EXPECT_EQ(*removed, 3u);

    auto stats = asset.GetCacheStats();
    EXPECT_TRUE(stats && stats->entryCount == 1u);

    // All three variants now miss.
    for (uint8_t p : {1u, 2u, 3u})
    {
        auto get = asset.GetAsset("sound.wav", static_cast<uint8_t>(p));
        EXPECT_TRUE(get.has_value());
        EXPECT_FALSE(get->found);
    }

    // The unrelated entry is still there.
    auto other = asset.GetAsset("other.wav", 1);
    EXPECT_TRUE(other && other->found && other->blob.size() == 1u);
}

TEST(AssetService_InvalidateUnknownPathReturnsZero)
{
    InMemoryAssetFixture fx("invalidate-miss");
    EXPECT_TRUE(fx.Ready());
    EXPECT_TRUE(fx.client.Connect(fx.sockPath).has_value());

    Spark::Daemon::AssetServiceClient asset(fx.client);
    auto removed = asset.InvalidateAsset("never-existed.bin");
    EXPECT_TRUE(removed.has_value());
    EXPECT_EQ(*removed, 0u);
}

TEST(AssetService_StatsHitsAndMisses)
{
    InMemoryAssetFixture fx("stats");
    EXPECT_TRUE(fx.Ready());
    EXPECT_TRUE(fx.client.Connect(fx.sockPath).has_value());

    Spark::Daemon::AssetServiceClient asset(fx.client);

    EXPECT_TRUE(asset.PutAsset("a", 0, {1, 2, 3}).has_value());

    EXPECT_TRUE(asset.GetAsset("a", 0).has_value()); // hit
    EXPECT_TRUE(asset.GetAsset("b", 0).has_value()); // miss
    EXPECT_TRUE(asset.GetAsset("c", 0).has_value()); // miss

    auto stats = asset.GetCacheStats();
    EXPECT_TRUE(stats.has_value());
    EXPECT_EQ(stats->entryCount, 1u);
    EXPECT_EQ(stats->totalBytes, 3u);
    EXPECT_EQ(stats->hitCount, 1u);
    EXPECT_EQ(stats->missCount, 2u);
}

// =========================================================================
// Disk persistence
// =========================================================================

TEST(AssetService_PutWritesFileToDisk)
{
    DiskAssetFixture fx("disk-put");

    Spark::Daemon::DaemonClient client;
    EXPECT_TRUE(client.Connect(fx.sockPath).has_value());
    Spark::Daemon::AssetServiceClient asset(client);

    EXPECT_TRUE(asset.PutAsset("level1/mesh.obj", 2, {0xAA, 0xBB, 0xCC}).has_value());

    size_t blobCount = 0;
    for (const auto& entry : std::filesystem::directory_iterator(fx.cacheDir))
        if (entry.path().extension() == ".asset")
            ++blobCount;
    EXPECT_EQ(blobCount, 1u);
}

TEST(AssetService_ReloadsFromDiskOnRestart)
{
    DiskAssetFixture fx("disk-reload");

    {
        Spark::Daemon::DaemonClient client;
        EXPECT_TRUE(client.Connect(fx.sockPath).has_value());
        Spark::Daemon::AssetServiceClient asset(client);
        EXPECT_TRUE(asset.PutAsset("a/b/c.png", 1, {1, 2}).has_value());
        EXPECT_TRUE(asset.PutAsset("a/b/c.png", 2, {3, 4, 5}).has_value());
        EXPECT_TRUE(asset.PutAsset("other.wav", 1, {9}).has_value());
    }

    fx.RestartServer();

    Spark::Daemon::DaemonClient client;
    EXPECT_TRUE(client.Connect(fx.sockPath).has_value());
    Spark::Daemon::AssetServiceClient asset(client);

    auto stats = asset.GetCacheStats();
    EXPECT_TRUE(stats && stats->entryCount == 3u && stats->totalBytes == 2u + 3u + 1u);

    auto v1 = asset.GetAsset("a/b/c.png", 1);
    auto v2 = asset.GetAsset("a/b/c.png", 2);
    auto v3 = asset.GetAsset("other.wav", 1);
    EXPECT_TRUE(v1 && v1->found && v1->blob.size() == 2u && v1->blob[0] == 1u);
    EXPECT_TRUE(v2 && v2->found && v2->blob.size() == 3u && v2->blob[1] == 4u);
    EXPECT_TRUE(v3 && v3->found && v3->blob.size() == 1u && v3->blob[0] == 9u);
}

TEST(AssetService_InvalidateRemovesFilesFromDisk)
{
    DiskAssetFixture fx("disk-invalidate");

    Spark::Daemon::DaemonClient client;
    EXPECT_TRUE(client.Connect(fx.sockPath).has_value());
    Spark::Daemon::AssetServiceClient asset(client);

    EXPECT_TRUE(asset.PutAsset("tex.png", 1, {1}).has_value());
    EXPECT_TRUE(asset.PutAsset("tex.png", 2, {2}).has_value());
    EXPECT_TRUE(asset.PutAsset("other.png", 1, {3}).has_value());

    size_t before = 0;
    for (const auto& entry : std::filesystem::directory_iterator(fx.cacheDir))
        if (entry.path().extension() == ".asset")
            ++before;
    EXPECT_EQ(before, 3u);

    auto removed = asset.InvalidateAsset("tex.png");
    EXPECT_TRUE(removed && *removed == 2u);

    size_t after = 0;
    for (const auto& entry : std::filesystem::directory_iterator(fx.cacheDir))
        if (entry.path().extension() == ".asset")
            ++after;
    EXPECT_EQ(after, 1u); // only other.png survives
}

TEST(AssetService_LongPathSurvivesDiskRoundTrip)
{
    // Edge case: ensure paths longer than a typical filename are encoded
    // correctly via the hash-in-filename, path-in-file layout.
    DiskAssetFixture fx("disk-longpath");

    std::string longPath;
    longPath.reserve(500);
    for (int i = 0; i < 20; ++i)
        longPath += "deep/nested/directory/";
    longPath += "asset.bin";

    {
        Spark::Daemon::DaemonClient client;
        EXPECT_TRUE(client.Connect(fx.sockPath).has_value());
        Spark::Daemon::AssetServiceClient asset(client);
        EXPECT_TRUE(asset.PutAsset(longPath, 1, {0xEF, 0xBE, 0xAD, 0xDE}).has_value());
    }

    fx.RestartServer();

    Spark::Daemon::DaemonClient client;
    EXPECT_TRUE(client.Connect(fx.sockPath).has_value());
    Spark::Daemon::AssetServiceClient asset(client);

    auto get = asset.GetAsset(longPath, 1);
    EXPECT_TRUE(get && get->found);
    EXPECT_EQ(get->blob.size(), 4u);
    EXPECT_EQ(get->blob[0], 0xEFu);
}

// =========================================================================
// ClearCache (Phase 6 follow-up)
// =========================================================================

TEST(AssetService_ClearCacheDropsAllEntriesAndZerosStats)
{
    InMemoryAssetFixture fx("clear");
    EXPECT_TRUE(fx.Ready());
    EXPECT_TRUE(fx.client.Connect(fx.sockPath).has_value());

    Spark::Daemon::AssetServiceClient asset(fx.client);
    EXPECT_TRUE(asset.PutAsset("a.png", 0, {1, 2, 3}).has_value());
    EXPECT_TRUE(asset.PutAsset("b.png", 0, {4, 5, 6}).has_value());
    EXPECT_TRUE(asset.GetAsset("a.png", 0).has_value()); // bump hit counter

    auto before = asset.GetCacheStats();
    EXPECT_TRUE(before && before->entryCount == 2u);
    EXPECT_TRUE(before->hitCount > 0u);

    EXPECT_TRUE(asset.ClearCache().has_value());

    auto after = asset.GetCacheStats();
    EXPECT_TRUE(after);
    EXPECT_EQ(after->entryCount, 0u);
    EXPECT_EQ(after->totalBytes, 0u);
    EXPECT_EQ(after->hitCount, 0u);
    EXPECT_EQ(after->missCount, 0u);
    EXPECT_EQ(after->evictionCount, 0u);

    // Previously-stored entries are now missing.
    auto get = asset.GetAsset("a.png", 0);
    EXPECT_TRUE(get && !get->found);
}

TEST(AssetService_ClearCacheRemovesDiskFiles)
{
    DiskAssetFixture fx("clear-disk");

    Spark::Daemon::DaemonClient client;
    EXPECT_TRUE(client.Connect(fx.sockPath).has_value());
    Spark::Daemon::AssetServiceClient asset(client);

    EXPECT_TRUE(asset.PutAsset("one.bin", 0, {0xAA}).has_value());
    EXPECT_TRUE(asset.PutAsset("two.bin", 0, {0xBB}).has_value());

    size_t before = 0;
    for (const auto& entry : std::filesystem::directory_iterator(fx.cacheDir))
        if (entry.path().extension() == ".asset")
            ++before;
    EXPECT_EQ(before, 2u);

    EXPECT_TRUE(asset.ClearCache().has_value());

    size_t after = 0;
    for (const auto& entry : std::filesystem::directory_iterator(fx.cacheDir))
        if (entry.path().extension() == ".asset")
            ++after;
    EXPECT_EQ(after, 0u);
}

TEST(AssetService_ClearCacheOnEmptyCacheIsNoOp)
{
    InMemoryAssetFixture fx("clear-empty");
    EXPECT_TRUE(fx.Ready());
    EXPECT_TRUE(fx.client.Connect(fx.sockPath).has_value());

    Spark::Daemon::AssetServiceClient asset(fx.client);
    EXPECT_TRUE(asset.ClearCache().has_value());

    auto stats = asset.GetCacheStats();
    EXPECT_TRUE(stats && stats->entryCount == 0u);
}

#endif // POSIX
