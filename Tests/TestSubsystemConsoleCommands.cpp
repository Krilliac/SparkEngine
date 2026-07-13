/**
 * @file TestSubsystemConsoleCommands.cpp
 * @brief Tests for the new settings sections (Weather, TimeOfDay, Streaming,
 *        Performance, World, UI) and subsystem console command registration.
 *
 * Uses standalone reimplementations for CI/Linux compatibility.
 */

#include "TestFramework.h"

#include <functional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{

    // ========================================================================
    // Minimal settings model matching the new EngineSettings sections
    // ========================================================================

    struct WeatherSettings
    {
        int weatherType = 0;
        float intensity = 0.0f;
        float transitionTime = 5.0f;
        float windSpeed = 0.0f;
        float windDirectionX = 1.0f;
        float windDirectionY = 0.0f;
        float windDirectionZ = 0.0f;
        float windGustiness = 0.0f;
        float fogDensity = 0.0f;
        float fogStartDistance = 10.0f;
        float fogEndDistance = 500.0f;
        float precipitationRate = 100.0f;
        float precipitationSize = 1.0f;
        float lightningFrequency = 0.0f;
        float thunderDelay = 2.0f;
        float ambientMultiplier = 1.0f;
        float directionalMultiplier = 1.0f;
    };

    struct TimeOfDaySettings
    {
        float startHour = 12.0f;
        float timeScale = 60.0f;
        bool paused = false;
        float sunriseHour = 6.0f;
        float sunsetHour = 20.0f;
        float moonIntensity = 0.1f;
        float ambientNightMultiplier = 0.2f;
    };

    struct StreamingSettings
    {
        float drawDistance = 1000.0f;
        float lodDistanceMultiplier = 1.0f;
        float streamingDistance = 2000.0f;
        int maxLoadedAreas = 4;
        float lodBias = 0.0f;
        int maxConcurrentLoads = 2;
        bool enableStreaming = true;
        float shadowDrawDistance = 200.0f;
    };

    struct PerformanceSettings
    {
        int targetFPS = 0;
        int workerThreadCount = 0;
        bool enableJobSystem = true;
        int maxParticles = 10000;
        int maxDecals = 256;
        bool enableAsyncCompute = false;
        bool enableAsyncLoading = true;
        float objectPoolGrowthFactor = 1.5f;
    };

    struct WorldSettings
    {
        float originRebaseThreshold = 5000.0f;
        float defaultGravityScale = 1.0f;
        bool enableOriginRebasing = true;
        float worldBoundsMin = -100000.0f;
        float worldBoundsMax = 100000.0f;
    };

    struct UISettings
    {
        float uiScale = 1.0f;
        float fontSize = 14.0f;
        bool showCrosshair = true;
        bool showHUD = true;
        bool showMinimap = true;
        bool showDamageNumbers = true;
        bool showSubtitles = true;
        float hudOpacity = 1.0f;
        float subtitleSize = 1.0f;
        bool showInteractionPrompts = true;
    };

    // ========================================================================
    // Console command model
    // ========================================================================

    using CommandHandler = std::function<std::string(const std::vector<std::string>&)>;

    struct CommandInfo
    {
        CommandHandler handler;
        std::string description;
        std::string category;
    };

    class MockConsole
    {
      public:
        void RegisterCommand(const std::string& name, CommandHandler handler, const std::string& desc,
                             const std::string& category = "")
        {
            m_commands[name] = {std::move(handler), desc, category};
        }

        std::string Execute(const std::string& name, const std::vector<std::string>& args = {})
        {
            auto it = m_commands.find(name);
            if (it == m_commands.end())
                return "Unknown command: " + name;
            return it->second.handler(args);
        }

        bool HasCommand(const std::string& name) const { return m_commands.count(name) > 0; }
        size_t CommandCount() const { return m_commands.size(); }

        std::vector<std::string> GetCommandsByCategory(const std::string& category) const
        {
            std::vector<std::string> result;
            for (auto& [name, info] : m_commands)
            {
                if (info.category == category)
                    result.push_back(name);
            }
            return result;
        }

      private:
        std::unordered_map<std::string, CommandInfo> m_commands;
    };

} // namespace

