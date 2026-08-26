/**
 * @file main.cpp
 * @brief SparkServer console entry point.
 */

#include "ServerApplication.h"

#include <atomic>
#include <csignal>
#include <iostream>
#include <string_view>
#include <vector>

namespace
{
    std::atomic<Spark::Server::ServerApplication*> g_application{nullptr};

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
