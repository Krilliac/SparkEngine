/**
 * @file SparkGameRacing.h
 * @brief SparkGameRacing module - Racing game showcase
 * @author Spark Engine Team
 * @date 2026
 *
 * SparkGameRacing is a game module that showcases SparkEngine's capabilities
 * for racing games: physics-based vehicle handling, track layouts with
 * checkpoints, AI drivers, race state management, multiple camera modes,
 * and a racing HUD with speedometer, minimap, and standings.
 *
 * Implements the Spark::IModule interface for the engine's module system.
 */

#pragma once

#include "Spark/SparkSDK.h"
#include <memory>

// Forward declarations
namespace Racing
{
    class RacingVehicleSystem;
    class RacingTrackSystem;
    class RacingRaceManager;
    class RacingAIDriver;
    class RacingCameraSystem;
    class RacingHUDSystem;
    class RacingEngineSystems;
} // namespace Racing

/**
 * @brief Game module that demonstrates racing game mechanics
 *
 * Wires up a complete racing game framework demonstrating:
 * - Physics-based vehicle driving with multiple vehicle types
 * - Track layouts with checkpoints, surface zones, and hazards
 * - Race lifecycle management with timing and positions
 * - AI drivers with difficulty scaling and rubber-banding
 * - Chase, cockpit, hood, orbit, and cinematic cameras
 * - HUD with speedometer, tachometer, minimap, and standings
 */
class SparkGameRacingModule : public Spark::IModule
{
  public:
    SparkGameRacingModule();
    ~SparkGameRacingModule() override;

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

    std::unique_ptr<Racing::RacingVehicleSystem> m_vehicleSystem;
    std::unique_ptr<Racing::RacingTrackSystem> m_trackSystem;
    std::unique_ptr<Racing::RacingRaceManager> m_raceManager;
    std::unique_ptr<Racing::RacingAIDriver> m_aiDriver;
    std::unique_ptr<Racing::RacingCameraSystem> m_cameraSystem;
    std::unique_ptr<Racing::RacingHUDSystem> m_hudSystem;
    std::unique_ptr<Racing::RacingEngineSystems> m_engineSystems;
};

// Module exports
extern "C"
{
    SPARK_MODULE_API Spark::IModule* CreateModule();
    SPARK_MODULE_API void DestroyModule(Spark::IModule* mod);
}