// ============================================================================
// Weather settings defaults
// ============================================================================

TEST(SettingsWeatherDefaults)
{
    WeatherSettings ws;
    ASSERT_EQ(ws.weatherType, 0);
    EXPECT_NEAR(ws.intensity, 0.0f, 1.0e-5f);
    EXPECT_NEAR(ws.transitionTime, 5.0f, 1.0e-5f);
    EXPECT_NEAR(ws.windSpeed, 0.0f, 1.0e-5f);
    EXPECT_NEAR(ws.fogDensity, 0.0f, 1.0e-5f);
    EXPECT_NEAR(ws.fogStartDistance, 10.0f, 1.0e-5f);
    EXPECT_NEAR(ws.fogEndDistance, 500.0f, 1.0e-5f);
    EXPECT_NEAR(ws.precipitationRate, 100.0f, 1.0e-5f);
    EXPECT_NEAR(ws.ambientMultiplier, 1.0f, 1.0e-5f);
    EXPECT_NEAR(ws.directionalMultiplier, 1.0f, 1.0e-5f);
}

TEST(SettingsTimeOfDayDefaults)
{
    TimeOfDaySettings tod;
    EXPECT_NEAR(tod.startHour, 12.0f, 1.0e-5f);
    EXPECT_NEAR(tod.timeScale, 60.0f, 1.0e-5f);
    ASSERT_EQ(tod.paused, false);
    EXPECT_NEAR(tod.sunriseHour, 6.0f, 1.0e-5f);
    EXPECT_NEAR(tod.sunsetHour, 20.0f, 1.0e-5f);
    EXPECT_NEAR(tod.moonIntensity, 0.1f, 1.0e-5f);
}

TEST(SettingsStreamingDefaults)
{
    StreamingSettings ss;
    EXPECT_NEAR(ss.drawDistance, 1000.0f, 1.0e-5f);
    EXPECT_NEAR(ss.lodDistanceMultiplier, 1.0f, 1.0e-5f);
    EXPECT_NEAR(ss.streamingDistance, 2000.0f, 1.0e-5f);
    ASSERT_EQ(ss.maxLoadedAreas, 4);
    EXPECT_NEAR(ss.lodBias, 0.0f, 1.0e-5f);
    ASSERT_EQ(ss.maxConcurrentLoads, 2);
    ASSERT_EQ(ss.enableStreaming, true);
    EXPECT_NEAR(ss.shadowDrawDistance, 200.0f, 1.0e-5f);
}

TEST(SettingsPerformanceDefaults)
{
    PerformanceSettings ps;
    ASSERT_EQ(ps.targetFPS, 0);
    ASSERT_EQ(ps.workerThreadCount, 0);
    ASSERT_EQ(ps.enableJobSystem, true);
    ASSERT_EQ(ps.maxParticles, 10000);
    ASSERT_EQ(ps.maxDecals, 256);
    ASSERT_EQ(ps.enableAsyncCompute, false);
    ASSERT_EQ(ps.enableAsyncLoading, true);
    EXPECT_NEAR(ps.objectPoolGrowthFactor, 1.5f, 1.0e-5f);
}

TEST(SettingsWorldDefaults)
{
    WorldSettings ws;
    EXPECT_NEAR(ws.originRebaseThreshold, 5000.0f, 1.0e-5f);
    EXPECT_NEAR(ws.defaultGravityScale, 1.0f, 1.0e-5f);
    ASSERT_EQ(ws.enableOriginRebasing, true);
}

