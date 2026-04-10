#include "RPGGameplayBridge.h"

#include "Character/RPGCharacterSystem.h"
#include "Engine/Dialogue/DialogueSystem.h"
#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <algorithm>
#include <sstream>

namespace RPG
{
    namespace
    {
        uint32_t ParseUintOrZero(const std::string& value)
        {
            if (value.empty())
            {
                return 0;
            }
            try
            {
                return static_cast<uint32_t>(std::stoul(value));
            }
            catch (...)
            {
                return 0;
            }
        }
    } // namespace

    bool RPGGameplayBridge::Initialize(Spark::IEngineContext* context, RPGCharacterSystem* characterSystem)
    {
        if (!context)
        {
            return false;
        }

        m_context = context;
        m_characterSystem = characterSystem;
        m_registeredDialogueTrees = 0;

        Spark::Gameplay::QuestSystem::GetInstance().SetPolicy(this);
        RegisterRPGQuests();
        RegisterRPGDialogueTrees();
        RegisterDialogueHooks();

        Spark::SimpleConsole::GetInstance().LogInfo("[RPG] Gameplay bridge initialized (engine quest/dialogue)");
        return true;
    }

    void RPGGameplayBridge::Shutdown()
    {
        Spark::Gameplay::QuestSystem::GetInstance().SetPolicy(nullptr);
        m_context = nullptr;
        m_characterSystem = nullptr;
        m_registeredDialogueTrees = 0;
    }

    void RPGGameplayBridge::Update(float deltaTime)
    {
        (void)deltaTime;
    }

    size_t RPGGameplayBridge::GetRegisteredQuestCount() const
    {
        const auto status = Spark::Gameplay::QuestSystem::GetInstance().Console_GetStatus();
        (void)status;
        // QuestSystem does not currently expose quest registry size directly.
        // Keep the canonical count in this bridge definition block.
        return 6;
    }

    bool RPGGameplayBridge::CanStartQuest(uint32_t entityId, const Spark::Gameplay::QuestDefinition& questDef) const
    {
        if (!m_characterSystem)
        {
            return true;
        }

        const auto* character = m_characterSystem->GetCharacter(entityId);
        if (!character)
        {
            return true;
        }

        return static_cast<uint32_t>(character->level) >= questDef.requiredLevel;
    }

