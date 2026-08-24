/**
 * @file UndoRedoManager.cpp
 * @brief Implementation of the undo/redo command history manager
 * @author Spark Engine Team
 * @date 2025
 */

#include "UndoRedoManager.h"
#include "Utils/LogMacros.h"
#include "Utils/Validate.h"
#include <algorithm>

namespace SparkEditor
{
    namespace
    {
        thread_local int g_dispatchDepth = 0;
    }

    UndoRedoManager& UndoRedoManager::GetInstance()
    {
        static UndoRedoManager instance;
        return instance;
    }

    UndoRedoManager::UndoRedoManager() = default;

    void UndoRedoManager::ExecuteCommand(std::unique_ptr<EditorCommand> command)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        if (!command)
        {
            return;
        }

        // Try to merge with the last command (e.g., continuous transform drags)
        const bool atTransientBarrier =
            m_transientSessionActive && m_undoStack.size() == m_transientUndoCheckpoint;
        if (!atTransientBarrier && !m_undoStack.empty() && m_undoStack.back()->MergeWith(command.get()))
        {
            ++m_editSequence;
            NotifyStackChanged();
            return;
        }

        SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "Executing command: '%s'", command->GetDescription().c_str());
        // Execute the command
        ++g_dispatchDepth;
        command->Execute();
        --g_dispatchDepth;

        // Push onto undo stack
        m_undoStack.push_back(std::move(command));
        ++m_editSequence;

        // Clear redo stack since we branched history
        m_redoStack.clear();

        // Trim if over capacity
        // Trimming from the front would invalidate the play-mode checkpoint.
        if (!m_transientSessionActive)
            TrimUndoStack();

        NotifyStackChanged();
    }

    bool UndoRedoManager::Undo()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        SPARK_WARN_IF(Spark::LogCategory::Editor, m_undoStack.empty(), "Undo called on empty undo stack");
        if (!CanUndo() || (m_transientSessionActive && m_undoStack.size() <= m_transientUndoCheckpoint))
        {
            return false;
        }

        auto command = std::move(m_undoStack.back());
        m_undoStack.pop_back();
        SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "Undoing command: '%s' (stack depth: %zu)",
                        command->GetDescription().c_str(), m_undoStack.size());
        ++g_dispatchDepth;
        command->Undo();
        --g_dispatchDepth;

        m_redoStack.push_back(std::move(command));
        ++m_editSequence;

        NotifyStackChanged();
        return true;
    }

    bool UndoRedoManager::Redo()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        SPARK_WARN_IF(Spark::LogCategory::Editor, m_redoStack.empty(), "Redo called on empty redo stack");
        if (!CanRedo())
        {
            return false;
        }

        auto command = std::move(m_redoStack.back());
        m_redoStack.pop_back();

        ++g_dispatchDepth;
        command->Execute();
        --g_dispatchDepth;

        m_undoStack.push_back(std::move(command));
        ++m_editSequence;

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
        // Note: m_editSequence/m_savedSequence are deliberately untouched —
        // clearing the history does not revert document edits, so the
        // unsaved-changes state must survive a history clear.
        NotifyStackChanged();
    }

    bool UndoRedoManager::BeginTransientSession()
    {
        if (m_transientSessionActive)
            return false;

        m_transientSessionActive = true;
        m_transientUndoCheckpoint = m_undoStack.size();
        m_transientEditSequence = m_editSequence;
        m_transientSavedSequence = m_savedSequence;
        m_transientSavedRedoStack = std::move(m_redoStack);
        NotifyStackChanged();
        return true;
    }

    bool UndoRedoManager::RollbackTransientSession()
    {
        if (!m_transientSessionActive)
            return false;

        if (m_undoStack.size() > m_transientUndoCheckpoint)
            m_undoStack.erase(m_undoStack.begin() + static_cast<std::ptrdiff_t>(m_transientUndoCheckpoint),
                              m_undoStack.end());
        m_redoStack = std::move(m_transientSavedRedoStack);
        m_editSequence = m_transientEditSequence;
        m_savedSequence = m_transientSavedSequence;
        m_transientSessionActive = false;
        TrimUndoStack();
        NotifyStackChanged();
        return true;
    }

    bool UndoRedoManager::CommitTransientSession()
    {
        if (!m_transientSessionActive)
            return false;

        m_transientSavedRedoStack.clear();
        m_transientSessionActive = false;
        TrimUndoStack();
        NotifyStackChanged();
        return true;
    }

    void UndoRedoManager::MarkSaved()
    {
        m_savedSequence = m_editSequence;
    }

    bool UndoRedoManager::HasUnsavedChanges() const
    {
        return m_editSequence != m_savedSequence;
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
        }
    }

    bool UndoRedoManager::IsDispatchingCommand()
    {
        return g_dispatchDepth > 0;
    }

    void UndoRedoManager::WarnIfMutationBypassesDispatch(const char* operation)
    {
        if (!IsDispatchingCommand())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor,
                           "Editor mutation bypassed UndoRedoManager dispatch at '%s'. Wrap this in an EditorCommand.",
                           operation ? operation : "unknown");
        }
    }

} // namespace SparkEditor
