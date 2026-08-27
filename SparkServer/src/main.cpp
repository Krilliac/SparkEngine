/**
 * @file main.cpp
 * @brief SparkServer console entry point.
 */

#include "ServerApplication.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#endif

#include <atomic>
#include <iostream>
#include <string_view>
#include <vector>

namespace
{
    std::atomic<Spark::Server::ServerApplication*> g_application{nullptr};

    void RequestApplicationStop()
    {
        if (auto* application = g_application.load(std::memory_order_acquire))
            application->RequestStop();
    }

#ifdef _WIN32
    // A supervisor graceful-stops this process with CTRL_BREAK_EVENT, and the
    // CREATE_NEW_PROCESS_GROUP it launches us in disables CTRL+C for that whole
    // group. A CRT SIGINT handler therefore never observes a stop request on
    // Windows, and the operating system never raises SIGTERM at all, so the
    // console control handler is the only path that reaches ServerApplication.
    BOOL WINAPI HandleConsoleControl(DWORD controlType)
    {
        // CTRL_CLOSE/LOGOFF/SHUTDOWN handlers run under short OS deadlines and
        // may be terminated before application-owned teardown completes. Do
        // not claim those events were handled when this callback only posts an
        // asynchronous request. CTRL_C/BREAK leave the normal main loop alive
        // long enough to observe the flag and complete graceful shutdown.
        if (controlType != CTRL_C_EVENT && controlType != CTRL_BREAK_EVENT)
            return FALSE;
        RequestApplicationStop();
        return TRUE;
    }
#else
    void HandleSignal(int)
    {
        RequestApplicationStop();
    }
#endif
} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string_view> arguments;
    arguments.reserve(static_cast<size_t>(argc > 0 ? argc - 1 : 0));
    for (int index = 1; index < argc; ++index)
        arguments.emplace_back(argv[index]);

    auto parsed = Spark::Server::ParseServerOptions(arguments);
    if (!parsed.options)
    {
        std::cerr << "SparkServer: " << parsed.error << "\n\n" << Spark::Server::ServerHelpText();
        return 2;
    }
    if (parsed.options->showHelp)
    {
        std::cout << Spark::Server::ServerHelpText();
        return 0;
    }

    Spark::Server::ServerApplication application(std::move(*parsed.options));
    g_application.store(&application, std::memory_order_release);
#ifdef _WIN32
    if (!::SetConsoleCtrlHandler(HandleConsoleControl, TRUE))
    {
        std::cerr << "SparkServer: could not install the console control handler\n";
        g_application.store(nullptr, std::memory_order_release);
        return 1;
    }
#else
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);
#endif

    const bool started = application.Start();
    if (!started)
        std::cerr << application.GetHealthJson() << '\n';
    const int result = started ? application.Run() : 1;
#ifdef _WIN32
    (void)::SetConsoleCtrlHandler(HandleConsoleControl, FALSE);
#endif
    g_application.store(nullptr, std::memory_order_release);
    return result;
}
