/**
 * @file CoroutineDebugPanel.cpp
 * @brief Implementation of the coroutine debug panel
 */

#include "CoroutineDebugPanel.h"
#include "../Core/EditorIcons.h"
#include "SparkUI/Runtime.h"
#include "SparkUI/Widgets.h"
#include "Utils/LogMacros.h"
#include <imgui.h>

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

        Spark::Ui::KeyValue(ICON_FA_CLOCK " Active", "%d", activeCount);
        ImGui::SameLine(0.0f, Spark::Ui::Em(1.0f));
        Spark::Ui::KeyValue("Suspended", "%d", suspendedCount);
        ImGui::SameLine(0.0f, Spark::Ui::Em(1.0f));
        Spark::Ui::KeyValue("Total Started", "%d", m_totalStarted);
        ImGui::SameLine(0.0f, Spark::Ui::Em(1.0f));
        Spark::Ui::KeyValue("Completed", "%d", m_totalCompleted);
        ImGui::SameLine(0.0f, Spark::Ui::Em(1.0f));
        Spark::Ui::KeyValue("Failed", "%d", m_totalFailed);

        Spark::Ui::KeyValue("Creation Rate", "%.1f/s", m_creationsPerSecond);
    }

    void CoroutineDebugPanel::RenderFilterBar()
    {
        const char* filters[] = {"All", "Running", "Suspended"};
        Spark::Ui::SegmentedControl("StatusFilter", filters, IM_ARRAYSIZE(filters), &m_statusFilter);

        ImGui::SameLine();
        if (Spark::Ui::DangerButton(ICON_FA_STOP " Cancel All"))
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
            Spark::Ui::EmptyState(ICON_FA_CLOCK, "No active coroutines",
                                  "State preview does not run game coroutines. "
                                  "Use Play Control > Launch Game to test runtime coroutines.");
            return;
        }

        Spark::Ui::KeyValue("Coroutines", "%d", static_cast<int>(m_coroutines.size()));

        if (Spark::Ui::BeginDataTable("CoroutineTable", 5))
        {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Elapsed", ImGuiTableColumnFlags_WidthFixed, Spark::Ui::Px(70));
            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, Spark::Ui::Px(100));
            ImGui::TableSetupColumn("Wait State");
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, Spark::Ui::Px(60));
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
                Spark::Ui::TableNumber("%.1f s", cr.elapsedTime);

                ImGui::TableNextColumn();
                Spark::Ui::StatusText(ToUiStatus(cr.status), "%s", GetStatusName(cr.status));

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(cr.waitState);

                ImGui::TableNextColumn();
                if (Spark::Ui::IconButton("cancel", ICON_FA_STOP, "Cancel this coroutine"))
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

    Spark::Ui::Status CoroutineDebugPanel::ToUiStatus(CoroutineStatus status)
    {
        switch (status)
        {
        case CoroutineStatus::Running:
            return Spark::Ui::Status::Success;
        case CoroutineStatus::Suspended:
            return Spark::Ui::Status::Warning;
        case CoroutineStatus::Failed:
            return Spark::Ui::Status::Error;
        case CoroutineStatus::Completed:
        default:
            return Spark::Ui::Status::Neutral;
        }
    }

} // namespace SparkEditor
