/**
 * @file TestShaderDiskCacheDaemon.cpp
 * @brief End-to-end tests for the ShaderDiskCache <-> daemon integration.
 *
 * Covers the three behaviours the wiring must guarantee:
 *   1. When a daemon client is attached, a lookup that hits the daemon
 *      returns without touching local disk (cross-process cache sharing).
 *   2. A daemon miss transparently falls through to the local disk cache.
 *   3. `Store` writes locally AND pushes to the daemon so other engines
 *      see warm entries on their next lookup.
 *   4. With no daemon client attached, behaviour is unchanged from
 *      pre-daemon builds.
 */

#include "TestFramework.h"

#if defined(__linux__) || defined(__APPLE__)

#include "Graphics/ShaderDaemonBridge.h"
#include "Graphics/ShaderDiskCache.h"
#include "Utils/DaemonClient.h"
#include "Utils/DaemonConnection.h"
#include "Utils/ShaderServiceClient.h"

#include "ControlService.h"
#include "DaemonServer.h"
#include "ShaderService.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace
{
    std::string UniqueDCSocketPath(const char* tag)
    {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "/tmp/spark-sdc-daemon-%s-%d.sock", tag, static_cast<int>(::getpid()));
        return buf;
    }

    std::filesystem::path UniqueLocalCache(const char* tag)
    {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "/tmp/spark-sdc-local-%s-%d", tag, static_cast<int>(::getpid()));
        std::filesystem::path p(buf);
        std::error_code ec;
        std::filesystem::remove_all(p, ec);
        return p;
    }

    bool WaitForSdcSocket(const std::string& path, std::chrono::milliseconds timeout)
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

    /// Shared daemon fixture: daemon + one connected client that can be used
    /// by multiple ShaderDiskCache instances within the same test.
    struct DaemonFixture
    {
        std::unique_ptr<Spark::Daemon::DaemonServer> server;
        std::thread thread;
        std::string sockPath;
        Spark::Daemon::DaemonClient client;

        explicit DaemonFixture(const char* tag) : sockPath(UniqueDCSocketPath(tag))
        {
            server = std::make_unique<Spark::Daemon::DaemonServer>();
            server->AddService(std::make_unique<Spark::Daemon::ControlService>(server->GetShouldStopFlag()));
            server->AddService(std::make_unique<Spark::Daemon::ShaderService>());
            thread = std::thread([this] { (void)server->Run(sockPath); });
            EXPECT_TRUE(WaitForSdcSocket(sockPath, std::chrono::milliseconds(2000)));
            EXPECT_TRUE(client.Connect(sockPath).has_value());
        }

        ~DaemonFixture()
        {
            client.Disconnect();
            if (server)
                server->Stop();
            if (thread.joinable())
                thread.join();
            ::unlink(sockPath.c_str());
        }
    };

    Spark::Graphics::ShaderSource MakeSampleSource()
    {
        Spark::Graphics::ShaderSource src;
        src.hlslCode = "float4 VSMain() : SV_POSITION { return 0; }";
        src.entryPoint = "VSMain";
        src.stage = Spark::Graphics::ShaderStage::Vertex;
        src.defines = {"USE_HDR=1"};
        return src;
    }

    Spark::Graphics::CompiledShaderBlob MakeSampleBlob(std::vector<uint8_t> bytecode)
    {
        Spark::Graphics::CompiledShaderBlob blob;
        blob.bytecode = std::move(bytecode);
        blob.target = Spark::Graphics::ShaderTarget::DXBC;
        blob.stage = Spark::Graphics::ShaderStage::Vertex;
        blob.entryPoint = "VSMain";
        blob.success = true;
        blob.inputCount = 3;
        blob.outputCount = 1;
        blob.cbufferCount = 2;
        return blob;
    }
} // namespace

// =========================================================================
// ShaderDaemonBridge codec
// =========================================================================

TEST(ShaderDaemonBridge_EncodeDecodeRoundTrip)
{
    auto blob = MakeSampleBlob({0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03});
    blob.errors = "warning: trivial shader";

    auto bytes = Spark::Graphics::EncodeCompiledShaderBlob(blob);
    EXPECT_TRUE(bytes.size() > 0u);

    Spark::Graphics::CompiledShaderBlob decoded;
    EXPECT_TRUE(Spark::Graphics::DecodeCompiledShaderBlob(bytes, decoded));

    EXPECT_EQ(decoded.bytecode.size(), blob.bytecode.size());
    for (size_t i = 0; i < blob.bytecode.size(); ++i)
        EXPECT_EQ(decoded.bytecode[i], blob.bytecode[i]);
    EXPECT_EQ(static_cast<int>(decoded.target), static_cast<int>(blob.target));
    EXPECT_EQ(static_cast<int>(decoded.stage), static_cast<int>(blob.stage));
    EXPECT_EQ(decoded.entryPoint, blob.entryPoint);
    EXPECT_EQ(decoded.errors, blob.errors);
    EXPECT_EQ(decoded.success, blob.success);
    EXPECT_EQ(decoded.inputCount, blob.inputCount);
    EXPECT_EQ(decoded.outputCount, blob.outputCount);
    EXPECT_EQ(decoded.cbufferCount, blob.cbufferCount);
}

TEST(ShaderDaemonBridge_RejectsUnknownVersion)
{
    std::vector<uint8_t> garbage{0xFFu, 0u, 0u, 0u};
    Spark::Graphics::CompiledShaderBlob out;
    EXPECT_FALSE(Spark::Graphics::DecodeCompiledShaderBlob(garbage, out));
}

// =========================================================================
// ShaderDiskCache daemon integration
// =========================================================================

