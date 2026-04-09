/**
 * @file UndoRedoManager.h
 * @brief Manages the undo/redo command history stack
 * @author Spark Engine Team
 * @date 2025
 *
 * The UndoRedoManager maintains two stacks (undo and redo) of EditorCommand
 * objects, allowing the user to undo and redo operations in the editor.
 *
 * Required undoable action categories (central checklist):
 * 1) Transform gizmo apply (translate/rotate/scale commits)
 * 2) Component add/remove/edit (including property and asset reference edits)
 * 3) Hierarchy mutations (create/delete/reparent/duplicate/rename)
 * 4) Asset assignment and reassignment (mesh/material/texture/script references)
 * 5) Multi-object/batch edits that mutate scene/editor state
 */

#pragma once

#include "EditorCommand.h"
#include <vector>
#include <memory>
#include <functional>
#include <string>

namespace SparkEditor
{

    /**
     * @brief Manages the undo/redo command stack for the editor
     *
     * Supports executing commands, undoing them, redoing them, and
     * navigating the command history. Notifies listeners when the
     * stack changes for UI updates.
     */
    class UndoRedoManager
    {
      public:
        /**
         * @brief Global undo/redo manager instance for editor command dispatch.
         */
        static UndoRedoManager& GetInstance();

        UndoRedoManager();
        ~UndoRedoManager() = default;

        // Non-copyable
        UndoRedoManager(const UndoRedoManager&) = delete;
        UndoRedoManager& operator=(const UndoRedoManager&) = delete;

        /**
         * @brief Execute a command and push it onto the undo stack
         * @param command The command to execute
         *
         * Executing a new command clears the redo stack.
         */
        void ExecuteCommand(std::unique_ptr<EditorCommand> command);

        /**
         * @brief Undo the most recent command
         * @return true if a command was undone, false if undo stack is empty
         */
        bool Undo();

        /**
         * @brief Redo the most recently undone command
         * @return true if a command was redone, false if redo stack is empty
         */
        bool Redo();

        /**
         * @brief Undo or redo to reach a specific index in the undo stack
         * @param targetIndex The target position in the undo stack (0 = initial state)
         */
        void UndoToIndex(size_t targetIndex);

        /**
         * @brief Check if undo is available
         * @return true if there are commands to undo
         */
        bool CanUndo() const;

        /**
         * @brief Check if redo is available
         * @return true if there are commands to redo
         */
        bool CanRedo() const;

        /**
         * @brief Get the description of the next command to undo
         * @return Description string, or empty if no undo available
         */
        std::string GetUndoDescription() const;

        /**
         * @brief Get the description of the next command to redo
         * @return Description string, or empty if no redo available
         */
        std::string GetRedoDescription() const;

        /**
         * @brief Get the undo stack for display in the history panel
         * @return Const reference to the undo command stack
         */
        const std::vector<std::unique_ptr<EditorCommand>>& GetUndoStack() const;

        /**
         * @brief Get the redo stack for display in the history panel
         * @return Const reference to the redo command stack
         */
        const std::vector<std::unique_ptr<EditorCommand>>& GetRedoStack() const;

        /**
         * @brief Get the current position in the undo stack
         * @return Current index (number of executed commands)
         */
        size_t GetCurrentIndex() const;

        /**
         * @brief Clear both undo and redo stacks
         */
        void Clear();

        /**
         * @brief Mark the current state as the saved state
         */
        void MarkSaved();

        /**
         * @brief Check if the current state differs from the last saved state
         * @return true if there are unsaved changes
         */
        bool HasUnsavedChanges() const;

        /**
         * @brief Set callback for when the stack changes
         * @param callback Function called whenever undo/redo state changes
         */
        void SetOnStackChanged(std::function<void()> callback);

        /**
         * @brief Get the maximum number of commands in the undo stack
         * @return Max stack depth
         */
        size_t GetMaxStackDepth() const { return m_maxStackDepth; }

        /**
         * @brief Set the maximum number of commands in the undo stack
         * @param depth Max stack depth
         */
        void SetMaxStackDepth(size_t depth) { m_maxStackDepth = depth; }

        /**
         * @brief Returns true while a command is being executed via UndoRedoManager.
         */
        static bool IsDispatchingCommand();

        /**
         * @brief Logs a warning when editor state mutation bypasses command dispatch.
         * @param operation Context string for the mutation path.
         */
        static void WarnIfMutationBypassesDispatch(const char* operation);

      private:
        std::vector<std::unique_ptr<EditorCommand>> m_undoStack;
        std::vector<std::unique_ptr<EditorCommand>> m_redoStack;

        size_t m_maxStackDepth = 100;
        size_t m_savedIndex = 0;

        std::function<void()> m_onStackChanged;

        void NotifyStackChanged();
        void TrimUndoStack();
    };

} // namespace SparkEditor
