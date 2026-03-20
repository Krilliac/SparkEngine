/**
 * @file CoroutineDebugPanel.cpp
 * @brief Implementation of the coroutine debug panel
 */

#include "CoroutineDebugPanel.h"
#include "../Core/EditorIcons.h"
#include <imgui.h>
#include <iostream>

namespace SparkEditor
{

    CoroutineDebugPanel::CoroutineDebugPanel() : EditorPanel("Coroutine Debug", "coroutine_debug_panel") {}

    bool CoroutineDebugPanel::Initialize()
    {
        std::cout << "Initializing Coroutine Debug panel\n";
        return true;
    }

    void CoroutineDebugPanel::Update(float /*deltaTime*/) {}

    void CoroutineDebugPanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            RenderCoroutineStats();
            ImGui::Separator();
            RenderActiveCoroutines();
        }
        EndPanel();
    }

    void CoroutineDebugPanel::Shutdown()
    {
        std::cout << "Shutting down Coroutine Debug panel\n";
    }

    void CoroutineDebugPanel::RenderActiveCoroutines()
    {
        int activeCount = static_cast<int>(m_coroutines.size());
        ImGui::Text("Active Coroutines: %d", activeCount);

        if (ImGui::Button(ICON_FA_STOP " Stop All"))
        {
            m_coroutines.clear();
        }

        if (m_coroutines.empty())
        {
            ImGui::TextDisabled("No active coroutines.");
            ImGui::TextDisabled("Coroutines are started during Play mode via CoroutineScheduler.");
            return;
        }

        if (ImGui::BeginTable("CoroutineTable", 4,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
        {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Elapsed", ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("State");
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableHeadersRow();

            for (int i = 0; i < static_cast<int>(m_coroutines.size()); ++i)
            {
                auto& cr = m_coroutines[i];
                ImGui::TableNextRow();
                ImGui::PushID(i);

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(cr.name);

                ImGui::TableNextColumn();
                ImGui::Text("%.1f s", cr.elapsedTime);

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(cr.waitState);

                ImGui::TableNextColumn();
                if (ImGui::SmallButton(ICON_FA_STOP "##stop"))
                {
                    m_coroutines.erase(m_coroutines.begin() + i);
                    ImGui::PopID();
                    break;
                }

                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    void CoroutineDebugPanel::RenderCoroutineStats()
    {
        ImGui::Text("Total Started: %d | Completed: %d", m_totalStarted, m_totalCompleted);
    }

} // namespace SparkEditor
