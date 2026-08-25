/**
 * @file ARPGEngineSystems.h
 * @brief Wires Spark Engine subsystems into ARPG gameplay
 * @author Spark Engine Team
 * @date 2026
 *
 * Integrates the ARPG module with engine infrastructure: EventBus (damage/kill
 * routing), SaveSystem (hero/dungeon persistence), DestructionSystem (breakable
 * dungeon props), AI/BehaviorTree (monster intelligence), AnimationSystem
 * (hero action state machine), CoroutineScheduler (one-shot action recovery),
 * AbilitySystem (spells, auras, procs), and WeatherSystem (dungeon atmosphere).
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Utils/EventBus.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Spark::Animation
{
    class AnimationStateMachine;
}

// Forward declarations — avoid pulling in heavy headers
namespace ARPG
{

    /** @brief High-level hero actions mirrored into the engine animation state machine. */
    enum class ARPGHeroAction : uint8_t
    {
        Idle,
        Move,
        BasicAttack,
        Cast,
        Death
    };
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
        ARPGEngineSystems();
        ~ARPGEngineSystems();

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

        /** @brief Per-frame animation update and headless action-recovery fallback. */
        void Update(float deltaTime);

        /** @brief Shutdown and release all subscriptions and registrations. */
        void Shutdown();

        /** @brief Render debug UI panel (editor only). */
        void RenderDebugUI();

        /**
         * @brief Play a hero action and recover one-shot actions to Idle after their clip duration.
         *
         * Uses the engine CoroutineScheduler when available and a deterministic local timer when
         * running in a stripped/headless context.
         */
        void PlayHeroAction(ARPGHeroAction action);

        [[nodiscard]] std::string GetHeroAnimationState() const;
        [[nodiscard]] bool IsHeroActionActive() const;
        [[nodiscard]] bool HasEngineAnimationBridge() const { return m_hasAnimationBridge; }
        [[nodiscard]] bool HasEngineAbilityBridge() const { return m_hasAbilityBridge; }
        [[nodiscard]] uint32_t GetRegisteredAbilityCount() const { return m_registeredAbilityCount; }
        [[nodiscard]] uint32_t GetRegisteredAuraCount() const { return m_registeredAuraCount; }
        [[nodiscard]] uint32_t GetRegisteredProcCount() const { return m_registeredProcCount; }

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
        void CompleteHeroAction(uint64_t generation);
        [[nodiscard]] static float GetActionDuration(ARPGHeroAction action);

        // Engine context
        Spark::IEngineContext* m_context = nullptr;

        // ARPG system back-references (non-owning)
        ARPGHeroSystem* m_heroes = nullptr;
        ARPGCombatSystem* m_combat = nullptr;
        ARPGLootSystem* m_loot = nullptr;
        ARPGDungeonSystem* m_dungeon = nullptr;

        // RAII event handles — auto-unsubscribe on destruction
        std::vector<Spark::SubscriptionHandle> m_eventHandles;

        std::unique_ptr<Spark::Animation::AnimationStateMachine> m_heroAnimation;
        std::string m_actionCoroutineName;
        uint64_t m_actionGeneration = 0;
        float m_actionTimeRemaining = 0.0f;
        bool m_actionUsesCoroutine = false;
        bool m_hasAnimationBridge = false;
        bool m_hasAbilityBridge = false;
        uint32_t m_registeredAbilityCount = 0;
        uint32_t m_registeredAuraCount = 0;
        uint32_t m_registeredProcCount = 0;

        bool m_initialized = false;
    };

} // namespace ARPG
