/**
 * @file TestShaderServiceClient.cpp
 * @brief End-to-end loopback tests for the Phase 2a Shader service.
 *
 * Spins up a DaemonServer with ControlService + ShaderService, connects
 * via DaemonClient, exercises GetCacheEntry / PutCacheEntry / ClearCache /
 * GetCacheStats through the typed ShaderServiceClient facade.
 */

#include "TestFramework.h"

#if defined(__linux__) || defined(__APPLE__)

#include "Utils/DaemonClient.h"
#include "Utils/ShaderServiceClient.h"

#include "ControlService.h"
#include "DaemonServer.h"
#include "ShaderService.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace
{
    std::string UniqueShaderSockPath(const char* tag)
    {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "/tmp/spark-shader-svc-%s-%d.sock", tag, static_cast<int>(::getpid()));
        return buf;
    }

    bool WaitForShaderSocket(const std::string& path, std::chrono::milliseconds timeout)
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

    struct ShaderServiceFixture
    {
        std::unique_ptr<Spark::Daemon::DaemonServer> server;
        std::thread thread;
        std::string sockPath;
        Spark::Daemon::DaemonClient client;

        explicit ShaderServiceFixture(const char* tag) : sockPath(UniqueShaderSockPath(tag))
        {
            server = std::make_unique<Spark::Daemon::DaemonServer>();
            server->AddService(std::make_unique<Spark::Daemon::ControlService>(server->GetShouldStopFlag()));
            server->AddService(std::make_unique<Spark::Daemon::ShaderService>());
            thread = std::thread([this] { (void)server->Run(sockPath); });
        }

        bool Ready() { return WaitForShaderSocket(sockPath, std::chrono::milliseconds(2000)); }

        ~ShaderServiceFixture()
        {
            client.Disconnect();
            if (server)
                server->Stop();
            if (thread.joinable())
                thread.join();
            ::unlink(sockPath.c_str());
        }
    };
} // namespace

TEST(ShaderService_PutThenGetRoundTrip)
{
    ShaderServiceFixture fx("rt");
    EXPECT_TRUE(fx.Ready());
    EXPECT_TRUE(fx.client.Connect(fx.sockPath).has_value());

    Spark::Daemon::ShaderServiceClient shader(fx.client);

    std::vector<uint8_t> blob{0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x23, 0x45, 0x67};
    auto put = shader.PutCacheEntry(0x1122334455667788ull, /*target*/ 1, /*stage*/ 0, blob);
    EXPECT_TRUE(put.has_value());

    auto get = shader.GetCacheEntry(0x1122334455667788ull, 1, 0);
    EXPECT_TRUE(get.has_value());
    EXPECT_TRUE(get->found);
    EXPECT_EQ(get->blob.size(), blob.size());
    for (size_t i = 0; i < blob.size(); ++i)
        EXPECT_EQ(get->blob[i], blob[i]);
}

TEST(ShaderService_MissReturnsEmptyNotFound)
{
    ShaderServiceFixture fx("miss");
    EXPECT_TRUE(fx.Ready());
    EXPECT_TRUE(fx.client.Connect(fx.sockPath).has_value());

    Spark::Daemon::ShaderServiceClient shader(fx.client);
    auto get = shader.GetCacheEntry(/*sourceHash*/ 0xFEEDFACEull, /*target*/ 2, /*stage*/ 1);
    EXPECT_TRUE(get.has_value());
    EXPECT_FALSE(get->found);
    EXPECT_EQ(get->blob.size(), 0u);
}

TEST(ShaderService_StatsReflectHitsAndMisses)
{
    ShaderServiceFixture fx("stats");
    EXPECT_TRUE(fx.Ready());
    EXPECT_TRUE(fx.client.Connect(fx.sockPath).has_value());

    Spark::Daemon::ShaderServiceClient shader(fx.client);

    std::vector<uint8_t> blob{1, 2, 3, 4};
    EXPECT_TRUE(shader.PutCacheEntry(42ull, 0, 0, blob).has_value());

    EXPECT_TRUE(shader.GetCacheEntry(42ull, 0, 0).has_value()); // hit
    EXPECT_TRUE(shader.GetCacheEntry(43ull, 0, 0).has_value()); // miss
    EXPECT_TRUE(shader.GetCacheEntry(44ull, 0, 0).has_value()); // miss

    auto stats = shader.GetCacheStats();
    EXPECT_TRUE(stats.has_value());
    EXPECT_EQ(stats->entryCount, 1u);
    EXPECT_EQ(stats->totalBytes, blob.size());
    EXPECT_EQ(stats->hitCount, 1u);
    EXPECT_EQ(stats->missCount, 2u);
}

