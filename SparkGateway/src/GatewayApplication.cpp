/**
 * @file GatewayApplication.cpp
 * @brief SparkGateway process lifecycle implementation.
 */

#include "GatewayApplication.h"
#include "GatewayAreaControl.h"

#include "Utils/ConfigParser.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <thread>

namespace Spark::Gateway
{
    namespace
    {
        template <typename T> bool ParseInteger(std::string_view text, T minValue, T maxValue, T& output)
        {
            T value{};
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size() || value < minValue || value > maxValue)
                return false;
            output = value;
            return true;
        }

        std::string EscapeJson(std::string_view value)
        {
            std::ostringstream stream;
            for (const unsigned char character : value)
            {
                switch (character)
                {
                case '"':
                    stream << "\\\"";
                    break;
                case '\\':
                    stream << "\\\\";
                    break;
                case '\n':
                    stream << "\\n";
                    break;
                case '\r':
                    stream << "\\r";
                    break;
                case '\t':
                    stream << "\\t";
                    break;
                default:
                    if (character < 0x20)
                        stream << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                               << static_cast<unsigned int>(character) << std::dec;
                    else
                        stream << static_cast<char>(character);
                }
            }
            return stream.str();
        }

        bool ApplyConfig(const std::filesystem::path& path, GatewayOptions& options, std::string& error)
        {
            ConfigParser config;
            if (!config.Load(path.string()))
            {
                error = "Cannot load gateway config: " + path.string();
                return false;
            }

            options.world.worldName = config.GetString("Gateway", "world_name", options.world.worldName);
            const int port = config.GetInt("Gateway", "port", options.world.port);
            const int interServerPort = config.GetInt("Gateway", "inter_server_port", options.world.interServerPort);
            const int maxClients = config.GetInt("Gateway", "max_total_clients", options.world.maxTotalClients);
            const float tickRate = config.GetFloat("Gateway", "tick_rate", options.world.tickRate);
            if (port < 1 || port > 65535 || interServerPort < 1 || interServerPort > 65535 || maxClients < 1 ||
                maxClients > 1000000 || !std::isfinite(tickRate) || tickRate < 1.0f || tickRate > 1000.0f)
            {
                error = "Gateway config contains an out-of-range port, capacity, or tick_rate";
                return false;
            }
            options.world.port = static_cast<uint16_t>(port);
            options.world.interServerPort = static_cast<uint16_t>(interServerPort);
            options.world.maxTotalClients = maxClients;
            options.world.tickRate = tickRate;
            options.world.enableLoadBalancing =
                config.GetBool("Gateway", "load_balancing", options.world.enableLoadBalancing);
            options.world.loadBalanceInterval =
                config.GetFloat("Gateway", "load_balance_interval", options.world.loadBalanceInterval);
            options.ingressEndpoint = config.GetString("Gateway", "ingress_endpoint");

            options.areas.clear();
            for (const std::string& section : config.GetSections())
            {
                constexpr std::string_view prefix = "Area.";
                if (!section.starts_with(prefix))
                    continue;
                AreaEndpoint endpoint;
                endpoint.area.areaName = section.substr(prefix.size());
                endpoint.host = config.GetString(section, "host", "127.0.0.1");
                endpoint.area.scenePath = config.GetString(section, "scene");
                const int areaPort = config.GetInt(section, "port", 0);
                const int areaInterPort = config.GetInt(section, "inter_server_port", 0);
                endpoint.area.maxClients = config.GetInt(section, "max_clients", endpoint.area.maxClients);
                endpoint.area.tickRate = config.GetFloat(section, "tick_rate", endpoint.area.tickRate);
                endpoint.area.enableAI = config.GetBool(section, "enable_ai", endpoint.area.enableAI);
                endpoint.area.enablePhysics = config.GetBool(section, "enable_physics", endpoint.area.enablePhysics);
                endpoint.area.enableScripting =
                    config.GetBool(section, "enable_scripting", endpoint.area.enableScripting);
                if (endpoint.area.areaName.empty() || endpoint.host.empty() || areaPort < 1 || areaPort > 65535 ||
                    areaInterPort < 1 || areaInterPort > 65535 || endpoint.area.maxClients < 1 ||
                    !std::isfinite(endpoint.area.tickRate) || endpoint.area.tickRate < 1.0f ||
                    endpoint.area.tickRate > 1000.0f)
                {
                    error = "Invalid endpoint in config section [" + section + "]";
                    return false;
                }
                endpoint.area.port = static_cast<uint16_t>(areaPort);
                endpoint.area.interServerPort = static_cast<uint16_t>(areaInterPort);
                options.areas.push_back(std::move(endpoint));
            }
            if (options.areas.empty())
            {
                error = "Gateway config must contain at least one [Area.<name>] section";
                return false;
            }

            const std::string healthFile = config.GetString("Status", "health_file");
            if (!healthFile.empty())
                options.healthFile = healthFile;
            const std::string keyFile = config.GetString("Security", "key_file");
            if (!keyFile.empty())
                options.keyFile = keyFile;
            const std::string stopFile = config.GetString("Status", "stop_file");
            if (!stopFile.empty())
                options.stopFile = stopFile;
            const int interval =
                config.GetInt("Status", "interval_ms", static_cast<int>(options.statusInterval.count()));
            if (interval < 100 || interval > 3600000)
            {
                error = "Status.interval_ms must be between 100 and 3600000";
                return false;
            }
            options.statusInterval = std::chrono::milliseconds(interval);
            return true;
        }
    } // namespace

    std::string_view GatewayHelpText()
    {
        return "SparkGateway\n"
               "  --config <gateway.ini>      Load gateway and [Area.<name>] definitions\n"
               "  --port <1..65535>            Override the client-facing coordinator port\n"
               "  --health-file <path>         Publish a JSON health snapshot\n"
               "  --key-file <path>            Owner-only >=256-bit local authentication key\n"
               "  --stop-file <path>           Stop when this sentinel file appears\n"
               "  --status-interval-ms <ms>    Health/status cadence\n"
               "  --run-for-ms <ms>            Bounded run for smoke automation\n"
               "  --help                       Print this help\n\n"
               "The key and every configured local area-control endpoint must be ready at startup.\n";
    }

    GatewayParseResult ParseGatewayOptions(std::span<const std::string_view> arguments)
    {
        GatewayOptions options;
        for (size_t index = 0; index < arguments.size(); ++index)
        {
            if (arguments[index] == "--config")
            {
                if (++index == arguments.size())
                    return {{}, "--config requires a path"};
                options.configPath = arguments[index];
            }
        }

        const bool helpRequested = std::find(arguments.begin(), arguments.end(), "--help") != arguments.end() ||
                                   std::find(arguments.begin(), arguments.end(), "-h") != arguments.end();
        if (options.configPath.empty() && !helpRequested)
            return {{}, "--config is required"};
        if (!options.configPath.empty())
        {
            std::string error;
            if (!ApplyConfig(options.configPath, options, error))
                return {{}, std::move(error)};
        }

        auto requireValue = [&](size_t& index) -> std::optional<std::string_view>
        {
            if (++index == arguments.size())
                return std::nullopt;
            return arguments[index];
        };
        for (size_t index = 0; index < arguments.size(); ++index)
        {
            const std::string_view argument = arguments[index];
            if (argument == "--help" || argument == "-h")
                options.showHelp = true;
            else if (argument == "--config")
                ++index;
            else if (argument == "--health-file" || argument == "--key-file" || argument == "--stop-file")
            {
                const auto value = requireValue(index);
                if (!value)
                    return {{}, std::string(argument) + " requires a path"};
                if (argument == "--health-file")
                    options.healthFile = *value;
                else if (argument == "--key-file")
                    options.keyFile = *value;
                else
                    options.stopFile = *value;
            }
            else if (argument == "--port")
            {
                const auto value = requireValue(index);
                uint16_t parsed = 0;
                if (!value ||
                    !ParseInteger(*value, static_cast<uint16_t>(1), std::numeric_limits<uint16_t>::max(), parsed))
                    return {{}, "--port must be between 1 and 65535"};
                options.world.port = parsed;
            }
            else if (argument == "--status-interval-ms" || argument == "--run-for-ms")
            {
                const auto value = requireValue(index);
                int64_t parsed = 0;
                const int64_t minimum = argument == "--status-interval-ms" ? 100 : 1;
                if (!value || !ParseInteger(*value, minimum, int64_t{3600000}, parsed))
                    return {{}, std::string(argument) + " is out of range"};
                if (argument == "--status-interval-ms")
                    options.statusInterval = std::chrono::milliseconds(parsed);
                else
                    options.runFor = std::chrono::milliseconds(parsed);
            }
            else
                return {{}, "Unknown argument: " + std::string(argument)};
        }
        if (!options.showHelp && options.keyFile.empty())
            return {{}, "A gateway --key-file is required"};
        if (options.ingressEndpoint.empty())
            options.ingressEndpoint = "spark-gateway-ingress-" + std::to_string(options.world.port);
        return {std::move(options), {}};
    }

    GatewayApplication::GatewayApplication(GatewayOptions options, std::unique_ptr<IGatewayAuthenticator> authenticator,
                                           std::unique_ptr<IAreaControlPlane> controlPlane)
        : m_options(std::move(options)), m_authenticator(std::move(authenticator)),
          m_controlPlane(std::move(controlPlane))
    {
    }

    GatewayApplication::~GatewayApplication()
    {
        Stop();
    }

    bool GatewayApplication::Start()
    {
        if (m_started.load(std::memory_order_acquire))
        {
            SetError("SparkGateway is already running");
            return false;
        }
        if (!m_authenticator || !m_controlPlane)
        {
            SetError("Gateway authentication and control-plane adapters are required");
            return false;
        }
        if (!m_authenticator->IsReady())
        {
            SetError("Gateway authentication key is unavailable or insecure");
            return false;
        }
        m_worldServer = std::make_unique<Net::WorldServer>();
        if (!m_worldServer->Start(m_options.world))
        {
            SetError("WorldServer failed to start");
            m_worldServer.reset();
            return false;
        }
        m_coordinator = std::make_unique<GatewayCoordinator>(*m_worldServer, *m_authenticator, *m_controlPlane);
        if (!m_coordinator->RegisterAreas(m_options.areas))
        {
            SetError("No valid area endpoints could be registered");
            m_worldServer->Stop();
            m_coordinator.reset();
            m_worldServer.reset();
            return false;
        }
        if (!m_coordinator->IsReady())
        {
            SetError("One or more authenticated local area-control endpoints are unavailable");
            m_coordinator.reset();
            m_worldServer->Stop();
            m_worldServer.reset();
            return false;
        }
        m_ingress = std::make_unique<LocalGatewayIngressService>(m_options.ingressEndpoint, *m_coordinator);
        if (!m_ingress->Start())
        {
            SetError("Authenticated local gateway ingress failed to start");
            m_coordinator.reset();
            m_worldServer->Stop();
            m_worldServer.reset();
            return false;
        }
        m_stopRequested.store(false, std::memory_order_release);
        m_started.store(true, std::memory_order_release);
        PublishHealth();
        return true;
    }

    int GatewayApplication::Run()
    {
        if (!m_started.load(std::memory_order_acquire))
            return 2;
        const auto startedAt = std::chrono::steady_clock::now();
        auto nextStatus = startedAt;
        bool draining = false;
        while (true)
        {
            const auto now = std::chrono::steady_clock::now();
            if (!m_options.stopFile.empty())
            {
                std::error_code stopError;
                if (std::filesystem::is_regular_file(m_options.stopFile, stopError))
                    RequestStop();
            }
            if (m_stopRequested.load(std::memory_order_acquire))
            {
                if (!draining)
                {
                    draining = true;
                    m_stopping.store(true, std::memory_order_release);
                    m_coordinator->BeginDrain();
                    PublishHealth();
                }
                if (m_coordinator->CanShutdown())
                    break;
            }
            if (now >= nextStatus)
            {
                PublishHealth();
                nextStatus = now + m_options.statusInterval;
            }
            if (m_options.runFor && now - startedAt >= *m_options.runFor)
                RequestStop();
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        Stop();
        return 0;
    }

    void GatewayApplication::RequestStop() noexcept
    {
        m_stopRequested.store(true, std::memory_order_release);
    }

    void GatewayApplication::Stop()
    {
        if (!m_started.load(std::memory_order_acquire))
            return;
        m_stopping.store(true, std::memory_order_release);
        PublishHealth();
        // Destroy ingress-facing coordinator before its WorldServer dependency.
        if (m_ingress)
            m_ingress->Stop();
        m_ingress.reset();
        m_coordinator.reset();
        if (m_worldServer)
            m_worldServer->Stop();
        m_worldServer.reset();
        m_started.store(false, std::memory_order_release);
        m_stopping.store(false, std::memory_order_release);
        PublishHealth();
    }

    GatewayHealth GatewayApplication::GetHealth() const
    {
        GatewayHealth health;
        health.live = m_started.load(std::memory_order_acquire) && m_worldServer && m_worldServer->IsRunning();
        health.stopping = m_stopping.load(std::memory_order_acquire);
        health.authenticationReady = m_authenticator && m_authenticator->IsReady();
        health.controlPlaneReady = m_controlPlane && m_controlPlane->IsReady();
        health.ingressReady = m_ingress && m_ingress->IsReady();
        health.ready =
            health.live && !health.stopping && health.ingressReady && m_coordinator && m_coordinator->IsReady();
        health.port = m_options.world.port;
        if (m_worldServer)
        {
            const Net::WorldServerStats stats = m_worldServer->GetStats();
            health.activeAreas = stats.activeAreas;
            health.players = stats.totalPlayers;
        }
        if (m_coordinator)
            health.sessions = m_coordinator->GetSessionCount();
        std::lock_guard lock(m_errorMutex);
        health.lastError = m_lastError;
        return health;
    }

    std::string GatewayApplication::GetHealthJson() const
    {
        const GatewayHealth health = GetHealth();
        std::ostringstream stream;
        stream << "{\"live\":" << (health.live ? "true" : "false") << ",\"ready\":" << (health.ready ? "true" : "false")
               << ",\"stopping\":" << (health.stopping ? "true" : "false")
               << ",\"authenticationReady\":" << (health.authenticationReady ? "true" : "false")
               << ",\"controlPlaneReady\":" << (health.controlPlaneReady ? "true" : "false")
               << ",\"ingressReady\":" << (health.ingressReady ? "true" : "false") << ",\"port\":" << health.port
               << ",\"activeAreas\":" << health.activeAreas << ",\"players\":" << health.players
               << ",\"sessions\":" << health.sessions << ",\"error\":\"" << EscapeJson(health.lastError) << "\"}";
        return stream.str();
    }

    void GatewayApplication::PublishHealth() const
    {
        const std::string json = GetHealthJson();
        std::cout << json << '\n';
        if (m_options.healthFile.empty())
            return;
        std::error_code error;
        const auto parent = m_options.healthFile.parent_path();
        if (!parent.empty())
            std::filesystem::create_directories(parent, error);
        const std::filesystem::path temporary = m_options.healthFile.string() + ".tmp";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output << json << '\n';
        }
        std::filesystem::rename(temporary, m_options.healthFile, error);
        if (error)
        {
            std::filesystem::remove(m_options.healthFile, error);
            error.clear();
            std::filesystem::rename(temporary, m_options.healthFile, error);
        }
    }

    void GatewayApplication::SetError(std::string message)
    {
        std::lock_guard lock(m_errorMutex);
        m_lastError = std::move(message);
    }
} // namespace Spark::Gateway
