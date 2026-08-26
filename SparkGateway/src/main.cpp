/**
 * @file main.cpp
 * @brief SparkGateway console entry point.
 */

#include "GatewayApplication.h"
#include "GatewayAreaControl.h"
#include "GatewaySecurity.h"

#include <atomic>
#include <csignal>
#include <iostream>
#include <memory>
#include <string_view>
#include <vector>

namespace
{
    std::atomic<Spark::Gateway::GatewayApplication*> g_application{nullptr};

    void HandleSignal(int)
    {
        if (auto* application = g_application.load(std::memory_order_relaxed))
            application->RequestStop();
    }
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
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);
    if (!application.Start())
    {
        std::cerr << application.GetHealthJson() << '\n';
        g_application.store(nullptr, std::memory_order_release);
        return 1;
    }
    const int result = application.Run();
    g_application.store(nullptr, std::memory_order_release);
    return result;
}
