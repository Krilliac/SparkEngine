/**
 * @file ServerApplication.h
 * @brief Headless Spark game-server process host and command-line contract.
 */

#pragma once

#include "Engine/Networking/DedicatedServer.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>

class ModuleManager;
class Timer;
class World;

namespace Spark
{
    class EventBus;
}

namespace Spark::Gateway
{
    class LocalAreaControlService;
}

namespace Spark::Server
{
    struct ServerOptions
    {
        Net::ServerConfig server;
        std::filesystem::path configPath;
        std::filesystem::path modulePath;
        std::filesystem::path manifestPath;
        std::filesystem::path healthFile;
        std::filesystem::path stopFile;
        std::filesystem::path gatewayKeyFile;
        std::filesystem::path controlStateFile;
        std::string controlEndpoint;
        std::chrono::milliseconds statusInterval{5000};
        std::optional<std::chrono::milliseconds> runFor;
        bool showHelp = false;
    };

    struct ParseResult
    {
        std::optional<ServerOptions> options;
        std::string error;
    };

    [[nodiscard]] ParseResult ParseServerOptions(std::span<const std::string_view> arguments);
    [[nodiscard]] std::string_view ServerHelpText();

    struct ServerHealth
    {
        bool live = false;
        bool ready = false;
        bool stopping = false;
        uint16_t port = 0;
        uint32_t players = 0;
        uint64_t ticks = 0;
        size_t loadedModules = 0;
        std::string gameModule;
        std::string currentMap;
        std::string lastError;
    };

    /** Owns one server-only process lifecycle. Loaded game logic remains in a dynamic module. */
    class ServerApplication
    {
      public:
        explicit ServerApplication(ServerOptions options);
        ~ServerApplication();

        ServerApplication(const ServerApplication&) = delete;
        ServerApplication& operator=(const ServerApplication&) = delete;

        /** [startup thread] Initialize the headless context, module, and network server. */
        [[nodiscard]] bool Start();
        /** [startup thread] Run module updates until a stop is requested. */
        [[nodiscard]] int Run();
        /** [any thread, atomic] Ask the main loop to stop at its next boundary. */
        void RequestStop() noexcept;
        /** [startup thread] Shut down modules before releasing server/context resources. */
        [[nodiscard]] bool Stop();
        /** [host thread] Return a point-in-time status snapshot. */
        [[nodiscard]] ServerHealth GetHealth() const;
        /** [host thread] Serialize GetHealth() as one compact JSON object. */
        [[nodiscard]] std::string GetHealthJson() const;

      private:
        [[nodiscard]] bool LoadSelectedModules();
        void PublishHealth() const;
        void SetError(std::string message);

        ServerOptions m_options;
        std::atomic<bool> m_stopRequested{false};
        std::atomic<bool> m_started{false};
        std::atomic<bool> m_stopping{false};
        mutable std::mutex m_errorMutex;
        std::string m_lastError;
        std::unique_ptr<World> m_world;
        std::unique_ptr<ModuleManager> m_modules;
        std::unique_ptr<Net::DedicatedServer> m_server;
        std::unique_ptr<Gateway::LocalAreaControlService> m_controlService;
    };
} // namespace Spark::Server
