/**
 * @file TriggerEditorPanel.cpp
 * @brief Implementation of the proximity trigger volume editor panel
 */

#include "TriggerEditorPanel.h"
#include "../Core/EditorIcons.h"
#include <imgui.h>
#include <iostream>
#include <cstdio>
#include <cstring>

namespace SparkEditor
{

    TriggerEditorPanel::TriggerEditorPanel() : EditorPanel("Trigger Editor", "trigger_editor_panel") {}

    bool TriggerEditorPanel::Initialize()
    {
        std::cout << "Initializing Trigger Editor panel\n";
        return true;
    }

    void TriggerEditorPanel::Update(float /*deltaTime*/) {}

    void TriggerEditorPanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            // Toolbar
            ImGui::Text(ICON_FA_CROSSHAIRS " Triggers: %d", static_cast<int>(m_triggers.size()));

            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_PLUS " Create"))
                m_showCreateDialog = true;

            ImGui::SameLine();
            ImGui::Checkbox("Show in Viewport", &m_showInViewport);

            ImGui::Separator();

            // Split view: list + details
            float listWidth = ImGui::GetContentRegionAvail().x * 0.4f;

            ImGui::BeginChild("TriggerList", ImVec2(listWidth, 0), true);
            RenderTriggerList();
            ImGui::EndChild();

            ImGui::SameLine();

            ImGui::BeginChild("TriggerDetails", ImVec2(0, 0), true);
            RenderTriggerDetails();
            ImGui::EndChild();

            if (m_showCreateDialog)
                RenderCreateDialog();
        }
        EndPanel();
    }

    void TriggerEditorPanel::Shutdown()
    {
        std::cout << "Shutting down Trigger Editor panel\n";
    }

    void TriggerEditorPanel::RenderTriggerList()
    {
        if (ImGui::BeginTable("TriggerTable", 4,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY))
        {
            ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 40);
            ImGui::TableSetupColumn("Label");
            ImGui::TableSetupColumn("Shape", ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, 30);
            ImGui::TableHeadersRow();

            for (int i = 0; i < static_cast<int>(m_triggers.size()); ++i)
            {
                auto& trigger = m_triggers[i];
                ImGui::TableNextRow();
                ImGui::PushID(i);

                ImGui::TableNextColumn();
                bool selected = (m_selectedTrigger == i);
                char idStr[16];
                snprintf(idStr, sizeof(idStr), "%u", trigger.id);
                if (ImGui::Selectable(idStr, selected, ImGuiSelectableFlags_SpanAllColumns))
                    m_selectedTrigger = i;

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(strlen(trigger.label) > 0 ? trigger.label : "(unnamed)");

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(trigger.shape == 0 ? "Sphere" : "AABB");

                ImGui::TableNextColumn();
                ImGui::Checkbox("##en", &trigger.enabled);

                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    void TriggerEditorPanel::RenderTriggerDetails()
    {
        if (m_selectedTrigger < 0 || m_selectedTrigger >= static_cast<int>(m_triggers.size()))
        {
            ImGui::TextDisabled("Select a trigger to view details");
            return;
        }

        auto& trigger = m_triggers[m_selectedTrigger];

        ImGui::Text("Trigger #%u", trigger.id);
        ImGui::Separator();

        ImGui::InputText("Label", trigger.label, sizeof(trigger.label));

        const char* shapes[] = {"Sphere", "AABB"};
        ImGui::Combo("Shape", &trigger.shape, shapes, IM_ARRAYSIZE(shapes));

        ImGui::DragFloat3("Center", trigger.center, 0.5f);

        if (trigger.shape == 0)
        {
            ImGui::DragFloat("Radius", &trigger.radius, 0.1f, 0.1f, 1000.0f, "%.1f m");
        }
        else
        {
            ImGui::DragFloat3("Half Extents", trigger.halfExtents, 0.1f, 0.1f, 1000.0f);
        }

        ImGui::Checkbox("Enabled", &trigger.enabled);
        ImGui::Text("Occupants: %d", trigger.occupantCount);

        ImGui::Separator();
        if (ImGui::Button(ICON_FA_TRASH " Delete Trigger"))
        {
            m_triggers.erase(m_triggers.begin() + m_selectedTrigger);
            m_selectedTrigger = -1;
        }
    }

    void TriggerEditorPanel::RenderCreateDialog()
    {
        ImGui::SetNextWindowSize(ImVec2(350, 280), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Create Trigger", &m_showCreateDialog))
        {
            ImGui::InputText("Label", m_newLabel, sizeof(m_newLabel));

            const char* shapes[] = {"Sphere", "AABB"};
            ImGui::Combo("Shape", &m_newShape, shapes, IM_ARRAYSIZE(shapes));

            ImGui::DragFloat3("Center", m_newCenter, 0.5f);

            if (m_newShape == 0)
            {
                ImGui::DragFloat("Radius", &m_newRadius, 0.1f, 0.1f, 1000.0f, "%.1f m");
            }
            else
            {
                ImGui::DragFloat3("Half Extents", m_newExtents, 0.1f, 0.1f, 1000.0f);
            }

            ImGui::Separator();
            if (ImGui::Button(ICON_FA_CHECK " Create"))
            {
                TriggerInfo trigger;
                trigger.id = static_cast<uint32_t>(m_triggers.size()) + 1;
                trigger.shape = m_newShape;
                trigger.center[0] = m_newCenter[0];
                trigger.center[1] = m_newCenter[1];
                trigger.center[2] = m_newCenter[2];
                trigger.radius = m_newRadius;
                trigger.halfExtents[0] = m_newExtents[0];
                trigger.halfExtents[1] = m_newExtents[1];
                trigger.halfExtents[2] = m_newExtents[2];
                strncpy(trigger.label, m_newLabel, sizeof(trigger.label) - 1);
                m_triggers.push_back(trigger);

                m_selectedTrigger = static_cast<int>(m_triggers.size()) - 1;
                m_showCreateDialog = false;

                // Reset create fields
                m_newLabel[0] = '\0';
                m_newCenter[0] = m_newCenter[1] = m_newCenter[2] = 0.0f;
            }
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_TIMES " Cancel"))
                m_showCreateDialog = false;
        }
        ImGui::End();
    }

} // namespace SparkEditor
