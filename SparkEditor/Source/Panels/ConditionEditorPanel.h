/**
 * @file ConditionEditorPanel.h
 * @brief Condition system editor panel for the Spark Engine Editor
 */

#pragma once

#include "../Core/EditorPanel.h"
#include <string>
#include <vector>

namespace SparkEditor
{

    /**
     * @brief Panel for building and testing data-driven conditions
     *
     * Provides a visual condition set builder (AND/OR groups with negation),
     * a world variable editor, and a world flag editor. Conditions are used
     * by quests, dialogue, AI, loot tables, and shops via ConditionSystem.
     */
    class ConditionEditorPanel : public EditorPanel
    {
      public:
        ConditionEditorPanel();
        ~ConditionEditorPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

      private:
        // Mirrors ConditionType from engine
        struct ConditionEntry
        {
            int type = 0;          // index into ConditionType enum
            char param1[128] = {}; // string/numeric param
            char param2[128] = {}; // second param
            bool negated = false;
        };

        struct ConditionGroupEntry
        {
            int logic = 0; // 0=AND, 1=OR
            std::vector<ConditionEntry> conditions;
        };

        struct WorldVariable
        {
            char name[128] = {};
            int64_t value = 0;
        };

        struct WorldFlag
        {
            char name[128] = {};
            bool value = false;
        };

        void RenderConditionBuilder();
        void RenderWorldVariables();
        void RenderWorldFlags();
        void RenderConditionGroup(int groupIndex);

        std::vector<ConditionGroupEntry> m_groups;
        std::vector<WorldVariable> m_variables;
        std::vector<WorldFlag> m_flags;

        char m_newVarName[128] = {};
        int64_t m_newVarValue = 0;
        char m_newFlagName[128] = {};
    };

} // namespace SparkEditor
