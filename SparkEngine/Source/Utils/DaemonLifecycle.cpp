/**
 * @file DaemonLifecycle.cpp
 * @brief Engine-side startup/shutdown glue for SparkDaemon (see header).
 */

#include "DaemonLifecycle.h"

#include "ConsoleVariable.h"
#include "DaemonConnection.h"
#include "DaemonProtocol.h"
#include "LogMacros.h"
#include "Process.h"
#include "ShaderServiceClient.h"

#include "../Graphics/ShaderDiskCache.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>

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

    // When enabled, if `TryConnect` fails (no running daemon), the lifecycle
    // helper launches `SparkDaemon` itself as a detached subprocess and retries
    // the connect. Default off — operators may want to control daemon launch
    // themselves (e.g. from a shell init script).
    Spark::CVar<bool> cv_DaemonAutoSpawn("spark.daemon.auto_spawn", false, Spark::CVarFlags::Save,
                                         "If the daemon isn't running, launch it as a detached subprocess.");

    // Empty = try `./SparkDaemon` relative to CWD. Override to point at a
    // specific binary (typical for installed builds where the daemon lives
    // elsewhere on disk).
    Spark::CVar<std::string> cv_DaemonBinary("spark.daemon.binary_path", std::string{}, Spark::CVarFlags::Save,
                                             "Override the SparkDaemon executable path (empty = ./SparkDaemon).");

    std::mutex g_lifecycleMutex;
    std::unique_ptr<Spark::Daemon::ShaderServiceClient> g_shaderClient;
    bool g_active = false;

    bool TrySpawnDaemon(const std::string& socketPath)
    {
        std::string binary = cv_DaemonBinary.Get();
        if (binary.empty())
            binary = "./SparkDaemon";

        // Only attempt spawn if the binary is present on disk — failing fast
        // here keeps log noise down on machines where the daemon isn't
        // installed at all.
        std::error_code ec;
        if (!std::filesystem::exists(binary, ec))
        {
            SPARK_LOG_INFO(Spark::LogCategory::Core, "Daemon auto-spawn: binary not found at %s", binary.c_str());
            return false;
        }

        auto builder = Spark::Process::Builder(binary).Detached();
        if (!socketPath.empty())
        {
            builder.Arg("--socket").Arg(socketPath);
        }
        auto result = builder.Launch();
        if (!result)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "Daemon auto-spawn: launch failed: %s", result.error().c_str());
            return false;
        }
        SPARK_LOG_INFO(Spark::LogCategory::Core, "Daemon auto-spawn: launched %s", binary.c_str());

        // Poll briefly for the socket to appear before retrying connect().
        // 2 s ceiling — the daemon binds in its first few ms of execution; a
        // longer wait almost certainly means the subprocess crashed on start.
        const std::string probePath =
            socketPath.empty() ? std::string("./") + Spark::Daemon::kDefaultSocketName : socketPath;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (std::filesystem::exists(probePath, ec))
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        SPARK_LOG_WARN(Spark::LogCategory::Core, "Daemon auto-spawn: socket never appeared at %s", probePath.c_str());
        return false;
    }
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
        const std::string& socketPath = cv_DaemonSocket.Get();

        if (!conn.TryConnect(socketPath))
        {
            if (cv_DaemonAutoSpawn.Get() && TrySpawnDaemon(socketPath))
            {
                (void)conn.TryConnect(socketPath);
            }
        }

        if (!conn.IsConnected())
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
