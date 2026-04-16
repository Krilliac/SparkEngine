/**
 * @file DaemonLifecycle.cpp
 * @brief Engine-side startup/shutdown glue for SparkDaemon (see header).
 */

#include "DaemonLifecycle.h"

#include "ConsoleVariable.h"
#include "DaemonConnection.h"
#include "LogMacros.h"
#include "ShaderServiceClient.h"

#include "../Graphics/ShaderDiskCache.h"

#include <memory>
#include <mutex>

namespace
{
    // Default off: the daemon is a power-user optimisation, not a standard
    // engine feature. Opt-in via `spark.daemon.enabled 1` in a console script
    // or command-line `+spark.daemon.enabled 1`.
    Spark::CVar<bool> cv_DaemonEnabled("spark.daemon.enabled", false, Spark::CVarFlags::Save,
                                       "Connect to a running SparkDaemon and share its shader cache across engines.");

    // Empty = use the default `./.spark-daemon.sock`. Override to point at
    // a daemon in a different working directory (e.g. a build tree shared
    // between editor and headless cook tools).
    Spark::CVar<std::string> cv_DaemonSocket("spark.daemon.socket_path", std::string{}, Spark::CVarFlags::Save,
                                             "Override the daemon socket path (empty = ./.spark-daemon.sock).");

    std::mutex g_lifecycleMutex;
    std::unique_ptr<Spark::Daemon::ShaderServiceClient> g_shaderClient;
    bool g_active = false;
} // namespace

namespace Spark::Daemon
{

    void InitializeDaemonLifecycle()
    {
        std::lock_guard lock(g_lifecycleMutex);
        if (g_active)
            return;

        if (!cv_DaemonEnabled.Get())
        {
            SPARK_LOG_INFO(Spark::LogCategory::Core, "Daemon disabled via spark.daemon.enabled=0");
            return;
        }

        auto& conn = DaemonConnection::Instance();
        if (!conn.TryConnect(cv_DaemonSocket.Get()))
        {
            SPARK_LOG_INFO(Spark::LogCategory::Core,
                           "Daemon requested but unreachable — engine will run with in-process caches only");
            return;
        }

        auto* client = conn.GetClient();
        if (!client)
            return; // Lost race against another thread; treat as disconnected.

        g_shaderClient = std::make_unique<ShaderServiceClient>(*client);
        Spark::Graphics::GetShaderDiskCache().SetDaemonClient(g_shaderClient.get());
        g_active = true;

        SPARK_LOG_INFO(Spark::LogCategory::Core, "Daemon wired: shader cache sharing via %s",
                       conn.GetSocketPath().c_str());
    }

    void ShutdownDaemonLifecycle()
    {
        std::lock_guard lock(g_lifecycleMutex);
        if (!g_active)
            return;

        // Clear the ShaderDiskCache pointer BEFORE destroying the client —
        // the cache must never hold a dangling pointer.
        Spark::Graphics::GetShaderDiskCache().SetDaemonClient(nullptr);
        g_shaderClient.reset();
        DaemonConnection::Instance().Shutdown();
        g_active = false;
    }

} // namespace Spark::Daemon
