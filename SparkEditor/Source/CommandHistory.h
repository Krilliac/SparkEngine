/**
 * @file CommandHistory.h
 * @brief Undo/Redo command pattern for the Spark Editor
 * @author Spark Engine Team
 * @date 2025
 *
 * Implements the Command Pattern with an undo/redo stack, used throughout
 * the editor for reversible operations. Every user action that modifies
 * scene state (moving objects, changing properties, adding/removing entities)
 * should be wrapped in an ICommand and executed through the CommandHistory.
 *
 * ## Architecture
 * - `ICommand` — interface that every undoable action implements.
 * - `CommandHistory` — manages the undo/redo stacks and executes commands.
 * - Concrete commands — `TransformCommand`, `PropertyCommand`, `AddEntityCommand`, etc.
 *
 * ## Usage
 * @code
 *   auto& history = CommandHistory::GetInstance();
 *
 *   // Execute a reversible action
 *   history.Execute(std::make_unique<TransformCommand>(entity, oldPos, newPos));
 *
 *   // Undo/Redo
 *   history.Undo();
 *   history.Redo();
 *
 *   // Check state
 *   if (history.CanUndo()) { ... }
 *   history.GetUndoDescription(); // "Move Entity 'Player'"
 * @endcode
 *
 * ## Keyboard Shortcuts
 * Ctrl+Z = Undo, Ctrl+Y / Ctrl+Shift+Z = Redo
 *
 * @see SparkEditor, InspectorPanel, SceneHierarchy
 */

#pragma once
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include "UndoRedo/UndoRedoManager.h"

namespace Spark::Editor
{

    // ============================================================================
    // ICommand — base interface for all undoable commands
    // ============================================================================

    /**
 * @brief Interface for a reversible editor command.
 *
 * Each command captures the state needed to both execute (Do) and reverse (Undo)
 * an operation. Commands are immutable after creation — they capture a snapshot
 * of the relevant state at creation time.
 */
    class ICommand
    {
      public:
        virtual ~ICommand() = default;

        /** @brief Execute (or re-execute) the command. */
        virtual void Execute() = 0;

        /** @brief Reverse the command, restoring prior state. */
        virtual void Undo() = 0;

        /**
     * @brief Human-readable description for the Edit menu and status bar.
     * @return String like "Move Entity 'Player'" or "Change Color to #FF0000".
     */
        virtual std::string GetDescription() const = 0;

        /**
     * @brief Try to merge this command with a subsequent one.
     *
     * Used for continuous operations like dragging — instead of recording
     * one command per mouse-move frame, merge them into a single undo step.
     *
     * @param other  The next command to potentially merge with.
     * @return True if the merge succeeded (other can be discarded).
     */
        virtual bool MergeWith(const ICommand& /*other*/) { return false; }

        /**
     * @brief Unique ID for this command type (used by MergeWith).
     * @return 0 if merging is not supported.
     */
        virtual int GetTypeId() const { return 0; }
    };

    // ============================================================================
    // CommandHistory — undo/redo stack manager
    // ============================================================================

    /**
 * @brief Manages the undo/redo stacks and executes commands.
 */
    class CommandHistory
    {
      public:
        static CommandHistory& GetInstance()
        {
            static CommandHistory instance;
            return instance;
        }

        /**
     * @brief Execute a command and push it onto the undo stack.
     *
     * Clears the redo stack (branching from an undo point discards the old future).
     * Attempts to merge with the previous command if both have the same type ID.
     */
        void Execute(std::unique_ptr<ICommand> command)
        {
            auto wrapped = std::make_unique<CommandHistoryAdapterCommand>(std::move(command));
            m_manager.ExecuteCommand(std::move(wrapped));
            NotifyChange();
        }

        /**
     * @brief Undo the most recent command.
     * @return True if an undo was performed.
     */
        bool Undo()
        {
            const bool changed = m_manager.Undo();
            if (changed)
            {
                NotifyChange();
            }
            return changed;
        }

        /**
     * @brief Redo the most recently undone command.
     * @return True if a redo was performed.
     */
        bool Redo()
        {
            const bool changed = m_manager.Redo();
            if (changed)
            {
                NotifyChange();
            }
            return changed;
        }

        bool CanUndo() const { return m_manager.CanUndo(); }
        bool CanRedo() const { return m_manager.CanRedo(); }

        /** @brief Get the description of the command that would be undone. */
        std::string GetUndoDescription() const { return m_manager.GetUndoDescription(); }

        /** @brief Get the description of the command that would be redone. */
        std::string GetRedoDescription() const { return m_manager.GetRedoDescription(); }

        /** @brief Clear all history (e.g., when opening a new scene). */
        void Clear()
        {
            m_manager.Clear();
            NotifyChange();
        }

        bool BeginTransientSession()
        {
            const bool changed = m_manager.BeginTransientSession();
            if (changed)
                NotifyChange();
            return changed;
        }

        bool RollbackTransientSession()
        {
            const bool changed = m_manager.RollbackTransientSession();
            if (changed)
                NotifyChange();
            return changed;
        }

        bool CommitTransientSession()
        {
            const bool changed = m_manager.CommitTransientSession();
            if (changed)
                NotifyChange();
            return changed;
        }

        bool IsTransientSessionActive() const { return m_manager.IsTransientSessionActive(); }

        /** @brief Set the maximum number of undo steps to retain. */
        void SetMaxHistory(size_t max) { m_manager.SetMaxStackDepth(max); }

