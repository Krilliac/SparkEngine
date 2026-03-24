/**
 * @file MMOEngineSystems.h
 * @brief Wires SparkGameMMO into engine subsystems: weather, abilities, dialogue,
 *        cinematic, AI, animation, events, and localization
 * @author Spark Engine Team
 * @date 2026
 *
 * MMOEngineSystems registers MMO-specific content (class abilities, NPC dialogue
 * trees, boss cinematic sequences, mob behavior trees, animation state machines,
 * event subscriptions, and localization tables) with the engine's built-in systems
 * accessed through IEngineContext.
 */

#pragma once

#include "Spark/SparkSDK.h"
#include "Utils/EventBus.h"

#include <string>
#include <vector>

namespace MMO
{

    // =========================================================================
    // MMO-specific event types (published through the engine EventBus)
    // =========================================================================

    /** @brief Fired when a player gains a level. */
    struct PlayerLevelUpEvent
    {
        uint32_t entityId = 0;
        int oldLevel = 0;
        int newLevel = 0;
    };

    /** @brief Fired when loot is distributed to a party or raid. */
    struct LootDistributedEvent
    {
        uint32_t looterId = 0;
        uint32_t itemId = 0;
        uint32_t sourceEntityId = 0;
    };

    /** @brief Fired when a dungeon instance is cleared. */
    struct DungeonClearedEvent
    {
        uint32_t dungeonId = 0;
        float completionTime = 0.0f;
        int partySize = 0;
    };

    // =========================================================================
    // MMOEngineSystems
    // =========================================================================

    /**
     * @class MMOEngineSystems
     * @brief Registers MMO content with engine subsystems and manages event subscriptions
     *
     * Centralizes all engine-system wiring so that Main.cpp stays focused on module
     * lifecycle. Each Register*() method is idempotent and safe to call if the
     * corresponding engine subsystem is unavailable (logs a warning and returns).
     */
    class MMOEngineSystems
    {
      public:
        MMOEngineSystems() = default;
        ~MMOEngineSystems() = default;

        /**
         * @brief Initialize and register all MMO content with engine subsystems
         * @param context Engine context providing subsystem access
         * @return true if at least the core registrations succeeded
         */
        bool Initialize(Spark::IEngineContext* context);

        /** @brief Per-frame update for systems that need ticking (weather sync, etc.) */
        void Update(float deltaTime);

        /** @brief Unsubscribe events and release references */
        void Shutdown();

        // --- Console helpers ---
        std::string GetAbilitySummary() const;
        std::string GetWeatherStatus() const;
        std::string GetCinematicList() const;
        std::string GetLocaleStatus() const;

      private:
        void RegisterWeatherAndTimeOfDay();
        void RegisterAbilities();
        void RegisterDialogueTrees();
        void RegisterCinematicSequences();
        void RegisterBehaviorTrees();
        void RegisterAnimationStateMachines();
        void SubscribeEvents();
        void RegisterLocalization();

        Spark::IEngineContext* m_context = nullptr;

        // RAII event handles — automatically unsubscribe on destruction
        std::vector<Spark::SubscriptionHandle> m_eventHandles;

        bool m_initialized = false;
    };

} // namespace MMO
