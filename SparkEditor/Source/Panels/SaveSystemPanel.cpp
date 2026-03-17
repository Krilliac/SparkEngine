/**
 * @file SaveSystemPanel.cpp
 * @brief Implementation of the save system management panel
 */

#include "SaveSystemPanel.h"
#include "../Core/EditorIcons.h"
#include <imgui.h>
#include <iostream>
#include <cstdio>

namespace SparkEditor
{

    SaveSystemPanel::SaveSystemPanel() : EditorPanel("Save System", "save_system_panel") {}

    bool SaveSystemPanel::Initialize()
    {
        std::cout << "Initializing Save System panel\n";
        // Initialize empty slots
        m_slots.resize(m_maxSlots);
        for (int i = 0; i < m_maxSlots; ++i)
        {
            m_slots[i].slot = i;
            snprintf(m_slots[i].name, sizeof(m_slots[i].name), "Slot %d", i + 1);
        }
        return true;
    }

    void SaveSystemPanel::Update(float /*deltaTime*/) {}

    void SaveSystemPanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            if (ImGui::BeginTabBar("SaveTabs"))
            {
                if (ImGui::BeginTabItem(ICON_FA_SAVE " Save Slots"))
                {
                    RenderSaveSlots();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_COG " Autosave Settings"))
                {
                    RenderAutosaveSettings();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        EndPanel();
    }

    void SaveSystemPanel::Shutdown()
    {
        std::cout << "Shutting down Save System panel\n";
    }

    void SaveSystemPanel::RenderSaveSlots()
    {
        if (ImGui::BeginTable("SaveSlotTable", 4,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
        {
            ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Scene");
            ImGui::TableSetupColumn("Play Time", ImGuiTableColumnFlags_WidthFixed, 100);
            ImGui::TableHeadersRow();

            for (int i = 0; i < static_cast<int>(m_slots.size()); ++i)
            {
                auto& slot = m_slots[i];
                ImGui::TableNextRow();

                ImGui::TableNextColumn();
                char slotLabel[16];
                snprintf(slotLabel, sizeof(slotLabel), "%d", slot.slot + 1);
                bool selected = (m_selectedSlot == i);
                if (ImGui::Selectable(slotLabel, selected, ImGuiSelectableFlags_SpanAllColumns))
                    m_selectedSlot = i;

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(slot.occupied ? slot.name : "(Empty)");

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(slot.occupied ? slot.sceneName : "-");

                ImGui::TableNextColumn();
                if (slot.occupied)
                {
                    int minutes = static_cast<int>(slot.playTime) / 60;
                    int seconds = static_cast<int>(slot.playTime) % 60;
                    ImGui::Text("%d:%02d", minutes, seconds);
                }
                else
                {
                    ImGui::TextUnformatted("-");
                }
            }

            ImGui::EndTable();
        }

        if (m_selectedSlot >= 0)
        {
            ImGui::Separator();
            auto& slot = m_slots[m_selectedSlot];
            ImGui::Text("Selected: Slot %d", slot.slot + 1);

            if (slot.occupied)
            {
                ImGui::SameLine();
                if (ImGui::Button(ICON_FA_TRASH " Delete Save"))
                {
                    slot.occupied = false;
                    memset(slot.sceneName, 0, sizeof(slot.sceneName));
                    slot.playTime = 0.0f;
                }
            }
        }
    }

    void SaveSystemPanel::RenderAutosaveSettings()
    {
        ImGui::Checkbox("Autosave Enabled", &m_autosaveEnabled);

        if (m_autosaveEnabled)
        {
            ImGui::DragFloat("Interval", &m_autosaveInterval, 10.0f, 30.0f, 3600.0f, "%.0f seconds");
            ImGui::DragInt("Rotation Count", &m_autosaveRotation, 1.0f, 1, 10);
            ImGui::TextDisabled("Keeps the last %d autosaves", m_autosaveRotation);
        }

        ImGui::Separator();
        ImGui::Checkbox("Quick Save/Load (F5/F9)", &m_autosaveEnabled);
    }

} // namespace SparkEditor
