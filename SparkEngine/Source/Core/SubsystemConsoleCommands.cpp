/**
 * @file SubsystemConsoleCommands.cpp
 * @brief Console commands for camera, network, AI, animation, weather, and post-processing
 *
 * Part 1 of subsystem console command registration. Part 2 is in
 * SubsystemConsoleCommandsExt.cpp (input, scripting, cinematic, replay, etc.).
 */

#include "SubsystemConsoleCommands.h"
#include "EngineContext.h"
#include "EngineSettings.h"
#include "Camera/SparkEngineCamera.h"
#include "Engine/AI/AISystem.h"
#include "Graphics/GraphicsEngine.h"
#include "Graphics/PostProcessingPipeline.h"
#include "Graphics/WeatherSystem.h"
#include "Utils/SparkConsole.h"

#include <sstream>
#include <string>

// Forward declaration for ext commands
namespace Spark
{
    void RegisterExtendedSubsystemConsoleCommands();
}

namespace Spark
{

    // ========================================================================
    // Camera commands
    // ========================================================================

    static void RegisterCameraCommands(SimpleConsole& console)
    {
        console.RegisterCommand(
            "cam_fov",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: cam_fov <degrees>  (e.g. 90, 120)";
                auto* cam = EngineContext::Get() ? EngineContext::Get()->GetCamera() : nullptr;
                if (!cam)
                    return "Camera not available";
                try
                {
                    cam->Console_SetFOV(std::stof(args[0]));
                    return "FOV set to " + args[0];
                }
                catch (const std::exception&)
                {
                    return "Invalid number";
                }
            },
            "Set camera field of view in degrees", "Camera");

