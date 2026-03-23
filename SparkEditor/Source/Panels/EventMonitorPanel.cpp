/**
 * @file EventMonitorPanel.cpp
 * @brief Implementation of the event system monitor panel
 */

#include "EventMonitorPanel.h"
#include "../Core/EditorIcons.h"
#include <imgui.h>
#include <iostream>
#include <cstring>
#include <cstdio>

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
            RenderToolbar();
            ImGui::Separator();
            RenderStats();
            ImGui::Separator();
            RenderEventLog();
        }
        EndPanel();
    }

    void EventMonitorPanel::Shutdown()
    {
        std::cout << "Shutting down Event Monitor panel\n";
    }

    void EventMonitorPanel::RenderToolbar()
    {
        if (ImGui::Button(m_paused ? ICON_FA_PLAY " Resume" : ICON_FA_PAUSE " Pause"))
            m_paused = !m_paused;

        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_TRASH " Clear"))
        {
            m_events.clear();
            memset(m_categoryCounts, 0, sizeof(m_categoryCounts));
        }

        ImGui::SameLine();
        ImGui::Checkbox("Auto-scroll", &m_autoScroll);

        ImGui::SameLine();
        ImGui::SetNextItemWidth(200);
        ImGui::InputText(ICON_FA_FILTER " Filter", m_filterText, sizeof(m_filterText));

        // Category filter
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        const char* categories[] = {"All", "Entity", "Physics", "Gameplay", "Audio", "UI"};
        ImGui::Combo("##Category", &m_categoryFilter, categories, IM_ARRAYSIZE(categories));

        // Max entries slider
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        ImGui::DragInt("Max", &m_maxEntries, 10.0f, 100, 5000);
    }

    void EventMonitorPanel::RenderStats()
    {
        ImGui::Text("Events: %d / %d", static_cast<int>(m_events.size()), m_maxEntries);
        ImGui::SameLine();

        // Per-category counts
        for (int c = 1; c < 6; ++c)
        {
            ImGui::SameLine();
            ImVec4 color = GetCategoryColor(static_cast<EventCategory>(c));
            ImGui::TextColored(color, "%s: %d", GetCategoryName(static_cast<EventCategory>(c)), m_categoryCounts[c]);
        }
    }

    void EventMonitorPanel::RenderEventLog()
    {
        ImGui::BeginChild("EventLog", ImVec2(0, 0), true);

        if (ImGui::BeginTable("EventTable", 4,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_Sortable))
        {
            ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("Event Type", ImGuiTableColumnFlags_WidthFixed, 200);
            ImGui::TableSetupColumn("Details");
            ImGui::TableHeadersRow();

            bool hasFilter = strlen(m_filterText) > 0;

            for (const auto& evt : m_events)
            {
                // Text filter
                if (hasFilter && strstr(evt.eventType, m_filterText) == nullptr)
                    continue;

                // Category filter
                if (m_categoryFilter > 0 && static_cast<int>(evt.category) != m_categoryFilter)
                    continue;

                ImGui::TableNextRow();

                // Time column — formatted as mm:ss.ms
                ImGui::TableNextColumn();
                int totalSeconds = static_cast<int>(evt.timestamp);
                int mins = totalSeconds / 60;
                int secs = totalSeconds % 60;
                int ms = static_cast<int>((evt.timestamp - totalSeconds) * 1000) % 1000;
                ImGui::Text("%02d:%02d.%03d", mins, secs, ms);

                // Category column — color-coded
                ImGui::TableNextColumn();
                ImVec4 color = GetCategoryColor(evt.category);
                ImGui::TextColored(color, "%s", GetCategoryName(evt.category));

                // Event type
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(evt.eventType);

                // Details
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(evt.details);
            }

            ImGui::EndTable();
        }

        if (m_autoScroll)
            ImGui::SetScrollHereY(1.0f);

        ImGui::EndChild();

        // Enforce circular buffer cap
        while (static_cast<int>(m_events.size()) > m_maxEntries)
            m_events.erase(m_events.begin());
    }

    ImVec4 EventMonitorPanel::GetCategoryColor(EventCategory category)
    {
        switch (category)
        {
        case EventCategory::Entity:
            return ImVec4(0.4f, 0.8f, 0.4f, 1.0f);
        case EventCategory::Physics:
            return ImVec4(0.3f, 0.6f, 1.0f, 1.0f);
        case EventCategory::Gameplay:
            return ImVec4(1.0f, 0.8f, 0.2f, 1.0f);
        case EventCategory::Audio:
            return ImVec4(0.8f, 0.4f, 0.8f, 1.0f);
        case EventCategory::UI:
            return ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
        default:
            return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        }
    }

    const char* EventMonitorPanel::GetCategoryName(EventCategory category)
    {
        switch (category)
        {
        case EventCategory::Entity:
            return "Entity";
        case EventCategory::Physics:
            return "Physics";
        case EventCategory::Gameplay:
            return "Gameplay";
        case EventCategory::Audio:
            return "Audio";
        case EventCategory::UI:
            return "UI";
        default:
            return "All";
        }
    }

} // namespace SparkEditor
