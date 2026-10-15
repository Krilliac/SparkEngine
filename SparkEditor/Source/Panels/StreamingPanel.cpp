/**
 * @file StreamingPanel.cpp
 * @brief Implementation of the area streaming management panel
 */

#include "StreamingPanel.h"
#include "../Core/EditorIcons.h"
#include "SparkUI/Runtime.h"
#include "SparkUI/Widgets.h"
#include "Utils/LogMacros.h"

#include <cmath>
#include <cstdio>

#include <imgui.h>

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
        bool addArea = false;
        if (Spark::Ui::BeginPanelToolbar("##StreamingAreaToolbar"))
        {
            addArea = Spark::Ui::SecondaryButton(ICON_FA_PLUS " Add Area");
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            Spark::Ui::SecondaryText("Areas:");
            ImGui::SameLine();
            Spark::Ui::MonoText("%d loaded / %d total", m_loadedAreaCount, static_cast<int>(m_areas.size()));
            Spark::Ui::EndPanelToolbar();
        }

        if (m_areas.empty())
        {
            addArea |= Spark::Ui::EmptyState(ICON_FA_MAP, "No streaming areas defined",
                                             "Add areas to enable seamless world streaming.", "Add Area");
        }

        if (addArea)
        {
            AreaInfo area;
            std::snprintf(area.name, sizeof(area.name), "Area_%d", static_cast<int>(m_areas.size()));
            m_areas.push_back(area);
        }

        if (m_areas.empty())
        {
            return;
        }

        if (ImGui::BeginTable("AreaTable", 5,
                              ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
        {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, Spark::Ui::Px(110));
            ImGui::TableSetupColumn("Entities", ImGuiTableColumnFlags_WidthFixed, Spark::Ui::Px(60));
            ImGui::TableSetupColumn("Memory", ImGuiTableColumnFlags_WidthFixed, Spark::Ui::Px(70));
            ImGui::TableSetupColumn("Visible", ImGuiTableColumnFlags_WidthFixed, Spark::Ui::Px(50));
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
                Spark::Ui::StatusText(GetStatusKind(area.status), "%s", GetStatusName(area.status));

                ImGui::TableNextColumn();
                Spark::Ui::TableNumber("%d", area.entityCount);

                ImGui::TableNextColumn();
                Spark::Ui::TableNumber("%.1f MB", area.memorySizeMB);

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
            Spark::Ui::SubHeader(area.name);
            if (Spark::Ui::BeginPropertyGrid("SelectedArea"))
            {
                Spark::Ui::PropertyLabel("Position");
                ImGui::DragFloat3("##Position", area.position, 1.0f);
                Spark::Ui::PropertyLabel("Extents");
                ImGui::DragFloat3("##Extents", area.extents, 1.0f, 1.0f, 10000.0f);
                Spark::Ui::EndPropertyGrid();
            }
        }
    }

    void StreamingPanel::RenderStreamingSettings()
    {
        if (Spark::Ui::BeginPropertyGrid("StreamingSettings"))
        {
            Spark::Ui::PropertyLabel("Load Distance");
            ImGui::DragFloat("##LoadDistance", &m_loadDistance, 10.0f, 100.0f, 10000.0f, "%.0f m");
            Spark::Ui::PropertyLabel("Unload Distance");
            ImGui::DragFloat("##UnloadDistance", &m_unloadDistance, 10.0f, 100.0f, 10000.0f, "%.0f m");
            Spark::Ui::PropertyLabel("Player Position");
            Spark::Ui::MonoText("(%.1f, %.1f, %.1f)", m_playerPosition[0], m_playerPosition[1], m_playerPosition[2]);
            Spark::Ui::EndPropertyGrid();
        }

        if (m_unloadDistance < m_loadDistance + 50.0f)
        {
            m_unloadDistance = m_loadDistance + 50.0f;
            Spark::Ui::StatusText(Spark::Ui::Status::Warning, "Unload distance adjusted (must exceed load + 50m)");
        }
    }

    void StreamingPanel::RenderOriginRebaseInfo()
    {
        const float distFromOrigin =
            std::sqrt(m_playerPosition[0] * m_playerPosition[0] + m_playerPosition[1] * m_playerPosition[1] +
                      m_playerPosition[2] * m_playerPosition[2]);
        const float ratio = m_originRebaseThreshold > 0.0f ? distFromOrigin / m_originRebaseThreshold : 0.0f;

        if (Spark::Ui::BeginPropertyGrid("OriginRebase"))
        {
            Spark::Ui::PropertyLabel("Rebase Threshold");
            ImGui::DragFloat("##RebaseThreshold", &m_originRebaseThreshold, 100.0f, 1000.0f, 50000.0f, "%.0f units");
            Spark::Ui::PropertyLabel("Distance from Origin");
            Spark::Ui::MonoText("%.1f", distFromOrigin);
            ImGui::ProgressBar(ratio, ImVec2(-1, 0));
            Spark::Ui::PropertyLabel("Current Origin Offset");
            Spark::Ui::MonoText("(%.1f, %.1f, %.1f)", m_originOffset[0], m_originOffset[1], m_originOffset[2]);
            Spark::Ui::EndPropertyGrid();
        }

        if (ratio > 0.9f)
            Spark::Ui::StatusText(Spark::Ui::Status::Error, "Approaching rebase threshold!");

        ImGui::Separator();
        ImGui::PushTextWrapPos(0.0f);
        Spark::Ui::SecondaryText("Origin rebasing shifts all world coordinates to maintain floating-point precision "
                                 "far from the origin.");
        ImGui::PopTextWrapPos();
    }

    void StreamingPanel::RenderMemoryBudget()
    {
        Spark::Ui::SubHeader("Streaming Memory Budget");

        if (Spark::Ui::BeginPropertyGrid("MemoryBudget"))
        {
            Spark::Ui::PropertyLabel("Budget");
            ImGui::DragFloat("##Budget", &m_memoryBudgetMB, 10.0f, 64.0f, 4096.0f, "%.0f MB");
            Spark::Ui::EndPropertyGrid();
        }

        float usageRatio = m_memoryBudgetMB > 0.0f ? m_memoryUsedMB / m_memoryBudgetMB : 0.0f;
        ImVec4 barColor = usageRatio > 0.9f   ? Spark::Ui::ColV(Spark::Ui::Role::ErrorText)
                          : usageRatio > 0.7f ? Spark::Ui::ColV(Spark::Ui::Role::WarningText)
                                              : Spark::Ui::ColV(Spark::Ui::Role::SuccessText);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, barColor);

        char overlay[64];
        snprintf(overlay, sizeof(overlay), "%.1f / %.0f MB (%.0f%%)", m_memoryUsedMB, m_memoryBudgetMB,
                 usageRatio * 100.0f);
        ImGui::ProgressBar(usageRatio, ImVec2(-1, 0), overlay);

        ImGui::PopStyleColor();

        if (usageRatio > 0.9f)
            Spark::Ui::StatusText(Spark::Ui::Status::Error, "Warning: approaching memory budget limit!");

        ImGui::Separator();
        Spark::Ui::KeyValue("Loaded Areas", "%d", m_loadedAreaCount);

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
        if (Spark::Ui::BeginPanelToolbar("##StreamingEventToolbar"))
        {
            if (Spark::Ui::SecondaryButton(ICON_FA_TRASH " Clear"))
                m_streamEvents.clear();
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            Spark::Ui::SecondaryText("Recent Streaming Events:");
            ImGui::SameLine();
            Spark::Ui::MonoText("%d", static_cast<int>(m_streamEvents.size()));
            Spark::Ui::EndPanelToolbar();
        }

        if (m_streamEvents.empty())
        {
            Spark::Ui::EmptyState(ICON_FA_LIST, "No streaming events recorded yet",
                                  "State preview does not stream game areas. "
                                  "Use Play Control > Launch Game to test runtime area streaming.");
            return;
        }

        if (Spark::Ui::BeginDataTable("StreamEventTable", 3))
        {
            ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, Spark::Ui::Px(80));
            ImGui::TableSetupColumn("Area");
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, Spark::Ui::Px(100));
            ImGui::TableHeadersRow();

            for (const auto& evt : m_streamEvents)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                Spark::Ui::TableNumber("%.2f s", evt.timestamp);
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

    Spark::Ui::Status StreamingPanel::GetStatusKind(AreaStatus status)
    {
        switch (status)
        {
        case AreaStatus::Loading:
            return Spark::Ui::Status::Info;
        case AreaStatus::Loaded:
            return Spark::Ui::Status::Success;
        case AreaStatus::Unloading:
            return Spark::Ui::Status::Warning;
        default:
            return Spark::Ui::Status::Neutral;
        }
    }

} // namespace SparkEditor
