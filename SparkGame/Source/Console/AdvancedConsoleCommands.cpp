#include "Core/Platform.h"
/**
 * @file AdvancedConsoleCommands.cpp
 * @brief Complete console command integration for all advanced systems
 * @author Spark Engine Team
 * @date 2025
 * 
 * This file provides comprehensive console integration for all the advanced
 * systems in the Spark Engine, now integrated into the main GraphicsEngine.
 */

#include "Utils/SparkConsole.h"
#include "Game/Game.h"
#include "Graphics/GraphicsEngine.h"
#include "Graphics/Shader.h"
#include "Graphics/TextureSystem.h"
#include "Graphics/MaterialSystem.h"
#include "Graphics/LightingSystem.h"
#include "Graphics/PostProcessingSystem.h"
#include "Graphics/AssetPipeline.h"
#include "Physics/PhysicsSystem.h"
#include "Core/EngineContext.h"
#include <sstream>

extern std::unique_ptr<EngineContext> g_engineContext;

namespace SparkConsole
{

    /**
 * @brief Register all advanced console commands for the unified GraphicsEngine
 */
    void RegisterAdvancedCommands(Game* game, GraphicsEngine* graphics)
    {
        auto& console = Spark::SimpleConsole::GetInstance();

        // ========================================================================
        // TEXTURE SYSTEM COMMANDS (via GraphicsEngine)
        // ========================================================================

        console.RegisterCommand(
            "tex_list",
            [graphics](const std::vector<std::string>&) -> std::string
            {
                if (auto textureSystem = graphics->GetTextureSystem())
                {
                    return textureSystem->Console_ListTextures();
                }
                return "Texture system not available";
            },
            "List all loaded textures");

        console.RegisterCommand(
            "tex_info",
            [graphics](const std::vector<std::string>& args) -> std::string
            {
                if (args.size() < 2)
                    return "Usage: tex_info <texture_name>";
                if (auto textureSystem = graphics->GetTextureSystem())
                {
                    return textureSystem->Console_GetTextureInfo(args[1]);
                }
                return "Texture system not available";
            },
            "Get texture information");

        console.RegisterCommand(
            "tex_quality",
            [graphics](const std::vector<std::string>& args) -> std::string
            {
                if (args.size() < 2)
                    return "Usage: tex_quality <quality>";
                if (auto textureSystem = graphics->GetTextureSystem())
                {
                    textureSystem->Console_SetQuality(args[1]);
                    return "Texture quality set to: " + args[1];
                }
                return "Texture system not available";
            },
            "Set texture quality (low/medium/high/ultra)");

        console.RegisterCommand(
            "tex_memory",
            [graphics](const std::vector<std::string>& args) -> std::string
            {
                if (args.size() < 2)
                    return "Usage: tex_memory <mb>";
                if (auto textureSystem = graphics->GetTextureSystem())
                {
                    size_t mb = static_cast<size_t>(std::stof(args[1]));
                    textureSystem->Console_SetMemoryBudget(mb);
                    return "Texture memory budget set to: " + args[1] + " MB";
                }
                return "Texture system not available";
            },
            "Set texture memory budget in MB");

        // ========================================================================
        // MATERIAL SYSTEM COMMANDS (via GraphicsEngine)
        // ========================================================================

        console.RegisterCommand(
            "mat_list",
            [graphics](const std::vector<std::string>&) -> std::string
            {
                if (auto materialSystem = graphics->GetMaterialSystem())
                {
                    return materialSystem->Console_ListMaterials();
                }
                return "Material system not available";
            },
            "List all loaded materials");

        console.RegisterCommand(
            "mat_info",
            [graphics](const std::vector<std::string>& args) -> std::string
            {
                if (args.size() < 2)
                    return "Usage: mat_info <material_name>";
                if (auto materialSystem = graphics->GetMaterialSystem())
                {
                    return materialSystem->Console_GetMaterialInfo(args[1]);
                }
                return "Material system not available";
            },
            "Get material information");

        // ========================================================================
        // LIGHTING SYSTEM COMMANDS (via GraphicsEngine)
        // ========================================================================

        console.RegisterCommand(
            "light_list",
            [graphics](const std::vector<std::string>&) -> std::string
            {
                if (auto lightingSystem = graphics->GetLightingSystem())
                {
                    return lightingSystem->Console_ListLights();
                }
                return "Lighting system not available";
            },
            "List all lights");

        console.RegisterCommand(
            "shadows",
            [graphics](const std::vector<std::string>& args) -> std::string
            {
                if (args.size() < 2)
                    return "Usage: shadows <on/off>";
                bool enabled = (args[1] == "on" || args[1] == "true" || args[1] == "1");
                if (auto lightingSystem = graphics->GetLightingSystem())
                {
                    lightingSystem->Console_EnableShadows(enabled);
                    return "Shadows " + std::string(enabled ? "enabled" : "disabled");
                }
                return "Lighting system not available";
            },
            "Enable/disable shadows");

        // ========================================================================
        // POST-PROCESSING COMMANDS (via GraphicsEngine)
        // ========================================================================

        console.RegisterCommand(
            "pp_list",
            [graphics](const std::vector<std::string>&) -> std::string
            {
                if (auto postProcessing = graphics->GetPostProcessingSystem())
                {
                    return postProcessing->Console_ListEffects();
                }
                return "Post-processing system not available";
            },
            "List post-processing effects");

        console.RegisterCommand(
            "exposure",
            [graphics](const std::vector<std::string>& args) -> std::string
            {
                if (args.size() < 2)
                    return "Usage: exposure <value>";
                if (auto postProcessing = graphics->GetPostProcessingSystem())
                {
                    postProcessing->Console_SetExposure(std::stof(args[1]));
                    return "Exposure set to: " + args[1];
                }
                return "Post-processing system not available";
            },
            "Set exposure value");

        console.RegisterCommand(
            "hdr",
            [graphics](const std::vector<std::string>& args) -> std::string
            {
                if (args.size() < 2)
                    return "Usage: hdr <on/off>";
                bool enabled = (args[1] == "on" || args[1] == "true" || args[1] == "1");
                graphics->Console_SetHDR(enabled);
                return "HDR " + std::string(enabled ? "enabled" : "disabled");
            },
            "Enable/disable HDR");

        // ========================================================================
        // ASSET PIPELINE COMMANDS (via GraphicsEngine)
        // ========================================================================

        console.RegisterCommand(
            "asset_list",
            [graphics](const std::vector<std::string>&) -> std::string
            {
                if (auto assetPipeline = graphics->GetAssetPipeline())
                {
                    return assetPipeline->Console_ListAssets();
                }
                return "Asset pipeline not available";
            },
            "List all loaded assets");

        console.RegisterCommand(
            "asset_load",
            [graphics](const std::vector<std::string>& args) -> std::string
            {
                if (args.size() < 2)
                    return "Usage: asset_load <asset_path>";
                if (auto assetPipeline = graphics->GetAssetPipeline())
                {
                    bool success = assetPipeline->Console_LoadAsset(args[1]);
                    return success ? "Asset loaded: " + args[1] : "Failed to load asset: " + args[1];
                }
                return "Asset pipeline not available";
            },
            "Load asset");

        // ========================================================================
        // PHYSICS SYSTEM COMMANDS (via GraphicsEngine)
        // ========================================================================

        console.RegisterCommand(
            "physics_list",
            [graphics](const std::vector<std::string>&) -> std::string
            {
                if (auto physicsSystem = g_engineContext ? g_engineContext->GetPhysics() : nullptr)
                {
                    return physicsSystem->Console_ListBodies();
                }
                return "Physics system not available";
            },
            "List all physics bodies");

        console.RegisterCommand(
            "gravity",
            [graphics](const std::vector<std::string>& args) -> std::string
            {
                if (args.size() < 4)
                    return "Usage: gravity <x> <y> <z>";
                if (auto physicsSystem = g_engineContext ? g_engineContext->GetPhysics() : nullptr)
                {
                    physicsSystem->Console_SetGravity(std::stof(args[1]), std::stof(args[2]), std::stof(args[3]));
                    return "Gravity set to: (" + args[1] + ", " + args[2] + ", " + args[3] + ")";
                }
                return "Physics system not available";
            },
            "Set gravity");

        // ========================================================================
        // UNIFIED GRAPHICS ENGINE COMMANDS
        // ========================================================================

        console.RegisterCommand(
            "render_stats",
            [graphics](const std::vector<std::string>&) -> std::string
            {
                auto stats = graphics->Console_GetStatistics();
                return "FPS: " + std::to_string(stats.fps) + ", Frame Time: " + std::to_string(stats.frameTime) + "ms" +
                       ", Draw Calls: " + std::to_string(stats.drawCalls) +
                       ", Triangles: " + std::to_string(stats.triangles);
            },
            "Get rendering statistics");

        console.RegisterCommand(
            "render_quality",
            [graphics](const std::vector<std::string>& args) -> std::string
            {
                if (args.size() < 2)
                    return "Usage: render_quality <low/medium/high/ultra>";
                graphics->Console_SetQuality(args[1]);
                return "Render quality set to: " + args[1];
            },
            "Set render quality preset");

        console.RegisterCommand(
            "wireframe",
            [graphics](const std::vector<std::string>& args) -> std::string
            {
                if (args.size() < 2)
                    return "Usage: wireframe <on/off>";
                bool enabled = (args[1] == "on" || args[1] == "true" || args[1] == "1");
                graphics->Console_SetWireframe(enabled);
                return "Wireframe mode " + std::string(enabled ? "enabled" : "disabled");
            },
            "Enable/disable wireframe mode");

        console.RegisterCommand(
            "vsync",
            [graphics](const std::vector<std::string>& args) -> std::string
            {
                if (args.size() < 2)
                    return "Usage: vsync <on/off>";
                bool enabled = (args[1] == "on" || args[1] == "true" || args[1] == "1");
                graphics->Console_SetVSync(enabled);
                return "VSync " + std::string(enabled ? "enabled" : "disabled");
            },
            "Enable/disable VSync");

        console.RegisterCommand(
            "screenshot",
            [graphics](const std::vector<std::string>& args) -> std::string
            {
                std::string filename = (args.size() >= 2) ? args[1] : "screenshot.png";
                bool success = graphics->Console_Screenshot(filename);
                return success ? "Screenshot saved: " + filename : "Failed to save screenshot";
            },
            "Take screenshot");

        // ========================================================================
        // SHADER SYSTEM COMMANDS
        // ========================================================================

        console.RegisterCommand(
            "shader_list",
            [graphics](const std::vector<std::string>&) -> std::string
            {
                // Access shader system through GraphicsEngine's reload path
                // The Shader class Console_ListShaders is used directly when a Shader* is available.
                // For the console, we delegate to the GraphicsEngine which owns the Shader.
                graphics->Console_ReloadShaders(); // trigger a no-op to ensure shaders are initialized
                return "Use 'render_stats' for shader status. Shader list requires direct Shader* access.";
            },
            "List all loaded shaders");

        console.RegisterCommand(
            "shader_reload",
            [graphics](const std::vector<std::string>&) -> std::string
            {
                graphics->Console_ReloadShaders();
                return "Shader reload triggered";
            },
            "Reload all shaders from disk");

        console.RegisterCommand(
            "shader_debug",
            [graphics](const std::vector<std::string>& args) -> std::string
            {
                if (args.size() < 2)
                    return "Usage: shader_debug <on/off>";
                bool enabled = (args[1] == "on" || args[1] == "true" || args[1] == "1");
                // Toggle debug compilation through graphics engine settings
                auto settings = graphics->GetGraphicsSettings();
                settings.debugMode = enabled;
                graphics->Console_ApplySettings(settings);
                return std::string("Shader debug mode ") + (enabled ? "enabled" : "disabled");
            },
            "Enable/disable shader debug compilation");

        // ========================================================================
        // COMPREHENSIVE METRICS COMMAND
        // ========================================================================

        console.RegisterCommand(
            "metrics",
            [graphics](const std::vector<std::string>&) -> std::string
            {
                std::stringstream ss;

                // Graphics metrics
                auto renderStats = graphics->Console_GetStatistics();
                ss << "=== Unified Rendering System ===\n";
                ss << "FPS: " << renderStats.fps << "\n";
                ss << "Frame Time: " << renderStats.frameTime << "ms\n";
                ss << "Draw Calls: " << renderStats.drawCalls << "\n";
                ss << "Triangles: " << renderStats.triangles << "\n";
                ss << "Visible Objects: " << renderStats.visibleObjects << "/" << renderStats.totalObjects << "\n";

                // System information
                ss << graphics->Console_GetSystemInfo();

                return ss.str();
            },
            "Get comprehensive system metrics");

        console.Log("Advanced console commands registered for unified GraphicsEngine", "SUCCESS");
    }

} // namespace SparkConsole
