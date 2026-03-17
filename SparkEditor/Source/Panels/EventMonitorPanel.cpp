/**
 * @file EventMonitorPanel.cpp
 * @brief Implementation of the event system monitor panel
 */

#include "EventMonitorPanel.h"
#include "../Core/EditorIcons.h"
#include <imgui.h>
#include <iostream>
#include <cstring>

namespace SparkEditor
{

    EventMonitorPanel::EventMonitorPanel() : EditorPanel("Event Monitor", "event_monitor_panel") {}

    bool EventMonitorPanel::Initialize()
    {
        std::cout << "Initializing Event Monitor panel\n";
        return true;
    }

    void EventMonitorPanel::Update(float deltaTime)
    {
        if (!m_paused)
            m_elapsed += deltaTime;
    }

    void EventMonitorPanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            // Toolbar
            if (ImGui::Button(m_paused ? ICON_FA_PLAY " Resume" : ICON_FA_PAUSE " Pause"))
                m_paused = !m_paused;

            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_TRASH " Clear"))
                m_events.clear();

            ImGui::SameLine();
            ImGui::Checkbox("Auto-scroll", &m_autoScroll);

            ImGui::SameLine();
            ImGui::SetNextItemWidth(200);
            ImGui::InputText(ICON_FA_FILTER " Filter", m_filterText, sizeof(m_filterText));

            ImGui::Separator();
            ImGui::Text("Events: %d", static_cast<int>(m_events.size()));

            // Event log
            ImGui::BeginChild("EventLog", ImVec2(0, 0), true);

            if (ImGui::BeginTable("EventTable", 3,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
            {
                ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableSetupColumn("Event Type", ImGuiTableColumnFlags_WidthFixed, 200);
                ImGui::TableSetupColumn("Details");
                ImGui::TableHeadersRow();

                bool hasFilter = strlen(m_filterText) > 0;

                for (const auto& evt : m_events)
                {
                    if (hasFilter && strstr(evt.eventType, m_filterText) == nullptr)
                        continue;

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%.2f", evt.timestamp);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(evt.eventType);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(evt.details);
                }

                ImGui::EndTable();
            }

            if (m_autoScroll)
                ImGui::SetScrollHereY(1.0f);

            ImGui::EndChild();
        }
        EndPanel();
    }

    void EventMonitorPanel::Shutdown()
    {
        std::cout << "Shutting down Event Monitor panel\n";
    }

} // namespace SparkEditor