        console.RegisterCommand(
            "cam_sensitivity",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: cam_sensitivity <value>";
                auto* cam = EngineContext::Get() ? EngineContext::Get()->GetCamera() : nullptr;
                if (!cam)
                    return "Camera not available";
                try
                {
                    cam->Console_SetMouseSensitivity(std::stof(args[0]));
                    return "Mouse sensitivity set to " + args[0];
                }
                catch (const std::exception&)
                {
                    return "Invalid number";
                }
            },
            "Set camera mouse sensitivity", "Camera");

        console.RegisterCommand(
            "cam_invert_y",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: cam_invert_y <on|off>";
                auto* cam = EngineContext::Get() ? EngineContext::Get()->GetCamera() : nullptr;
                if (!cam)
                    return "Camera not available";
                bool invert = (args[0] == "on" || args[0] == "true" || args[0] == "1");
                cam->Console_SetInvertY(invert);
                return invert ? "Y-axis inverted" : "Y-axis normal";
            },
            "Toggle Y-axis inversion", "Camera");

        console.RegisterCommand(
            "cam_speed",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: cam_speed <units_per_second>";
                auto* cam = EngineContext::Get() ? EngineContext::Get()->GetCamera() : nullptr;
                if (!cam)
                    return "Camera not available";
                try
                {
                    cam->Console_SetMoveSpeed(std::stof(args[0]));
                    return "Camera speed set to " + args[0];
                }
                catch (const std::exception&)
                {
                    return "Invalid number";
                }
            },
            "Set camera movement speed", "Camera");

        console.RegisterCommand(
            "cam_pos",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.size() < 3)
                    return "Usage: cam_pos <x> <y> <z>";
                auto* cam = EngineContext::Get() ? EngineContext::Get()->GetCamera() : nullptr;
                if (!cam)
                    return "Camera not available";
                try
                {
                    cam->Console_SetPosition(std::stof(args[0]), std::stof(args[1]), std::stof(args[2]));
                    return "Camera position set";
                }
                catch (const std::exception&)
                {
                    return "Invalid number";
                }
            },
            "Set camera world position", "Camera");

        console.RegisterCommand(
            "cam_rotation",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.size() < 2)
                    return "Usage: cam_rotation <pitch> <yaw> [roll]";
                auto* cam = EngineContext::Get() ? EngineContext::Get()->GetCamera() : nullptr;
                if (!cam)
                    return "Camera not available";
                try
                {
                    float roll = (args.size() >= 3) ? std::stof(args[2]) : 0.0f;
                    cam->Console_SetRotation(std::stof(args[0]), std::stof(args[1]), roll);
                    return "Camera rotation set";
                }
                catch (const std::exception&)
                {
                    return "Invalid number";
                }
            },
            "Set camera rotation (pitch, yaw, optional roll)", "Camera");

        console.RegisterCommand(
            "cam_clipping",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.size() < 2)
                    return "Usage: cam_clipping <near> <far>";
                auto* cam = EngineContext::Get() ? EngineContext::Get()->GetCamera() : nullptr;
                if (!cam)
                    return "Camera not available";
                try
                {
                    cam->Console_SetClippingPlanes(std::stof(args[0]), std::stof(args[1]));
                    return "Clipping planes set to near=" + args[0] + " far=" + args[1];
                }
                catch (const std::exception&)
                {
                    return "Invalid number";
                }
            },
            "Set near/far clipping planes", "Camera");

        console.RegisterCommand(
            "cam_lookat",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.size() < 3)
                    return "Usage: cam_lookat <x> <y> <z>";
                auto* cam = EngineContext::Get() ? EngineContext::Get()->GetCamera() : nullptr;
                if (!cam)
                    return "Camera not available";
                try
                {
                    cam->Console_LookAt(std::stof(args[0]), std::stof(args[1]), std::stof(args[2]));
                    return "Camera looking at target";
                }
                catch (const std::exception&)
                {
                    return "Invalid number";
                }
            },
            "Point camera at a world position", "Camera");

        console.RegisterCommand(
            "cam_info",
            [](const std::vector<std::string>&) -> std::string
            {
                auto* cam = EngineContext::Get() ? EngineContext::Get()->GetCamera() : nullptr;
                if (!cam)
                    return "Camera not available";
                auto state = cam->Console_GetState();
                std::stringstream ss;
                ss << "=== Camera State ===\n"
                   << "  Position: (" << state.position.x << ", " << state.position.y << ", " << state.position.z
                   << ")\n"
                   << "  Rotation: (" << state.rotation.x << ", " << state.rotation.y << ", " << state.rotation.z
                   << ")\n"
                   << "  FOV: " << state.currentFov << " deg (default=" << state.defaultFov << ")\n"
                   << "  Speed: " << state.moveSpeed << "  Sensitivity: " << state.mouseSensitivity << "\n"
                   << "  Near/Far: " << state.nearPlane << " / " << state.farPlane << "\n"
                   << "  InvertY: " << (state.invertY ? "YES" : "NO") << "  Zoomed: " << (state.isZoomed ? "YES" : "NO")
                   << "\n";
                return ss.str();
            },
            "Show current camera state", "Camera");

        console.RegisterCommand(
            "cam_reset",
            [](const std::vector<std::string>&) -> std::string
            {
                auto* cam = EngineContext::Get() ? EngineContext::Get()->GetCamera() : nullptr;
                if (!cam)
                    return "Camera not available";
                cam->Console_ResetToDefaults();
                return "Camera reset to defaults";
            },
            "Reset camera to default settings", "Camera");
    }

    // ========================================================================
    // Network commands
    // ========================================================================

    static void RegisterNetworkCommands(SimpleConsole& console)
    {
        console.RegisterCommand(
            "net_lag",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: net_lag <milliseconds>  (0 to disable)";
                auto& settings = EngineSettings::GetInstance();
                try
                {
                    settings.SetValue("Network", "SimulatedLatencyMs", args[0]);
                    return "Simulated latency set to " + args[0] + " ms";
                }
                catch (const std::exception&)
                {
                    return "Invalid number";
                }
            },
            "Simulate network latency (dev only)", "Network");

        console.RegisterCommand(
            "net_loss",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: net_loss <percent>  (0.0-1.0, e.g. 0.05 = 5%)";
                auto& settings = EngineSettings::GetInstance();
                settings.SetValue("Network", "SimulatedPacketLoss", args[0]);
                return "Simulated packet loss set to " + args[0];
            },
            "Simulate packet loss (dev only)", "Network");

        console.RegisterCommand(
            "net_jitter",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: net_jitter <milliseconds>  (0 to disable)";
                auto& settings = EngineSettings::GetInstance();
                settings.SetValue("Network", "SimulatedJitterMs", args[0]);
                return "Simulated jitter set to " + args[0] + " ms";
            },
            "Simulate network jitter (dev only)", "Network");
    }

    // ========================================================================
    // AI commands
    // ========================================================================

    static void RegisterAICommands(SimpleConsole& console)
    {
        console.RegisterCommand(
            "ai_list",
            [](const std::vector<std::string>&) -> std::string
            {
                auto* ctx = EngineContext::Get();
                if (!ctx)
                    return "Engine context not available";
                auto* ai = ctx->GetAI();
                auto* world = ctx->GetWorld();
                if (!ai || !world)
                    return "AI system or World not available";
                return ai->Console_ListAgents(*world);
            },
            "List all active AI agents", "AI");

        console.RegisterCommand(
            "ai_info",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: ai_info <entity_id>";
                auto* ctx = EngineContext::Get();
                if (!ctx)
                    return "Engine context not available";
                auto* ai = ctx->GetAI();
                auto* world = ctx->GetWorld();
                if (!ai || !world)
                    return "AI system or World not available";
                try
                {
                    auto id = static_cast<entt::entity>(std::stoul(args[0]));
                    return ai->Console_GetAgentInfo(*world, id);
                }
                catch (const std::exception&)
                {
                    return "Invalid entity ID";
                }
            },
            "Get detailed info about an AI agent", "AI");
    }

    // ========================================================================
    // Animation commands (settings-driven)
    // ========================================================================

    static void RegisterAnimationCommands(SimpleConsole& console)
    {
        console.RegisterCommand(
            "anim_blend_time",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Blend time: " + EngineSettings::GetInstance().GetValue("Animation", "DefaultBlendTime");
                EngineSettings::GetInstance().SetValue("Animation", "DefaultBlendTime", args[0]);
                return "Default blend time set to " + args[0] + "s";
            },
            "Get/set default animation blend time", "Animation");

        console.RegisterCommand(
            "anim_root_motion",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Root motion: " + EngineSettings::GetInstance().GetValue("Animation", "EnableRootMotion");
                bool enable = (args[0] == "on" || args[0] == "true" || args[0] == "1");
                EngineSettings::GetInstance().SetValue("Animation", "EnableRootMotion", enable ? "true" : "false");
                return enable ? "Root motion enabled" : "Root motion disabled";
            },
            "Toggle root motion extraction", "Animation");

        console.RegisterCommand(
            "anim_ik_iterations",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "IK iterations: " +
                           EngineSettings::GetInstance().GetValue("Animation", "IKSolverIterations");
                EngineSettings::GetInstance().SetValue("Animation", "IKSolverIterations", args[0]);
                return "IK solver iterations set to " + args[0];
            },
            "Get/set IK solver iteration count", "Animation");

        console.RegisterCommand(
            "anim_events",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Animation events: " +
                           EngineSettings::GetInstance().GetValue("Animation", "EnableAnimationEvents");
                bool enable = (args[0] == "on" || args[0] == "true" || args[0] == "1");
                EngineSettings::GetInstance().SetValue("Animation", "EnableAnimationEvents", enable ? "true" : "false");
                return enable ? "Animation events enabled" : "Animation events disabled";
            },
            "Toggle animation event firing", "Animation");
    }

    // ========================================================================
    // Weather commands
    // ========================================================================

    static void RegisterWeatherCommands(SimpleConsole& console)
    {
        console.RegisterCommand(
            "weather_set",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: weather_set <clear|cloudy|rain|snow|fog|storm> [intensity] [transition_sec]";
                auto* weather = EngineContext::Get() ? EngineContext::Get()->GetWeather() : nullptr;
                if (!weather)
                    return "Weather system not available";

                static const std::unordered_map<std::string, WeatherType> types = {
                    {"clear", WeatherType::Clear}, {"cloudy", WeatherType::Cloudy}, {"rain", WeatherType::Rain},
                    {"snow", WeatherType::Snow},   {"fog", WeatherType::Fog},       {"storm", WeatherType::Storm}};
                auto it = types.find(args[0]);
                if (it == types.end())
                    return "Unknown weather type. Options: clear, cloudy, rain, snow, fog, storm";

                float intensity = (args.size() > 1) ? std::stof(args[1]) : -1.0f;
                float transition = (args.size() > 2) ? std::stof(args[2]) : 3.0f;
                weather->SetWeather(it->second, intensity, transition);
                return "Weather set to " + args[0];
            },
            "Set weather condition with optional intensity and transition time", "Weather");

        console.RegisterCommand(
            "weather_info",
            [](const std::vector<std::string>&) -> std::string
            {
                auto* weather = EngineContext::Get() ? EngineContext::Get()->GetWeather() : nullptr;
                if (!weather)
                    return "Weather system not available";
                static const char* typeNames[] = {"Clear", "Cloudy", "Rain", "Snow", "Fog", "Storm"};
                auto& state = weather->GetCurrentState();
                int typeIdx = static_cast<int>(state.type);
                std::stringstream ss;
                ss << "=== Weather State ===\n"
                   << "  Type: " << (typeIdx < 6 ? typeNames[typeIdx] : "Unknown") << "\n"
                   << "  Intensity: " << state.intensity << "\n"
                   << "  Wind: speed=" << state.windSpeed << " dir=(" << state.windDirection.x << ", "
                   << state.windDirection.y << ", " << state.windDirection.z << ")\n"
                   << "  Fog: density=" << state.fogDensity << " range=" << state.fogStartDistance << "-"
                   << state.fogEndDistance << "\n"
                   << "  Precipitation: rate=" << state.precipitationRate << " size=" << state.precipitationSize << "\n"
                   << "  Wetness: " << state.wetness << "  Snow: " << state.snowCoverage << "\n";
                return ss.str();
            },
            "Show current weather state", "Weather");

        console.RegisterCommand(
            "weather_wind",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: weather_wind <speed> [dirX dirY dirZ]";
                auto* weather = EngineContext::Get() ? EngineContext::Get()->GetWeather() : nullptr;
                if (!weather)
                    return "Weather system not available";
                try
                {
                    float speed = std::stof(args[0]);
                    float dx = (args.size() > 1) ? std::stof(args[1]) : 1.0f;
                    float dy = (args.size() > 2) ? std::stof(args[2]) : 0.0f;
                    float dz = (args.size() > 3) ? std::stof(args[3]) : 0.0f;
                    weather->SetWind(dx, dy, dz, speed);
                    return "Wind set to speed=" + args[0];
                }
                catch (const std::exception&)
                {
                    return "Invalid number";
                }
            },
            "Set wind speed and direction", "Weather");
    }

    // ========================================================================
    // Post-processing commands
    // ========================================================================

    static void RegisterPostProcessCommands(SimpleConsole& console)
    {
        console.RegisterCommand(
            "pp_list",
            [](const std::vector<std::string>&) -> std::string
            {
                auto* gfx = EngineContext::Get() ? EngineContext::Get()->GetGraphics() : nullptr;
                if (!gfx)
                    return "Graphics engine not available";
                auto* pp = gfx->GetPostProcessingPipeline();
                if (!pp)
                    return "Post-processing pipeline not available";
                return pp->Console_ListEffects();
            },
            "List all post-processing effects and their status", "PostProcess");

        console.RegisterCommand(
            "pp_enable",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: pp_enable <effect_name>  (bloom, fxaa, dof, vignette, etc.)";
                auto* gfx = EngineContext::Get() ? EngineContext::Get()->GetGraphics() : nullptr;
                if (!gfx)
                    return "Graphics engine not available";
                auto* pp = gfx->GetPostProcessingPipeline();
                if (!pp)
                    return "Post-processing pipeline not available";

                static const std::unordered_map<std::string, Graphics::PostProcessPass> passes = {
                    {"bloom", Graphics::PostProcessPass::Bloom},
                    {"autoexposure", Graphics::PostProcessPass::AutoExposure},
                    {"tonemapping", Graphics::PostProcessPass::Tonemapping},
                    {"colorgrading", Graphics::PostProcessPass::ColorGrading},
                    {"fxaa", Graphics::PostProcessPass::FXAA},
                    {"dof", Graphics::PostProcessPass::DepthOfField},
                    {"motionblur", Graphics::PostProcessPass::MotionBlur},
                    {"vignette", Graphics::PostProcessPass::Vignette},
                    {"chromatic", Graphics::PostProcessPass::ChromaticAberration},
                    {"filmgrain", Graphics::PostProcessPass::FilmGrain},
                    {"lensdistortion", Graphics::PostProcessPass::LensDistortion},
                    {"lightshafts", Graphics::PostProcessPass::LightShafts},
                    {"lensflare", Graphics::PostProcessPass::LensFlare},
                    {"sharpen", Graphics::PostProcessPass::Sharpen}};
                auto it = passes.find(args[0]);
                if (it == passes.end())
                    return "Unknown effect. Use pp_list to see available effects.";
                pp->SetEffectEnabled(it->second, true);
                return args[0] + " enabled";
            },
            "Enable a post-processing effect", "PostProcess");

        console.RegisterCommand(
            "pp_disable",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: pp_disable <effect_name>";
                auto* gfx = EngineContext::Get() ? EngineContext::Get()->GetGraphics() : nullptr;
                if (!gfx)
                    return "Graphics engine not available";
                auto* pp = gfx->GetPostProcessingPipeline();
                if (!pp)
                    return "Post-processing pipeline not available";

                static const std::unordered_map<std::string, Graphics::PostProcessPass> passes = {
                    {"bloom", Graphics::PostProcessPass::Bloom},
                    {"autoexposure", Graphics::PostProcessPass::AutoExposure},
                    {"tonemapping", Graphics::PostProcessPass::Tonemapping},
                    {"colorgrading", Graphics::PostProcessPass::ColorGrading},
                    {"fxaa", Graphics::PostProcessPass::FXAA},
                    {"dof", Graphics::PostProcessPass::DepthOfField},
                    {"motionblur", Graphics::PostProcessPass::MotionBlur},
                    {"vignette", Graphics::PostProcessPass::Vignette},
                    {"chromatic", Graphics::PostProcessPass::ChromaticAberration},
                    {"filmgrain", Graphics::PostProcessPass::FilmGrain},
                    {"lensdistortion", Graphics::PostProcessPass::LensDistortion},
                    {"lightshafts", Graphics::PostProcessPass::LightShafts},
                    {"lensflare", Graphics::PostProcessPass::LensFlare},
                    {"sharpen", Graphics::PostProcessPass::Sharpen}};
                auto it = passes.find(args[0]);
                if (it == passes.end())
                    return "Unknown effect. Use pp_list to see available effects.";
                pp->SetEffectEnabled(it->second, false);
                return args[0] + " disabled";
            },
            "Disable a post-processing effect", "PostProcess");

        console.RegisterCommand(
            "pp_exposure",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: pp_exposure <value>  (e.g. 1.0)";
                auto* gfx = EngineContext::Get() ? EngineContext::Get()->GetGraphics() : nullptr;
                if (!gfx)
                    return "Graphics engine not available";
                auto* pp = gfx->GetPostProcessingPipeline();
                if (!pp)
                    return "Post-processing pipeline not available";
                try
                {
                    pp->Console_SetExposure(std::stof(args[0]));
                    return "Exposure set to " + args[0];
                }
                catch (const std::exception&)
                {
                    return "Invalid number";
                }
            },
            "Set exposure value", "PostProcess");
    }

    // ========================================================================
    // Public API
    // ========================================================================

    void RegisterSubsystemConsoleCommands()
    {
        auto& console = SimpleConsole::GetInstance();

        RegisterCameraCommands(console);
        RegisterNetworkCommands(console);
        RegisterAICommands(console);
        RegisterAnimationCommands(console);
        RegisterWeatherCommands(console);
        RegisterPostProcessCommands(console);

        // Extended commands (input, scripting, cinematic, replay, localization, etc.)
        RegisterExtendedSubsystemConsoleCommands();
    }

} // namespace Spark
