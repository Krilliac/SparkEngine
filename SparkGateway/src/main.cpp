/**
 * @file main.cpp
 * @brief SparkGateway console entry point.
 */

#include "GatewayApplication.h"
#include "GatewayAreaControl.h"
#include "GatewaySecurity.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#endif

#include <atomic>
#include <iostream>
#include <memory>
#include <string_view>
#include <vector>

namespace
{
    std::atomic<Spark::Gateway::GatewayApplication*> g_application{nullptr};

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
    // console control handler is the only path that reaches GatewayApplication.
    BOOL WINAPI HandleConsoleControl(DWORD controlType)
    {
        // Close/logoff/shutdown callbacks have bounded OS deadlines and cannot
        // promise that this process's asynchronous drain will finish. Only
        // claim the console events for which the main loop remains available
        // to observe the request and execute the graceful path.
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

    auto parsed = Spark::Gateway::ParseGatewayOptions(arguments);
    if (!parsed.options)
    {
        std::cerr << "SparkGateway: " << parsed.error << "\n\n" << Spark::Gateway::GatewayHelpText();
        return 2;
    }
    if (parsed.options->showHelp)
    {
        std::cout << Spark::Gateway::GatewayHelpText();
        return 0;
    }

    // Local ingress and area control use owner-only key material and HMAC-authenticated framing.
    auto authenticator = std::make_unique<Spark::Gateway::KeyFileAuthenticator>(parsed.options->keyFile);
    auto controlPlane = std::make_unique<Spark::Gateway::LocalAreaControlPlane>(parsed.options->keyFile);
    Spark::Gateway::GatewayApplication application(std::move(*parsed.options), std::move(authenticator),
                                                   std::move(controlPlane));
    g_application.store(&application, std::memory_order_release);
#ifdef _WIN32
    if (!::SetConsoleCtrlHandler(HandleConsoleControl, TRUE))
    {
        std::cerr << "SparkGateway: could not install the console control handler\n";
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
