/**
 * @file DaemonLifecycle.cpp
 * @brief Engine-side startup/shutdown glue for SparkDaemon (see header).
 */

#include "DaemonLifecycle.h"

#include "AssetServiceClient.h"
#include "ConsoleVariable.h"
#include "DaemonConnection.h"
#include "DaemonDiagnostics.h"
#include "DaemonFraming.h"
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

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <sys/stat.h>
#endif

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
                                             "Override the SparkDaemon executable path (empty = platform default).");

    // When true, a successful connect runs `daemon.clear_cache all` immediately.
    // Useful for local development where engine shader/asset sources have
    // diverged from whatever the long-lived daemon still remembers.
    Spark::CVar<bool> cv_DaemonClearOnStartup("spark.daemon.clear_on_startup", false, Spark::CVarFlags::Save,
                                              "After connecting, drop all cached entries from the daemon.");

    std::mutex g_lifecycleMutex;
    std::unique_ptr<Spark::Daemon::ShaderServiceClient> g_shaderClient;
    std::unique_ptr<Spark::Daemon::AssetServiceClient> g_assetClient;
    bool g_active = false;
    bool g_statsCommandRegistered = false;
    bool g_clearCacheCommandRegistered = false;
    bool g_invalidateCommandRegistered = false;

    constexpr const char* kStatsCommandName = "daemon.stats";
    constexpr const char* kClearCacheCommandName = "daemon.clear_cache";
    constexpr const char* kInvalidateCommandName = "daemon.invalidate";

    Spark::Daemon::Expected<void, std::string> WaitForDaemonEndpoint(const std::string& endpoint,
                                                                     std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;

#if defined(_WIN32)
        const std::wstring pipeName = Spark::Daemon::NormalizePipeName(endpoint);
        if (pipeName.empty())
            return Spark::Daemon::Unexpected<std::string>("named-pipe endpoint is not valid UTF-8: " + endpoint);

        DWORD lastError = ERROR_FILE_NOT_FOUND;
        do
        {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
                break;

            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            const DWORD waitMs = static_cast<DWORD>((std::max)(int64_t{1}, (std::min)(int64_t{50}, remaining.count())));
            if (::WaitNamedPipeW(pipeName.c_str(), waitMs))
                return {};

            lastError = ::GetLastError();
            if (lastError != ERROR_FILE_NOT_FOUND && lastError != ERROR_PIPE_BUSY && lastError != ERROR_SEM_TIMEOUT)
            {
                return Spark::Daemon::Unexpected<std::string>("named-pipe readiness probe failed for " + endpoint +
                                                              " (Win32 error " + std::to_string(lastError) + ")");
            }

            // A pipe that has not been created yet returns immediately instead
            // of honouring waitMs, so avoid a startup-speed busy loop.
            if (lastError == ERROR_FILE_NOT_FOUND)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } while (std::chrono::steady_clock::now() < deadline);

        return Spark::Daemon::Unexpected<std::string>("named pipe did not become ready within " +
                                                      std::to_string(timeout.count()) + " ms at " + endpoint +
                                                      " (last Win32 error " + std::to_string(lastError) + ")");
#else
        int lastError = ENOENT;
        do
        {
            struct stat endpointStatus = {};
            if (::stat(endpoint.c_str(), &endpointStatus) == 0)
            {
                if (S_ISSOCK(endpointStatus.st_mode))
                    return {};
                return Spark::Daemon::Unexpected<std::string>(
                    "daemon endpoint exists but is not a Unix-domain socket: " + endpoint);
            }

            lastError = errno;
            if (lastError != ENOENT)
            {
                return Spark::Daemon::Unexpected<std::string>("Unix-socket readiness probe failed for " + endpoint +
                                                              ": " + std::strerror(lastError));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } while (std::chrono::steady_clock::now() < deadline);

        return Spark::Daemon::Unexpected<std::string>("Unix socket did not become ready within " +
                                                      std::to_string(timeout.count()) + " ms at " + endpoint + ": " +
                                                      std::strerror(lastError));
#endif
    }

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

    /// Drive the InvalidateAsset RPC for a single logical path.
    /// Returns a user-facing summary line describing how many platform
    /// variants were dropped.
    std::string RunInvalidateCommand(const std::string& path)
    {
        auto& conn = Spark::Daemon::DaemonConnection::Instance();
        auto* client = conn.GetClient();
        if (!conn.IsConnected() || client == nullptr)
            return "daemon: not connected";

        Spark::Daemon::AssetServiceClient tmp(*client);
        auto* c = g_assetClient ? g_assetClient.get() : &tmp;
        auto r = c->InvalidateAsset(path);
        if (!r)
            return std::string("daemon.invalidate: ") + r.error();

        return "daemon.invalidate: " + path + " (dropped " + std::to_string(*r) +
               (*r == 1u ? " variant)" : " variants)");
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

    void EnsureInvalidateCommandRegistered()
    {
        if (g_invalidateCommandRegistered)
            return;
        auto& console = Spark::InGameConsole::GetInstance();
        console.RegisterCommand(
            kInvalidateCommandName, "Ask the SparkDaemon to drop every platform variant of a single asset",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.size() != 1 || args[0].empty())
                    return "usage: daemon.invalidate <path>";
                return RunInvalidateCommand(args[0]);
            },
            "daemon.invalidate <path>");
        g_invalidateCommandRegistered = true;
    }

    void UnregisterInvalidateCommand()
    {
        if (!g_invalidateCommandRegistered)
            return;
        Spark::InGameConsole::GetInstance().UnregisterCommand(kInvalidateCommandName);
        g_invalidateCommandRegistered = false;
    }

    bool TrySpawnDaemon(const std::string& socketPath)
    {
        std::string binary = cv_DaemonBinary.Get();
        if (binary.empty())
        {
#if defined(_WIN32)
            binary = "./SparkDaemon.exe";
#else
            binary = "./SparkDaemon";
#endif
        }

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

        // Poll briefly for the platform IPC endpoint before retrying connect().
        // Windows publishes a named pipe (not a filesystem entry); POSIX
        // publishes an AF_UNIX socket. The probe owns its deadline on both
        // platforms so a crashed child cannot stall engine startup.
        const std::string probePath =
            socketPath.empty() ? std::string("./") + Spark::Daemon::kDefaultSocketName : socketPath;
        auto ready = WaitForDaemonEndpoint(probePath, std::chrono::seconds(2));
        if (!ready)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "Daemon auto-spawn: %s", ready.error().c_str());
            return false;
        }
        return true;
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
        EnsureInvalidateCommandRegistered();

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

        // Optional: drop every cached entry right after connect. Operators
        // opt in via `spark.daemon.clear_on_startup 1` when local sources
        // have drifted from whatever the long-lived daemon still holds.
        if (cv_DaemonClearOnStartup.Get())
        {
            const auto shaderResult = g_shaderClient->ClearCache();
            const auto assetResult = g_assetClient->ClearCache();
            if (shaderResult && assetResult)
            {
                SPARK_LOG_INFO(Spark::LogCategory::Core, "Daemon clear-on-startup: both caches dropped");
            }
            else
            {
                SPARK_LOG_WARN(Spark::LogCategory::Core, "Daemon clear-on-startup: shader=%s asset=%s",
                               shaderResult ? "ok" : shaderResult.error().c_str(),
                               assetResult ? "ok" : assetResult.error().c_str());
            }
        }
    }

    void ShutdownDaemonLifecycle()
    {
        std::lock_guard lock(g_lifecycleMutex);

        UnregisterStatsCommand();
        UnregisterClearCacheCommand();
        UnregisterInvalidateCommand();

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