TEST(ShaderDiskCache_StoreAlsoPushesToDaemon)
{
    DaemonFixture fx("store-push");
    Spark::Daemon::ShaderServiceClient shaderClient(fx.client);

    auto localDir = UniqueLocalCache("store-push");
    Spark::Graphics::ShaderDiskCache cache;
    cache.Initialize(localDir);
    cache.SetDaemonClient(&shaderClient);

    auto src = MakeSampleSource();
    auto blob = MakeSampleBlob({0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7});
    cache.Store(src, Spark::Graphics::ShaderTarget::DXBC, blob);

    // Local disk got the bytecode-only file.
    EXPECT_EQ(cache.GetEntryCount(), 1u);

    // Daemon got the full blob — verify via a fresh, daemon-only cache.
    Spark::Graphics::ShaderDiskCache cache2;
    auto localDir2 = UniqueLocalCache("store-push2");
    cache2.Initialize(localDir2);
    cache2.SetDaemonClient(&shaderClient);

    auto hit = cache2.Lookup(src, Spark::Graphics::ShaderTarget::DXBC);
    EXPECT_TRUE(hit.has_value());
    EXPECT_EQ(hit->bytecode.size(), blob.bytecode.size());
    EXPECT_EQ(hit->inputCount, blob.inputCount); // Metadata survived daemon round-trip
    EXPECT_EQ(cache2.GetDaemonHits(), 1u);
    EXPECT_EQ(cache2.GetDaemonMisses(), 0u);
    // cache2's local disk is empty — the hit came entirely from the daemon.
    EXPECT_EQ(cache2.GetEntryCount(), 0u);

    std::error_code ec;
    std::filesystem::remove_all(localDir, ec);
    std::filesystem::remove_all(localDir2, ec);
}

TEST(ShaderDiskCache_DaemonMissFallsThroughToLocalDisk)
{
    DaemonFixture fx("fallthrough");
    Spark::Daemon::ShaderServiceClient shaderClient(fx.client);

    auto localDir = UniqueLocalCache("fallthrough");
    Spark::Graphics::ShaderDiskCache cache;
    cache.Initialize(localDir);

    // Store locally WITHOUT a daemon client — only the local disk has the bytes.
    auto src = MakeSampleSource();
    auto blob = MakeSampleBlob({0xB0, 0xB1, 0xB2});
    cache.Store(src, Spark::Graphics::ShaderTarget::SPIRV, blob);
    EXPECT_EQ(cache.GetEntryCount(), 1u);

    // Now attach a daemon client that doesn't have the entry.
    cache.SetDaemonClient(&shaderClient);
    auto hit = cache.Lookup(src, Spark::Graphics::ShaderTarget::SPIRV);
    EXPECT_TRUE(hit.has_value());
    EXPECT_EQ(hit->bytecode.size(), blob.bytecode.size());

    // Daemon was asked and reported a miss; we fell through to local disk.
    EXPECT_EQ(cache.GetDaemonHits(), 0u);
    EXPECT_EQ(cache.GetDaemonMisses(), 1u);

    std::error_code ec;
    std::filesystem::remove_all(localDir, ec);
}

TEST(ShaderDiskCache_NoDaemonBehavesAsBefore)
{
    auto localDir = UniqueLocalCache("no-daemon");
    Spark::Graphics::ShaderDiskCache cache;
    cache.Initialize(localDir);
    // Intentionally no SetDaemonClient().

    auto src = MakeSampleSource();
    auto blob = MakeSampleBlob({0xC0, 0xC1});
    cache.Store(src, Spark::Graphics::ShaderTarget::DXBC, blob);

    auto hit = cache.Lookup(src, Spark::Graphics::ShaderTarget::DXBC);
    EXPECT_TRUE(hit.has_value());
    EXPECT_EQ(hit->bytecode.size(), blob.bytecode.size());

    EXPECT_EQ(cache.GetDaemonHits(), 0u);
    EXPECT_EQ(cache.GetDaemonMisses(), 0u);

    std::error_code ec;
    std::filesystem::remove_all(localDir, ec);
}

// =========================================================================
// DaemonConnection singleton
// =========================================================================

TEST(DaemonConnection_TryConnectReturnsFalseWhenSocketMissing)
{
    auto& conn = Spark::Daemon::DaemonConnection::Instance();
    conn.Shutdown();
    EXPECT_FALSE(conn.IsConnected());
    EXPECT_FALSE(conn.TryConnect("/tmp/spark-does-not-exist-123456.sock"));
    EXPECT_FALSE(conn.IsConnected());
    EXPECT_EQ(conn.GetClient(), nullptr);
}

TEST(DaemonConnection_TryConnectSucceedsOnLiveDaemon)
{
    DaemonFixture fx("conn-live");

    auto& conn = Spark::Daemon::DaemonConnection::Instance();
    conn.Shutdown();
    EXPECT_TRUE(conn.TryConnect(fx.sockPath));
    EXPECT_TRUE(conn.IsConnected());
    EXPECT_TRUE(conn.GetClient() != nullptr);

    // Reuse the shared client to ping through the Control service.
    auto pingResult = conn.GetClient()->Ping();
    EXPECT_TRUE(pingResult.has_value());

    conn.Shutdown();
    EXPECT_FALSE(conn.IsConnected());
    EXPECT_EQ(conn.GetClient(), nullptr);
}

TEST(DaemonConnection_TryConnectIsIdempotent)
{
    DaemonFixture fx("conn-idem");

    auto& conn = Spark::Daemon::DaemonConnection::Instance();
    conn.Shutdown();
    EXPECT_TRUE(conn.TryConnect(fx.sockPath));
    // Second call must return true without disturbing the existing connection.
    EXPECT_TRUE(conn.TryConnect(fx.sockPath));
    EXPECT_TRUE(conn.IsConnected());
    conn.Shutdown();
}

#endif // POSIX
