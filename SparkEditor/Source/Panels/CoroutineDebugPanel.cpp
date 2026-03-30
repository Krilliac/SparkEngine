/**
 * @file CoroutineDebugPanel.cpp
 * @brief Implementation of the coroutine debug panel
 */

#include "CoroutineDebugPanel.h"
#include "../Core/EditorIcons.h"
#include "Utils/LogMacros.h"
#include <imgui.h>
#include <iostream>

namespace SparkEditor
{

    CoroutineDebugPanel::CoroutineDebugPanel() : EditorPanel("Coroutine Debug", "coroutine_debug_panel") {}

    bool CoroutineDebugPanel::Initialize()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "CoroutineDebugPanel initialized");
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
            RenderFilterBar();
            ImGui::Separator();
            RenderActiveCoroutines();
        }
        EndPanel();
    }

    void CoroutineDebugPanel::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "CoroutineDebugPanel shutting down");
    }

    void CoroutineDebugPanel::RenderCoroutineStats()
    {
        int activeCount = 0;
        int suspendedCount = 0;
        for (const auto& cr : m_coroutines)
        {
            if (cr.status == CoroutineStatus::Running)
                ++activeCount;
            else if (cr.status == CoroutineStatus::Suspended)
                ++suspendedCount;
        }

        ImGui::Text(ICON_FA_CLOCK " Active: %d | Suspended: %d | Total Started: %d | Completed: %d | Failed: %d",
                    activeCount, suspendedCount, m_totalStarted, m_totalCompleted, m_totalFailed);

        ImGui::Text("Creation Rate: %.1f/s", m_creationsPerSecond);
    }

    void CoroutineDebugPanel::RenderFilterBar()
    {
        const char* filters[] = {"All", "Running", "Suspended"};
        ImGui::SetNextItemWidth(120);
        ImGui::Combo("Status Filter", &m_statusFilter, filters, IM_ARRAYSIZE(filters));

        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_STOP " Cancel All"))
        {
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "CoroutineDebugPanel: cancelling all %d coroutines",
                           static_cast<int>(m_coroutines.size()));
            m_totalCompleted += static_cast<int>(m_coroutines.size());
            m_coroutines.clear();
        }
    }

    void CoroutineDebugPanel::RenderActiveCoroutines()
    {
        if (m_coroutines.empty())
        {
            ImGui::TextDisabled("No active coroutines.");
            ImGui::TextDisabled("Coroutines are started during Play mode via CoroutineScheduler.");
            return;
        }

        ImGui::Text("Coroutines: %d", static_cast<int>(m_coroutines.size()));

        if (ImGui::BeginTable("CoroutineTable", 5,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_ScrollY))
        {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Elapsed", ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("Wait State");
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableHeadersRow();

            for (int i = 0; i < static_cast<int>(m_coroutines.size()); ++i)
            {
                auto& cr = m_coroutines[i];

                // Apply status filter
                if (m_statusFilter == 1 && cr.status != CoroutineStatus::Running)
                    continue;
                if (m_statusFilter == 2 && cr.status != CoroutineStatus::Suspended)
                    continue;

                ImGui::TableNextRow();
                ImGui::PushID(i);

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(cr.name);

                ImGui::TableNextColumn();
                ImGui::Text("%.1f s", cr.elapsedTime);

                ImGui::TableNextColumn();
                ImVec4 statusColor = GetStatusColor(cr.status);
                ImGui::TextColored(statusColor, "%s", GetStatusName(cr.status));

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(cr.waitState);

                ImGui::TableNextColumn();
                if (ImGui::SmallButton(ICON_FA_STOP "##cancel"))
                {
                    m_coroutines.erase(m_coroutines.begin() + i);
                    ++m_totalCompleted;
                    ImGui::PopID();
                    break;
                }

                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    const char* CoroutineDebugPanel::GetStatusName(CoroutineStatus status)
    {
        switch (status)
        {
        case CoroutineStatus::Running:
            return "Running";
        case CoroutineStatus::Suspended:
            return "Suspended";
        case CoroutineStatus::Completed:
            return "Completed";
        case CoroutineStatus::Failed:
            return "Failed";
        default:
            return "Unknown";
        }
    }

    ImVec4 CoroutineDebugPanel::GetStatusColor(CoroutineStatus status)
    {
        switch (status)
        {
        case CoroutineStatus::Running:
            return ImVec4(0.2f, 0.8f, 0.2f, 1.0f);
        case CoroutineStatus::Suspended:
            return ImVec4(1.0f, 0.8f, 0.0f, 1.0f);
        case CoroutineStatus::Completed:
            return ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
        case CoroutineStatus::Failed:
            return ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
        default:
            return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        }
    }

} // namespace SparkEditor
