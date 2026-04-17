/**
 * @file DaemonLifecycle.cpp
 * @brief Engine-side startup/shutdown glue for SparkDaemon (see header).
 */

#include "DaemonLifecycle.h"

#include "AssetServiceClient.h"
#include "ConsoleVariable.h"
#include "DaemonConnection.h"
#include "DaemonDiagnostics.h"
#include "DaemonProtocol.h"
#include "InGameConsole.h"
#include "LogMacros.h"
#include "Process.h"
#include "ShaderServiceClient.h"

#include "../Graphics/ShaderDiskCache.h"

#include <algorithm>
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
    std::unique_ptr<Spark::Daemon::AssetServiceClient> g_assetClient;
    bool g_active = false;
    bool g_statsCommandRegistered = false;
    bool g_clearCacheCommandRegistered = false;

    constexpr const char* kStatsCommandName = "daemon.stats";
    constexpr const char* kClearCacheCommandName = "daemon.clear_cache";

    /// Query the daemon and render its state via DaemonDiagnostics.
    /// Returns a user-facing multi-line string.
    std::string RunStatsCommand()
    {
        auto& conn = Spark::Daemon::DaemonConnection::Instance();
        Spark::Daemon::DaemonStatsSnapshot snap;

        auto* client = conn.GetClient();
        if (!conn.IsConnected() || client == nullptr)
            return Spark::Daemon::FormatDaemonStats(snap); // socketPath empty → "not connected"

        snap.socketPath = conn.GetSocketPath();

        auto statsResp = client->Request(Spark::Daemon::ServiceId::Control,
                                         static_cast<uint16_t>(Spark::Daemon::ControlMessage::StatsRequest), {});
        if (!statsResp)
            return std::string("daemon.stats: ") + statsResp.error();
        if (!Spark::Daemon::DecodeDaemonStats(statsResp->payload, snap.control))
            return "daemon.stats: failed to decode Control StatsResponse";

        const bool hasShader =
            std::find(snap.control.registeredIds.begin(), snap.control.registeredIds.end(),
                      static_cast<uint16_t>(Spark::Daemon::ServiceId::Shader)) != snap.control.registeredIds.end();
        const bool hasAsset =
            std::find(snap.control.registeredIds.begin(), snap.control.registeredIds.end(),
                      static_cast<uint16_t>(Spark::Daemon::ServiceId::Asset)) != snap.control.registeredIds.end();

        if (hasShader)
        {
            // Lazy-build a one-shot client if lifecycle-owned one isn't up yet
            // (can happen if the CVar flipped without restart); otherwise use
            // the shared one.
            if (g_shaderClient)
            {
                if (auto s = g_shaderClient->GetCacheStats())
                    snap.shader = *s;
            }
            else
            {
                Spark::Daemon::ShaderServiceClient tmp(*client);
                if (auto s = tmp.GetCacheStats())
                    snap.shader = *s;
            }
        }
        if (hasAsset)
        {
            if (g_assetClient)
            {
                if (auto a = g_assetClient->GetCacheStats())
                    snap.asset = *a;
            }
            else
            {
                Spark::Daemon::AssetServiceClient tmp(*client);
                if (auto a = tmp.GetCacheStats())
                    snap.asset = *a;
            }
        }

        return Spark::Daemon::FormatDaemonStats(snap);
    }

    /// Drive the ClearCache RPC for the selected scope(s).
    /// Returns a user-facing summary line. Assumes args has already been
    /// parsed into `scope` by the console handler.
    std::string RunClearCacheCommand(Spark::Daemon::DaemonCacheScope scope)
    {
        using Spark::Daemon::DaemonCacheScope;

        auto& conn = Spark::Daemon::DaemonConnection::Instance();
        auto* client = conn.GetClient();
        if (!conn.IsConnected() || client == nullptr)
            return "daemon: not connected";

        std::string cleared;
        std::string failed;
        auto note = [](std::string& bucket, const char* label)
        {
            if (!bucket.empty())
                bucket += ", ";
            bucket += label;
        };

        if ((static_cast<uint8_t>(scope) & static_cast<uint8_t>(DaemonCacheScope::Shader)) != 0)
        {
            Spark::Daemon::ShaderServiceClient tmp(*client);
            auto* c = g_shaderClient ? g_shaderClient.get() : &tmp;
            if (auto r = c->ClearCache())
                note(cleared, "shader");
            else
                note(failed, "shader");
        }
        if ((static_cast<uint8_t>(scope) & static_cast<uint8_t>(DaemonCacheScope::Asset)) != 0)
        {
            Spark::Daemon::AssetServiceClient tmp(*client);
            auto* c = g_assetClient ? g_assetClient.get() : &tmp;
            if (auto r = c->ClearCache())
                note(cleared, "asset");
            else
                note(failed, "asset");
        }

        std::string out = "daemon.clear_cache:";
        if (!cleared.empty())
            out += " cleared [" + cleared + "]";
        if (!failed.empty())
            out += " failed [" + failed + "]";
        if (cleared.empty() && failed.empty())
            out += " nothing to do";
        return out;
    }

    /// Idempotent registration of the `daemon.stats` console command.
    /// Registration is independent of whether a daemon is currently wired —
    /// the command itself handles the disconnected case. This way operators
    /// can always type `daemon.stats` to find out what's happening.
    void EnsureStatsCommandRegistered()
    {
        if (g_statsCommandRegistered)
            return;
        auto& console = Spark::InGameConsole::GetInstance();
        console.RegisterCommand(
            kStatsCommandName, "Query the SparkDaemon and print its summary + per-cache stats",
            [](const std::vector<std::string>&) -> std::string { return RunStatsCommand(); }, kStatsCommandName);
        g_statsCommandRegistered = true;
    }

    void UnregisterStatsCommand()
    {
        if (!g_statsCommandRegistered)
            return;
        Spark::InGameConsole::GetInstance().UnregisterCommand(kStatsCommandName);
        g_statsCommandRegistered = false;
    }

    void EnsureClearCacheCommandRegistered()
    {
        if (g_clearCacheCommandRegistered)
            return;
        auto& console = Spark::InGameConsole::GetInstance();
        console.RegisterCommand(
            kClearCacheCommandName, "Ask the SparkDaemon to drop entries from its shader and/or asset cache",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.size() != 1)
                    return "usage: daemon.clear_cache <shader|asset|all>";
                auto scope = Spark::Daemon::ParseDaemonCacheScope(args[0]);
                if (scope == Spark::Daemon::DaemonCacheScope::None)
                    return "usage: daemon.clear_cache <shader|asset|all>";
                return RunClearCacheCommand(scope);
            },
            "daemon.clear_cache <shader|asset|all>");
        g_clearCacheCommandRegistered = true;
    }

    void UnregisterClearCacheCommand()
    {
        if (!g_clearCacheCommandRegistered)
            return;
        Spark::InGameConsole::GetInstance().UnregisterCommand(kClearCacheCommandName);
        g_clearCacheCommandRegistered = false;
    }

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

        // Register the diagnostic commands regardless of whether the daemon
        // is reachable — the handlers themselves render "not connected" when
        // appropriate, so operators always have a way to query state.
        EnsureStatsCommandRegistered();
        EnsureClearCacheCommandRegistered();

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
        g_assetClient = std::make_unique<AssetServiceClient>(*client);
        Spark::Graphics::GetShaderDiskCache().SetDaemonClient(g_shaderClient.get());
        g_active = true;

        SPARK_LOG_INFO(Spark::LogCategory::Core, "Daemon wired: shader cache sharing via %s",
                       conn.GetSocketPath().c_str());
    }

    void ShutdownDaemonLifecycle()
    {
        std::lock_guard lock(g_lifecycleMutex);

        UnregisterStatsCommand();
        UnregisterClearCacheCommand();

        if (!g_active)
            return;

        // Clear the ShaderDiskCache pointer BEFORE destroying the client —
        // the cache must never hold a dangling pointer.
        Spark::Graphics::GetShaderDiskCache().SetDaemonClient(nullptr);
        g_shaderClient.reset();
        g_assetClient.reset();
        DaemonConnection::Instance().Shutdown();
        g_active = false;
    }

} // namespace Spark::Daemon
