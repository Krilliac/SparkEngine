/**
 * @file ConditionEditorPanel.cpp
 * @brief Implementation of the condition system editor panel
 */

#include "ConditionEditorPanel.h"
#include "../Core/EditorIcons.h"
#include "Utils/LogMacros.h"
#include <imgui.h>
#include <iostream>
#include <cstring>
#include <cstdio>
#include "SparkUI/Runtime.h"
#include "SparkUI/Widgets.h"

namespace SparkEditor
{

    ConditionEditorPanel::ConditionEditorPanel() : EditorPanel("Condition Editor", "condition_editor_panel") {}

    bool ConditionEditorPanel::Initialize()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "ConditionEditorPanel initialized");
        return true;
    }

    void ConditionEditorPanel::Update(float /*deltaTime*/) {}

    void ConditionEditorPanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            if (ImGui::BeginTabBar("ConditionTabs"))
            {
                if (ImGui::BeginTabItem(ICON_FA_PROJECT_DIAGRAM " Condition Builder"))
                {
                    RenderConditionBuilder();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_SLIDERS " World Variables"))
                {
                    RenderWorldVariables();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_FLAG " World Flags"))
                {
                    RenderWorldFlags();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        EndPanel();
    }

    void ConditionEditorPanel::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "ConditionEditorPanel shutting down");
    }

    void ConditionEditorPanel::RenderConditionBuilder()
    {
        Spark::Ui::SecondaryText("Condition sets: groups are combined with AND. Each group uses its own logic.");

        if (ImGui::Button(ICON_FA_PLUS " Add Group"))
        {
            ConditionGroupEntry group;
            m_groups.push_back(group);
            SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "ConditionEditorPanel: added condition group %d",
                            static_cast<int>(m_groups.size()));
        }

        ImGui::Separator();

        for (int g = 0; g < static_cast<int>(m_groups.size()); ++g)
        {
            ImGui::PushID(g);
            RenderConditionGroup(g);
            ImGui::PopID();

            if (g < static_cast<int>(m_groups.size()) - 1)
            {
                ImGui::Spacing();
                Spark::Ui::SubHeader("AND");
                ImGui::Spacing();
            }
        }
    }

    void ConditionEditorPanel::RenderConditionGroup(int groupIndex)
    {
        auto& group = m_groups[groupIndex];

        char headerText[64];
        snprintf(headerText, sizeof(headerText), "Group %d (%s)", groupIndex + 1, group.logic == 0 ? "AND" : "OR");

        bool open = Spark::Ui::SectionHeader(headerText);

        // Delete group button right-aligned on the header row
        const float deleteWidth = ImGui::CalcTextSize(ICON_FA_TRASH).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - deleteWidth);
        if (ImGui::SmallButton(ICON_FA_TRASH "##delgroup"))
        {
            m_groups.erase(m_groups.begin() + groupIndex);
            return;
        }

        if (!open)
            return;

        ImGui::Indent();

        // Logic selector
        const char* logicOptions[] = {"AND (all must pass)", "OR (any must pass)"};
        ImGui::Combo("Group Logic", &group.logic, logicOptions, IM_ARRAYSIZE(logicOptions));

        // Condition type names matching ConditionType enum
        const char* conditionTypeNames[] = {"IsAlive",
                                            "IsDead",
                                            "HealthAbove",
                                            "HealthBelow",
                                            "HasComponent",
                                            "HasTag",
                                            "HasAura",
                                            "InArea",
                                            "NearEntity",
                                            "InCell",
                                            "HasItem",
                                            "QuestComplete",
                                            "QuestActive",
                                            "LevelAbove",
                                            "LevelBelow",
                                            "HasAbility",
                                            "IsClass",
                                            "IsFaction",
                                            "TimeOfDayBetween",
                                            "WeatherIs",
                                            "FlagSet",
                                            "VariableEquals",
                                            "VariableGreaterThan",
                                            "VariableLessThan",
                                            "AlwaysTrue",
                                            "AlwaysFalse",
                                            "Custom"};

        for (int c = 0; c < static_cast<int>(group.conditions.size()); ++c)
        {
            auto& cond = group.conditions[c];
            ImGui::PushID(c);

            ImGui::Separator();

            // Condition type dropdown
            ImGui::SetNextItemWidth(Spark::Ui::Px(180));
            ImGui::Combo("##Type", &cond.type, conditionTypeNames, IM_ARRAYSIZE(conditionTypeNames));

            ImGui::SameLine();
            ImGui::Checkbox("NOT", &cond.negated);

            // Parameter inputs
            ImGui::SetNextItemWidth(Spark::Ui::Px(150));
            ImGui::InputText("Param 1", cond.param1, sizeof(cond.param1));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(Spark::Ui::Px(150));
            ImGui::InputText("Param 2", cond.param2, sizeof(cond.param2));

            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_FA_TRASH "##delcond"))
            {
                group.conditions.erase(group.conditions.begin() + c);
                --c;
            }

            ImGui::PopID();
        }

        ImGui::Separator();
        if (ImGui::SmallButton(ICON_FA_PLUS " Add Condition"))
        {
            ConditionEntry cond;
            group.conditions.push_back(cond);
        }

        ImGui::Unindent();
    }

    void ConditionEditorPanel::RenderWorldVariables()
    {
        ImGui::Text("Variables: %d", static_cast<int>(m_variables.size()));

        if (ImGui::BeginTable("VarTable", 3,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
        {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, Spark::Ui::Px(120));
            ImGui::TableSetupColumn("##Del", ImGuiTableColumnFlags_WidthFixed, Spark::Ui::Px(30));
            ImGui::TableHeadersRow();

            for (int i = 0; i < static_cast<int>(m_variables.size()); ++i)
            {
                auto& var = m_variables[i];
                ImGui::TableNextRow();
                ImGui::PushID(i);

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                ImGui::InputText("##name", var.name, sizeof(var.name));

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                int val = static_cast<int>(var.value);
                if (ImGui::InputInt("##val", &val))
                    var.value = val;

                ImGui::TableNextColumn();
                if (ImGui::SmallButton(ICON_FA_TRASH))
                {
                    m_variables.erase(m_variables.begin() + i);
                    --i;
                }

                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        // Add new variable
        ImGui::Separator();
        ImGui::SetNextItemWidth(Spark::Ui::Px(200));
        ImGui::InputText("Name##newvar", m_newVarName, sizeof(m_newVarName));
        ImGui::SameLine();
        int newVal = static_cast<int>(m_newVarValue);
        ImGui::SetNextItemWidth(Spark::Ui::Px(100));
        if (ImGui::InputInt("Value##newvar", &newVal))
            m_newVarValue = newVal;
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_PLUS " Add") && strlen(m_newVarName) > 0)
        {
            SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "ConditionEditorPanel: added world variable '%s'",
                            m_newVarName);
            WorldVariable var;
            strncpy(var.name, m_newVarName, sizeof(var.name) - 1);
            var.value = m_newVarValue;
            m_variables.push_back(var);
            m_newVarName[0] = '\0';
            m_newVarValue = 0;
        }
    }

    void ConditionEditorPanel::RenderWorldFlags()
    {
        ImGui::Text("Flags: %d", static_cast<int>(m_flags.size()));

        if (ImGui::BeginTable("FlagTable", 3,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
        {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, Spark::Ui::Px(50));
            ImGui::TableSetupColumn("##Del", ImGuiTableColumnFlags_WidthFixed, Spark::Ui::Px(30));
            ImGui::TableHeadersRow();

            for (int i = 0; i < static_cast<int>(m_flags.size()); ++i)
            {
                auto& flag = m_flags[i];
                ImGui::TableNextRow();
                ImGui::PushID(i);

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                ImGui::InputText("##fname", flag.name, sizeof(flag.name));

                ImGui::TableNextColumn();
                ImGui::Checkbox("##fval", &flag.value);

                ImGui::TableNextColumn();
                if (ImGui::SmallButton(ICON_FA_TRASH))
                {
                    m_flags.erase(m_flags.begin() + i);
                    --i;
                }

                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        // Add new flag
        ImGui::Separator();
        ImGui::SetNextItemWidth(Spark::Ui::Px(200));
        ImGui::InputText("Name##newflag", m_newFlagName, sizeof(m_newFlagName));
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_PLUS " Add Flag") && strlen(m_newFlagName) > 0)
        {
            WorldFlag flag;
            strncpy(flag.name, m_newFlagName, sizeof(flag.name) - 1);
            m_flags.push_back(flag);
            m_newFlagName[0] = '\0';
        }
    }

} // namespace SparkEditor