    void RPGGameplayBridge::OnQuestStarted(uint32_t entityId, const Spark::Gameplay::QuestDefinition& questDef)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[RPG] Quest started through engine system: %s (entity=%u)",
                       questDef.name.c_str(), entityId);
    }

    void RPGGameplayBridge::OnObjectiveProgress(uint32_t entityId, uint32_t questId,
                                                const Spark::Gameplay::QuestObjective& objective)
    {
        SPARK_LOG_DEBUG(Spark::LogCategory::Game, "[RPG] Objective update entity=%u quest=%u '%s' (%u/%u)", entityId,
                        questId, objective.description.c_str(), objective.currentCount, objective.requiredCount);
    }

    void RPGGameplayBridge::OnQuestCompleted(uint32_t entityId, const Spark::Gameplay::QuestDefinition& questDef)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[RPG] Quest completed through engine system: %s (entity=%u)",
                       questDef.name.c_str(), entityId);
    }

    void RPGGameplayBridge::RegisterRPGQuests()
    {
        auto& questSystem = Spark::Gameplay::QuestSystem::GetInstance();

        Spark::Gameplay::QuestDefinition wolfHunt;
        wolfHunt.questId = 1;
        wolfHunt.name = "Shadow Wolves";
        wolfHunt.description = "Clear the shadow wolves threatening Oakhollow village.";
        wolfHunt.requiredLevel = 1;
        wolfHunt.objectives.push_back({"Slay shadow wolves", Spark::Gameplay::QuestObjective::Type::Kill, 100, 5});
        wolfHunt.xpReward = 50;
        wolfHunt.itemRewards = {{1, 3}};
        questSystem.RegisterQuest(wolfHunt);

        Spark::Gameplay::QuestDefinition herbGather;
        herbGather.questId = 2;
        herbGather.name = "Healing Herbs";
        herbGather.description = "Gather moonpetal herbs from the forest to make healing salves.";
        herbGather.requiredLevel = 2;
        herbGather.prerequisiteQuestId = 1;
        herbGather.objectives.push_back(
            {"Gather moonpetal herbs", Spark::Gameplay::QuestObjective::Type::Collect, 200, 8});
        herbGather.xpReward = 75;
        herbGather.itemRewards = {{2, 5}};
        questSystem.RegisterQuest(herbGather);

        Spark::Gameplay::QuestDefinition investigate;
        investigate.questId = 3;
        investigate.name = "The Dark Below";
        investigate.description = "Speak with the hermit in Thornwood about the ancient crypt.";
        investigate.requiredLevel = 3;
        investigate.prerequisiteQuestId = 2;
        investigate.objectives.push_back(
            {"Speak with the forest hermit", Spark::Gameplay::QuestObjective::Type::Talk, 300, 1});
        investigate.xpReward = 60;
        questSystem.RegisterQuest(investigate);

        Spark::Gameplay::QuestDefinition cryptExplore;
        cryptExplore.questId = 4;
        cryptExplore.name = "Descent into Darkness";
        cryptExplore.description = "Explore the Shadow Crypt and defeat the necromancer.";
        cryptExplore.requiredLevel = 5;
        cryptExplore.prerequisiteQuestId = 3;
        cryptExplore.objectives.push_back(
            {"Reach the crypt's inner sanctum", Spark::Gameplay::QuestObjective::Type::Reach, 400, 1});
        cryptExplore.objectives.push_back(
            {"Defeat the necromancer", Spark::Gameplay::QuestObjective::Type::Kill, 401, 1});
        cryptExplore.xpReward = 200;
        cryptExplore.itemRewards = {{3, 1}};
        questSystem.RegisterQuest(cryptExplore);

        Spark::Gameplay::QuestDefinition merchantEscort;
        merchantEscort.questId = 5;
        merchantEscort.name = "Safe Passage";
        merchantEscort.description = "Escort the merchant through Mirefen Swamp.";
        merchantEscort.requiredLevel = 4;
        merchantEscort.objectives.push_back(
            {"Guide the merchant to safety", Spark::Gameplay::QuestObjective::Type::Custom, 500, 1});
        merchantEscort.xpReward = 120;
        questSystem.RegisterQuest(merchantEscort);

        Spark::Gameplay::QuestDefinition castleSiege;
        castleSiege.questId = 6;
        castleSiege.name = "The Fallen Keep";
        castleSiege.description = "Retake Thornwall Castle from the undead legion.";
        castleSiege.requiredLevel = 8;
        castleSiege.prerequisiteQuestId = 4;
        castleSiege.objectives.push_back(
            {"Eliminate undead soldiers", Spark::Gameplay::QuestObjective::Type::Kill, 600, 10});
        castleSiege.objectives.push_back(
            {"Breach the inner gate", Spark::Gameplay::QuestObjective::Type::Interact, 601, 1});
        castleSiege.objectives.push_back(
            {"Defeat the Death Knight", Spark::Gameplay::QuestObjective::Type::Kill, 602, 1});
        castleSiege.xpReward = 500;
        castleSiege.itemRewards = {{4, 1}};
        questSystem.RegisterQuest(castleSiege);
    }

    void RPGGameplayBridge::RegisterRPGDialogueTrees()
    {
        auto* dialogue = m_context ? m_context->GetDialogue() : nullptr;
        if (!dialogue)
        {
            return;
        }

        auto blacksmith = std::make_unique<Spark::DialogueTree>();
        blacksmith->SetId("rpg_blacksmith");
        blacksmith->SetStartNodeId("100");

        Spark::DialogueNode startNode;
        startNode.id = "100";
        startNode.type = Spark::DialogueNodeType::Choice;
        startNode.speakerName = "Grimjaw the Blacksmith";
        startNode.text = "Welcome, traveler. What can I do for you?";
        Spark::DialogueChoice forgeChoice;
        forgeChoice.text = "I need a weapon forged.";
        forgeChoice.nextNodeId = "101";

        Spark::DialogueChoice strengthChoice;
        strengthChoice.text = "[Strength 12] Arm wrestle for a discount?";
        strengthChoice.nextNodeId = "103";
        strengthChoice.condition = "rpg_stat_gte:strength,12";

        Spark::DialogueChoice farewellChoice;
        farewellChoice.text = "Nothing, farewell.";
        farewellChoice.nextNodeId = "199";

        startNode.choices.push_back(std::move(forgeChoice));
        startNode.choices.push_back(std::move(strengthChoice));
        startNode.choices.push_back(std::move(farewellChoice));
        blacksmith->AddNode(startNode);

        Spark::DialogueNode forge;
        forge.id = "101";
        forge.type = Spark::DialogueNodeType::Text;
        forge.speakerName = "Grimjaw the Blacksmith";
        forge.text = "Bring me iron ore and I'll forge something worthy of a hero.";
        forge.nextNodeId = "199";
        blacksmith->AddNode(forge);

        Spark::DialogueNode checkSuccess;
        checkSuccess.id = "103";
        checkSuccess.type = Spark::DialogueNodeType::Event;
        checkSuccess.eventName = "rpg.startQuest";
        checkSuccess.eventData = "1";
        checkSuccess.nextNodeId = "104";
        blacksmith->AddNode(checkSuccess);

        Spark::DialogueNode rewardText;
        rewardText.id = "104";
        rewardText.type = Spark::DialogueNodeType::Text;
        rewardText.speakerName = "Grimjaw the Blacksmith";
        rewardText.text = "By the forge! You beat me. I'll mark a hunt contract for you.";
        rewardText.nextNodeId = "199";
        blacksmith->AddNode(rewardText);

        Spark::DialogueNode end;
        end.id = "199";
        end.type = Spark::DialogueNodeType::End;
        blacksmith->AddNode(end);

        dialogue->RegisterTree("rpg_blacksmith", std::move(blacksmith));
        m_registeredDialogueTrees = 1;
    }

    void RPGGameplayBridge::RegisterDialogueHooks()
    {
        auto* dialogue = m_context ? m_context->GetDialogue() : nullptr;
        if (!dialogue)
        {
            return;
        }

        dialogue->RegisterCondition("rpg_stat_gte",
                                    [this](const std::string& param)
                                    {
                                        const auto sep = param.find(',');
                                        if (sep == std::string::npos)
                                        {
                                            return false;
                                        }

                                        const std::string statName = param.substr(0, sep);
                                        float requiredValue = 0.0f;
                                        try
                                        {
                                            requiredValue = std::stof(param.substr(sep + 1));
                                        }
                                        catch (const std::exception&)
                                        {
                                            return false;
                                        }
                                        const uint32_t characterId = GetActiveDialogueCharacter();
                                        if (characterId == 0)
                                        {
                                            return false;
                                        }

                                        return LookupCharacterStat(characterId, statName) >= requiredValue;
                                    });

        dialogue->RegisterEventAction("rpg.startQuest",
                                      [this](const std::string& payload)
                                      {
                                          const uint32_t questId = ParseUintOrZero(payload);
                                          const uint32_t characterId = GetActiveDialogueCharacter();
                                          if (questId == 0 || characterId == 0)
                                          {
                                              return;
                                          }
                                          Spark::Gameplay::QuestSystem::GetInstance().StartQuest(characterId, questId);
                                      });
    }

    uint32_t RPGGameplayBridge::GetActiveDialogueCharacter() const
    {
        const auto* dialogue = m_context ? m_context->GetDialogue() : nullptr;
        if (!dialogue)
        {
            return 0;
        }

        return ParseUintOrZero(dialogue->GetVariable("rpg.characterId"));
    }

    float RPGGameplayBridge::LookupCharacterStat(uint32_t characterId, const std::string& statName) const
    {
        if (!m_characterSystem)
        {
            return 0.0f;
        }

        const auto stats = m_characterSystem->ComputeEffectiveStats(characterId);
        if (statName == "strength")
            return stats.strength;
        if (statName == "dexterity")
            return stats.dexterity;
        if (statName == "intelligence")
            return stats.intelligence;
        if (statName == "wisdom")
            return stats.wisdom;
        if (statName == "constitution")
            return stats.constitution;
        if (statName == "charisma")
            return stats.charisma;

        return 0.0f;
    }

    void RPGGameplayBridge::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (ImGui::TreeNode("RPG Gameplay Bridge"))
        {
            ImGui::Text("Quest policy installed: %s",
                        Spark::Gameplay::QuestSystem::GetInstance().GetPolicy() == this ? "Yes" : "No");
            ImGui::Text("Registered quests: %zu", GetRegisteredQuestCount());
            ImGui::Text("Registered dialogue trees: %zu", m_registeredDialogueTrees);
            ImGui::TreePop();
        }
#endif
    }

} // namespace RPG
