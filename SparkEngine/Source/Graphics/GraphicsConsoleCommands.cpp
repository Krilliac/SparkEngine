/**
 * @file GraphicsConsoleCommands.cpp
 * @brief Implementation of graphics console command registration
 *
 * These commands were previously registered inline in SparkEngine.cpp's
 * RegisterEngineConsoleCommands(). Extracting them here keeps graphics
 * console logic co-located with the graphics subsystem.
 */

#include "../Core/Platform.h"
#include "GraphicsConsoleCommands.h"
#include "GraphicsEngine.h"
#include "../Utils/SparkConsole.h"
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
                int seconds = args.empty() ? 10 : std::stoi(args[0]);
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
                float r = std::stof(args[0]);
                float g = std::stof(args[1]);
                float b = std::stof(args[2]);
                float a = (args.size() >= 4) ? std::stof(args[3]) : 1.0f;
                engine.Console_SetClearColor(r, g, b, a);
                return "Clear color updated";
            },
            "Set the background clear color");

        console.RegisterCommand(
            "gfx_renderscale",
            [&engine](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: gfx_renderscale <0.25-2.0>";
                float scale = std::stof(args[0]);
                engine.Console_SetRenderScale(scale);
                return "Render scale set to: " + std::to_string(scale);
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
    }

} // namespace Spark::Graphics
