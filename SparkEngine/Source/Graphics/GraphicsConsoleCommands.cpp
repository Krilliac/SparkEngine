/**
 * @file GraphicsConsoleCommands.cpp
 * @brief Implementation of graphics console command registration
 *
 * These commands were previously registered inline in SparkEngine.cpp's
 * RegisterEngineConsoleCommands(). Extracting them here keeps graphics
 * console logic co-located with the graphics subsystem.
 */

#include "GraphicsConsoleCommands.h"
#include "../Core/Platform.h"
#include "GraphicsEngine.h"
#include "../Utils/SparkConsole.h"
#ifdef SPARK_HYBRID_RT
#include "HybridRT/HybridRTManager.h"
#endif
#include "../Utils/Validate.h"
#include <sstream>
#include <string>
#include <vector>

namespace Spark::Graphics
{

    void RegisterGraphicsConsoleCommands(GraphicsEngine& engine)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Registering graphics console commands");
        auto& console = Spark::SimpleConsole::GetInstance();

        console.RegisterCommand(
            "gfx_vsync",
            [&engine](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: gfx_vsync <on|off>";
                bool enable = (args[0] == "on" || args[0] == "true" || args[0] == "1");
                engine.Console_SetVSync(enable);
                return enable ? "VSync enabled" : "VSync disabled";
            },
            "Enable/disable VSync");

        console.RegisterCommand(
            "gfx_wireframe",
            [&engine](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: gfx_wireframe <on|off>";
                bool enable = (args[0] == "on" || args[0] == "true" || args[0] == "1");
                engine.Console_SetWireframe(enable);
                return enable ? "Wireframe mode enabled" : "Wireframe mode disabled";
            },
            "Enable/disable wireframe rendering");

        console.RegisterCommand(
            "gfx_metrics",
            [&engine](const std::vector<std::string>&) -> std::string
            {
                try
                {
                    auto metrics = engine.Console_GetStatistics();
                    std::stringstream ss;
                    ss << "=== Graphics Metrics ===\n";
                    ss << "FPS: " << metrics.fps << "\n";
                    ss << "Frame Time: " << metrics.frameTime << "ms\n";
                    ss << "Draw Calls: " << metrics.drawCalls << "\n";
                    ss << "Triangles: " << metrics.triangles << "\n";
                    return ss.str();
                }
                catch (...)
                {
                    return "Metrics not available";
                }
            },
            "Display graphics performance metrics");

        console.RegisterCommand(
            "gfx_screenshot",
            [&engine](const std::vector<std::string>& args) -> std::string
            {
                std::string filename = args.empty() ? "" : args[0];
                bool success = engine.Console_Screenshot(filename);
                return success ? "Screenshot saved" : "Failed to save screenshot";
            },
            "Take a screenshot");

        console.RegisterCommand(
            "gfx_quality",
            [&engine](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: gfx_quality <low|medium|high|ultra>";
                engine.Console_SetQuality(args[0]);
                return "Quality set to: " + args[0];
            },
            "Set graphics quality preset");

        console.RegisterCommand(
            "gfx_renderpath",
            [&engine](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: gfx_renderpath <forward|deferred>";
                engine.Console_SetRenderPath(args[0]);
                return "Render path set to: " + args[0];
            },
            "Set render path (forward/deferred)");

        console.RegisterCommand(
            "gfx_feature",
            [&engine](const std::vector<std::string>& args) -> std::string
            {
                if (args.size() < 2)
                    return "Usage: gfx_feature <name> <on|off>";
                bool enable = (args[1] == "on" || args[1] == "true" || args[1] == "1");
                engine.Console_EnableFeature(args[0], enable);
                return args[0] + (enable ? " enabled" : " disabled");
            },
            "Enable/disable a graphics feature");

        console.RegisterCommand(
            "gfx_reload_shaders",
            [&engine](const std::vector<std::string>&) -> std::string
            {
                engine.Console_ReloadShaders();
                return "Shaders reloaded";
            },
            "Reload all shaders");

        console.RegisterCommand(
            "gfx_sysinfo", [&engine](const std::vector<std::string>&) -> std::string
            { return engine.Console_GetSystemInfo(); }, "Display graphics system information");

