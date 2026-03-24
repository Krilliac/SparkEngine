/**
 * @file SparkGame.h
 * @brief SparkGame module - engine capabilities showcase
 * @author Spark Engine Team
 * @date 2026
 *
 * SparkGame demonstrates real engine subsystem usage: EventBus pub/sub,
 * SaveSystem persistence, coroutine scheduling, weather cycling,
 * localization, ECS entity management, and time-of-day control.
 *
 * Implements the Spark::IModule interface for the module system.
 */

#pragma once

#include "Spark/SparkSDK.h"
#include <memory>

// Forward declaration
class GameplayShowcase;

/**
 * @brief Engine showcase game module — demonstrates core subsystem integration
 *
 * Wires up a GameplayShowcase that exercises EventBus, SaveSystem,
 * CoroutineScheduler, WeatherSystem, LocalizationSystem, TimeOfDaySystem,
 * and ECS entity lifecycle from a single game module.
 */
class SparkGameDefaultModule : public Spark::IModule
{
  public:
    SparkGameDefaultModule();
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

    std::unique_ptr<GameplayShowcase> m_showcase;
};

// Module exports
extern "C"
{
    SPARK_MODULE_API Spark::IModule* CreateModule();
    SPARK_MODULE_API void DestroyModule(Spark::IModule* mod);
}
