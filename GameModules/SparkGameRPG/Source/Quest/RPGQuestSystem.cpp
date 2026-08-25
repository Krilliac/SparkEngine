/**
 * @file RPGQuestSystem.cpp
 * @brief Quest definitions, tracking, objective updates, and journal
 */

#include "RPGQuestSystem.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <sstream>

namespace RPG
{

    bool RPGQuestSystem::Initialize(Spark::IEngineContext* context)
    {
        m_context = context;
        RegisterDefaultQuests();

        Spark::SimpleConsole::GetInstance().LogInfo("[RPG] Quest system initialized (" +
                                                    std::to_string(m_quests.size()) + " quests)");
        return true;
    }

    void RPGQuestSystem::Shutdown()
    {
        m_quests.clear();
        m_progress.clear();
    }

    void RPGQuestSystem::RegisterDefaultQuests()
    {
        // Quest 1: Starter kill quest in the village outskirts
        QuestDef wolfHunt;
        wolfHunt.questId = 1;
        wolfHunt.name = "Shadow Wolves";
        wolfHunt.description = "Clear the shadow wolves threatening Oakhollow village.";
        wolfHunt.requiredLevel = 1;
        wolfHunt.areaName = "Forest";
        wolfHunt.objectives = {
            {1, ObjectiveType::Kill, "Slay shadow wolves", 100, 5, 0},
        };
        wolfHunt.rewards = {50, 25, {{1, 3}}, 0, ""};
        RegisterQuest(wolfHunt);

        // Quest 2: Collection quest chained from quest 1
        QuestDef herbGather;
        herbGather.questId = 2;
        herbGather.name = "Healing Herbs";
        herbGather.description = "Gather moonpetal herbs from the forest to make healing salves.";
        herbGather.requiredLevel = 2;
        herbGather.prerequisiteQuestId = 1;
        herbGather.areaName = "Forest";
        herbGather.objectives = {
            {2, ObjectiveType::Collect, "Gather moonpetal herbs", 200, 8, 0},
        };
        herbGather.rewards = {75, 40, {{2, 5}}, 5, "Oakhollow"};
        RegisterQuest(herbGather);

        // Quest 3: Talk quest — investigate the dungeon
        QuestDef investigate;
        investigate.questId = 3;
        investigate.name = "The Dark Below";
        investigate.description = "Speak with the hermit in Thornwood about the ancient crypt.";
        investigate.requiredLevel = 3;
        investigate.prerequisiteQuestId = 2;
        investigate.areaName = "Forest";
        investigate.objectives = {
            {3, ObjectiveType::Talk, "Speak with the forest hermit", 5, 1, 0},
        };
        investigate.rewards = {60, 0, {}, 10, "Oakhollow"};
        RegisterQuest(investigate);

        // Quest 4: Dungeon exploration quest
        QuestDef cryptExplore;
        cryptExplore.questId = 4;
        cryptExplore.name = "Descent into Darkness";
        cryptExplore.description = "Explore the Shadow Crypt beneath Thornwood and defeat the necromancer.";
        cryptExplore.requiredLevel = 5;
        cryptExplore.prerequisiteQuestId = 3;
        cryptExplore.areaName = "Dungeon";
        cryptExplore.objectives = {
            {4, ObjectiveType::Explore, "Reach the crypt's inner sanctum", 3, 1, 0},
            {5, ObjectiveType::Kill, "Defeat the necromancer", 204, 1, 0},
        };
        cryptExplore.rewards = {200, 100, {{3, 1}}, 20, "Oakhollow"};
        RegisterQuest(cryptExplore);

        // Quest 5: Escort quest in the swamp
        QuestDef merchantEscort;
        merchantEscort.questId = 5;
        merchantEscort.name = "Safe Passage";
        merchantEscort.description = "Escort the merchant through the Mirefen Swamp to the castle road.";
        merchantEscort.requiredLevel = 4;
        merchantEscort.areaName = "Swamp";
        merchantEscort.objectives = {
            {6, ObjectiveType::Escort, "Guide the merchant to safety", 500, 1, 0},
        };
        merchantEscort.rewards = {120, 80, {}, 10, "Merchant Guild"};
        RegisterQuest(merchantEscort);

        // Quest 6: Castle quest — multi-objective
        QuestDef castleSiege;
        castleSiege.questId = 6;
        castleSiege.name = "The Fallen Keep";
        castleSiege.description = "Retake Thornwall Castle from the undead legion.";
        castleSiege.requiredLevel = 8;
        castleSiege.prerequisiteQuestId = 4;
        castleSiege.areaName = "Castle";
        castleSiege.objectives = {
            {7, ObjectiveType::Kill, "Eliminate undead soldiers", 600, 10, 0},
            {8, ObjectiveType::Explore, "Breach the inner gate", 601, 1, 0},
            {9, ObjectiveType::Kill, "Defeat the Death Knight", 602, 1, 0},
        };
        castleSiege.rewards = {500, 200, {{4, 1}}, 30, "Kingdom"};
        RegisterQuest(castleSiege);
    }

