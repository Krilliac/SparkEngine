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
 *   - Player movement (WASD + mouse look)
 *   - Collectible items with score tracking
 *   - Enemy patrol with chase behavior
 *   - Health system with damage and healing
 *   - Win/lose conditions
 *   - Sound and animation triggers
 *   - HUD display (health bar, score counter)
 *
 * Implements the Spark::IModule interface for the module system.
 */

#pragma once

#include "Spark/SparkSDK.h"

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
    void LoadAndCompileScripts();
    void SpawnGameEntities();

    Spark::IEngineContext* m_context{nullptr};
    bool m_initialized{false};
    bool m_paused{false};
};

extern "C"
{
    SPARK_MODULE_API Spark::IModule* CreateModule();
    SPARK_MODULE_API void DestroyModule(Spark::IModule* mod);
}
