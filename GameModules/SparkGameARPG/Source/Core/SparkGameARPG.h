/**
 * @file SparkGameARPG.h
 * @brief SparkGameARPG module - Diablo-style action-RPG showcase
 * @author Spark Engine Team
 * @date 2026
 *
 * SparkGameARPG is a game module that showcases SparkEngine's ARPG
 * capabilities: isometric hero combat with multiple classes, randomized
 * loot with affixes and rarity tiers, procedural dungeon generation,
 * skill trees with runes/modifiers, and monster encounters with
 * champion affixes and boss fights.
 *
 * Implements the Spark::IModule interface for the module system.
 */

#pragma once

#include "Spark/SparkSDK.h"
#include <memory>

// Forward declarations
namespace ARPG
{
    class ARPGHeroSystem;
    class ARPGCombatSystem;
    class ARPGLootSystem;
    class ARPGDungeonSystem;
    class ARPGSkillSystem;
    class ARPGMonsterSystem;
} // namespace ARPG

/**
 * @brief Game module that demonstrates Diablo-style ARPG mechanics
 *
 * Wires up a complete ARPG framework demonstrating:
 * - 6 hero classes with attributes, leveling, and stat growth
 * - Real-time combat with damage types, AoE, and projectiles
 * - Randomized loot generation with affixes and rarity tiers
 * - Procedural dungeon levels with monster density scaling
 * - Skill trees with active/passive abilities per class
 * - Monster types with champion affixes and boss encounters
 */
class SparkGameARPGModule : public Spark::IModule
{
  public:
    SparkGameARPGModule();
    ~SparkGameARPGModule() override;

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

    // ARPG systems
    std::unique_ptr<ARPG::ARPGHeroSystem> m_heroSystem;
    std::unique_ptr<ARPG::ARPGCombatSystem> m_combatSystem;
    std::unique_ptr<ARPG::ARPGLootSystem> m_lootSystem;
    std::unique_ptr<ARPG::ARPGDungeonSystem> m_dungeonSystem;
    std::unique_ptr<ARPG::ARPGSkillSystem> m_skillSystem;
    std::unique_ptr<ARPG::ARPGMonsterSystem> m_monsterSystem;
};

// Module exports
extern "C"
{
    SPARK_MODULE_API Spark::IModule* CreateModule();
    SPARK_MODULE_API void DestroyModule(Spark::IModule* mod);
}
