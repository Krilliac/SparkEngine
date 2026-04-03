/**
 * @file SparkGameOpenWorld.h
 * @brief Open world game module - large-world exploration showcase
 * @author Spark Engine Team
 * @date 2026
 *
 * SparkGameOpenWorld is a game module that showcases SparkEngine's open-world
 * capabilities: 8 biome regions with seamless streaming, survival mechanics,
 * wildlife ecology, settlement building, resource gathering and crafting,
 * dynamic world events, and full engine subsystem integration.
 *
 * Implements the Spark::IModule interface for the module system.
 */

#pragma once

#include "Spark/SparkSDK.h"
#include <memory>

namespace OpenWorld
{
    class OWWorldSetup;
    class OWPlayerSystem;
    class OWExplorationSystem;
    class OWWildlifeSystem;
    class OWSettlementSystem;
    class OWGatheringSystem;
    class OWDynamicEventSystem;
    class OWEngineSystems;
} // namespace OpenWorld

/**
 * @brief Game module showcasing open-world gameplay on SparkEngine
 *
 * Wires up 8 subsystems covering world streaming, player survival,
 * exploration/discovery, wildlife ecology, NPC settlements, resource
 * gathering/crafting, dynamic events, and engine integration.
 */
class SparkGameOpenWorldModule : public Spark::IModule
{
  public:
    SparkGameOpenWorldModule();
    ~SparkGameOpenWorldModule() override;

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

    std::unique_ptr<OpenWorld::OWWorldSetup> m_worldSetup;
    std::unique_ptr<OpenWorld::OWPlayerSystem> m_playerSystem;
    std::unique_ptr<OpenWorld::OWExplorationSystem> m_explorationSystem;
    std::unique_ptr<OpenWorld::OWWildlifeSystem> m_wildlifeSystem;
    std::unique_ptr<OpenWorld::OWSettlementSystem> m_settlementSystem;
    std::unique_ptr<OpenWorld::OWGatheringSystem> m_gatheringSystem;
    std::unique_ptr<OpenWorld::OWDynamicEventSystem> m_eventSystem;
    std::unique_ptr<OpenWorld::OWEngineSystems> m_engineSystems;
};

// Module exports
extern "C"
{
    SPARK_MODULE_API Spark::IModule* CreateModule();
    SPARK_MODULE_API void DestroyModule(Spark::IModule* mod);
}
