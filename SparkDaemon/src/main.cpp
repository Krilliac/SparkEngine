/**
 * @file main.cpp
 * @brief SparkDaemon entry point.
 *
 * Usage:
 *   SparkDaemon [--socket <path>] [--cache-dir <path>] [--asset-cache-dir <path>]
 *
 * Defaults:
 *   --socket            `./.spark-daemon.sock`
 *   --cache-dir         disabled (in-memory shader cache only)
 *   --asset-cache-dir   disabled (in-memory asset cache only)
 *
 * The socket file is removed on clean shutdown. SIGINT / SIGTERM trigger a
 * graceful exit. `--cache-dir` enables shader blob persistence (loaded on
 * startup, written on PutCacheEntry); `--asset-cache-dir` enables the same
 * for the asset service.
 */

#include "AssetService.h"
#include "ControlService.h"
#include "DaemonServer.h"
#include "ShaderService.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

#if !defined(_WIN32)
#include <csignal>
#include <unistd.h>
#endif

namespace
{
    Spark::Daemon::DaemonServer* g_serverForSignal = nullptr;

#if !defined(_WIN32)
    void HandleSignal(int /*sig*/)
    {
        if (g_serverForSignal)
            g_serverForSignal->Stop();
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
        else if (arg == "--help" || arg == "-h")
        {
            std::puts("SparkDaemon [--socket <path>] [--cache-dir <path>] [--asset-cache-dir <path>]");
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
    server.AddService(std::move(asset));

    g_serverForSignal = &server;
#if !defined(_WIN32)
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);
    std::signal(SIGPIPE, SIG_IGN);
#endif

    std::printf("SparkDaemon: listening on %s\n", socketPath.c_str());
    auto result = server.Run(socketPath);
    g_serverForSignal = nullptr;

    if (!result)
    {
        std::fprintf(stderr, "SparkDaemon: %s\n", result.error().c_str());
        return 1;
    }
    std::puts("SparkDaemon: shutdown complete");
    return 0;
}
