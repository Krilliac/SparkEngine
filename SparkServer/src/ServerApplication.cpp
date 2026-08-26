/**
 * @file ServerApplication.cpp
 * @brief SparkServer process lifecycle implementation.
 */

#include "ServerApplication.h"

#include "GatewayAreaControl.h"

#include "Core/EngineContext.h"
#include "Core/EngineRuntime.h"
#include "Core/FixedTimestepAccumulator.h"
#include "Core/ModuleManager.h"
#include "Engine/Coroutine/CoroutineScheduler.h"
#include "Engine/ECS/Components.h"
#include "Engine/Events/EventSystem.h"
#include "Engine/SaveSystem/SaveSystem.h"
#include "Utils/ConfigParser.h"
#include "Utils/Timer.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <sstream>
#include <thread>
#include <vector>

namespace Spark::Server
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

        bool ParseFloat(std::string_view text, float minValue, float maxValue, float& output)
        {
            float value = 0.0f;
            std::istringstream parser{std::string{text}};
            parser.imbue(std::locale::classic());
            if (!(parser >> std::noskipws >> value) || parser.peek() != std::char_traits<char>::eof() ||
                !std::isfinite(value) || value < minValue || value > maxValue)
                return false;
            output = value;
            return true;
        }

        std::vector<std::string> SplitCommaSeparated(std::string_view value)
        {
            std::vector<std::string> result;
            size_t start = 0;
            while (start <= value.size())
            {
                const size_t comma = value.find(',', start);
                const size_t end = comma == std::string_view::npos ? value.size() : comma;
                std::string item(value.substr(start, end - start));
                const auto first = item.find_first_not_of(" \t");
                const auto last = item.find_last_not_of(" \t");
                if (first != std::string::npos)
                    result.emplace_back(item.substr(first, last - first + 1));
                if (comma == std::string_view::npos)
                    break;
                start = comma + 1;
            }
            return result;
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

        bool ApplyConfigFile(const std::filesystem::path& path, ServerOptions& options, std::string& error)
        {
            ConfigParser config;
            if (!config.Load(path.string()))
            {
                error = "Cannot load server config: " + path.string();
                return false;
            }

            options.server.serverName = config.GetString("Server", "name", options.server.serverName);
            options.server.motd = config.GetString("Server", "motd", options.server.motd);
            const int port = config.GetInt("Network", "port", options.server.port);
            const int maxClients = config.GetInt("Network", "max_clients", options.server.maxClients);
            const float tickRate = config.GetFloat("Network", "tick_rate", options.server.tickRate);
            if (port < 1 || port > 65535 || maxClients < 1 || maxClients > 100000 || !std::isfinite(tickRate) ||
                tickRate < 1.0f || tickRate > 1000.0f)
            {
                error = "Server config contains an out-of-range port, max_clients, or tick_rate";
                return false;
            }
            options.server.port = static_cast<uint16_t>(port);
            options.server.maxClients = maxClients;
            options.server.tickRate = tickRate;
            options.server.lanOnly = config.GetBool("Network", "lan_only", options.server.lanOnly);
            options.server.enableLanBroadcast =
                config.GetBool("Network", "lan_broadcast", options.server.enableLanBroadcast);
            options.server.mapRotation = SplitCommaSeparated(config.GetString("Game", "maps", "default"));
            options.server.scoreLimit = config.GetInt("Game", "score_limit", options.server.scoreLimit);
            options.server.timeLimitMinutes =
                config.GetFloat("Game", "time_limit_minutes", options.server.timeLimitMinutes);

            const std::string module = config.GetString("Modules", "module");
            const std::string manifest = config.GetString("Modules", "manifest");
            if (!module.empty())
                options.modulePath = module;
            if (!manifest.empty())
                options.manifestPath = manifest;
            if (!options.modulePath.empty() && !options.manifestPath.empty())
            {
                error = "Server config selects both a module and a manifest";
                return false;
            }

            const std::string healthFile = config.GetString("Status", "health_file");
            if (!healthFile.empty())
                options.healthFile = healthFile;
            options.controlEndpoint = config.GetString("GatewayControl", "endpoint");
            const std::string gatewayKey = config.GetString("GatewayControl", "key_file");
            const std::string controlState = config.GetString("GatewayControl", "epoch_state_file");
            if (!gatewayKey.empty())
                options.gatewayKeyFile = gatewayKey;
            if (!controlState.empty())
                options.controlStateFile = controlState;
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

    std::string_view ServerHelpText()
    {
        return "SparkServer\n"
               "  --config <server.ini>       Load defaults from an INI file\n"
               "  --module <game-library>     Load one dynamic game module\n"
               "  --manifest <modules.json>   Load a module manifest\n"
               "  --port <1..65535>            Override the game port\n"
               "  --max-clients <count>        Override the player limit\n"
               "  --tick-rate <hz>             Override the simulation tick rate\n"
               "  --map <name[,name...]>       Override the map rotation\n"
               "  --name <display-name>        Override the server name\n"
               "  --lan-only                   Restrict the server to LAN clients\n"
               "  --no-lan-broadcast           Disable LAN discovery broadcasts\n"
               "  --health-file <path>         Publish a JSON health snapshot\n"
               "  --stop-file <path>           Stop when this sentinel file appears\n"
               "  --control-endpoint <name>    Local gateway area-control endpoint\n"
               "  --gateway-key-file <path>   Owner-only gateway HMAC key\n"
               "  --control-state-file <path> Persist handoff fencing epochs\n"
               "  --status-interval-ms <ms>    Health/status cadence\n"
               "  --run-for-ms <ms>            Bounded run for smoke automation\n"
               "  --help                       Print this help\n";
    }

    ParseResult ParseServerOptions(std::span<const std::string_view> arguments)
    {
        ServerOptions options;
        // Match the documented/config-file default when the CLI launches a
        // module directly without an explicit --map override.
        options.server.mapRotation = {"default"};
        std::filesystem::path configPath;
        for (size_t index = 0; index < arguments.size(); ++index)
        {
            if (arguments[index] == "--config")
            {
                if (++index == arguments.size())
                    return {{}, "--config requires a path"};
                configPath = arguments[index];
            }
        }
        if (!configPath.empty())
        {
            options.configPath = configPath;
            std::string error;
            if (!ApplyConfigFile(configPath, options, error))
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
            else if (argument == "--module" || argument == "--manifest" || argument == "--health-file" ||
                     argument == "--stop-file" || argument == "--control-endpoint" ||
                     argument == "--gateway-key-file" || argument == "--control-state-file" || argument == "--name" ||
                     argument == "--map")
            {
                const auto value = requireValue(index);
                if (!value)
                    return {{}, std::string(argument) + " requires a value"};
                if (argument == "--module")
                    options.modulePath = *value;
                else if (argument == "--manifest")
                    options.manifestPath = *value;
                else if (argument == "--health-file")
                    options.healthFile = *value;
                else if (argument == "--stop-file")
                    options.stopFile = *value;
                else if (argument == "--control-endpoint")
                    options.controlEndpoint = *value;
                else if (argument == "--gateway-key-file")
                    options.gatewayKeyFile = *value;
                else if (argument == "--control-state-file")
                    options.controlStateFile = *value;
                else if (argument == "--name")
                    options.server.serverName = *value;
                else
                    options.server.mapRotation = SplitCommaSeparated(*value);
            }
            else if (argument == "--port")
            {
                const auto value = requireValue(index);
                uint16_t parsed = 0;
                if (!value ||
                    !ParseInteger(*value, static_cast<uint16_t>(1), std::numeric_limits<uint16_t>::max(), parsed))
                    return {{}, "--port must be between 1 and 65535"};
                options.server.port = parsed;
            }
            else if (argument == "--max-clients")
            {
                const auto value = requireValue(index);
                int parsed = 0;
                if (!value || !ParseInteger(*value, 1, 100000, parsed))
                    return {{}, "--max-clients must be between 1 and 100000"};
                options.server.maxClients = parsed;
            }
            else if (argument == "--tick-rate")
            {
                const auto value = requireValue(index);
                float parsed = 0.0f;
                if (!value || !ParseFloat(*value, 1.0f, 1000.0f, parsed))
                    return {{}, "--tick-rate must be between 1 and 1000"};
                options.server.tickRate = parsed;
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
            else if (argument == "--lan-only")
                options.server.lanOnly = true;
            else if (argument == "--no-lan-broadcast")
                options.server.enableLanBroadcast = false;
            else
                return {{}, "Unknown argument: " + std::string(argument)};
        }

        if (!options.modulePath.empty() && !options.manifestPath.empty())
            return {{}, "Select either --module or --manifest, not both"};
        if (!options.showHelp && options.modulePath.empty() && options.manifestPath.empty())
            return {{}, "A dynamic game module is required (--module or --manifest)"};
        if (!options.showHelp && (options.controlEndpoint.empty() != options.gatewayKeyFile.empty()))
            return {{}, "--control-endpoint and --gateway-key-file must be supplied together"};
        if (!options.controlEndpoint.empty() && options.controlStateFile.empty())
            options.controlStateFile = "Temp/spark-area-control-epochs.txt";
        if (options.server.mapRotation.empty())
            return {{}, "At least one non-empty map is required"};
        return {std::move(options), {}};
    }

    ServerApplication::ServerApplication(ServerOptions options) : m_options(std::move(options)) {}

    ServerApplication::~ServerApplication()
    {
        if (m_started.load(std::memory_order_acquire))
            (void)Stop();
    }

    bool ServerApplication::LoadSelectedModules()
    {
        const bool loaded = !m_options.modulePath.empty()
                                ? m_modules->LoadModule(m_options.modulePath.string())
                                : m_modules->LoadModulesFromManifest(m_options.manifestPath.string());
        if (!loaded)
        {
            SetError("Failed to load the selected dynamic game module(s)");
            return false;
        }
        m_modules->InitializeAll(EngineContext::Get());
        if (m_modules->GetGameModuleName().empty())
        {
            SetError("The selected module set contains no initialized Game module");
            (void)m_modules->ShutdownAll();
            m_modules->UnloadAll();
            return false;
        }
        return true;
    }

    bool ServerApplication::Start()
    {
        if (m_started.load(std::memory_order_acquire))
        {
            SetError("SparkServer is already running");
            return false;
        }

        auto& runtime = GetEngineRuntime();
        runtime.timer = std::make_unique<Timer>();
        runtime.timer->Start();
        runtime.eventBus = std::make_unique<Spark::EventBus>();
        EngineContext::SetOwned(
            std::make_unique<EngineContext>(nullptr, nullptr, runtime.timer.get(), runtime.eventBus.get()));
        EngineContext* context = EngineContext::Get();
        if (!context)
        {
            SetError("Failed to create the headless EngineContext");
            return false;
        }

        runtime.InitializeHeadlessAssetServices(*context);
        m_world = std::make_unique<World>();
        context->SetWorld(m_world.get());
        context->SetSaveSystem(&Spark::SaveSystem::GetInstance());
        context->SetCoroutineScheduler(&Spark::CoroutineScheduler::GetInstance());
        m_modules = std::make_unique<ModuleManager>();
        m_modules->SetFileCache(runtime.fileCache.get());
        if (!LoadSelectedModules())
        {
            runtime.ShutdownHeadlessAssetServices();
            EngineContext::ResetOwned();
            runtime.eventBus.reset();
            runtime.timer.reset();
            return false;
        }

        m_server = std::make_unique<Net::DedicatedServer>();
        if (!m_server->Start(m_options.server))
        {
            SetError("DedicatedServer failed to bind or initialize networking");
            (void)m_modules->ShutdownAll();
            m_modules->UnloadAll();
            m_modules.reset();
            runtime.ShutdownHeadlessAssetServices();
            EngineContext::ResetOwned();
            runtime.eventBus.reset();
            runtime.timer.reset();
            return false;
        }

        Spark::FixedTimestepAccumulator::GetInstance().Initialize(1.0f / m_options.server.tickRate);
        m_stopRequested.store(false, std::memory_order_release);
        if (!m_options.controlEndpoint.empty())
        {
            m_controlService = std::make_unique<Gateway::LocalAreaControlService>(
                m_options.controlEndpoint, m_options.gatewayKeyFile, m_options.controlStateFile);
            if (!m_controlService->Start())
            {
                SetError("Failed to start authenticated gateway area-control service");
                m_server->Stop();
                m_server.reset();
                m_modules->UnloadAll();
                m_modules.reset();
                m_world.reset();
                Spark::FixedTimestepAccumulator::GetInstance().Shutdown();
                runtime.ShutdownHeadlessAssetServices();
                EngineContext::ResetOwned();
                runtime.eventBus.reset();
                runtime.timer.reset();
                return false;
            }
        }
        m_started.store(true, std::memory_order_release);
        PublishHealth();
        return true;
    }

    int ServerApplication::Run()
    {
        if (!m_started.load(std::memory_order_acquire))
            return 2;

        const auto startedAt = std::chrono::steady_clock::now();
        auto nextStatus = startedAt;
        auto lastTick = startedAt;
        const auto frameBudget = std::chrono::duration<float>(1.0f / m_options.server.tickRate);
        bool draining = false;
        while (true)
        {
            const auto tickStart = std::chrono::steady_clock::now();
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
                    PublishHealth();
                }
                if (m_modules->CanShutdownAll())
                    break;
            }
            const float deltaTime = std::clamp(std::chrono::duration<float>(tickStart - lastTick).count(), 0.0f, 0.25f);
            lastTick = tickStart;
            m_modules->UpdateAll(deltaTime);
            auto& fixed = Spark::FixedTimestepAccumulator::GetInstance();
            fixed.Advance(deltaTime);
            for (uint32_t step = fixed.GetFixedStepCount(); step > 0; --step)
                m_modules->FixedUpdateAll(fixed.GetFixedTimestep());

            if (tickStart >= nextStatus)
            {
                PublishHealth();
                nextStatus = tickStart + m_options.statusInterval;
            }
            if (m_options.runFor && tickStart - startedAt >= *m_options.runFor)
                RequestStop();

            const auto elapsed = std::chrono::steady_clock::now() - tickStart;
            if (elapsed < frameBudget)
                std::this_thread::sleep_for(frameBudget - elapsed);
        }
        return Stop() ? 0 : 3;
    }

    void ServerApplication::RequestStop() noexcept
    {
        m_stopRequested.store(true, std::memory_order_release);
    }

    bool ServerApplication::Stop()
    {
        if (!m_started.load(std::memory_order_acquire))
            return true;
        m_stopping.store(true, std::memory_order_release);
        PublishHealth();
        if (m_modules && !m_modules->CanShutdownAll())
        {
            SetError("A game module vetoed graceful shutdown");
            m_stopping.store(false, std::memory_order_release);
            PublishHealth();
            return false;
        }

        if (m_modules)
            m_modules->ShutdownAllAfterPreflight();
        if (m_controlService)
            m_controlService->Stop();
        m_controlService.reset();
        if (m_server)
            m_server->Stop();
        if (m_modules)
            m_modules->UnloadAll();
        m_server.reset();
        m_modules.reset();
        Spark::FixedTimestepAccumulator::GetInstance().Shutdown();
        m_world.reset();

        auto& runtime = GetEngineRuntime();
        runtime.ShutdownHeadlessAssetServices();
        EngineContext::ResetOwned();
        runtime.eventBus.reset();
        runtime.timer.reset();
        m_started.store(false, std::memory_order_release);
        m_stopping.store(false, std::memory_order_release);
        PublishHealth();
        return true;
    }

    ServerHealth ServerApplication::GetHealth() const
    {
        ServerHealth health;
        health.live = m_started.load(std::memory_order_acquire);
        health.stopping = m_stopping.load(std::memory_order_acquire);
        health.ready = health.live && !health.stopping && m_server && m_server->IsRunning() && m_modules &&
                       !m_modules->GetGameModuleName().empty() &&
                       (m_options.controlEndpoint.empty() || (m_controlService && m_controlService->IsReady()));
        health.port = m_options.server.port;
        if (m_modules)
        {
            health.loadedModules = m_modules->GetModuleCount();
            health.gameModule = m_modules->GetGameModuleName();
        }
        if (m_server)
        {
            const Net::ServerStats& stats = m_server->GetStats();
            health.players = stats.currentPlayers;
            health.ticks = stats.totalTicksProcessed;
            health.currentMap = stats.currentMap;
        }
        std::lock_guard lock(m_errorMutex);
        health.lastError = m_lastError;
        return health;
    }

    std::string ServerApplication::GetHealthJson() const
    {
        const ServerHealth health = GetHealth();
        std::ostringstream stream;
        stream << "{\"live\":" << (health.live ? "true" : "false") << ",\"ready\":" << (health.ready ? "true" : "false")
               << ",\"stopping\":" << (health.stopping ? "true" : "false") << ",\"port\":" << health.port
               << ",\"players\":" << health.players << ",\"ticks\":" << health.ticks
               << ",\"loadedModules\":" << health.loadedModules << ",\"gameModule\":\"" << EscapeJson(health.gameModule)
               << "\",\"map\":\"" << EscapeJson(health.currentMap) << "\",\"error\":\"" << EscapeJson(health.lastError)
               << "\"}";
        return stream.str();
    }

    void ServerApplication::PublishHealth() const
    {
        const std::string json = GetHealthJson();
        std::cout << json << '\n';
        if (m_options.healthFile.empty())
            return;

        std::error_code error;
        const std::filesystem::path parent = m_options.healthFile.parent_path();
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

    void ServerApplication::SetError(std::string message)
    {
        std::lock_guard lock(m_errorMutex);
        m_lastError = std::move(message);
    }
} // namespace Spark::Server
