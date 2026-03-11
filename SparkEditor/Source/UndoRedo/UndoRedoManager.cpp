/**
 * @file UndoRedoManager.cpp
 * @brief Implementation of the undo/redo command history manager
 * @author Spark Engine Team
 * @date 2025
 */

#include "UndoRedoManager.h"
#include <algorithm>

namespace SparkEditor
{

    UndoRedoManager::UndoRedoManager() = default;

    void UndoRedoManager::ExecuteCommand(std::unique_ptr<EditorCommand> command)
    {
        if (!command)
        {
            return;
        }

        // Try to merge with the last command (e.g., continuous transform drags)
        if (!m_undoStack.empty() && m_undoStack.back()->MergeWith(command.get()))
        {
            NotifyStackChanged();
            return;
        }

        // Execute the command
        command->Execute();

        // Push onto undo stack
        m_undoStack.push_back(std::move(command));

        // Clear redo stack since we branched history
        m_redoStack.clear();

        // Trim if over capacity
        TrimUndoStack();

        NotifyStackChanged();
    }

    bool UndoRedoManager::Undo()
    {
        if (!CanUndo())
        {
            return false;
        }

        auto command = std::move(m_undoStack.back());
        m_undoStack.pop_back();

        command->Undo();

        m_redoStack.push_back(std::move(command));

        NotifyStackChanged();
        return true;
    }

    bool UndoRedoManager::Redo()
    {
        if (!CanRedo())
        {
            return false;
        }

        auto command = std::move(m_redoStack.back());
        m_redoStack.pop_back();

        command->Execute();

        m_undoStack.push_back(std::move(command));

        NotifyStackChanged();
        return true;
    }

    void UndoRedoManager::UndoToIndex(size_t targetIndex)
    {
        if (targetIndex > m_undoStack.size())
        {
            return;
        }

        while (m_undoStack.size() > targetIndex)
        {
            Undo();
        }

        while (m_undoStack.size() < targetIndex && CanRedo())
        {
            Redo();
        }
    }

    bool UndoRedoManager::CanUndo() const
    {
        return !m_undoStack.empty();
    }

    bool UndoRedoManager::CanRedo() const
    {
        return !m_redoStack.empty();
    }

    std::string UndoRedoManager::GetUndoDescription() const
    {
        if (m_undoStack.empty())
        {
            return "";
        }
        return m_undoStack.back()->GetDescription();
    }

    std::string UndoRedoManager::GetRedoDescription() const
    {
        if (m_redoStack.empty())
        {
            return "";
        }
        return m_redoStack.back()->GetDescription();
    }

    const std::vector<std::unique_ptr<EditorCommand>>& UndoRedoManager::GetUndoStack() const
    {
        return m_undoStack;
    }

    const std::vector<std::unique_ptr<EditorCommand>>& UndoRedoManager::GetRedoStack() const
    {
        return m_redoStack;
    }

    size_t UndoRedoManager::GetCurrentIndex() const
    {
        return m_undoStack.size();
    }

    void UndoRedoManager::Clear()
    {
        m_undoStack.clear();
        m_redoStack.clear();
        m_savedIndex = 0;
        NotifyStackChanged();
    }

    void UndoRedoManager::MarkSaved()
    {
        m_savedIndex = m_undoStack.size();
    }

    bool UndoRedoManager::HasUnsavedChanges() const
    {
        return m_undoStack.size() != m_savedIndex;
    }

    void UndoRedoManager::SetOnStackChanged(std::function<void()> callback)
    {
        m_onStackChanged = std::move(callback);
    }

    void UndoRedoManager::NotifyStackChanged()
    {
        if (m_onStackChanged)
        {
            m_onStackChanged();
        }
    }

    void UndoRedoManager::TrimUndoStack()
    {
        while (m_undoStack.size() > m_maxStackDepth)
        {
            m_undoStack.erase(m_undoStack.begin());
            if (m_savedIndex > 0)
            {
                --m_savedIndex;
            }
        }
    }

} // namespace SparkEditor