    // === Quest registry ===

    void RPGQuestSystem::RegisterQuest(const QuestDef& quest)
    {
        m_quests[quest.questId] = quest;
    }

    const QuestDef* RPGQuestSystem::GetQuestDef(uint32_t questId) const
    {
        auto it = m_quests.find(questId);
        return it != m_quests.end() ? &it->second : nullptr;
    }

    std::string RPGQuestSystem::GetQuestListString() const
    {
        std::ostringstream ss;
        ss << "=== RPG Quests ===\n";
        for (const auto& [id, quest] : m_quests)
        {
            ss << "[" << id << "] " << quest.name << " (Lv" << quest.requiredLevel << ", " << quest.areaName << ")\n";
            ss << "    " << quest.description << "\n";
            ss << "    Objectives: " << quest.objectives.size() << " | Rewards: " << quest.rewards.xp << " XP, "
               << quest.rewards.currency << " gold\n";
        }
        return ss.str();
    }

    // === Quest tracking ===

    bool RPGQuestSystem::AcceptQuest(uint32_t characterId, uint32_t questId, int characterLevel)
    {
        const auto* def = GetQuestDef(questId);
        if (characterId == 0 || !def || !IsQuestAvailable(characterId, questId, characterLevel))
            return false;

        auto& charProgress = m_progress[characterId];

        QuestProgress progress;
        progress.questId = questId;
        progress.state = QuestState::InProgress;
        progress.objectives = def->objectives; // Copy with zeroed counts

        charProgress[questId] = progress;

        SPARK_LOG_INFO(Spark::LogCategory::Game, "RPG quest accepted: %s (id=%u)", def->name.c_str(), questId);
        Spark::SimpleConsole::GetInstance().LogInfo("[RPG] Quest accepted: " + def->name);
        return true;
    }

    bool RPGQuestSystem::AbandonQuest(uint32_t characterId, uint32_t questId)
    {
        auto charIt = m_progress.find(characterId);
        if (charIt == m_progress.end())
            return false;

        auto questIt = charIt->second.find(questId);
        if (questIt == charIt->second.end())
            return false;
        if (questIt->second.state != QuestState::InProgress)
            return false;

        questIt->second.state = QuestState::Failed;

        const auto* def = GetQuestDef(questId);
        SPARK_LOG_INFO(Spark::LogCategory::Game, "RPG quest abandoned: %s (id=%u)", def ? def->name.c_str() : "?",
                       questId);
        Spark::SimpleConsole::GetInstance().LogInfo("[RPG] Quest abandoned: " + (def ? def->name : "?"));
        return true;
    }

    void RPGQuestSystem::UpdateObjective(uint32_t characterId, ObjectiveType type, uint32_t targetId, int count)
    {
        if (count <= 0)
            return;

        auto charIt = m_progress.find(characterId);
        if (charIt == m_progress.end())
            return;

        for (auto& [questId, progress] : charIt->second)
        {
            if (progress.state != QuestState::InProgress)
                continue;

            for (auto& obj : progress.objectives)
            {
                if (obj.type == type && obj.targetId == targetId && !obj.IsComplete())
                {
                    obj.currentCount += count;
                    if (obj.currentCount > obj.requiredCount)
                        obj.currentCount = obj.requiredCount;

                    if (obj.IsComplete())
                    {
                        Spark::SimpleConsole::GetInstance().LogInfo("[RPG] Objective complete: " + obj.description);
                    }
                }
            }
        }
    }