TEST(SettingsUIDefaults)
{
    UISettings ui;
    EXPECT_NEAR(ui.uiScale, 1.0f, 1.0e-5f);
    EXPECT_NEAR(ui.fontSize, 14.0f, 1.0e-5f);
    ASSERT_EQ(ui.showCrosshair, true);
    ASSERT_EQ(ui.showHUD, true);
    ASSERT_EQ(ui.showMinimap, true);
    ASSERT_EQ(ui.showDamageNumbers, true);
    ASSERT_EQ(ui.showSubtitles, true);
    EXPECT_NEAR(ui.hudOpacity, 1.0f, 1.0e-5f);
    ASSERT_EQ(ui.showInteractionPrompts, true);
}

// ============================================================================
// Console command registration tests
// ============================================================================

TEST(ConsoleCommandCategories)
{
    MockConsole console;

    // Simulate camera command registration
    console.RegisterCommand("cam_fov", [](const std::vector<std::string>&) { return "ok"; }, "Set FOV", "Camera");
    console.RegisterCommand("cam_speed", [](const std::vector<std::string>&) { return "ok"; }, "Set speed", "Camera");
    console.RegisterCommand("cam_info", [](const std::vector<std::string>&) { return "ok"; }, "Camera info", "Camera");

    auto cameraCmds = console.GetCommandsByCategory("Camera");
    ASSERT_EQ(cameraCmds.size(), 3u);
    ASSERT_TRUE(console.HasCommand("cam_fov"));
    ASSERT_TRUE(console.HasCommand("cam_speed"));
    ASSERT_TRUE(console.HasCommand("cam_info"));
}

TEST(ConsoleNetworkLagSim)
{
    MockConsole console;
    float simulatedLatency = 0.0f;

    console.RegisterCommand(
        "net_lag",
        [&simulatedLatency](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "Usage: net_lag <ms>";
            simulatedLatency = std::stof(args[0]);
            return "Latency set to " + args[0] + " ms";
        },
        "Simulate latency", "Network");

    auto result = console.Execute("net_lag", {"100"});
    ASSERT_TRUE(result.find("100") != std::string::npos);
    EXPECT_NEAR(simulatedLatency, 100.0f, 1.0e-5f);
}

TEST(ConsoleWeatherCommand)
{
    MockConsole console;
    int weatherType = -1;
    float weatherIntensity = -1.0f;

    static const std::unordered_map<std::string, int> types = {{"clear", 0}, {"cloudy", 1}, {"rain", 2},
                                                               {"snow", 3},  {"fog", 4},    {"storm", 5}};

    console.RegisterCommand(
        "weather_set",
        [&weatherType, &weatherIntensity](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "Usage: weather_set <type> [intensity]";
            auto it = types.find(args[0]);
            if (it == types.end())
                return "Unknown weather type";
            weatherType = it->second;
            weatherIntensity = (args.size() > 1) ? std::stof(args[1]) : -1.0f;
            return "Weather set to " + args[0];
        },
        "Set weather", "Weather");

    console.Execute("weather_set", {"rain", "0.8"});
    ASSERT_EQ(weatherType, 2);
    EXPECT_NEAR(weatherIntensity, 0.8f, 1.0e-5f);

    console.Execute("weather_set", {"storm"});
    ASSERT_EQ(weatherType, 5);
    EXPECT_NEAR(weatherIntensity, -1.0f, 1.0e-5f);

    auto result = console.Execute("weather_set", {"blizzard"});
    ASSERT_TRUE(result.find("Unknown") != std::string::npos);
}

TEST(ConsolePostProcessToggle)
{
    MockConsole console;
    std::unordered_map<std::string, bool> effects = {
        {"bloom", false}, {"fxaa", true}, {"dof", false}, {"vignette", false}};

    console.RegisterCommand(
        "pp_enable",
        [&effects](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "Usage: pp_enable <effect>";
            auto it = effects.find(args[0]);
            if (it == effects.end())
                return "Unknown effect";
            it->second = true;
            return args[0] + " enabled";
        },
        "Enable effect", "PostProcess");

    console.RegisterCommand(
        "pp_disable",
        [&effects](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "Usage: pp_disable <effect>";
            auto it = effects.find(args[0]);
            if (it == effects.end())
                return "Unknown effect";
            it->second = false;
            return args[0] + " disabled";
        },
        "Disable effect", "PostProcess");

    console.Execute("pp_enable", {"bloom"});
    ASSERT_TRUE(effects["bloom"]);
    ASSERT_TRUE(effects["fxaa"]);

    console.Execute("pp_disable", {"fxaa"});
    ASSERT_FALSE(effects["fxaa"]);
    ASSERT_TRUE(effects["bloom"]);
}

