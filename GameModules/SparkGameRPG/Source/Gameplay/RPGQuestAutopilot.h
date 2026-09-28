/**
 * @file RPGQuestAutopilot.h
 * @brief Automated player that finishes a Thornwood hunting quest through the RPGDemoSession entry points
 *
 * The autopilot plays the loop a player runs by hand with rpg_travel, rpg_rest, rpg_attack and rpg_flee:
 * rest in Oakhollow, walk into Thornwood, fight the encounter when it is the quarry (flee anything else),
 * and repeat until the quest completes. Every action is one call to the same RPGDemoSession method the
 * console command calls, so the autopilot has no path the player lacks. Attacks respect real ability
 * cooldowns: an attack the session refuses as "still on cooldown" changes nothing and is retried on a later
 * frame after RPGCombatSystem::Update has advanced the cooldown.
 *
 * Contract: game thread only; Step() is called once per frame after the combat update. Owned by
 * SparkGameRPGModule while `rpg_autoplay` is active (or by a test). Allocates only for result strings.
 * Bounded: it stops after MaxTrips trips into Thornwood.
 */

#pragma once

#include <cstdint>
#include <string>

namespace RPG
{
    class RPGDemoSession;

    /// @brief Hunts in Thornwood through RPGDemoSession until a quest completes, the hero falls, or the trip cap.
    class RPGQuestAutopilot
    {
      public:
        /// @brief Why the autopilot is running or stopped.
        enum class Outcome : uint8_t
        {
            Running,
            Completed, ///< The quest reached QuestState::Completed
            Defeated,  ///< Rowan fell in a fight
            TripLimit, ///< MaxTrips trips into Thornwood did not finish the quest
            Stalled,   ///< A session action failed in a way the loop cannot recover from
        };

        /// Trips into Thornwood before the autopilot gives up; bounds the run.
        static constexpr uint32_t MaxTrips = 400;

        /**
         * @param questId Engine QuestSystem quest to finish
         * @param quarry Encounter name to fight (substring match); empty fights every encounter
         */
        RPGQuestAutopilot(uint32_t questId, std::string quarry);

        /// @brief Perform at most one session action; returns the outcome after it.
        Outcome Step(RPGDemoSession& session);

        [[nodiscard]] Outcome GetOutcome() const { return m_outcome; }
        [[nodiscard]] uint32_t GetQuestId() const { return m_questId; }
        [[nodiscard]] uint32_t GetTrips() const { return m_trips; }
        [[nodiscard]] uint32_t GetKills() const { return m_kills; }

        /// @brief One-line progress report for the rpg_autoplay console command.
        [[nodiscard]] std::string GetStatusString() const;

      private:
        uint32_t m_questId = 0;
        std::string m_quarry;
        Outcome m_outcome = Outcome::Running;
        bool m_needsRest = true;      ///< Rest in Oakhollow before the next trip
        bool m_fightingQuarry = true; ///< The engaged encounter is the quarry (an encounter already engaged at
                                      ///< start is fought)
        uint32_t m_trips = 0;
        uint32_t m_kills = 0;
        std::string m_lastResult;
    };
} // namespace RPG