TEST(ShaderService_ClearDropsAllEntries)
{
    ShaderServiceFixture fx("clear");
    EXPECT_TRUE(fx.Ready());
    EXPECT_TRUE(fx.client.Connect(fx.sockPath).has_value());

    Spark::Daemon::ShaderServiceClient shader(fx.client);

    EXPECT_TRUE(shader.PutCacheEntry(1ull, 0, 0, {9, 9, 9}).has_value());
    EXPECT_TRUE(shader.PutCacheEntry(2ull, 0, 0, {8, 8}).has_value());

    auto beforeClear = shader.GetCacheStats();
    EXPECT_TRUE(beforeClear.has_value());
    EXPECT_EQ(beforeClear->entryCount, 2u);

    EXPECT_TRUE(shader.ClearCache().has_value());

    auto afterClear = shader.GetCacheStats();
    EXPECT_TRUE(afterClear.has_value());
    EXPECT_EQ(afterClear->entryCount, 0u);
    EXPECT_EQ(afterClear->totalBytes, 0u);
    EXPECT_EQ(afterClear->hitCount, 0u);
    EXPECT_EQ(afterClear->missCount, 0u);

    auto get = shader.GetCacheEntry(1ull, 0, 0);
    EXPECT_TRUE(get.has_value());
    EXPECT_FALSE(get->found);
}

TEST(ShaderService_OverwriteReplacesBlob)
{
    ShaderServiceFixture fx("overwrite");
    EXPECT_TRUE(fx.Ready());
    EXPECT_TRUE(fx.client.Connect(fx.sockPath).has_value());

    Spark::Daemon::ShaderServiceClient shader(fx.client);

    EXPECT_TRUE(shader.PutCacheEntry(7ull, 1, 1, std::vector<uint8_t>{1, 2, 3}).has_value());
    EXPECT_TRUE(shader.PutCacheEntry(7ull, 1, 1, std::vector<uint8_t>{9, 9, 9, 9, 9}).has_value());

    auto get = shader.GetCacheEntry(7ull, 1, 1);
    EXPECT_TRUE(get.has_value());
    EXPECT_TRUE(get->found);
    EXPECT_EQ(get->blob.size(), 5u);
    EXPECT_EQ(get->blob[0], 9u);

    auto stats = shader.GetCacheStats();
    EXPECT_TRUE(stats.has_value());
    EXPECT_EQ(stats->entryCount, 1u);
    EXPECT_EQ(stats->totalBytes, 5u);
}

TEST(ShaderService_DifferentTargetsAreDistinctEntries)
{
    ShaderServiceFixture fx("targets");
    EXPECT_TRUE(fx.Ready());
    EXPECT_TRUE(fx.client.Connect(fx.sockPath).has_value());

    Spark::Daemon::ShaderServiceClient shader(fx.client);

    // Same hash, different target values — must not collide.
    EXPECT_TRUE(shader.PutCacheEntry(100ull, /*target=DXBC*/ 1, 0, {0xAA}).has_value());
    EXPECT_TRUE(shader.PutCacheEntry(100ull, /*target=SPIRV*/ 2, 0, {0xBB, 0xBB}).has_value());
    EXPECT_TRUE(shader.PutCacheEntry(100ull, /*target=GLSL*/ 3, 0, {0xCC, 0xCC, 0xCC}).has_value());

    auto dxbc = shader.GetCacheEntry(100ull, 1, 0);
    auto spirv = shader.GetCacheEntry(100ull, 2, 0);
    auto glsl = shader.GetCacheEntry(100ull, 3, 0);
    EXPECT_TRUE(dxbc && dxbc->found && dxbc->blob.size() == 1u);
    EXPECT_TRUE(spirv && spirv->found && spirv->blob.size() == 2u);
    EXPECT_TRUE(glsl && glsl->found && glsl->blob.size() == 3u);

    auto stats = shader.GetCacheStats();
    EXPECT_TRUE(stats.has_value());
    EXPECT_EQ(stats->entryCount, 3u);
}

