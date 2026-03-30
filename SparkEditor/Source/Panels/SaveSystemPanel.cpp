/**
 * @file SaveSystemPanel.cpp
 * @brief Implementation of the save system management panel
 */

#include "SaveSystemPanel.h"
#include "../Core/EditorIcons.h"
#include "Utils/LogMacros.h"
#include <imgui.h>
#include <iostream>
#include <cstdio>
#include <cstring>

namespace SparkEditor
{

    SaveSystemPanel::SaveSystemPanel() : EditorPanel("Save System", "save_system_panel") {}

    bool SaveSystemPanel::Initialize()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "SaveSystemPanel initialized");
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
                    if (m_selectedSlot >= 0)
                    {
                        ImGui::Separator();
                        RenderSlotActions();
                    }
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
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "SaveSystemPanel shutting down");
    }

    void SaveSystemPanel::RenderSaveSlots()
    {
        if (ImGui::BeginTable("SaveSlotTable", 6,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
        {
            ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed, 40);
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Scene");
            ImGui::TableSetupColumn("Play Time", ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_WidthFixed, 100);
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
                {
                    m_selectedSlot = i;
                    m_renaming = false;
                }

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

                ImGui::TableNextColumn();
                if (slot.occupied)
                    ImGui::Text("%.1f KB", slot.fileSizeKB);
                else
                    ImGui::TextUnformatted("-");

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(slot.occupied ? slot.lastModified : "-");
            }

            ImGui::EndTable();
        }
    }

    void SaveSystemPanel::RenderSlotActions()
    {
        auto& slot = m_slots[m_selectedSlot];
        ImGui::Text("Selected: Slot %d", slot.slot + 1);

        if (slot.occupied)
        {
            // Rename
            if (m_renaming)
            {
                ImGui::SetNextItemWidth(200);
                if (ImGui::InputText("##rename", m_renameBuffer, sizeof(m_renameBuffer),
                                     ImGuiInputTextFlags_EnterReturnsTrue))
                {
                    strncpy(slot.name, m_renameBuffer, sizeof(slot.name) - 1);
                    m_renaming = false;
                }
                ImGui::SameLine();
                if (ImGui::Button("OK"))
                {
                    strncpy(slot.name, m_renameBuffer, sizeof(slot.name) - 1);
                    m_renaming = false;
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel"))
                    m_renaming = false;
            }
            else
            {
                if (ImGui::Button(ICON_FA_DOWNLOAD " Load Save"))
                {
                    SPARK_LOG_INFO(Spark::LogCategory::Editor, "SaveSystemPanel: loading slot %d ('%s')", slot.slot,
                                   slot.name);
                }

                ImGui::SameLine();
                if (ImGui::Button(ICON_FA_PEN " Rename"))
                {
                    m_renaming = true;
                    strncpy(m_renameBuffer, slot.name, sizeof(m_renameBuffer) - 1);
                }

                ImGui::SameLine();
                if (ImGui::Button(ICON_FA_FILE_EXPORT " Export"))
                {
                    SPARK_LOG_INFO(Spark::LogCategory::Editor, "SaveSystemPanel: exporting slot %d", slot.slot);
                }

                ImGui::SameLine();
                if (ImGui::Button(ICON_FA_TRASH " Delete"))
                {
                    slot.occupied = false;
                    memset(slot.sceneName, 0, sizeof(slot.sceneName));
                    memset(slot.lastModified, 0, sizeof(slot.lastModified));
                    slot.playTime = 0.0f;
                    slot.fileSizeKB = 0.0f;
                }
            }
        }
        else
        {
            if (ImGui::Button(ICON_FA_FILE_IMPORT " Import Save"))
            {
                SPARK_LOG_INFO(Spark::LogCategory::Editor, "SaveSystemPanel: importing save into slot %d", slot.slot);
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
        ImGui::Checkbox("Quick Save/Load (F5/F9)", &m_quickSaveEnabled);
    }

} // namespace SparkEditor
