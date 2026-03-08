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
            command->Execute();

            // Try to merge with the last command
            if (!m_undoStack.empty() && command->GetTypeId() != 0 &&
                m_undoStack.back()->GetTypeId() == command->GetTypeId())
            {
                if (m_undoStack.back()->MergeWith(*command))
                {
                    // Merged — discard the new command
                    m_redoStack.clear();
                    NotifyChange();
                    return;
                }
            }

            m_undoStack.push_back(std::move(command));
            m_redoStack.clear();

            // Enforce max history size
            while (m_undoStack.size() > m_maxHistory)
            {
                m_undoStack.erase(m_undoStack.begin());
            }

            NotifyChange();
        }

        /**
     * @brief Undo the most recent command.
     * @return True if an undo was performed.
     */
        bool Undo()
        {
            if (m_undoStack.empty())
                return false;

            auto cmd = std::move(m_undoStack.back());
            m_undoStack.pop_back();
            cmd->Undo();
            m_redoStack.push_back(std::move(cmd));
            NotifyChange();
            return true;
        }

        /**
     * @brief Redo the most recently undone command.
     * @return True if a redo was performed.
     */
        bool Redo()
        {
            if (m_redoStack.empty())
                return false;

            auto cmd = std::move(m_redoStack.back());
            m_redoStack.pop_back();
            cmd->Execute();
            m_undoStack.push_back(std::move(cmd));
            NotifyChange();
            return true;
        }

        bool CanUndo() const { return !m_undoStack.empty(); }
        bool CanRedo() const { return !m_redoStack.empty(); }

        /** @brief Get the description of the command that would be undone. */
        std::string GetUndoDescription() const
        {
            return m_undoStack.empty() ? "" : m_undoStack.back()->GetDescription();
        }

        /** @brief Get the description of the command that would be redone. */
        std::string GetRedoDescription() const
        {
            return m_redoStack.empty() ? "" : m_redoStack.back()->GetDescription();
        }

        /** @brief Clear all history (e.g., when opening a new scene). */
        void Clear()
        {
            m_undoStack.clear();
            m_redoStack.clear();
            NotifyChange();
        }

        /** @brief Set the maximum number of undo steps to retain. */
        void SetMaxHistory(size_t max) { m_maxHistory = max; }

        /** @brief Number of undo steps available. */
        size_t UndoCount() const { return m_undoStack.size(); }

        /** @brief Number of redo steps available. */
        size_t RedoCount() const { return m_redoStack.size(); }

        /** @brief Register a callback invoked whenever the history changes. */
        void OnChange(std::function<void()> callback) { m_changeCallbacks.push_back(std::move(callback)); }

        /**
     * @brief Mark the current state as the "saved" point.
     *
     * `IsModified()` returns false until further commands are executed.
     */
        void MarkSaved() { m_savedIndex = m_undoStack.size(); }

        /** @brief True if the scene has been modified since the last save. */
        bool IsModified() const { return m_undoStack.size() != m_savedIndex; }

      private:
        CommandHistory() = default;

        void NotifyChange()
        {
            for (auto& cb : m_changeCallbacks)
            {
                if (cb)
                    cb();
            }
        }

        std::vector<std::unique_ptr<ICommand>> m_undoStack;
        std::vector<std::unique_ptr<ICommand>> m_redoStack;
        std::vector<std::function<void()>> m_changeCallbacks;
        size_t m_maxHistory = 100;
        size_t m_savedIndex = 0;
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
