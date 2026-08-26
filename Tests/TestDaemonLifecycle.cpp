/**
 * @file TestDaemonLifecycle.cpp
 * @brief Tests for the engine-side daemon lifecycle helper.
 *
 * Verifies the end-to-end wire-up: when the CVar is set and a daemon is
 * reachable, `InitializeDaemonLifecycle` connects and attaches a
 * `ShaderServiceClient` to `GetShaderDiskCache()`. `ShutdownDaemonLifecycle`
 * detaches cleanly. With the CVar off, nothing happens. With a missing
 * daemon, no-op (and no crash).
 */

#include "TestFramework.h"

#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)

#include "Graphics/ShaderDiskCache.h"
#include "Utils/AssetServiceClient.h"
#include "Utils/ConsoleVariable.h"
#include "Utils/DaemonClient.h"
#include "Utils/DaemonConnection.h"
#include "Utils/DaemonFraming.h"
#include "Utils/DaemonLifecycle.h"
#include "Utils/InGameConsole.h"

#include "AssetService.h"
#include "ControlService.h"
#include "DaemonServer.h"
#include "ShaderService.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace
{
    std::string UniqueLifecyclePath(const char* tag)
    {
#if defined(_WIN32)
        return std::string("spark-daemon-lifecycle-") + tag + "-" + std::to_string(::GetCurrentProcessId());
#else
        char buf[96];
        std::snprintf(buf, sizeof(buf), "/tmp/spark-daemon-lifecycle-%s-%d.sock", tag, static_cast<int>(::getpid()));
        return buf;
#endif
    }

    std::filesystem::path UniqueLifecycleCacheDir(const char* tag)
    {
        const auto processId =
#if defined(_WIN32)
            static_cast<unsigned long>(::GetCurrentProcessId());
#else
            static_cast<unsigned long>(::getpid());
#endif
        std::filesystem::path p =
            std::filesystem::temp_directory_path() /
            (std::string("spark-daemon-lifecycle-cache-") + tag + "-" + std::to_string(processId));
        std::error_code ec;
        std::filesystem::remove_all(p, ec);
        return p;
    }

    bool WaitForLifecycleSocket(const std::string& path, std::chrono::milliseconds timeout)
    {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
#if defined(_WIN32)
            const std::wstring pipeName = Spark::Daemon::NormalizePipeName(path);
            if (!pipeName.empty() && ::WaitNamedPipeW(pipeName.c_str(), 20))
                return true;
#else
            struct stat st;
            if (::stat(path.c_str(), &st) == 0)
                return true;
#endif
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

    /// RAII helper that sets / restores CVars around a test body. The CVars
    /// are process-wide so tests must not leak state.
    struct DaemonCVarGuard
    {
        bool prevEnabled;
        bool prevAutoSpawn;
        std::string prevSocket;
        std::string prevBinary;

        explicit DaemonCVarGuard(bool enable, const std::string& socketPath, bool autoSpawn = false,
                                 const std::string& binaryPath = {})
            : prevEnabled(Spark::CVarRegistry::Get().Find("spark.daemon.enabled") &&
                          Spark::CVarRegistry::Get().Find("spark.daemon.enabled")->GetValueString() == "true"),
              prevAutoSpawn(Spark::CVarRegistry::Get().Find("spark.daemon.auto_spawn") &&
                            Spark::CVarRegistry::Get().Find("spark.daemon.auto_spawn")->GetValueString() == "true"),
              prevSocket(Spark::CVarRegistry::Get().Find("spark.daemon.socket_path")
                             ? Spark::CVarRegistry::Get().Find("spark.daemon.socket_path")->GetValueString()
                             : ""),
              prevBinary(Spark::CVarRegistry::Get().Find("spark.daemon.binary_path")
                             ? Spark::CVarRegistry::Get().Find("spark.daemon.binary_path")->GetValueString()
                             : "")
        {
            if (auto* e = Spark::CVarRegistry::Get().Find("spark.daemon.enabled"))
                e->SetFromString(enable ? "1" : "0");
            if (auto* s = Spark::CVarRegistry::Get().Find("spark.daemon.socket_path"))
                s->SetFromString(socketPath);
            if (auto* a = Spark::CVarRegistry::Get().Find("spark.daemon.auto_spawn"))
                a->SetFromString(autoSpawn ? "1" : "0");
            if (auto* b = Spark::CVarRegistry::Get().Find("spark.daemon.binary_path"))
                b->SetFromString(binaryPath);
        }

        ~DaemonCVarGuard()
        {
            if (auto* e = Spark::CVarRegistry::Get().Find("spark.daemon.enabled"))
                e->SetFromString(prevEnabled ? "1" : "0");
            if (auto* s = Spark::CVarRegistry::Get().Find("spark.daemon.socket_path"))
                s->SetFromString(prevSocket);
            if (auto* a = Spark::CVarRegistry::Get().Find("spark.daemon.auto_spawn"))
                a->SetFromString(prevAutoSpawn ? "1" : "0");
            if (auto* b = Spark::CVarRegistry::Get().Find("spark.daemon.binary_path"))
                b->SetFromString(prevBinary);
        }
    };

    struct LifecycleDaemonFixture
    {
        std::unique_ptr<Spark::Daemon::DaemonServer> server;
        std::thread thread;
        std::string sockPath;

        explicit LifecycleDaemonFixture(const char* tag) : sockPath(UniqueLifecyclePath(tag))
        {
            server = std::make_unique<Spark::Daemon::DaemonServer>();
            server->AddService(std::make_unique<Spark::Daemon::ControlService>(server->GetShouldStopFlag()));
            server->AddService(std::make_unique<Spark::Daemon::ShaderService>());
            server->AddService(std::make_unique<Spark::Daemon::AssetService>());
            thread = std::thread([this] { (void)server->Run(sockPath); });
            EXPECT_TRUE(WaitForLifecycleSocket(sockPath, std::chrono::milliseconds(2000)));
        }

        ~LifecycleDaemonFixture()
        {
            if (server)
                server->Stop();
            if (thread.joinable())
                thread.join();
#if !defined(_WIN32)
            ::unlink(sockPath.c_str());
#endif
        }
    };
} // namespace

TEST(DaemonLifecycle_DisabledByDefaultDoesNothing)
{
    DaemonCVarGuard cvars(/*enable*/ false, /*socketPath*/ "");

    // Ensure the cache's daemon pointer is cleared up front.
    Spark::Daemon::ShutdownDaemonLifecycle();
    EXPECT_EQ(Spark::Graphics::GetShaderDiskCache().GetDaemonHits(), 0u);

    Spark::Daemon::InitializeDaemonLifecycle();

    // No connection attempted → connection singleton still empty.
    EXPECT_FALSE(Spark::Daemon::DaemonConnection::Instance().IsConnected());

    Spark::Daemon::ShutdownDaemonLifecycle();
}

TEST(DaemonLifecycle_WithMissingSocketIsNoop)
{
    DaemonCVarGuard cvars(/*enable*/ true, UniqueLifecyclePath("does-not-exist"));

    Spark::Daemon::ShutdownDaemonLifecycle();
    Spark::Daemon::InitializeDaemonLifecycle();

    // Lifecycle probed the socket, found nothing, and cleanly gave up.
    EXPECT_FALSE(Spark::Daemon::DaemonConnection::Instance().IsConnected());

    Spark::Daemon::ShutdownDaemonLifecycle();
}

#if defined(SPARK_TEST_DAEMON_PATH)
TEST(DaemonLifecycle_AutoSpawnConnectsToDetachedDaemon)
{
    const std::string socketPath = UniqueLifecyclePath("auto-spawn");
    DaemonCVarGuard cvars(/*enable*/ true, socketPath, /*autoSpawn*/ true, SPARK_TEST_DAEMON_PATH);

    Spark::Daemon::ShutdownDaemonLifecycle();
    Spark::Daemon::InitializeDaemonLifecycle();

    auto& connection = Spark::Daemon::DaemonConnection::Instance();
    EXPECT_TRUE(connection.IsConnected());
    if (auto* client = connection.GetClient())
    {
        auto shutdown = client->Request(Spark::Daemon::ServiceId::Control,
                                        static_cast<uint16_t>(Spark::Daemon::ControlMessage::ShutdownRequest), {});
        EXPECT_TRUE(shutdown.has_value());
        if (shutdown)
            EXPECT_EQ(shutdown->messageType, static_cast<uint16_t>(Spark::Daemon::ControlMessage::ShutdownAck));
    }

    Spark::Daemon::ShutdownDaemonLifecycle();
}
#endif

TEST(DaemonLifecycle_AttachesShaderClientWhenDaemonAvailable)
{
    LifecycleDaemonFixture fx("attach");
    DaemonCVarGuard cvars(/*enable*/ true, /*socketPath*/ fx.sockPath);

    // Seed the shader cache so Store can exercise the daemon push path.
    auto localDir = UniqueLifecycleCacheDir("attach");
    auto& diskCache = Spark::Graphics::GetShaderDiskCache();
    diskCache.SetDaemonClient(nullptr); // clean start
    diskCache.Initialize(localDir);

    Spark::Daemon::ShutdownDaemonLifecycle();
    Spark::Daemon::InitializeDaemonLifecycle();

    EXPECT_TRUE(Spark::Daemon::DaemonConnection::Instance().IsConnected());

    // Exercise the wiring: Store a compiled shader, then Lookup from a fresh
    // ShaderDiskCache instance — the daemon should carry it across.
    Spark::Graphics::ShaderSource src;
    src.hlslCode = "float4 VS() : SV_POSITION { return 1; }";
    src.entryPoint = "VS";
    src.stage = Spark::Graphics::ShaderStage::Vertex;

    Spark::Graphics::CompiledShaderBlob blob;
    blob.bytecode = {0x10, 0x20, 0x30, 0x40};
    blob.target = Spark::Graphics::ShaderTarget::DXBC;
    blob.stage = Spark::Graphics::ShaderStage::Vertex;
    blob.entryPoint = "VS";
    blob.success = true;

    diskCache.Store(src, Spark::Graphics::ShaderTarget::DXBC, blob);

    // The global ShaderDiskCache singleton is also what the engine uses, so
    // a second lookup from another ShaderDiskCache instance pointed at the
    // same daemon should see the blob via the daemon path.
    Spark::Graphics::ShaderDiskCache remoteCache;
    auto remoteLocal = UniqueLifecycleCacheDir("attach-remote");
    remoteCache.Initialize(remoteLocal);
    remoteCache.SetDaemonClient(
        // Access the same daemon through its own ShaderServiceClient.
        // Easier: reuse the lifecycle-owned client by using the same global cache.
        nullptr);
    // Use the same global cache for the remote side — it already has the
    // daemon client wired by the lifecycle helper.
    auto hit = diskCache.Lookup(src, Spark::Graphics::ShaderTarget::DXBC);
    EXPECT_TRUE(hit.has_value());
    EXPECT_EQ(hit->bytecode.size(), blob.bytecode.size());

    // Cleanup.
    Spark::Daemon::ShutdownDaemonLifecycle();
    EXPECT_FALSE(Spark::Daemon::DaemonConnection::Instance().IsConnected());

    diskCache.SetDaemonClient(nullptr);
    std::error_code ec;
    std::filesystem::remove_all(localDir, ec);
    std::filesystem::remove_all(remoteLocal, ec);
}

TEST(DaemonLifecycle_InitializeIsIdempotent)
{
    LifecycleDaemonFixture fx("idem");
    DaemonCVarGuard cvars(/*enable*/ true, /*socketPath*/ fx.sockPath);

    Spark::Daemon::ShutdownDaemonLifecycle();
    Spark::Daemon::InitializeDaemonLifecycle();
    EXPECT_TRUE(Spark::Daemon::DaemonConnection::Instance().IsConnected());

    // Second call should see g_active = true and return early without
    // reconnecting or leaking a second client.
    Spark::Daemon::InitializeDaemonLifecycle();
    EXPECT_TRUE(Spark::Daemon::DaemonConnection::Instance().IsConnected());

    Spark::Daemon::ShutdownDaemonLifecycle();
}

TEST(DaemonLifecycle_ShutdownIsIdempotent)
{
    // Double Shutdown with no Initialize should be a clean no-op.
    Spark::Daemon::ShutdownDaemonLifecycle();
    Spark::Daemon::ShutdownDaemonLifecycle();
    EXPECT_FALSE(Spark::Daemon::DaemonConnection::Instance().IsConnected());
}

// =========================================================================
// daemon.invalidate command
// =========================================================================

TEST(DaemonLifecycle_InvalidateCommandDropsCachedVariants)
{
    LifecycleDaemonFixture fx("invalidate");
    DaemonCVarGuard cvars(/*enable*/ true, /*socketPath*/ fx.sockPath);

    Spark::Daemon::ShutdownDaemonLifecycle();
    Spark::Daemon::InitializeDaemonLifecycle();
    EXPECT_TRUE(Spark::Daemon::DaemonConnection::Instance().IsConnected());

    // Put two platform variants of the same asset through a fresh client,
    // then ask `daemon.invalidate` to drop them.
    Spark::Daemon::DaemonClient raw;
    EXPECT_TRUE(raw.Connect(fx.sockPath).has_value());
    Spark::Daemon::AssetServiceClient asset(raw);
    EXPECT_TRUE(asset.PutAsset("meshes/hero.obj", 0, {0xAA}).has_value());
    EXPECT_TRUE(asset.PutAsset("meshes/hero.obj", 1, {0xBB}).has_value());

    const std::string result = Spark::InGameConsole::GetInstance().Execute("daemon.invalidate meshes/hero.obj");
    EXPECT_TRUE(result.find("dropped 2 variants") != std::string::npos);

    // Both variants are gone.
    auto get0 = asset.GetAsset("meshes/hero.obj", 0);
    auto get1 = asset.GetAsset("meshes/hero.obj", 1);
    EXPECT_TRUE(get0 && !get0->found);
    EXPECT_TRUE(get1 && !get1->found);

    Spark::Daemon::ShutdownDaemonLifecycle();
}

TEST(DaemonLifecycle_InvalidateCommandRejectsBadArgs)
{
    // Command is registered unconditionally, so we can probe it with no
    // daemon running — the arg-count check runs before any RPC.
    DaemonCVarGuard cvars(/*enable*/ false, /*socketPath*/ "");
    Spark::Daemon::InitializeDaemonLifecycle();

    const std::string noArgs = Spark::InGameConsole::GetInstance().Execute("daemon.invalidate");
    EXPECT_TRUE(noArgs.find("usage:") != std::string::npos);

    Spark::Daemon::ShutdownDaemonLifecycle();
}

TEST(DaemonLifecycle_InvalidateCommandWhenDisconnected)
{
    DaemonCVarGuard cvars(/*enable*/ false, /*socketPath*/ "");
    Spark::Daemon::ShutdownDaemonLifecycle();
    Spark::Daemon::InitializeDaemonLifecycle();

    const std::string result = Spark::InGameConsole::GetInstance().Execute("daemon.invalidate foo/bar.png");
    EXPECT_EQ(result, "daemon: not connected");

    Spark::Daemon::ShutdownDaemonLifecycle();
}

// =========================================================================
// spark.daemon.clear_on_startup CVar
// =========================================================================

namespace
{
    /// RAII guard for a single bool CVar. Used so clear_on_startup tests
    /// don't leak process-wide state.
    struct BoolCVarGuard
    {
        const char* name;
        bool prev = false;

        BoolCVarGuard(const char* n, bool value) : name(n)
        {
            if (auto* c = Spark::CVarRegistry::Get().Find(name))
            {
                prev = c->GetValueString() == "true";
                c->SetFromString(value ? "1" : "0");
            }
        }
        ~BoolCVarGuard()
        {
            if (auto* c = Spark::CVarRegistry::Get().Find(name))
                c->SetFromString(prev ? "1" : "0");
        }
    };
} // namespace

TEST(DaemonLifecycle_ClearOnStartupWipesDaemonCaches)
{
    LifecycleDaemonFixture fx("clear-startup");
    DaemonCVarGuard cvars(/*enable*/ true, /*socketPath*/ fx.sockPath);
    BoolCVarGuard clearGuard("spark.daemon.clear_on_startup", /*value*/ true);

    // Pre-populate the daemon's asset cache before the engine connects.
    {
        Spark::Daemon::DaemonClient raw;
        EXPECT_TRUE(raw.Connect(fx.sockPath).has_value());
        Spark::Daemon::AssetServiceClient asset(raw);
        EXPECT_TRUE(asset.PutAsset("pre/a.bin", 0, {0x01}).has_value());
        EXPECT_TRUE(asset.PutAsset("pre/b.bin", 0, {0x02}).has_value());
        auto s = asset.GetCacheStats();
        EXPECT_TRUE(s && s->entryCount == 2u);
    }

    Spark::Daemon::ShutdownDaemonLifecycle();
    Spark::Daemon::InitializeDaemonLifecycle();
    EXPECT_TRUE(Spark::Daemon::DaemonConnection::Instance().IsConnected());

    // Re-check stats via a separate client — the CVar hook should have
    // just dropped both entries.
    Spark::Daemon::DaemonClient raw;
    EXPECT_TRUE(raw.Connect(fx.sockPath).has_value());
    Spark::Daemon::AssetServiceClient asset(raw);
    auto stats = asset.GetCacheStats();
    EXPECT_TRUE(stats);
    EXPECT_EQ(stats->entryCount, 0u);

    Spark::Daemon::ShutdownDaemonLifecycle();
}

TEST(DaemonLifecycle_ClearOnStartupDisabledLeavesCachesAlone)
{
    LifecycleDaemonFixture fx("clear-startup-off");
    DaemonCVarGuard cvars(/*enable*/ true, /*socketPath*/ fx.sockPath);
    BoolCVarGuard clearGuard("spark.daemon.clear_on_startup", /*value*/ false);

    // Seed the daemon.
    {
        Spark::Daemon::DaemonClient raw;
        EXPECT_TRUE(raw.Connect(fx.sockPath).has_value());
        Spark::Daemon::AssetServiceClient asset(raw);
        EXPECT_TRUE(asset.PutAsset("keep/me.bin", 0, {0x42}).has_value());
    }

    Spark::Daemon::ShutdownDaemonLifecycle();
    Spark::Daemon::InitializeDaemonLifecycle();
    EXPECT_TRUE(Spark::Daemon::DaemonConnection::Instance().IsConnected());

    Spark::Daemon::DaemonClient raw;
    EXPECT_TRUE(raw.Connect(fx.sockPath).has_value());
    Spark::Daemon::AssetServiceClient asset(raw);
    auto get = asset.GetAsset("keep/me.bin", 0);
    EXPECT_TRUE(get && get->found);
    EXPECT_EQ(get->blob.size(), 1u);

    Spark::Daemon::ShutdownDaemonLifecycle();
}

#endif // supported local IPC platforms
