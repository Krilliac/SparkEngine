/**
 * @file SparkGameMMO.h
 * @brief SparkGameMMO module - MMO networking showcase
 * @author Spark Engine Team
 * @date 2026
 *
 * SparkGameMMO is a game module that showcases SparkEngine's full MMO
 * networking stack: WorldServer/AreaServer architecture, seamless area
 * streaming, entity replication with delta compression, client-side
 * prediction, world origin rebasing, and multi-channel chat.
 *
 * Implements the Spark::IModule interface for the new module system.
 */

#pragma once

#include "Spark/SparkSDK.h"
#include <memory>

// Forward declarations
namespace MMO
{
    class MMOWorldSetup;
    class MMOPlayerSystem;
    class MMOChatSystem;
    class MMOInventorySystem;
    class MMOCraftingSystem;
    class MMOGuildSystem;
    class MMOTradingSystem;
    class MMOPartySystem;
    class MMOAchievementSystem;
    class MMOReputationSystem;
    class MMODungeonSystem;
    class MMOWorldBossSystem;
} // namespace MMO

/**
 * @brief Game module that demonstrates MMO networking capabilities
 *
 * Wires up the full AreaServer/WorldServer pipeline, configures multiple
 * world areas with seamless streaming, and demonstrates:
 * - Area-based world partitioning (town, wilderness, dungeon)
 * - Player spawning with replicated state
 * - Chat channels (area, global, party, whisper)
 * - Entity migration across area boundaries
 * - World origin rebasing for large-world coordinates
 * - Client-side prediction with server reconciliation
 */
class SparkGameMMOModule : public Spark::IModule
{
  public:
    SparkGameMMOModule();
    ~SparkGameMMOModule() override;

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

    std::unique_ptr<MMO::MMOWorldSetup> m_worldSetup;
    std::unique_ptr<MMO::MMOPlayerSystem> m_playerSystem;
    std::unique_ptr<MMO::MMOChatSystem> m_chatSystem;

    // MMO gameplay systems
    std::unique_ptr<MMO::MMOInventorySystem> m_inventorySystem;
    std::unique_ptr<MMO::MMOCraftingSystem> m_craftingSystem;
    std::unique_ptr<MMO::MMOGuildSystem> m_guildSystem;
    std::unique_ptr<MMO::MMOTradingSystem> m_tradingSystem;
    std::unique_ptr<MMO::MMOPartySystem> m_partySystem;
    std::unique_ptr<MMO::MMOAchievementSystem> m_achievementSystem;
    std::unique_ptr<MMO::MMOReputationSystem> m_reputationSystem;
    std::unique_ptr<MMO::MMODungeonSystem> m_dungeonSystem;
    std::unique_ptr<MMO::MMOWorldBossSystem> m_worldBossSystem;
};

// Module exports
extern "C"
{
    SPARK_MODULE_API Spark::IModule* CreateModule();
    SPARK_MODULE_API void DestroyModule(Spark::IModule* mod);
}
