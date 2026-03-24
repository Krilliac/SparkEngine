/**
 * @file SparkGameRTS.h
 * @brief SparkGameRTS module - RTS mechanics showcase
 * @author Spark Engine Team
 * @date 2026
 *
 * SparkGameRTS is a game module that showcases SparkEngine's RTS
 * capabilities: faction-based unit management, base building with tech
 * trees and production queues, mineral/gas economy, unit commands
 * (move, attack, patrol, hold), grid-based fog of war, and match
 * management with win conditions.
 *
 * Implements the Spark::IModule interface for the module system.
 */

#pragma once

#include "Spark/SparkSDK.h"
#include <memory>

// Forward declarations
namespace RTS
{
    class RTSUnitSystem;
    class RTSBuildingSystem;
    class RTSResourceSystem;
    class RTSCommandSystem;
    class RTSFogOfWarSystem;
    class RTSMatchSystem;
    class RTSEngineSystems;
} // namespace RTS

/**
 * @brief Game module that demonstrates RTS mechanics
 *
 * Wires up a complete RTS framework demonstrating:
 * - Three factions (Human, Sentinel, Swarm) with distinct unit rosters
 * - Base building with tech trees and production queues
 * - Resource economy (minerals, gas, supply management)
 * - Unit command system (move, attack, patrol, hold, gather)
 * - Grid-based fog of war with vision ranges
 * - Match setup with faction selection and win conditions
 */
class SparkGameRTSModule : public Spark::IModule
{
  public:
    SparkGameRTSModule();
    ~SparkGameRTSModule() override;

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

    // RTS systems
    std::unique_ptr<RTS::RTSUnitSystem> m_unitSystem;
    std::unique_ptr<RTS::RTSBuildingSystem> m_buildingSystem;
    std::unique_ptr<RTS::RTSResourceSystem> m_resourceSystem;
    std::unique_ptr<RTS::RTSCommandSystem> m_commandSystem;
    std::unique_ptr<RTS::RTSFogOfWarSystem> m_fogOfWarSystem;
    std::unique_ptr<RTS::RTSMatchSystem> m_matchSystem;
    std::unique_ptr<RTS::RTSEngineSystems> m_engineSystems;
};

// Module exports
extern "C"
{
    SPARK_MODULE_API Spark::IModule* CreateModule();
    SPARK_MODULE_API void DestroyModule(Spark::IModule* mod);
}
