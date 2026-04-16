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

#include "../SparkDaemon/src/ControlService.h"
#include "../SparkDaemon/src/DaemonServer.h"
#include "../SparkDaemon/src/ShaderService.h"

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

#endif // POSIX
