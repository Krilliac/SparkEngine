/**
 * @file SparkGameVisualScript.h
 * @brief Visual-script-only game module — zero C++ game logic
 * @author Spark Engine Team
 * @date 2026
 *
 * This game module demonstrates SparkEngine's visual scripting system by
 * implementing ALL game logic through visual script graphs. The C++ code
 * in this module is limited to the IModule shell that loads and attaches
 * the pre-compiled AngelScript files to entities.
 *
 * Game features (all defined in visual scripts):
 *   - Player movement (WASD + sprint + jump)
 *   - Collectible items with score tracking
 *   - Enemy patrol with chase behavior
 *   - Health system with damage and healing
 *   - Win/lose conditions
 *   - Sound and animation triggers
 *   - Console status and deterministic full-demo restart
 *
 * Implements the Spark::IModule interface for the module system.
 */

#pragma once

#include "Engine/ECS/Components/CoreComponents.h"
#include "Spark/SparkSDK.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * @brief Game module with all logic defined in visual scripts
 *
 * The module shell spawns entities and attaches visual scripts to them.
 * Every gameplay behavior is authored in the visual script editor.
 */
class SparkGameVisualScriptModule : public Spark::IModule
{
  public:
    SparkGameVisualScriptModule() = default;
    ~SparkGameVisualScriptModule() override;

    Spark::ModuleInfo GetModuleInfo() const override;
    bool OnLoad(Spark::IEngineContext* context) override;
    void OnUnload() override;
    void OnUpdate(float deltaTime) override;
    void OnFixedUpdate(float fixedDeltaTime) override;
    void OnRender() override;
    void OnResize(int width, int height) override;
    void OnPause() override;
    void OnResume() override;
    void OnImGui() override;

  private:
    bool LoadAndCompileScripts();
    bool SpawnGameEntities();
    bool AttachScript(EntityID entity, const std::string& className);
    void DestroyGameEntities();
    void RegisterConsoleCommands();
    void UnregisterConsoleCommands();
    std::string GetStatusString() const;

    Spark::IEngineContext* m_context{nullptr};
    std::filesystem::path m_scriptRoot;
    std::unordered_map<std::string, std::string> m_scriptSources;
    std::vector<EntityID> m_scriptEntities;
    bool m_initialized{false};
    bool m_paused{false};
};

extern "C"
{
    SPARK_MODULE_API Spark::IModule* CreateModule();
    SPARK_MODULE_API void DestroyModule(Spark::IModule* mod);
}
