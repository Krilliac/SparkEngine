/**
 * @file UndoHistoryPanel.cpp
 * @brief Implementation of the undo history panel
 * @author Spark Engine Team
 * @date 2025
 */

#include "UndoHistoryPanel.h"
#include "../Core/EditorIcons.h"
#include "../../../SparkEngine/Source/Utils/Validate.h"
#include <imgui.h>

namespace SparkEditor
{

    UndoHistoryPanel::UndoHistoryPanel(UndoRedoManager* undoRedoManager)
        : EditorPanel("Undo History", "UndoHistory"), m_undoRedoManager(undoRedoManager)
    {
    }

    bool UndoHistoryPanel::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
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
        bool canUndo = m_undoRedoManager && m_undoRedoManager->CanUndo();
        bool canRedo = m_undoRedoManager && m_undoRedoManager->CanRedo();

        ImGui::BeginDisabled(!canUndo);
        if (ImGui::Button(ICON_FA_UNDO " Undo"))
        {
            m_undoRedoManager->Undo();
        }
        ImGui::EndDisabled();

        ImGui::SameLine();

        ImGui::BeginDisabled(!canRedo);
        if (ImGui::Button(ICON_FA_REDO " Redo"))
        {
            m_undoRedoManager->Redo();
        }
        ImGui::EndDisabled();

        ImGui::SameLine();

        if (ImGui::Button(ICON_FA_TRASH " Clear"))
        {
            if (m_undoRedoManager)
            {
                m_undoRedoManager->Clear();
            }
        }

        ImGui::SameLine();
        ImGui::Checkbox("Auto-scroll", &m_autoScroll);
    }

    void UndoHistoryPanel::RenderHistoryList()
    {
        if (!m_undoRedoManager)
        {
            ImGui::TextDisabled("No undo/redo manager available");
            return;
        }

        const auto& undoStack = m_undoRedoManager->GetUndoStack();
        const auto& redoStack = m_undoRedoManager->GetRedoStack();
        size_t currentIndex = m_undoRedoManager->GetCurrentIndex();

        ImGui::BeginChild("HistoryList", ImVec2(0, 0), true);

        // Initial state entry
        bool isAtInitial = (currentIndex == 0);
        if (isAtInitial)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 0.8f, 0.3f, 1.0f));
        }
        if (ImGui::Selectable(ICON_FA_HOME " Initial State", isAtInitial))
        {
            m_undoRedoManager->UndoToIndex(0);
        }
        if (isAtInitial)
        {
            ImGui::PopStyleColor();
        }

        // Undo stack entries (executed commands)
        for (size_t i = 0; i < undoStack.size(); ++i)
        {
            bool isCurrent = (i == currentIndex - 1);

            if (isCurrent)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 0.8f, 0.3f, 1.0f));
            }

            std::string label = ICON_FA_EDIT " " + undoStack[i]->GetDescription();
            if (ImGui::Selectable(label.c_str(), isCurrent))
            {
                m_undoRedoManager->UndoToIndex(i + 1);
            }

            if (isCurrent)
            {
                ImGui::PopStyleColor();
                if (m_autoScroll)
                {
                    ImGui::SetScrollHereY();
                }
            }
        }

        // Redo stack entries (grayed out, shown in reverse since they're future commands)
        if (!redoStack.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 0.7f));
            for (auto it = redoStack.rbegin(); it != redoStack.rend(); ++it)
            {
                std::string label = ICON_FA_REDO " " + (*it)->GetDescription() + " (redo)";
                ImGui::Selectable(label.c_str(), false);
            }
            ImGui::PopStyleColor();
        }

        // Status line
        ImGui::Separator();
        ImGui::TextDisabled("Commands: %zu | Redo available: %zu", undoStack.size(), redoStack.size());

        ImGui::EndChild();
    }

} // namespace SparkEditor
