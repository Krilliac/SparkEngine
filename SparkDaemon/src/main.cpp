/**
 * @file main.cpp
 * @brief SparkDaemon entry point.
 *
 * Usage:
 *   SparkDaemon [--socket <path>]
 *               [--cache-dir <path>] [--shader-cache-max-mb <N>]
 *               [--asset-cache-dir <path>] [--asset-cache-max-mb <N>]
 *
 * Defaults:
 *   --socket                 `./.spark-daemon.sock`
 *   --cache-dir              disabled (in-memory shader cache only)
 *   --shader-cache-max-mb    0  (unbounded)
 *   --asset-cache-dir        disabled (in-memory asset cache only)
 *   --asset-cache-max-mb     0  (unbounded)
 *
 * The socket file is removed on clean shutdown. SIGINT / SIGTERM (POSIX) and
 * console control events such as Ctrl+C / Ctrl+Break (Windows) trigger a
 * graceful exit, which drains every supervised orchestration child before the
 * process leaves. `--cache-dir` enables shader blob persistence (loaded on
 * startup, written on PutCacheEntry); `--asset-cache-dir` enables the same
 * for the asset service.
 */

#include "AssetService.h"
#include "ControlService.h"
#include "DaemonServer.h"
#include "OrchestrationService.h"
#include "ShaderService.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>
#endif

namespace
{
    // Read from the Win32 console-control thread as well as the main thread.
    std::atomic<Spark::Daemon::DaemonServer*> g_serverForSignal{nullptr};

#if defined(_WIN32)
    // Windows raises no SIGTERM and disables CTRL+C for a supervised process
    // group, so without this handler the daemon has no graceful-stop path at
    // all: the OS default handler would terminate it with
    // STATUS_CONTROL_C_EXIT before OrchestrationService could stop and reap
    // its supervised children.
    BOOL WINAPI HandleConsoleControl(DWORD controlType)
    {
        // The OS can terminate close/logoff/shutdown handlers before the
        // orchestration service drains its children. Return FALSE for those
        // events instead of claiming a graceful path we cannot guarantee.
        if (controlType != CTRL_C_EVENT && controlType != CTRL_BREAK_EVENT)
            return FALSE;
        if (auto* server = g_serverForSignal.load(std::memory_order_acquire))
            server->Stop();
        return TRUE;
    }
#else
    void HandleSignal(int /*sig*/)
    {
        if (auto* server = g_serverForSignal.load(std::memory_order_acquire))
            server->Stop();
    }
#endif

    std::string DefaultSocketPath()
    {
        // Resolved relative to the current working directory so developers can
        // run multiple daemons from multiple build trees without collisions.
        std::string path = "./";
        path += Spark::Daemon::kDefaultSocketName;
        return path;
    }
} // namespace

