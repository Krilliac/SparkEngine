/**
 * @file StreamingPanel.cpp
 * @brief Implementation of the area streaming management panel
 */

#include "StreamingPanel.h"
#include "../Core/EditorIcons.h"
#include <imgui.h>
#include <iostream>
#include <cstdio>
#include <cmath>
#include "Utils/LogMacros.h"

namespace SparkEditor
{

    StreamingPanel::StreamingPanel() : EditorPanel("Streaming", "streaming_panel") {}

    bool StreamingPanel::Initialize()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Initializing Streaming panel");
        return true;
    }

    void StreamingPanel::Update(float /*deltaTime*/)
    {
        // Calculate loaded area count and memory usage
        m_loadedAreaCount = 0;
        m_memoryUsedMB = 0.0f;
        for (const auto& area : m_areas)
        {
            if (area.status == AreaStatus::Loaded || area.status == AreaStatus::Loading)
            {
                ++m_loadedAreaCount;
                m_memoryUsedMB += area.memorySizeMB;
            }
        }
    }

    void StreamingPanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            if (ImGui::BeginTabBar("StreamingTabs"))
            {
                if (ImGui::BeginTabItem(ICON_FA_MAP " Areas"))
                {
                    RenderAreaList();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_COG " Settings"))
                {
                    RenderStreamingSettings();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_GLOBE " Origin Rebase"))
                {
                    RenderOriginRebaseInfo();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_MEMORY " Memory"))
                {
                    RenderMemoryBudget();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_LIST " Event Log"))
                {
                    RenderEventLog();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        EndPanel();
    }

    void StreamingPanel::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Shutting down Streaming panel");
    }

    void StreamingPanel::RenderAreaList()
    {
        ImGui::Text("Areas: %d loaded / %d total", m_loadedAreaCount, static_cast<int>(m_areas.size()));

        if (ImGui::Button(ICON_FA_PLUS " Add Area"))
        {
            AreaInfo area;
            snprintf(area.name, sizeof(area.name), "Area_%d", static_cast<int>(m_areas.size()));
            m_areas.push_back(area);
        }

        ImGui::Separator();

        if (m_areas.empty())
        {
            ImGui::TextDisabled("No streaming areas defined.");
            ImGui::TextDisabled("Add areas to enable seamless world streaming.");
            return;
        }

        if (ImGui::BeginTable("AreaTable", 5,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
        {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("Entities", ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableSetupColumn("Memory", ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableSetupColumn("Visible", ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableHeadersRow();

            for (int i = 0; i < static_cast<int>(m_areas.size()); ++i)
            {
                auto& area = m_areas[i];
                ImGui::TableNextRow();
                ImGui::PushID(i);

                ImGui::TableNextColumn();
                bool selected = (m_selectedArea == i);
                if (ImGui::Selectable(area.name, selected, ImGuiSelectableFlags_SpanAllColumns))
                    m_selectedArea = i;

                ImGui::TableNextColumn();
                ImVec4 statusColor = GetStatusColor(area.status);
                ImGui::TextColored(statusColor, "%s", GetStatusName(area.status));

                ImGui::TableNextColumn();
                ImGui::Text("%d", area.entityCount);

                ImGui::TableNextColumn();
                ImGui::Text("%.1f MB", area.memorySizeMB);

                ImGui::TableNextColumn();
                ImGui::Checkbox("##vis", &area.visible);

                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        // Selected area details
        if (m_selectedArea >= 0 && m_selectedArea < static_cast<int>(m_areas.size()))
        {
            auto& area = m_areas[m_selectedArea];
            ImGui::Separator();
            ImGui::Text("Selected: %s", area.name);
            ImGui::DragFloat3("Position", area.position, 1.0f);
            ImGui::DragFloat3("Extents", area.extents, 1.0f, 1.0f, 10000.0f);
        }
    }

    void StreamingPanel::RenderStreamingSettings()
    {
        ImGui::DragFloat("Load Distance", &m_loadDistance, 10.0f, 100.0f, 10000.0f, "%.0f m");
        ImGui::DragFloat("Unload Distance", &m_unloadDistance, 10.0f, 100.0f, 10000.0f, "%.0f m");

        if (m_unloadDistance < m_loadDistance + 50.0f)
        {
            m_unloadDistance = m_loadDistance + 50.0f;
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Unload distance adjusted (must exceed load + 50m)");
        }

        ImGui::Separator();
        ImGui::Text("Player Position: (%.1f, %.1f, %.1f)", m_playerPosition[0], m_playerPosition[1],
                    m_playerPosition[2]);
    }

    void StreamingPanel::RenderOriginRebaseInfo()
    {
        ImGui::DragFloat("Rebase Threshold", &m_originRebaseThreshold, 100.0f, 1000.0f, 50000.0f, "%.0f units");

        float distFromOrigin =
            std::sqrt(m_playerPosition[0] * m_playerPosition[0] + m_playerPosition[1] * m_playerPosition[1] +
                      m_playerPosition[2] * m_playerPosition[2]);

        ImGui::Text("Distance from Origin: %.1f", distFromOrigin);

        float ratio = m_originRebaseThreshold > 0.0f ? distFromOrigin / m_originRebaseThreshold : 0.0f;
        ImGui::ProgressBar(ratio, ImVec2(-1, 0));

        if (ratio > 0.9f)
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Approaching rebase threshold!");

        ImGui::Separator();
        ImGui::Text("Current Origin Offset: (%.1f, %.1f, %.1f)", m_originOffset[0], m_originOffset[1],
                    m_originOffset[2]);

        ImGui::Separator();
        ImGui::TextDisabled("Origin rebasing shifts all world coordinates to maintain");
        ImGui::TextDisabled("floating-point precision far from the origin.");
    }

    void StreamingPanel::RenderMemoryBudget()
    {
        ImGui::Text("Streaming Memory Budget");
        ImGui::Separator();

        ImGui::DragFloat("Budget", &m_memoryBudgetMB, 10.0f, 64.0f, 4096.0f, "%.0f MB");

        float usageRatio = m_memoryBudgetMB > 0.0f ? m_memoryUsedMB / m_memoryBudgetMB : 0.0f;
        ImVec4 barColor = usageRatio > 0.9f   ? ImVec4(1.0f, 0.2f, 0.2f, 1.0f)
                          : usageRatio > 0.7f ? ImVec4(1.0f, 0.8f, 0.0f, 1.0f)
                                              : ImVec4(0.2f, 0.8f, 0.2f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, barColor);

        char overlay[64];
        snprintf(overlay, sizeof(overlay), "%.1f / %.0f MB (%.0f%%)", m_memoryUsedMB, m_memoryBudgetMB,
                 usageRatio * 100.0f);
        ImGui::ProgressBar(usageRatio, ImVec2(-1, 0), overlay);

        ImGui::PopStyleColor();

        if (usageRatio > 0.9f)
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Warning: approaching memory budget limit!");

        ImGui::Separator();
        ImGui::Text("Loaded Areas: %d", m_loadedAreaCount);

        // Per-area memory breakdown
        for (const auto& area : m_areas)
        {
            if (area.status == AreaStatus::Loaded)
            {
                ImGui::BulletText("%s: %.1f MB (%d entities)", area.name, area.memorySizeMB, area.entityCount);
            }
        }
    }

    void StreamingPanel::RenderEventLog()
    {
        ImGui::Text("Recent Streaming Events: %d", static_cast<int>(m_streamEvents.size()));

        if (ImGui::Button(ICON_FA_TRASH " Clear"))
            m_streamEvents.clear();

        ImGui::Separator();

        if (m_streamEvents.empty())
        {
            ImGui::TextDisabled("No streaming events recorded yet.");
            ImGui::TextDisabled("Events appear when areas load or unload during Play mode.");
            return;
        }

        if (ImGui::BeginTable("StreamEventTable", 3,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY))
        {
            ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("Area");
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 100);
            ImGui::TableHeadersRow();

            for (const auto& evt : m_streamEvents)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%.2f s", evt.timestamp);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(evt.areaName);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(evt.action);
            }
            ImGui::EndTable();
        }
    }

    const char* StreamingPanel::GetStatusName(AreaStatus status)
    {
        switch (status)
        {
        case AreaStatus::Loading:
            return "Loading";
        case AreaStatus::Loaded:
            return "Loaded";
        case AreaStatus::Unloading:
            return "Unloading";
        default:
            return "Unloaded";
        }
    }

    ImVec4 StreamingPanel::GetStatusColor(AreaStatus status)
    {
        switch (status)
        {
        case AreaStatus::Loading:
            return ImVec4(1.0f, 0.8f, 0.0f, 1.0f);
        case AreaStatus::Loaded:
            return ImVec4(0.2f, 0.8f, 0.2f, 1.0f);
        case AreaStatus::Unloading:
            return ImVec4(1.0f, 0.5f, 0.0f, 1.0f);
        default:
            return ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
        }
    }

} // namespace SparkEditor
