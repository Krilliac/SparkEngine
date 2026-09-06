/**
 * @file WorkflowPanel.cpp
 * @brief ImGui panel for browsing and running editor workflows
 */

#include "WorkflowPanel.h"
#include "../Core/EditorIcons.h"
#include "../../../SparkEngine/Source/Utils/Validate.h"

#include <imgui.h>

namespace SparkEditor
{

    WorkflowPanel::WorkflowPanel() : EditorPanel("Workflows", "workflow_panel") {}

    bool WorkflowPanel::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Initializing Workflow panel");
        return true;
    }

    void WorkflowPanel::Update(float /*deltaTime*/) {}

    void WorkflowPanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            RenderWorkflowList();

            if (m_status != WorkflowStatus::Idle)
            {
                ImGui::Separator();
                RenderActiveWorkflow();
            }

            if (!m_log.empty())
            {
                ImGui::Separator();
                RenderLogOutput();
            }
        }
        EndPanel();
    }

    void WorkflowPanel::Shutdown() {}

    // ========================================================================
    // Rendering helpers
    // ========================================================================

    void WorkflowPanel::RenderWorkflowList()
    {
        ImGui::Text(ICON_FA_COGS " Available Workflows");
        ImGui::Spacing();

        const auto& reg = WorkflowRegistry::Instance();
        auto categories = reg.GetCategories();

        for (const auto& category : categories)
        {
            std::string headerLabel = ICON_FA_FOLDER " " + category;
            if (ImGui::CollapsingHeader(headerLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::Indent(8.0f);

                auto workflows = reg.GetByCategory(category);
                for (const auto* wf : workflows)
                {
                    ImGui::PushID(wf->GetName().c_str());

                    bool isRunning = (m_status == WorkflowStatus::Running && m_activeWorkflowName == wf->GetName());

                    // Run button
                    if (isRunning)
                        ImGui::BeginDisabled();

                    if (ImGui::Button(ICON_FA_PLAY, ImVec2(28, 24)))
                    {
                        // Destructive workflows (Clean & Rebuild deletes a build tree) must
                        // never run straight off a single click.
                        if (wf->RequiresConfirmation())
                            m_pendingConfirmWorkflow = wf->GetName();
                        else
                            RunWorkflow(*wf);
                    }

                    if (isRunning)
                        ImGui::EndDisabled();

                    ImGui::SameLine();

                    // Name and description
                    ImGui::Text("%s", wf->GetName().c_str());
                    if (ImGui::IsItemHovered() && !wf->GetDescription().empty())
                    {
                        ImGui::SetTooltip("%s\n(%u steps)", wf->GetDescription().c_str(), wf->GetStepCount());
                    }

                    ImGui::PopID();
                }

                ImGui::Unindent(8.0f);
            }
        }

        if (categories.empty())
        {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "No workflows registered");
        }

        RenderConfirmationModal();
    }

    void WorkflowPanel::RenderConfirmationModal()
    {
        static constexpr const char* kModalId = "Confirm workflow##WorkflowConfirm";

        if (m_pendingConfirmWorkflow.empty())
        {
            m_confirmModalOpen = false;
            return;
        }

        if (!m_confirmModalOpen)
        {
            ImGui::OpenPopup(kModalId);
            m_confirmModalOpen = true;
        }

        // ImGui closes a modal on Escape or on a click outside without running the Cancel branch. Pass
        // p_open so that close is observable: without it m_pendingConfirmWorkflow stayed set, the popup
        // was re-opened on the next frame, and the dialog could not be dismissed except with Cancel.
        bool stayOpen = true;
        if (!ImGui::BeginPopupModal(kModalId, &stayOpen, ImGuiWindowFlags_AlwaysAutoResize))
        {
            // BeginPopupModal returns false once the popup is closed; treat that as a cancel.
            m_pendingConfirmWorkflow.clear();
            m_confirmModalOpen = false;
            return;
        }

        if (!stayOpen)
        {
            m_pendingConfirmWorkflow.clear();
            m_confirmModalOpen = false;
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }

        const EditorWorkflow* workflow = WorkflowRegistry::Instance().Find(m_pendingConfirmWorkflow);
        ImGui::TextWrapped("'%s' deletes files that cannot be recovered.", m_pendingConfirmWorkflow.c_str());
        if (workflow && !workflow->GetDescription().empty())
            ImGui::TextWrapped("%s", workflow->GetDescription().c_str());
        ImGui::Spacing();
        ImGui::TextWrapped("The editor is unresponsive while a workflow runs.");
        ImGui::Separator();

        if (ImGui::Button("Run", ImVec2(110, 0)))
        {
            const std::string name = m_pendingConfirmWorkflow;
            m_pendingConfirmWorkflow.clear();
            m_confirmModalOpen = false;
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            if (const EditorWorkflow* confirmed = WorkflowRegistry::Instance().Find(name))
                RunWorkflow(*confirmed);
            return;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(110, 0)))
        {
            m_pendingConfirmWorkflow.clear();
            m_confirmModalOpen = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    void WorkflowPanel::RenderActiveWorkflow()
    {
        ImGui::Text(ICON_FA_SPINNER " %s", m_activeWorkflowName.c_str());

        float progress =
            (m_totalSteps > 0) ? static_cast<float>(m_currentStep) / static_cast<float>(m_totalSteps) : 0.0f;

        // Status color
        ImVec4 statusColor(0.8f, 0.8f, 0.8f, 1.0f);
        const char* statusText = "Idle";
        switch (m_status)
        {
        case WorkflowStatus::Running:
            statusColor = ImVec4(0.3f, 0.7f, 1.0f, 1.0f);
            statusText = "Running";
            break;
        case WorkflowStatus::Completed:
            statusColor = ImVec4(0.3f, 0.8f, 0.3f, 1.0f);
            statusText = "Completed";
            progress = 1.0f;
            break;
        case WorkflowStatus::Failed:
            statusColor = ImVec4(0.9f, 0.3f, 0.3f, 1.0f);
            statusText = "Failed";
            break;
        case WorkflowStatus::Cancelled:
            statusColor = ImVec4(0.9f, 0.7f, 0.2f, 1.0f);
            statusText = "Cancelled";
            break;
        default:
            break;
        }

        ImGui::ProgressBar(progress, ImVec2(-1, 18));
        ImGui::TextColored(statusColor, "%s (%u/%u steps)", statusText, m_currentStep, m_totalSteps);
    }

    void WorkflowPanel::RenderLogOutput()
    {
        if (ImGui::CollapsingHeader(ICON_FA_TERMINAL " Log", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::BeginChild("##WorkflowLog", ImVec2(0, 150), true);
            for (const auto& line : m_log)
            {
                ImVec4 color(0.8f, 0.8f, 0.8f, 1.0f);
                if (line.find("[FAILED]") != std::string::npos)
                    color = ImVec4(0.9f, 0.3f, 0.3f, 1.0f);
                else if (line.find("[Skip]") != std::string::npos)
                    color = ImVec4(0.9f, 0.7f, 0.2f, 1.0f);
                else if (line.find("[Workflow]") != std::string::npos)
                    color = ImVec4(0.5f, 0.8f, 1.0f, 1.0f);

                ImGui::TextColored(color, "%s", line.c_str());
            }
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();
        }
    }

    // ========================================================================
    // Execution
    // ========================================================================

    void WorkflowPanel::RunWorkflow(const EditorWorkflow& workflow)
    {
        m_activeWorkflowName = workflow.GetName();
        m_totalSteps = workflow.GetStepCount();
        m_currentStep = 0;
        m_log.clear();
        m_status = WorkflowStatus::Running;

        WorkflowContext ctx;
        ctx.editorUI = m_editorUI;

        WorkflowResult result = workflow.Execute(ctx);

        m_currentStep = result.stepsCompleted;
        m_status = result.status;
        m_log = std::move(result.log);
    }

} // namespace SparkEditor
