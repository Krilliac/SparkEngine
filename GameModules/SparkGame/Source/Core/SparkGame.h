/**
 * @file SparkGame.h
 * @brief SparkGame module - minimal default game template
 * @author Spark Engine Team
 * @date 2026
 *
 * SparkGame is the simplest possible game module for SparkEngine.
 * It serves as a blank-slate starting point with no subsystems or
 * game-specific logic — just the IModule interface wired up and ready.
 *
 * Implements the Spark::IModule interface for the module system.
 */

#pragma once

#include "Spark/SparkSDK.h"

/**
 * @brief Minimal default game module — blank-slate engine template
 *
 * Provides the bare minimum IModule implementation needed to load
 * into SparkEngine. No subsystems, no game logic — just a clean
 * starting point for new projects.
 */
class SparkGameDefaultModule : public Spark::IModule
{
  public:
    SparkGameDefaultModule() = default;
    ~SparkGameDefaultModule() override;

    // --- Spark::IModule interface ---
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
    void RegisterConsoleCommands();

    Spark::IEngineContext* m_context{nullptr};
    bool m_initialized{false};
    bool m_paused{false};
};

// Module exports
extern "C"
{
    SPARK_MODULE_API Spark::IModule* CreateModule();
    SPARK_MODULE_API void DestroyModule(Spark::IModule* mod);
}