        console.RegisterCommand(
            "gfx_benchmark",
            [&engine](const std::vector<std::string>& args) -> std::string
            {
                int seconds = 10;
                if (!args.empty())
                {
                    try
                    {
                        seconds = std::stoi(args[0]);
                    }
                    catch (const std::exception&)
                    {
                        return std::string("Invalid number format");
                    }
                }
                return engine.Console_Benchmark(seconds);
            },
            "Run graphics benchmark");

        console.RegisterCommand(
            "gfx_hdr",
            [&engine](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: gfx_hdr <on|off>";
                bool enable = (args[0] == "on" || args[0] == "true" || args[0] == "1");
                engine.Console_SetHDR(enable);
                return enable ? "HDR enabled" : "HDR disabled";
            },
            "Enable/disable HDR rendering");

        console.RegisterCommand(
            "gfx_debug",
            [&engine](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: gfx_debug <on|off>";
                bool enable = (args[0] == "on" || args[0] == "true" || args[0] == "1");
                engine.Console_SetDebugMode(enable);
                return enable ? "Debug mode enabled" : "Debug mode disabled";
            },
            "Enable/disable debug rendering mode");

        console.RegisterCommand(
            "gfx_clearcolor",
            [&engine](const std::vector<std::string>& args) -> std::string
            {
                if (args.size() < 3)
                    return "Usage: gfx_clearcolor <r> <g> <b> [a]";
                try
                {
                    float r = std::stof(args[0]);
                    float g = std::stof(args[1]);
                    float b = std::stof(args[2]);
                    float a = (args.size() >= 4) ? std::stof(args[3]) : 1.0f;
                    engine.Console_SetClearColor(r, g, b, a);
                    return std::string("Clear color updated");
                }
                catch (const std::exception&)
                {
                    return std::string("Invalid numeric argument");
                }
            },
            "Set the background clear color");

        console.RegisterCommand(
            "gfx_renderscale",
            [&engine](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: gfx_renderscale <0.25-2.0>";
                try
                {
                    float scale = std::stof(args[0]);
                    engine.Console_SetRenderScale(scale);
                    return "Render scale set to: " + std::to_string(scale);
                }
                catch (const std::exception&)
                {
                    return std::string("Invalid numeric argument");
                }
            },
            "Set render resolution scale");

        console.RegisterCommand(
            "gfx_reset",
            [&engine](const std::vector<std::string>&) -> std::string
            {
                engine.Console_ResetToDefaults();
                return "Graphics settings reset to defaults";
            },
            "Reset all graphics settings to defaults");

        console.RegisterCommand(
            "gfx_gc",
            [&engine](const std::vector<std::string>&) -> std::string
            {
                engine.Console_ForceGarbageCollection();
                return "Graphics garbage collection complete";
            },
            "Force graphics resource garbage collection");

        console.RegisterCommand(
            "gfx_gpu_timing",
            [&engine](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: gfx_gpu_timing <on|off>";
                bool enable = (args[0] == "on" || args[0] == "true" || args[0] == "1");
                engine.Console_SetGPUTiming(enable);
                return enable ? "GPU timing enabled" : "GPU timing disabled";
            },
            "Enable/disable GPU timing queries");

        console.RegisterCommand(
            "gfx_vram",
            [&engine](const std::vector<std::string>&) -> std::string
            {
                size_t usage = engine.Console_GetVRAMUsage();
                std::stringstream ss;
                ss << "VRAM Usage: " << (usage / (1024 * 1024)) << " MB";
                return ss.str();
            },
            "Display VRAM usage");

        console.RegisterCommand(
            "gfx_reset_device",
            [&engine](const std::vector<std::string>&) -> std::string
            {
                engine.Console_ResetDevice();
                return "Graphics device reset";
            },
            "Reset the graphics device");

        // ====================================================================
        // Ray Tracing Console Commands
        // ====================================================================
#ifdef SPARK_HYBRID_RT
        console.RegisterCommand(
            "rt.status",
            [&engine](const std::vector<std::string>&) -> std::string
            {
                if (engine.GetHybridRT())
                    return engine.GetHybridRT()->Console_GetStatus();
                return "Hybrid RT not initialized";
            },
            "Display hybrid ray tracing status");

