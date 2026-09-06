/**
 * @file WorkflowPanel.h
 * @brief ImGui panel for browsing and running editor workflows
 */

#pragma once

#include "../Core/EditorPanel.h"
#include "../Workflow/EditorWorkflow.h"

#include <string>
#include <vector>

namespace SparkEditor
{

    class EditorUI; // forward

    /**
     * @brief Editor panel for browsing and executing registered workflows.
     *
     * Lists workflows grouped by category with run buttons, progress display,
     * and a scrolling log output area.
     */
    class WorkflowPanel : public EditorPanel
    {
      public:
        WorkflowPanel();
        ~WorkflowPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

        /// @brief Set the EditorUI pointer for workflow context access.
        void SetEditorUI(EditorUI* ui) { m_editorUI = ui; }

      private:
        void RenderWorkflowList();
        void RenderActiveWorkflow();
        void RenderLogOutput();
        void RenderConfirmationModal();

        void RunWorkflow(const EditorWorkflow& workflow);

        EditorUI* m_editorUI = nullptr;

        // Active workflow state
        WorkflowStatus m_status = WorkflowStatus::Idle;
        std::string m_activeWorkflowName;
        uint32_t m_currentStep = 0;
        uint32_t m_totalSteps = 0;
        std::vector<std::string> m_log;

        /// @brief Name of a destructive workflow awaiting explicit confirmation.
        std::string m_pendingConfirmWorkflow;
        /// Whether the confirmation popup has been opened for m_pendingConfirmWorkflow. Without this
        /// the panel re-opened the popup every frame after ImGui closed it (Escape), making it
        /// impossible to dismiss with anything but the Cancel button.
        bool m_confirmModalOpen = false;
    };

} // namespace SparkEditor
