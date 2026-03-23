/**
 * @file AbilityEditorPanel.h
 * @brief Ability/aura/proc editor panel for the Spark Engine Editor
 */

#pragma once

#include "../Core/EditorPanel.h"
#include <string>
#include <vector>

namespace SparkEditor
{

    /**
     * @brief Panel for browsing and editing ability, aura, and proc definitions
     *
     * Provides tabbed views for the three registries in AbilitySystem:
     * abilities (spells), auras (buffs/debuffs), and procs (triggered effects).
     * Displays all registered definitions with their full parameter sets.
     */
    class AbilityEditorPanel : public EditorPanel
    {
      public:
        AbilityEditorPanel();
        ~AbilityEditorPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

      private:
        // --- Ability editing state ---
        struct AbilityEntry
        {
            uint32_t id = 0;
            char name[128] = {};
            char description[256] = {};
            int targetType = 0; // maps to AbilityTargetType
            float range = 10.0f;
            float radius = 0.0f;
            float castTime = 0.0f;
            float cooldown = 1.0f;
            float resourceCost = 0.0f;
            bool requiresTarget = true;
            bool canCastWhileMoving = false;
            bool isChanneled = false;
            float channelDuration = 0.0f;
            int effectCount = 0;
        };

        struct AuraEntry
        {
            uint32_t id = 0;
            char name[128] = {};
            int auraType = 0; // maps to AuraType
            int school = 0;   // maps to AbilitySchool
            float duration = 10.0f;
            float tickInterval = 1.0f;
            float valuePerTick = 0.0f;
            float flatModifier = 0.0f;
            float percentModifier = 0.0f;
            int stackType = 0; // maps to AuraStackType
            int maxStacks = 1;
            bool isPermanent = false;
            bool isHidden = false;
            bool dispellable = true;
        };

        struct ProcEntry
        {
            uint32_t sourceAuraId = 0;
            uint32_t triggeredAbilityId = 0;
            uint32_t triggerMask = 0;
            float chance = 100.0f;
            float cooldown = 0.0f;
            int charges = 0;
        };

        void RenderAbilitiesTab();
        void RenderAurasTab();
        void RenderProcsTab();
        void RenderAbilityDetails(AbilityEntry& ability);
        void RenderAuraDetails(AuraEntry& aura);

        std::vector<AbilityEntry> m_abilities;
        std::vector<AuraEntry> m_auras;
        std::vector<ProcEntry> m_procs;

        int m_selectedAbility = -1;
        int m_selectedAura = -1;
        int m_selectedProc = -1;
        char m_filterText[128] = {};
    };

} // namespace SparkEditor
