/**
 * @file SparkGamePlatformer.h
 * @brief SparkGamePlatformer module - 3D platformer showcase
 * @author Spark Engine Team
 * @date 2026
 *
 * SparkGamePlatformer is a game module that showcases SparkEngine's 3D
 * platformer mechanics: physics-based movement with state machine, coyote
 * time and jump buffering, diverse platform types, collectibles with
 * power-ups, environmental hazards, checkpoint/respawn, and a third-person
 * follow camera with geometry collision.
 *
 * Implements the Spark::IModule interface for the engine's module system.
 */

#pragma once

#include "Spark/SparkSDK.h"
#include <memory>

// Forward declarations
namespace Platformer
{
    class PlatformerPlayerController;
    class PlatformerLevelSystem;
    class PlatformerCollectibleSystem;
    class PlatformerHazardSystem;
    class PlatformerCheckpointSystem;
    class PlatformerCameraSystem;
    class PlatformerEngineSystems;
} // namespace Platformer

/**
 * @brief Game module demonstrating 3D platformer mechanics
 *
 * Wires up a complete platformer gameplay loop:
 * - Player controller with advanced movement (double jump, wall slide, dash)
 * - Level system with themed stages and star ratings
 * - Collectibles (coins, gems, stars, keys, power-ups)
 * - Hazards (spikes, lava, crushers, sawblades, lasers)
 * - Checkpoint respawn system
 * - Third-person camera with smooth follow and collision
 */
class SparkGamePlatformerModule : public Spark::IModule
{
  public:
    SparkGamePlatformerModule();
    ~SparkGamePlatformerModule() override;

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
    bool LoadPlayableLevel(uint32_t index);

    Spark::IEngineContext* m_context{nullptr};
    bool m_initialized{false};
    bool m_paused{false};

    std::unique_ptr<Platformer::PlatformerPlayerController> m_playerController;
    std::unique_ptr<Platformer::PlatformerLevelSystem> m_levelSystem;
    std::unique_ptr<Platformer::PlatformerCollectibleSystem> m_collectibleSystem;
    std::unique_ptr<Platformer::PlatformerHazardSystem> m_hazardSystem;
    std::unique_ptr<Platformer::PlatformerCheckpointSystem> m_checkpointSystem;
    std::unique_ptr<Platformer::PlatformerCameraSystem> m_cameraSystem;
    std::unique_ptr<Platformer::PlatformerEngineSystems> m_engineSystems;
};

// Module exports
extern "C"
{
    SPARK_MODULE_API Spark::IModule* CreateModule();
    SPARK_MODULE_API void DestroyModule(Spark::IModule* mod);
}
