/**
 * @file CollabServerMain.cpp
 * @brief Isolated Spark collaboration broker entry point.
 */

#include "CollaborationService.h"
#include "ControlService.h"
#include "DaemonServer.h"

#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>

#if defined(_WIN32)
#include <windows.h>
#else
#include <csignal>
#endif

namespace
{
    std::atomic<Spark::Daemon::DaemonServer*> g_serverForSignal{nullptr};

#if defined(_WIN32)
    BOOL WINAPI HandleConsoleControl(DWORD controlType)
    {
        if (controlType != CTRL_C_EVENT && controlType != CTRL_BREAK_EVENT)
            return FALSE;
        if (auto* server = g_serverForSignal.load(std::memory_order_acquire))
            server->Stop();
        return TRUE;
    }
#else
    void HandleSignal(int)
    {
        if (auto* server = g_serverForSignal.load(std::memory_order_acquire))
            server->Stop();
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
    g_serverForSignal.store(&server, std::memory_order_release);
#if defined(_WIN32)
    if (!::SetConsoleCtrlHandler(HandleConsoleControl, TRUE))
    {
        std::fprintf(stderr, "SparkCollabServer: could not install console control handler\n");
        g_serverForSignal.store(nullptr, std::memory_order_release);
        return 1;
    }
#else
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);
    std::signal(SIGPIPE, SIG_IGN);
#endif
    std::printf("SparkCollabServer: listening on %s\n", socketPath.c_str());
    auto result = server.Run(socketPath);
#if defined(_WIN32)
    (void)::SetConsoleCtrlHandler(HandleConsoleControl, FALSE);
#endif
    g_serverForSignal.store(nullptr, std::memory_order_release);
    if (!result)
    {
        std::fprintf(stderr, "SparkCollabServer: %s\n", result.error().c_str());
        return 1;
    }
    std::puts("SparkCollabServer: shutdown complete");
    return 0;
}