// =========================================================================
// Phase 2b — disk persistence
// =========================================================================

#include <filesystem>
#include <fstream>

namespace
{
    std::filesystem::path UniqueCacheDir(const char* tag)
    {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "/tmp/spark-shader-cache-%s-%d", tag, static_cast<int>(::getpid()));
        std::filesystem::path p(buf);
        std::error_code ec;
        std::filesystem::remove_all(p, ec);
        return p;
    }

    /// Fixture that owns a disk-backed daemon on the same socket+cache for the
    /// full test — optionally allows restarting the daemon to simulate reload.
    struct PersistentShaderFixture
    {
        std::string sockPath;
        std::filesystem::path cacheDir;
        std::unique_ptr<Spark::Daemon::DaemonServer> server;
        std::thread thread;

        explicit PersistentShaderFixture(const char* tag)
            : sockPath(UniqueShaderSockPath(tag)), cacheDir(UniqueCacheDir(tag))
        {
            StartServer();
        }

        void StartServer()
        {
            server = std::make_unique<Spark::Daemon::DaemonServer>();
            server->AddService(std::make_unique<Spark::Daemon::ControlService>(server->GetShouldStopFlag()));
            auto shaderSvc = std::make_unique<Spark::Daemon::ShaderService>();
            auto loaded = shaderSvc->Initialize(cacheDir);
            EXPECT_TRUE(loaded.has_value());
            server->AddService(std::move(shaderSvc));
            thread = std::thread([this] { (void)server->Run(sockPath); });
            EXPECT_TRUE(WaitForShaderSocket(sockPath, std::chrono::milliseconds(2000)));
        }

        void RestartServer()
        {
            if (server)
                server->Stop();
            if (thread.joinable())
                thread.join();
            server.reset();
            // Give the kernel a moment to release the socket path before rebinding.
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            StartServer();
        }

        ~PersistentShaderFixture()
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

TEST(ShaderService_PutWritesFileToDisk)
{
    PersistentShaderFixture fx("put-disk");

    Spark::Daemon::DaemonClient client;
    EXPECT_TRUE(client.Connect(fx.sockPath).has_value());
    Spark::Daemon::ShaderServiceClient shader(client);

    EXPECT_TRUE(shader.PutCacheEntry(0xABCDEFull, /*target*/ 1, /*stage*/ 2, {1, 2, 3, 4, 5}).has_value());

    // Exactly one .blob file should exist, with non-zero size.
    size_t blobCount = 0;
    size_t totalBytes = 0;
    for (const auto& entry : std::filesystem::directory_iterator(fx.cacheDir))
    {
        if (entry.path().extension() == ".blob")
        {
            ++blobCount;
            totalBytes += entry.file_size();
        }
    }
    EXPECT_EQ(blobCount, 1u);
    EXPECT_EQ(totalBytes, 5u);
}

TEST(ShaderService_ReloadsFromDiskOnRestart)
{
    PersistentShaderFixture fx("reload");

    {
        Spark::Daemon::DaemonClient client;
        EXPECT_TRUE(client.Connect(fx.sockPath).has_value());
        Spark::Daemon::ShaderServiceClient shader(client);
        EXPECT_TRUE(shader.PutCacheEntry(0x1111ull, 0, 0, {0xA0, 0xA1}).has_value());
        EXPECT_TRUE(shader.PutCacheEntry(0x2222ull, 1, 1, {0xB0, 0xB1, 0xB2}).has_value());
        EXPECT_TRUE(shader.PutCacheEntry(0x3333ull, 2, 2, {0xC0, 0xC1, 0xC2, 0xC3}).has_value());
    }

    // Tear down and restart the server — warm-cache entries should survive.
    fx.RestartServer();

    Spark::Daemon::DaemonClient client;
    EXPECT_TRUE(client.Connect(fx.sockPath).has_value());
    Spark::Daemon::ShaderServiceClient shader(client);

    auto stats = shader.GetCacheStats();
    EXPECT_TRUE(stats.has_value());
    EXPECT_EQ(stats->entryCount, 3u);
    EXPECT_EQ(stats->totalBytes, 2u + 3u + 4u);

    auto first = shader.GetCacheEntry(0x1111ull, 0, 0);
    auto second = shader.GetCacheEntry(0x2222ull, 1, 1);
    auto third = shader.GetCacheEntry(0x3333ull, 2, 2);
    EXPECT_TRUE(first && first->found && first->blob.size() == 2u && first->blob[0] == 0xA0u);
    EXPECT_TRUE(second && second->found && second->blob.size() == 3u && second->blob[1] == 0xB1u);
    EXPECT_TRUE(third && third->found && third->blob.size() == 4u && third->blob[3] == 0xC3u);
}

TEST(ShaderService_ClearRemovesBlobFiles)
{
    PersistentShaderFixture fx("clear-disk");

    Spark::Daemon::DaemonClient client;
    EXPECT_TRUE(client.Connect(fx.sockPath).has_value());
    Spark::Daemon::ShaderServiceClient shader(client);

    EXPECT_TRUE(shader.PutCacheEntry(1ull, 0, 0, {1}).has_value());
    EXPECT_TRUE(shader.PutCacheEntry(2ull, 0, 0, {2}).has_value());

    size_t beforeClear = 0;
    for (const auto& entry : std::filesystem::directory_iterator(fx.cacheDir))
        if (entry.path().extension() == ".blob")
            ++beforeClear;
    EXPECT_EQ(beforeClear, 2u);

    EXPECT_TRUE(shader.ClearCache().has_value());

    size_t afterClear = 0;
    for (const auto& entry : std::filesystem::directory_iterator(fx.cacheDir))
        if (entry.path().extension() == ".blob")
            ++afterClear;
    EXPECT_EQ(afterClear, 0u);
}

TEST(ShaderService_OverwriteReplacesFileContents)
{
    PersistentShaderFixture fx("overwrite-disk");

    Spark::Daemon::DaemonClient client;
    EXPECT_TRUE(client.Connect(fx.sockPath).has_value());
    Spark::Daemon::ShaderServiceClient shader(client);

    EXPECT_TRUE(shader.PutCacheEntry(77ull, 3, 3, std::vector<uint8_t>(100, 0xAA)).has_value());
    EXPECT_TRUE(shader.PutCacheEntry(77ull, 3, 3, std::vector<uint8_t>(50, 0xBB)).has_value());

    fx.RestartServer();

    Spark::Daemon::DaemonClient client2;
    EXPECT_TRUE(client2.Connect(fx.sockPath).has_value());
    Spark::Daemon::ShaderServiceClient shader2(client2);

    auto get = shader2.GetCacheEntry(77ull, 3, 3);
    EXPECT_TRUE(get && get->found);
    EXPECT_EQ(get->blob.size(), 50u);
    EXPECT_EQ(get->blob[0], 0xBBu);

    auto stats = shader2.GetCacheStats();
    EXPECT_TRUE(stats && stats->entryCount == 1u && stats->totalBytes == 50u);
}

TEST(ShaderService_MalformedFilenameIsIgnoredOnLoad)
{
    // Pre-populate the cache dir with a junk file + one valid blob, then start
    // the daemon. Only the valid blob should be loaded.
    auto cacheDir = UniqueCacheDir("junk");
    std::filesystem::create_directories(cacheDir);

    // Valid filename: hash=0x0000000012345678, target=001, stage=002
    {
        std::ofstream out(cacheDir / "0000000012345678_001_002.blob", std::ios::binary);
        const char bytes[] = {'O', 'K', '!'};
        out.write(bytes, sizeof(bytes));
    }
    // Garbage filename — right extension, wrong format.
    {
        std::ofstream out(cacheDir / "not-a-valid-name.blob", std::ios::binary);
        out.write("junk", 4);
    }
    // Wrong extension entirely.
    {
        std::ofstream out(cacheDir / "0000000011111111_001_002.txt", std::ios::binary);
        out.write("other", 5);
    }

    Spark::Daemon::ShaderService svc;
    auto loaded = svc.Initialize(cacheDir);
    EXPECT_TRUE(loaded.has_value());
    EXPECT_EQ(*loaded, 1u);
    EXPECT_EQ(svc.GetEntryCount(), 1u);

    std::error_code ec;
    std::filesystem::remove_all(cacheDir, ec);
}

#endif // POSIX
