/**
 * @file SparkGameRPG.h
 * @brief SparkGameRPG module - RPG mechanics showcase
 * @author Spark Engine Team
 * @date 2026
 *
 * SparkGameRPG is a game module that showcases SparkEngine's RPG
 * capabilities: character classes with stat growth, action-based combat
 * with cooldowns and combos, branching dialogue with skill checks,
 * quest tracking, weight-based inventory, NPC AI with schedules, and
 * world areas with encounter tables.
 *
 * Implements the Spark::IModule interface for the module system.
 */

#pragma once

#include "Spark/SparkSDK.h"
#include <memory>

// Forward declarations
namespace RPG
{
    class RPGWorldSetup;
    class RPGCharacterSystem;
    class RPGCombatSystem;
    class RPGGameplayBridge;
    class RPGDemoSession;
    class RPGInventorySystem;
    class RPGNPCSystem;
    class RPGEngineSystems;
} // namespace RPG

/**
 * @brief Game module that demonstrates RPG mechanics
 *
 * Wires up a complete RPG framework demonstrating:
 * - Character creation with 6 classes and stat allocation
 * - Action-based combat with elemental damage and combos
 * - Branching dialogue trees with skill checks
 * - Quest chains with multiple objective types
 * - Equipment and consumable inventory with weight limits
 * - NPC AI with schedules, patrol, and disposition
 * - World areas (village, forest, dungeon, castle, swamp)
 */
class SparkGameRPGModule : public Spark::IModule
{
  public:
    SparkGameRPGModule();
    ~SparkGameRPGModule() override;

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
    void UnregisterConsoleCommands();

    Spark::IEngineContext* m_context{nullptr};
    bool m_initialized{false};
    bool m_paused{false};

    // RPG systems
    std::unique_ptr<RPG::RPGWorldSetup> m_worldSetup;
    std::unique_ptr<RPG::RPGCharacterSystem> m_characterSystem;
    std::unique_ptr<RPG::RPGCombatSystem> m_combatSystem;
    std::unique_ptr<RPG::RPGGameplayBridge> m_gameplayBridge;
    std::unique_ptr<RPG::RPGInventorySystem> m_inventorySystem;
    std::unique_ptr<RPG::RPGNPCSystem> m_npcSystem;
    std::unique_ptr<RPG::RPGEngineSystems> m_engineSystems;
    std::unique_ptr<RPG::RPGDemoSession> m_demoSession;
};

// Module exports
extern "C"
{
    SPARK_MODULE_API Spark::IModule* CreateModule();
    SPARK_MODULE_API void DestroyModule(Spark::IModule* mod);
}
