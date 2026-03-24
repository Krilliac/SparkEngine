/**
 * @file ARPGEngineSystems.h
 * @brief Wires Spark Engine subsystems into ARPG gameplay
 * @author Spark Engine Team
 * @date 2026
 *
 * Integrates the ARPG module with engine infrastructure: EventBus (damage/kill
 * routing), SaveSystem (hero/dungeon persistence), DestructionSystem (breakable
 * dungeon props), AI/BehaviorTree (monster intelligence), AnimationSystem
 * (hero state machines), CoroutineScheduler (wave spawning, buff timers),
 * AbilitySystem (spells, auras, procs), and WeatherSystem (dungeon atmosphere).
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Utils/EventBus.h"

#include <vector>

// Forward declarations — avoid pulling in heavy headers
namespace ARPG
{
    class ARPGHeroSystem;
    class ARPGCombatSystem;
    class ARPGLootSystem;
    class ARPGDungeonSystem;
} // namespace ARPG

namespace ARPG
{

    /**
     * @brief Bridges Spark Engine systems into ARPG gameplay logic.
     *
     * Owns event subscriptions (RAII handles) and registers ARPG-specific data
     * with engine singletons (save serializers, fracture patterns, behavior trees,
     * animation state machines, abilities, auras, procs, and weather presets).
     *
     * Lifetime: created after all ARPG subsystems are initialized, destroyed
     * before any of them are shut down.
     */
    class ARPGEngineSystems
    {
      public:
        ARPGEngineSystems() = default;
        ~ARPGEngineSystems() = default;

        ARPGEngineSystems(const ARPGEngineSystems&) = delete;
        ARPGEngineSystems& operator=(const ARPGEngineSystems&) = delete;

        /**
         * @brief Initialize all engine integrations.
         * @param context   Engine context for subsystem access.
         * @param heroes    Non-owning pointer to the hero system (for XP awards).
         * @param combat    Non-owning pointer to the combat system (for damage tracking).
         * @param loot      Non-owning pointer to the loot system (for drop triggers).
         * @param dungeon   Non-owning pointer to the dungeon system (for floor/progress data).
         * @return true on success.
         */
        bool Initialize(Spark::IEngineContext* context, ARPGHeroSystem* heroes, ARPGCombatSystem* combat,
                        ARPGLootSystem* loot, ARPGDungeonSystem* dungeon);

        /** @brief Per-frame update for coroutine-driven and weather systems. */
        void Update(float deltaTime);

        /** @brief Shutdown and release all subscriptions and registrations. */
        void Shutdown();

        /** @brief Render debug UI panel (editor only). */
        void RenderDebugUI();

      private:
        // --- Registration helpers (called once during Initialize) ---
        void SetupEventSubscriptions();
        void SetupSaveSystem();
        void SetupDestruction();
        void SetupAI();
        void SetupAnimation();
        void SetupCoroutines();
        void SetupAbilities();
        void SetupWeather();

        // Engine context
        Spark::IEngineContext* m_context = nullptr;

        // ARPG system back-references (non-owning)
        ARPGHeroSystem* m_heroes = nullptr;
        ARPGCombatSystem* m_combat = nullptr;
        ARPGLootSystem* m_loot = nullptr;
        ARPGDungeonSystem* m_dungeon = nullptr;

        // RAII event handles — auto-unsubscribe on destruction
        std::vector<Spark::SubscriptionHandle> m_eventHandles;

        bool m_initialized = false;
    };

} // namespace ARPG
