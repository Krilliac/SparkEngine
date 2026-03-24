/**
 * @file RPGQuestSystem.h
 * @brief Quest tracking with objectives, chains, rewards, and journal
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides quest definitions with multiple objective types (kill, collect,
 * talk, explore, escort), quest chains where completing one unlocks the next,
 * rewards (XP, items, reputation), and a journal categorizing quests by state.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/RPGEnums.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace RPG
{

    /// @brief A single objective within a quest
    struct QuestObjective
    {
        uint32_t objectiveId = 0;
        ObjectiveType type = ObjectiveType::Kill;
        std::string description;
        uint32_t targetId = 0; ///< Entity/item ID to interact with
        int requiredCount = 1;
        int currentCount = 0;

        bool IsComplete() const { return currentCount >= requiredCount; }
    };

    /// @brief Rewards granted upon quest completion
    struct QuestRewards
    {
        uint32_t xp = 0;
        int currency = 0;
        std::vector<std::pair<uint32_t, int>> items; ///< (itemId, count) pairs
        int reputationChange = 0;
        std::string factionName;
    };

    /// @brief Definition of a quest
    struct QuestDef
    {
        uint32_t questId = 0;
        std::string name;
        std::string description;
        int requiredLevel = 1;
        uint32_t prerequisiteQuestId = 0; ///< 0 = no prerequisite (quest chain)
        std::vector<QuestObjective> objectives;
        QuestRewards rewards;
        std::string areaName; ///< Where the quest takes place
    };

    /// @brief Tracks a player's progress on a quest
    struct QuestProgress
    {
        uint32_t questId = 0;
        QuestState state = QuestState::NotStarted;
        std::vector<QuestObjective> objectives; ///< Copy with current counts
    };

    /**
     * @brief Quest registry, tracking, and journal management
     */
    class RPGQuestSystem
    {
      public:
        RPGQuestSystem() = default;
        ~RPGQuestSystem() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Update(float deltaTime);
        void Shutdown();
        void RenderDebugUI();

        // === Quest registry ===
        void RegisterQuest(const QuestDef& quest);
        const QuestDef* GetQuestDef(uint32_t questId) const;
        size_t GetQuestCount() const { return m_quests.size(); }
        std::string GetQuestListString() const;

        // === Quest tracking ===
        bool AcceptQuest(uint32_t characterId, uint32_t questId);
        bool AbandonQuest(uint32_t characterId, uint32_t questId);
        void UpdateObjective(uint32_t characterId, ObjectiveType type, uint32_t targetId, int count = 1);
        bool CompleteQuest(uint32_t characterId, uint32_t questId);

        // === Journal queries ===
        std::vector<const QuestProgress*> GetActiveQuests(uint32_t characterId) const;
        std::vector<const QuestProgress*> GetCompletedQuests(uint32_t characterId) const;
        const QuestProgress* GetQuestProgress(uint32_t characterId, uint32_t questId) const;
        bool IsQuestComplete(uint32_t characterId, uint32_t questId) const;
        bool IsQuestAvailable(uint32_t characterId, uint32_t questId, int characterLevel) const;

      private:
        void RegisterDefaultQuests();
        bool AreAllObjectivesComplete(const QuestProgress& progress) const;

        Spark::IEngineContext* m_context{nullptr};
        std::unordered_map<uint32_t, QuestDef> m_quests;

        // characterId -> (questId -> progress)
        std::unordered_map<uint32_t, std::unordered_map<uint32_t, QuestProgress>> m_progress;
    };

} // namespace RPG