TEST(ConsoleInputBindings)
{
    MockConsole console;
    std::unordered_map<std::string, std::string> bindings;

    console.RegisterCommand(
        "input_bind",
        [&bindings](const std::vector<std::string>& args) -> std::string
        {
            if (args.size() < 2)
                return "Usage: input_bind <action> <key>";
            bindings[args[0]] = args[1];
            return "Bound " + args[0] + " to " + args[1];
        },
        "Bind key", "Input");

    console.RegisterCommand(
        "input_unbind",
        [&bindings](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "Usage: input_unbind <action>";
            bindings.erase(args[0]);
            return "Unbound " + args[0];
        },
        "Unbind key", "Input");

    console.Execute("input_bind", {"jump", "space"});
    console.Execute("input_bind", {"shoot", "mouse1"});
    ASSERT_EQ(bindings["jump"], "space");
    ASSERT_EQ(bindings["shoot"], "mouse1");

    console.Execute("input_unbind", {"jump"});
    ASSERT_TRUE(bindings.find("jump") == bindings.end());
    ASSERT_EQ(bindings["shoot"], "mouse1");
}

TEST(ConsoleSettingsShortcuts)
{
    MockConsole console;
    int targetFPS = 0;
    float drawDist = 1000.0f;
    bool showHUD = true;

    console.RegisterCommand(
        "fps_cap",
        [&targetFPS](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "FPS: " + std::to_string(targetFPS);
            targetFPS = std::stoi(args[0]);
            return "FPS set to " + args[0];
        },
        "Set FPS cap", "Performance");

    console.RegisterCommand(
        "draw_distance",
        [&drawDist](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "Draw distance: " + std::to_string(drawDist);
            drawDist = std::stof(args[0]);
            return "Draw distance set to " + args[0];
        },
        "Set draw distance", "Streaming");

    console.RegisterCommand(
        "hud",
        [&showHUD](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "Usage: hud <on|off>";
            showHUD = (args[0] == "on" || args[0] == "true" || args[0] == "1");
            return showHUD ? "HUD shown" : "HUD hidden";
        },
        "Toggle HUD", "UI");

    console.Execute("fps_cap", {"144"});
    ASSERT_EQ(targetFPS, 144);

    auto result = console.Execute("fps_cap");
    ASSERT_TRUE(result.find("144") != std::string::npos);

    console.Execute("draw_distance", {"2500"});
    EXPECT_NEAR(drawDist, 2500.0f, 1.0e-5f);

    console.Execute("hud", {"off"});
    ASSERT_FALSE(showHUD);

    console.Execute("hud", {"on"});
    ASSERT_TRUE(showHUD);
}

TEST(ConsoleUnknownCommand)
{
    MockConsole console;
    auto result = console.Execute("nonexistent");
    ASSERT_TRUE(result.find("Unknown") != std::string::npos);
}

TEST(ConsoleMissingArgs)
{
    MockConsole console;
    console.RegisterCommand(
        "cam_pos",
        [](const std::vector<std::string>& args) -> std::string
        {
            if (args.size() < 3)
                return "Usage: cam_pos <x> <y> <z>";
            return "Position set";
        },
        "Set position", "Camera");

    auto result = console.Execute("cam_pos", {"1.0", "2.0"});
    ASSERT_TRUE(result.find("Usage") != std::string::npos);

    result = console.Execute("cam_pos", {"1.0", "2.0", "3.0"});
    ASSERT_EQ(result, "Position set");
}