        /** @brief Number of undo steps available. */
        size_t UndoCount() const { return m_manager.GetUndoStack().size(); }

        /** @brief Number of redo steps available. */
        size_t RedoCount() const { return m_manager.GetRedoStack().size(); }

        /** @brief Register a callback invoked whenever the history changes. */
        void OnChange(std::function<void()> callback) { m_changeCallbacks.push_back(std::move(callback)); }

        /**
     * @brief Mark the current state as the "saved" point.
     *
     * `IsModified()` returns false until further commands are executed.
     */
        void MarkSaved() { m_manager.MarkSaved(); }

        /** @brief True if the scene has been modified since the last save. */
        bool IsModified() const { return m_manager.HasUnsavedChanges(); }

        /**
         * @brief Runtime guard for mutation paths that bypass command dispatch.
         */
        static void WarnIfBypassingDispatch(const char* operation)
        {
            SparkEditor::UndoRedoManager::WarnIfMutationBypassesDispatch(operation);
        }

      private:
        class CommandHistoryAdapterCommand final : public SparkEditor::EditorCommand
        {
          public:
            explicit CommandHistoryAdapterCommand(std::unique_ptr<ICommand> command) : m_command(std::move(command)) {}

            void Execute() override
            {
                if (m_command)
                {
                    m_command->Execute();
                }
            }

            void Undo() override
            {
                if (m_command)
                {
                    m_command->Undo();
                }
            }

            std::string GetDescription() const override
            {
                return m_command ? m_command->GetDescription() : "CommandHistoryAdapter";
            }

            bool MergeWith(const SparkEditor::EditorCommand* other) override
            {
                const auto* rhs = dynamic_cast<const CommandHistoryAdapterCommand*>(other);
                if (!rhs || !m_command || !rhs->m_command)
                {
                    return false;
                }
                if (m_command->GetTypeId() == 0 || m_command->GetTypeId() != rhs->m_command->GetTypeId())
                {
                    return false;
                }
                return m_command->MergeWith(*rhs->m_command);
            }

          private:
            std::unique_ptr<ICommand> m_command;
        };

        CommandHistory() : m_manager(SparkEditor::UndoRedoManager::GetInstance()) {}

        void NotifyChange()
        {
            for (auto& cb : m_changeCallbacks)
            {
                if (cb)
                    cb();
            }
        }

        SparkEditor::UndoRedoManager& m_manager;
        std::vector<std::function<void()>> m_changeCallbacks;
    };

    // ============================================================================
    // Common Concrete Commands
    // ============================================================================

    /**
 * @brief Command that modifies a single typed property value.
 *
 * @tparam T  The property type (float, int, bool, string, XMFLOAT3, etc.)
 */
    template <typename T> class PropertyCommand : public ICommand
    {
      public:
        /**
     * @param target       Pointer to the property being modified.
     * @param newValue     The new value to set.
     * @param description  Human-readable description of the change.
     * @param typeId       Optional type ID for merge support (non-zero enables merging).
     */
        PropertyCommand(T* target, const T& newValue, const std::string& description, int typeId = 0)
            : m_target(target), m_oldValue(*target), m_newValue(newValue), m_description(description), m_typeId(typeId)
        {
        }

        void Execute() override
        {
            if (m_target)
                *m_target = m_newValue;
        }
        void Undo() override
        {
            if (m_target)
                *m_target = m_oldValue;
        }
        std::string GetDescription() const override { return m_description; }
        int GetTypeId() const override { return m_typeId; }

        bool MergeWith(const ICommand& other) override
        {
            if (m_typeId == 0)
                return false;
            auto* otherProp = dynamic_cast<const PropertyCommand<T>*>(&other);
            if (!otherProp || otherProp->m_target != m_target)
                return false;
            m_newValue = otherProp->m_newValue;
            return true;
        }

      private:
        T* m_target;
        T m_oldValue;
        T m_newValue;
        std::string m_description;
        int m_typeId;
    };

    /**
 * @brief Command that groups multiple sub-commands into a single undo step.
 *
 * Use for compound operations like "Duplicate Entity" which involves
 * creating an entity + copying all components.
 */
    class CompoundCommand : public ICommand
    {
      public:
        explicit CompoundCommand(const std::string& description) : m_description(description) {}

        void Add(std::unique_ptr<ICommand> cmd) { m_commands.push_back(std::move(cmd)); }

        void Execute() override
        {
            for (auto& cmd : m_commands)
                cmd->Execute();
        }

        void Undo() override
        {
            // Undo in reverse order
            for (auto it = m_commands.rbegin(); it != m_commands.rend(); ++it)
            {
                (*it)->Undo();
            }
        }

        std::string GetDescription() const override { return m_description; }

      private:
        std::string m_description;
        std::vector<std::unique_ptr<ICommand>> m_commands;
    };

    /**
 * @brief Command backed by generic lambdas for quick one-off operations.
 */
    class LambdaCommand : public ICommand
    {
      public:
        LambdaCommand(std::function<void()> doAction, std::function<void()> undoAction, const std::string& description)
            : m_do(std::move(doAction)), m_undo(std::move(undoAction)), m_description(description)
        {
        }

        void Execute() override
        {
            if (m_do)
                m_do();
        }
        void Undo() override
        {
            if (m_undo)
                m_undo();
        }
        std::string GetDescription() const override { return m_description; }

      private:
        std::function<void()> m_do;
        std::function<void()> m_undo;
        std::string m_description;
    };

} // namespace Spark::Editor
