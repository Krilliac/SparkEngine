/**
 * @file CollabServerMain.cpp
 * @brief Isolated Spark collaboration broker entry point.
 */

#include "CollaborationService.h"
#include "ControlService.h"
#include "DaemonServer.h"

#include <cstdio>
#include <memory>
#include <string>
#include <string_view>

#if !defined(_WIN32)
#include <csignal>
#endif

namespace
{
    Spark::Daemon::DaemonServer* g_serverForSignal = nullptr;

#if !defined(_WIN32)
    void HandleSignal(int)
    {
        if (g_serverForSignal)
            g_serverForSignal->Stop();
    }
#endif
} // namespace

int main(int argc, char** argv)
{
    std::string socketPath = "./.spark-collab.sock";
    Spark::Daemon::CollaborationConfig config;
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view argument = argv[i];
        if (argument == "--socket" && i + 1 < argc)
            socketPath = argv[++i];
        else if (argument == "--help" || argument == "-h")
        {
            std::puts("SparkCollabServer [--socket <path>]");
            return 0;
        }
        else
        {
            std::fprintf(stderr, "SparkCollabServer: unknown argument '%s'\n", argv[i]);
            return 2;
        }
    }

    Spark::Daemon::DaemonServer server;
    auto statsProvider = [&server] { return server.SnapshotStats(); };
    server.AddService(std::make_unique<Spark::Daemon::ControlService>(server.GetShouldStopFlag(), statsProvider));
    server.AddService(std::make_unique<Spark::Daemon::CollaborationService>(config));
    g_serverForSignal = &server;
#if !defined(_WIN32)
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);
    std::signal(SIGPIPE, SIG_IGN);
#endif
    std::printf("SparkCollabServer: listening on %s\n", socketPath.c_str());
    auto result = server.Run(socketPath);
    g_serverForSignal = nullptr;
    if (!result)
    {
        std::fprintf(stderr, "SparkCollabServer: %s\n", result.error().c_str());
        return 1;
    }
    std::puts("SparkCollabServer: shutdown complete");
    return 0;
}
