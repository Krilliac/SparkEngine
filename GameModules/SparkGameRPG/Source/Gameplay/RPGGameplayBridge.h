#pragma once

#include "Spark/IEngineContext.h"
#include "Engine/Gameplay/QuestSystem.h"

#include <cstdint>
#include <string>

namespace RPG
{
    class RPGCharacterSystem;

    class RPGGameplayBridge final : public Spark::Gameplay::QuestSystem::IQuestPolicy
    {
      public:
        bool Initialize(Spark::IEngineContext* context, RPGCharacterSystem* characterSystem);
        void Shutdown();
        void Update(float deltaTime);
        void RenderDebugUI();

        [[nodiscard]] size_t GetRegisteredQuestCount() const;
        [[nodiscard]] size_t GetRegisteredDialogueTreeCount() const { return m_registeredDialogueTrees; }

        // Quest policy strategy (engine extension point)
        [[nodiscard]] bool CanStartQuest(uint32_t entityId,
                                         const Spark::Gameplay::QuestDefinition& questDef) const override;
        void OnQuestStarted(uint32_t entityId, const Spark::Gameplay::QuestDefinition& questDef) override;
        void OnObjectiveProgress(uint32_t entityId, uint32_t questId,
                                 const Spark::Gameplay::QuestObjective& objective) override;
        void OnQuestCompleted(uint32_t entityId, const Spark::Gameplay::QuestDefinition& questDef) override;

      private:
        void RegisterRPGQuests();
        void RegisterRPGDialogueTrees();
        void RegisterDialogueHooks();

        [[nodiscard]] uint32_t GetActiveDialogueCharacter() const;
        [[nodiscard]] float LookupCharacterStat(uint32_t characterId, const std::string& statName) const;

        Spark::IEngineContext* m_context = nullptr;
        RPGCharacterSystem* m_characterSystem = nullptr;
        size_t m_registeredDialogueTrees = 0;
    };

} // namespace RPG
