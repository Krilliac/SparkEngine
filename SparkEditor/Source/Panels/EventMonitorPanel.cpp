/**
 * @file EventMonitorPanel.cpp
 * @brief Implementation of the event system monitor panel
 */

#include "EventMonitorPanel.h"
#include "../Core/EditorIcons.h"
#include "SparkUI/Runtime.h"
#include "SparkUI/Widgets.h"
#include "Utils/LogMacros.h"
#include <imgui.h>
#include <cstring>
#include <cstdio>

namespace SparkEditor
{

    namespace
    {
        /// Category name tinted with its categorical colour; the word carries the meaning, the
        /// colour only helps scanning. Categories start at 1 (0 is "All").
        void CategoryLabel(int category, const char* text)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, Spark::Ui::CategoryColor(category - 1));
            ImGui::TextUnformatted(text);
            ImGui::PopStyleColor();
        }
    } // namespace

    EventMonitorPanel::EventMonitorPanel() : EditorPanel("Event Monitor", "event_monitor_panel") {}

    bool EventMonitorPanel::Initialize()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "EventMonitorPanel initialized");
        return true;
    }

    void EventMonitorPanel::Update(float deltaTime)
    {
        if (!m_paused)
        {
            m_elapsed += deltaTime;
        }
    }

    void EventMonitorPanel::Render()
    {
        if (!IsVisible())
        {
            return;
        }

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
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "EventMonitorPanel shutting down");
    }

    void EventMonitorPanel::RenderToolbar()
    {
        if (!Spark::Ui::BeginPanelToolbar("EventMonitorToolbar"))
        {
            return;
        }

        if (Spark::Ui::IconButton("pause", m_paused ? ICON_FA_PLAY : ICON_FA_PAUSE,
                                  m_paused ? "Resume capture" : "Pause capture", m_paused))
        {
            m_paused = !m_paused;
        }

        ImGui::SameLine();
        if (Spark::Ui::IconButton("clear", ICON_FA_TRASH, "Clear the event log"))
        {
            SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "EventMonitorPanel: cleared %d events",
                            static_cast<int>(m_events.size()));
            m_events.clear();
            memset(m_categoryCounts, 0, sizeof(m_categoryCounts));
        }

        Spark::Ui::ToolbarSeparator();
        ImGui::Checkbox("Auto-scroll", &m_autoScroll);

        // Category filter
        ImGui::SameLine();
        ImGui::SetNextItemWidth(Spark::Ui::Px(120));
        const char* categories[] = {"All", "Entity", "Physics", "Gameplay", "Audio", "UI"};
        ImGui::Combo("##Category", &m_categoryFilter, categories, IM_ARRAYSIZE(categories));

        // Max entries slider
        ImGui::SameLine();
        ImGui::SetNextItemWidth(Spark::Ui::Px(100));
        ImGui::DragInt("Max", &m_maxEntries, 10.0f, 100, 5000);

        // Event-type search fills the rest of the row.
        ImGui::SameLine();
        Spark::Ui::SearchField("EventFilter", m_filterText, sizeof(m_filterText), "Filter event types");

        Spark::Ui::EndPanelToolbar();
    }

    void EventMonitorPanel::RenderStats()
    {
        Spark::Ui::KeyValue("Events", "%d / %d", static_cast<int>(m_events.size()), m_maxEntries);

        // Per-category counts
        for (int c = 1; c < 6; ++c)
        {
            ImGui::SameLine(0.0f, Spark::Ui::Em(1.0f));
            char label[48];
            std::snprintf(label, sizeof(label), "%s: %d", GetCategoryName(static_cast<EventCategory>(c)),
                          m_categoryCounts[c]);
            CategoryLabel(c, label);
        }
    }

    void EventMonitorPanel::RenderEventLog()
    {
        if (Spark::Ui::BeginDataTable("EventTable", 4))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, Spark::Ui::Px(80));
            ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, Spark::Ui::Px(80));
            ImGui::TableSetupColumn("Event Type", ImGuiTableColumnFlags_WidthFixed, Spark::Ui::Px(200));
            ImGui::TableSetupColumn("Details");
            ImGui::TableHeadersRow();

            bool hasFilter = strlen(m_filterText) > 0;

            for (const auto& evt : m_events)
            {
                // Text filter
                if (hasFilter && strstr(evt.eventType, m_filterText) == nullptr)
                {
                    continue;
                }

                // Category filter
                if (m_categoryFilter > 0 && static_cast<int>(evt.category) != m_categoryFilter)
                {
                    continue;
                }

                ImGui::TableNextRow();

                // Time column — formatted as mm:ss.ms
                ImGui::TableNextColumn();
                int totalSeconds = static_cast<int>(evt.timestamp);
                int mins = totalSeconds / 60;
                int secs = totalSeconds % 60;
                int ms = static_cast<int>((evt.timestamp - totalSeconds) * 1000) % 1000;
                Spark::Ui::TableNumber("%02d:%02d.%03d", mins, secs, ms);

                // Category column — tinted with its categorical colour
                ImGui::TableNextColumn();
                CategoryLabel(static_cast<int>(evt.category), GetCategoryName(evt.category));

                // Event type
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(evt.eventType);

                // Details
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(evt.details);
            }

            // The table scrolls itself (ScrollY), so auto-scroll targets its inner window.
            if (m_autoScroll)
            {
                ImGui::SetScrollHereY(1.0f);
            }

            ImGui::EndTable();
        }

        // Enforce circular buffer cap
        while (static_cast<int>(m_events.size()) > m_maxEntries)
        {
            m_events.erase(m_events.begin());
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
