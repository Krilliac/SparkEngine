/**
 * @file EventResponsePanel.h
 * @brief Editor panel for creating and managing event response rules (no-code gameplay)
 *
 * Provides a visual rule builder for the EventResponseSystem's "When/If/Then"
 * rules. Non-coders can define gameplay behaviors without writing any code.
 */

#pragma once

#include "../Core/EditorPanel.h"

#include <cstdint>
#include <string>
#include <vector>

namespace SparkEditor
{

    /**
     * @brief Panel for building data-driven event response rules
     *
     * Left side: rule list with enable/disable toggles and + New Rule button.
     * Right side: rule editor with trigger type, condition groups, and action list.
     */
    class EventResponsePanel : public EditorPanel
    {
      public:
        EventResponsePanel();
        ~EventResponsePanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

      private:
        // -- UI Sections --
        void RenderRuleList();
        void RenderRuleEditor();
        void RenderTriggerSection();
        void RenderConditionSection();
        void RenderActionList();
        void RenderActionEditor(int actionIndex);

        // -- Local rule editing state (mirrors EventResponseRule) --
        struct ActionEntry
        {
            int typeIndex = 0;
            char params[4][128] = {};
            int paramCount = 0;
        };

        struct ConditionGroupUI
        {
            int logic = 0; // 0=AND, 1=OR
            struct CondEntry
            {
                int typeIndex = 0;
                char param1[128] = {};
                char param2[128] = {};
                bool negated = false;
            };
            std::vector<CondEntry> conditions;
        };

        struct RuleEntry
        {
            char name[128] = {};
            uint32_t sourceEntityId = 0;
            int triggerIndex = 0;
            char triggerParam[128] = {};
            bool enabled = true;
            bool oneShot = false;
            std::vector<ActionEntry> actions;
            std::vector<ConditionGroupUI> conditionGroups;
        };

        void SyncFromEngine();
        void SyncToEngine();
        void AddNewRule();

        std::vector<RuleEntry> m_rules;
        int m_selectedRule = -1;
        bool m_dirty = false;
        char m_searchFilter[128] = {};

        // Statistics
        uint32_t m_totalFired = 0;
    };

} // namespace SparkEditor
