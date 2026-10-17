/**
 * @file UndoHistoryPanel.cpp
 * @brief Implementation of the undo history panel
 * @author Spark Engine Team
 * @date 2025
 */

#include "UndoHistoryPanel.h"
#include "../Core/EditorIcons.h"
#include "../../../SparkEngine/Source/Utils/Validate.h"
#include "SparkUI/Widgets.h"
#include "Utils/LogMacros.h"
#include <imgui.h>
#include <string>

namespace SparkEditor
{

    UndoHistoryPanel::UndoHistoryPanel(UndoRedoManager* undoRedoManager)
        : EditorPanel("Undo History", "UndoHistory"), m_undoRedoManager(undoRedoManager)
    {
    }

    bool UndoHistoryPanel::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Initializing Undo History panel");
        m_isInitialized = true;
        return true;
    }

    void UndoHistoryPanel::Update(float /*deltaTime*/) {}

    void UndoHistoryPanel::Render()
    {
        if (!m_isVisible)
        {
            return;
        }

        if (!BeginPanel())
        {
            EndPanel();
            return;
        }

        RenderToolbar();
        ImGui::Separator();
        RenderHistoryList();

        EndPanel();
    }

    void UndoHistoryPanel::Shutdown() {}

    void UndoHistoryPanel::RenderToolbar()
    {
        if (!Spark::Ui::BeginPanelToolbar("##UndoHistoryToolbar"))
        {
            return;
        }

        bool canUndo = m_undoRedoManager && m_undoRedoManager->CanUndo();
        bool canRedo = m_undoRedoManager && m_undoRedoManager->CanRedo();

        if (Spark::Ui::IconButton("Undo", ICON_FA_UNDO, canUndo ? "Undo" : "Nothing to undo.", false, canUndo))
        {
            m_undoRedoManager->Undo();
        }

        ImGui::SameLine();

        if (Spark::Ui::IconButton("Redo", ICON_FA_REDO, canRedo ? "Redo" : "Nothing to redo.", false, canRedo))
        {
            m_undoRedoManager->Redo();
        }

        Spark::Ui::ToolbarSeparator();

        constexpr const char* kClearPopupId = "Clear undo history?##UndoHistoryPanel";
        const bool hasHistory = canUndo || canRedo;
        if (Spark::Ui::IconButton("Clear", ICON_FA_TRASH,
                                  hasHistory ? "Clear undo history" : "Undo history is already empty.", false,
                                  hasHistory))
        {
            ImGui::OpenPopup(kClearPopupId);
        }
        if (Spark::Ui::ConfirmDestructive(kClearPopupId, "Clear undo history?",
                                          "Every undo and redo step is discarded. This cannot be undone.", "Clear"))
        {
            m_undoRedoManager->Clear();
        }

        ImGui::SameLine();
        ImGui::Checkbox("Auto-scroll", &m_autoScroll);
        Spark::Ui::EndPanelToolbar();
    }

    void UndoHistoryPanel::RenderHistoryList()
    {
        if (!m_undoRedoManager)
        {
            Spark::Ui::EmptyState(ICON_FA_HISTORY, "No undo/redo manager available",
                                  "Undo history appears here once the editor wires in its undo manager.");
            return;
        }

        const auto& undoStack = m_undoRedoManager->GetUndoStack();
        const auto& redoStack = m_undoRedoManager->GetRedoStack();
        size_t currentIndex = m_undoRedoManager->GetCurrentIndex();

        if (undoStack.empty() && redoStack.empty())
        {
            Spark::Ui::EmptyState(ICON_FA_HISTORY, "No edits yet", "Changes you make in the editor are listed here.");
            return;
        }

        ImGui::BeginChild("HistoryList", ImVec2(0, 0), true);

        // The current position is marked by the selection highlight; the success colour only reinforces it.
        bool isAtInitial = (currentIndex == 0);
        {
            const Spark::Ui::ScopedRoleColor current(ImGuiCol_Text, isAtInitial ? Spark::Ui::Role::SuccessText
                                                                                : Spark::Ui::Role::TextPrimary);
            if (ImGui::Selectable(ICON_FA_HOME " Initial State", isAtInitial))
            {
                m_undoRedoManager->UndoToIndex(0);
            }
        }

        // Undo stack entries (executed commands)
        for (size_t i = 0; i < undoStack.size(); ++i)
        {
            bool isCurrent = (i == currentIndex - 1);

            std::string label = ICON_FA_EDIT " " + undoStack[i]->GetDescription();
            {
                const Spark::Ui::ScopedRoleColor current(ImGuiCol_Text, isCurrent ? Spark::Ui::Role::SuccessText
                                                                                  : Spark::Ui::Role::TextPrimary);
                if (ImGui::Selectable(label.c_str(), isCurrent))
                {
                    m_undoRedoManager->UndoToIndex(i + 1);
                }
            }

            if (isCurrent && m_autoScroll)
            {
                ImGui::SetScrollHereY();
            }
        }

        // Redo stack entries (secondary text and a "(redo)" suffix, shown in reverse since they're future commands)
        if (!redoStack.empty())
        {
            const Spark::Ui::ScopedRoleColor future(ImGuiCol_Text, Spark::Ui::Role::TextSecondary);
            for (auto it = redoStack.rbegin(); it != redoStack.rend(); ++it)
            {
                std::string label = ICON_FA_REDO " " + (*it)->GetDescription() + " (redo)";
                ImGui::Selectable(label.c_str(), false);
            }
        }

        // Status line
        ImGui::Separator();
        Spark::Ui::SecondaryText("Commands: %zu | Redo available: %zu", undoStack.size(), redoStack.size());

        ImGui::EndChild();
    }

} // namespace SparkEditor
