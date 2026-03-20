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

namespace SparkEditor
{

    StreamingPanel::StreamingPanel() : EditorPanel("Streaming", "streaming_panel") {}

    bool StreamingPanel::Initialize()
    {
        std::cout << "Initializing Streaming panel\n";
        return true;
    }

    void StreamingPanel::Update(float /*deltaTime*/) {}

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
                ImGui::EndTabBar();
            }
        }
        EndPanel();
    }

    void StreamingPanel::Shutdown()
    {
        std::cout << "Shutting down Streaming panel\n";
    }

    void StreamingPanel::RenderAreaList()
    {
        ImGui::Text("Loaded Areas: %d / %d", m_loadedAreaCount, static_cast<int>(m_areas.size()));

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

        for (int i = 0; i < static_cast<int>(m_areas.size()); ++i)
        {
            auto& area = m_areas[i];
            ImGui::PushID(i);

            bool selected = (m_selectedArea == i);
            ImVec4 color = area.loaded ? ImVec4(0.2f, 0.8f, 0.2f, 1.0f) : ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, color);

            if (ImGui::Selectable(area.name, selected))
                m_selectedArea = i;

            ImGui::PopStyleColor();

            if (selected)
            {
                ImGui::Indent();
                ImGui::DragFloat3("Position", area.position, 1.0f);
                ImGui::DragFloat3("Extents", area.extents, 1.0f, 1.0f, 10000.0f);
                ImGui::Text("Entities: %d", area.entityCount);
                ImGui::Checkbox("Visible", &area.visible);
                ImGui::Unindent();
            }

            ImGui::PopID();
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
        {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Approaching rebase threshold!");
        }

        ImGui::Separator();
        ImGui::TextDisabled("Origin rebasing shifts all world coordinates to maintain");
        ImGui::TextDisabled("floating-point precision far from the origin.");
    }

} // namespace SparkEditor