        console.RegisterCommand(
            "rt.quality",
            [&engine](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: rt.quality <off|low|medium|high|ultra>";
                auto* rt = engine.GetHybridRT();
                if (!rt)
                    return "Hybrid RT not available";
                auto q = Spark::RHI::RayTracingQuality::Medium;
                if (args[0] == "off")
                    q = Spark::RHI::RayTracingQuality::Off;
                else if (args[0] == "low")
                    q = Spark::RHI::RayTracingQuality::Low;
                else if (args[0] == "medium")
                    q = Spark::RHI::RayTracingQuality::Medium;
                else if (args[0] == "high")
                    q = Spark::RHI::RayTracingQuality::High;
                else if (args[0] == "ultra")
                    q = Spark::RHI::RayTracingQuality::Ultra;
                rt->SetQuality(q);
                return "RT quality set to: " + args[0];
            },
            "Set ray tracing quality preset");

        console.RegisterCommand(
            "rt.mode",
            [&engine](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: rt.mode <auto|sdfgi|hardware|off>";
                auto* rt = engine.GetHybridRT();
                if (!rt)
                    return "Hybrid RT not available";
                auto b = Spark::RHI::RayTracingBackend::Disabled;
                if (args[0] == "auto")
                    b = Spark::RHI::RayTracingBackend::Disabled; // Disabled override = auto
                else if (args[0] == "sdfgi")
                    b = Spark::RHI::RayTracingBackend::Software_SDFGI;
                else if (args[0] == "hardware")
                    b = Spark::RHI::RayTracingBackend::HardwareDXR;
                else if (args[0] == "off")
                {
                    rt->SetQuality(Spark::RHI::RayTracingQuality::Off);
                    return "RT disabled";
                }
                rt->SetBackendOverride(b);
                return "RT mode set to: " + args[0];
            },
            "Set ray tracing backend (auto/sdfgi/hardware/off)");

        console.RegisterCommand(
            "rt.reflections",
            [&engine](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: rt.reflections <0|1>";
                auto* rt = engine.GetHybridRT();
                if (!rt)
                    return "Hybrid RT not available";
                bool enable = (args[0] == "1" || args[0] == "on");
                auto effects = rt->GetEnabledEffects();
                if (enable)
                    effects = effects | Spark::RHI::RTEffect::Reflections;
                else
                    effects = static_cast<Spark::RHI::RTEffect>(
                        static_cast<uint32_t>(effects) & ~static_cast<uint32_t>(Spark::RHI::RTEffect::Reflections));
                rt->SetEnabledEffects(effects);
                return enable ? "RT reflections enabled" : "RT reflections disabled";
            },
            "Enable/disable RT reflections");

        console.RegisterCommand(
            "rt.gi",
            [&engine](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: rt.gi <0|1>";
                auto* rt = engine.GetHybridRT();
                if (!rt)
                    return "Hybrid RT not available";
                bool enable = (args[0] == "1" || args[0] == "on");
                auto effects = rt->GetEnabledEffects();
                if (enable)
                    effects = effects | Spark::RHI::RTEffect::GlobalIllumination;
                else
                    effects = static_cast<Spark::RHI::RTEffect>(
                        static_cast<uint32_t>(effects) &
                        ~static_cast<uint32_t>(Spark::RHI::RTEffect::GlobalIllumination));
                rt->SetEnabledEffects(effects);
                return enable ? "RT GI enabled" : "RT GI disabled";
            },
            "Enable/disable RT global illumination");

        console.RegisterCommand(
            "rt.shadows",
            [&engine](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: rt.shadows <0|1>";
                auto* rt = engine.GetHybridRT();
                if (!rt)
                    return "Hybrid RT not available";
                bool enable = (args[0] == "1" || args[0] == "on");
                auto effects = rt->GetEnabledEffects();
                if (enable)
                    effects = effects | Spark::RHI::RTEffect::Shadows;
                else
                    effects = static_cast<Spark::RHI::RTEffect>(static_cast<uint32_t>(effects) &
                                                                ~static_cast<uint32_t>(Spark::RHI::RTEffect::Shadows));
                rt->SetEnabledEffects(effects);
                return enable ? "RT shadows enabled" : "RT shadows disabled";
            },
            "Enable/disable RT soft shadows");
#endif // SPARK_HYBRID_RT
    }

} // namespace Spark::Graphics