    bool RPGQuestSystem::CompleteQuest(uint32_t characterId, uint32_t questId)
    {
        auto charIt = m_progress.find(characterId);
        if (charIt == m_progress.end())
            return false;

        auto questIt = charIt->second.find(questId);
        if (questIt == charIt->second.end())
            return false;
        if (questIt->second.state != QuestState::InProgress)
            return false;

        if (!AreAllObjectivesComplete(questIt->second))
            return false;

        questIt->second.state = QuestState::Completed;

        const auto* def = GetQuestDef(questId);
        SPARK_LOG_INFO(Spark::LogCategory::Game, "RPG quest completed: %s (id=%u)", def ? def->name.c_str() : "?",
                       questId);
        Spark::SimpleConsole::GetInstance().LogInfo("[RPG] Quest completed: " + (def ? def->name : "?"));
        return true;
    }

    // === Journal queries ===

    std::vector<const QuestProgress*> RPGQuestSystem::GetActiveQuests(uint32_t characterId) const
    {
        std::vector<const QuestProgress*> result;
        auto charIt = m_progress.find(characterId);
        if (charIt == m_progress.end())
            return result;

        for (const auto& [questId, progress] : charIt->second)
        {
            if (progress.state == QuestState::InProgress)
                result.push_back(&progress);
        }
        return result;
    }

    std::vector<const QuestProgress*> RPGQuestSystem::GetCompletedQuests(uint32_t characterId) const
    {
        std::vector<const QuestProgress*> result;
        auto charIt = m_progress.find(characterId);
        if (charIt == m_progress.end())
            return result;

        for (const auto& [questId, progress] : charIt->second)
        {
            if (progress.state == QuestState::Completed)
                result.push_back(&progress);
        }
        return result;
    }

    const QuestProgress* RPGQuestSystem::GetQuestProgress(uint32_t characterId, uint32_t questId) const
    {
        auto charIt = m_progress.find(characterId);
        if (charIt == m_progress.end())
            return nullptr;

        auto questIt = charIt->second.find(questId);
        return questIt != charIt->second.end() ? &questIt->second : nullptr;
    }

    bool RPGQuestSystem::IsQuestComplete(uint32_t characterId, uint32_t questId) const
    {
        const auto* progress = GetQuestProgress(characterId, questId);
        return progress && progress->state == QuestState::Completed;
    }

    bool RPGQuestSystem::IsQuestAvailable(uint32_t characterId, uint32_t questId, int characterLevel) const
    {
        const auto* def = GetQuestDef(questId);
        if (!def)
            return false;

        // Level check
        if (characterLevel < def->requiredLevel)
            return false;

        // Prerequisite check
        if (def->prerequisiteQuestId != 0)
        {
            if (!IsQuestComplete(characterId, def->prerequisiteQuestId))
                return false;
        }

        // Not already accepted or completed
        const auto* progress = GetQuestProgress(characterId, questId);
        return !progress || progress->state == QuestState::Failed;
    }

    bool RPGQuestSystem::AreAllObjectivesComplete(const QuestProgress& progress) const
    {
        for (const auto& obj : progress.objectives)
        {
            if (!obj.IsComplete())
                return false;
        }
        return true;
    }

    void RPGQuestSystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (ImGui::TreeNode("RPG Quest System"))
        {
            ImGui::Text("Quests: %zu", m_quests.size());

            if (ImGui::TreeNode("Quest Registry"))
            {
                for (const auto& [id, quest] : m_quests)
                {
                    if (ImGui::TreeNode(quest.name.c_str()))
                    {
                        ImGui::Text("Level: %d | Area: %s", quest.requiredLevel, quest.areaName.c_str());
                        ImGui::TextWrapped("%s", quest.description.c_str());
                        ImGui::Text("Objectives: %zu | XP: %u | Gold: %d", quest.objectives.size(), quest.rewards.xp,
                                    quest.rewards.currency);
                        if (quest.prerequisiteQuestId > 0)
                            ImGui::Text("Requires quest: %u", quest.prerequisiteQuestId);
                        ImGui::TreePop();
                    }
                }
                ImGui::TreePop();
            }
            ImGui::TreePop();
        }
#endif
    }

} // namespace RPG