int main(int argc, char** argv)
{
    std::string socketPath = DefaultSocketPath();
    std::string cacheDir;
    std::string assetCacheDir;
    uint64_t shaderMaxBytes = 0;
    uint64_t assetMaxBytes = 0;
    std::vector<std::filesystem::path> orchestrationRoots;
    size_t orchestrationMaxProcesses = 16;
    std::filesystem::path orchestrationStateFile = "./.spark-orchestration.state";
    auto parseMb = [](const char* s, uint64_t& out) -> bool
    {
        try
        {
            out = static_cast<uint64_t>(std::stoull(s)) * 1024ull * 1024ull;
            return true;
        }
        catch (...)
        {
            return false;
        }
    };
    for (int i = 1; i < argc; ++i)
    {
        std::string_view arg = argv[i];
        if (arg == "--socket" && i + 1 < argc)
        {
            socketPath = argv[++i];
        }
        else if (arg == "--cache-dir" && i + 1 < argc)
        {
            cacheDir = argv[++i];
        }
        else if (arg == "--asset-cache-dir" && i + 1 < argc)
        {
            assetCacheDir = argv[++i];
        }
        else if (arg == "--shader-cache-max-mb" && i + 1 < argc)
        {
            if (!parseMb(argv[++i], shaderMaxBytes))
            {
                std::fprintf(stderr, "SparkDaemon: invalid --shader-cache-max-mb value '%s'\n", argv[i]);
                return 2;
            }
        }
        else if (arg == "--asset-cache-max-mb" && i + 1 < argc)
        {
            if (!parseMb(argv[++i], assetMaxBytes))
            {
                std::fprintf(stderr, "SparkDaemon: invalid --asset-cache-max-mb value '%s'\n", argv[i]);
                return 2;
            }
        }
        else if (arg == "--orchestrator-allow-root" && i + 1 < argc)
        {
            orchestrationRoots.emplace_back(argv[++i]);
        }
        else if (arg == "--orchestrator-max-processes" && i + 1 < argc)
        {
            try
            {
                orchestrationMaxProcesses = static_cast<size_t>(std::stoull(argv[++i]));
            }
            catch (...)
            {
                std::fprintf(stderr, "SparkDaemon: invalid --orchestrator-max-processes value '%s'\n", argv[i]);
                return 2;
            }
            if (orchestrationMaxProcesses == 0 || orchestrationMaxProcesses > 256)
            {
                std::fprintf(stderr, "SparkDaemon: --orchestrator-max-processes must be between 1 and 256\n");
                return 2;
            }
        }
        else if (arg == "--orchestrator-state-file" && i + 1 < argc)
        {
            orchestrationStateFile = argv[++i];
        }
        else if (arg == "--help" || arg == "-h")
        {
            std::puts("SparkDaemon [--socket <path>]\n"
                      "            [--cache-dir <path>] [--shader-cache-max-mb <N>]\n"
                      "            [--asset-cache-dir <path>] [--asset-cache-max-mb <N>]\n"
                      "            --orchestrator-allow-root <path> [--orchestrator-max-processes <N>]\n"
                      "            [--orchestrator-state-file <path>]");
            return 0;
        }
        else
        {
            std::fprintf(stderr, "SparkDaemon: unknown argument '%s'\n", argv[i]);
            return 2;
        }
    }

    Spark::Daemon::DaemonServer server;
    auto statsProvider = [&server] { return server.SnapshotStats(); };
    server.AddService(std::make_unique<Spark::Daemon::ControlService>(server.GetShouldStopFlag(), statsProvider));

    auto shader = std::make_unique<Spark::Daemon::ShaderService>();
    if (!cacheDir.empty())
    {
        auto loaded = shader->Initialize(std::filesystem::path{cacheDir});
        if (!loaded)
        {
            std::fprintf(stderr, "SparkDaemon: could not open shader cache directory %s\n", cacheDir.c_str());
            return 1;
        }
        std::printf("SparkDaemon: shader cache %s (%zu entries loaded)\n", cacheDir.c_str(), *loaded);
    }
    if (shaderMaxBytes > 0)
    {
        shader->SetMaxBytes(shaderMaxBytes);
        std::printf("SparkDaemon: shader cache capped at %llu MB (LRU eviction)\n",
                    static_cast<unsigned long long>(shaderMaxBytes / (1024ull * 1024ull)));
    }
    server.AddService(std::move(shader));

    auto asset = std::make_unique<Spark::Daemon::AssetService>();
    if (!assetCacheDir.empty())
    {
        auto loaded = asset->Initialize(std::filesystem::path{assetCacheDir});
        if (!loaded)
        {
            std::fprintf(stderr, "SparkDaemon: could not open asset cache directory %s\n", assetCacheDir.c_str());
            return 1;
        }
        std::printf("SparkDaemon: asset cache %s (%zu entries loaded)\n", assetCacheDir.c_str(), *loaded);
    }
    if (assetMaxBytes > 0)
    {
        asset->SetMaxBytes(assetMaxBytes);
        std::printf("SparkDaemon: asset cache capped at %llu MB (LRU eviction)\n",
                    static_cast<unsigned long long>(assetMaxBytes / (1024ull * 1024ull)));
    }
    server.AddService(std::move(asset));

    if (!orchestrationRoots.empty())
    {
        Spark::Daemon::OrchestrationConfig orchestrationConfig;
        orchestrationConfig.allowedExecutableRoots = std::move(orchestrationRoots);
        orchestrationConfig.maximumRunningProcesses = orchestrationMaxProcesses;
        orchestrationConfig.journalPath = std::move(orchestrationStateFile);
        server.AddService(std::make_unique<Spark::Daemon::OrchestrationService>(std::move(orchestrationConfig)));
        std::printf("SparkDaemon: orchestration enabled (%zu process cap)\n", orchestrationMaxProcesses);
    }

    g_serverForSignal.store(&server, std::memory_order_release);
#if defined(_WIN32)
    if (!::SetConsoleCtrlHandler(HandleConsoleControl, TRUE))
    {
        std::fprintf(stderr, "SparkDaemon: could not install console control handler\n");
        g_serverForSignal.store(nullptr, std::memory_order_release);
        return 1;
    }
#else
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);
    std::signal(SIGPIPE, SIG_IGN);
#endif

    std::printf("SparkDaemon: listening on %s\n", socketPath.c_str());
    auto result = server.Run(socketPath);
#if defined(_WIN32)
    (void)::SetConsoleCtrlHandler(HandleConsoleControl, FALSE);
#endif
    g_serverForSignal.store(nullptr, std::memory_order_release);

    if (!result)
    {
        std::fprintf(stderr, "SparkDaemon: %s\n", result.error().c_str());
        return 1;
    }
    std::puts("SparkDaemon: shutdown complete");
    return 0;
}
