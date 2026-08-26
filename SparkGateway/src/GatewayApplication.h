/**
 * @file GatewayApplication.h
 * @brief SparkGateway process configuration, lifecycle, and health surface.
 */

#pragma once

#include "GatewayCoordinator.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Spark::Gateway
{
    class LocalGatewayIngressService;
    struct GatewayOptions
    {
        Net::WorldServerConfig world;
        std::vector<AreaEndpoint> areas;
        std::filesystem::path configPath;
        std::filesystem::path healthFile;
        std::filesystem::path keyFile;
        std::filesystem::path stopFile;
        std::string ingressEndpoint;
        std::chrono::milliseconds statusInterval{5000};
        std::optional<std::chrono::milliseconds> runFor;
        bool showHelp = false;
    };

    struct GatewayParseResult
    {
        std::optional<GatewayOptions> options;
        std::string error;
    };

    [[nodiscard]] GatewayParseResult ParseGatewayOptions(std::span<const std::string_view> arguments);
    [[nodiscard]] std::string_view GatewayHelpText();

    struct GatewayHealth
    {
        bool live = false;
        bool ready = false;
        bool stopping = false;
        bool authenticationReady = false;
        bool controlPlaneReady = false;
        bool ingressReady = false;
        uint16_t port = 0;
        uint32_t activeAreas = 0;
        uint32_t players = 0;
        size_t sessions = 0;
        std::string lastError;
    };

    class GatewayApplication
    {
      public:
        GatewayApplication(GatewayOptions options, std::unique_ptr<IGatewayAuthenticator> authenticator,
                           std::unique_ptr<IAreaControlPlane> controlPlane);
        ~GatewayApplication();

        GatewayApplication(const GatewayApplication&) = delete;
        GatewayApplication& operator=(const GatewayApplication&) = delete;

        /** [startup thread] Start the world coordinator and register configured areas. */
        [[nodiscard]] bool Start();
        /** [startup thread] Publish health until stopped. Network adapters call Coordinator(). */
        [[nodiscard]] int Run();
        /** [any thread, atomic] Request graceful shutdown. */
        void RequestStop() noexcept;
        /** [startup thread] Stop accepting work, then stop the world coordinator. */
        void Stop();

        [[nodiscard]] GatewayCoordinator* Coordinator() { return m_coordinator.get(); }
        [[nodiscard]] const GatewayCoordinator* Coordinator() const { return m_coordinator.get(); }
        [[nodiscard]] GatewayHealth GetHealth() const;
        [[nodiscard]] std::string GetHealthJson() const;

      private:
        void PublishHealth() const;
        void SetError(std::string message);

        GatewayOptions m_options;
        std::unique_ptr<IGatewayAuthenticator> m_authenticator;
        std::unique_ptr<IAreaControlPlane> m_controlPlane;
        std::unique_ptr<Net::WorldServer> m_worldServer;
        std::unique_ptr<GatewayCoordinator> m_coordinator;
        std::unique_ptr<LocalGatewayIngressService> m_ingress;
        std::atomic<bool> m_stopRequested{false};
        std::atomic<bool> m_started{false};
        std::atomic<bool> m_stopping{false};
        mutable std::mutex m_errorMutex;
        std::string m_lastError;
    };
} // namespace Spark::Gateway
