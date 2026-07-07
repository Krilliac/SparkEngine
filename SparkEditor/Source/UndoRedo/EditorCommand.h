/**
 * @file EditorCommand.h
 * @brief Command pattern base class and concrete commands for the undo/redo system
 * @author Spark Engine Team
 * @date 2025
 *
 * Defines the command interface and concrete command implementations used
 * by the UndoRedoManager to support undo/redo operations in the editor.
 */

#pragma once

#include <string>
#include <cstdint>
#include <vector>
#include <memory>
#include <functional>
#include <variant>
#include <unordered_map>
#include <utility>

#ifdef _WIN32
#include <DirectXMath.h>
#else
#include "Core/Platform.h"
#endif
using namespace DirectX;

#include "Core/EngineContext.h"
#include "SceneManager/SceneManager.h"

namespace SparkEditor
{

    /**
     * @brief Base class for all editor commands supporting undo/redo
     *
     * Commands encapsulate an operation that can be executed and undone.
     * Each command stores enough state to reverse the operation.
     */
    class EditorCommand
    {
      public:
        virtual ~EditorCommand() = default;

        /**
         * @brief Execute or re-execute the command
         */
        virtual void Execute() = 0;

        /**
         * @brief Undo the command, restoring previous state
         */
        virtual void Undo() = 0;

        /**
         * @brief Get a human-readable description of this command
         * @return Description string for display in the undo history
         */
        virtual std::string GetDescription() const = 0;

        /**
         * @brief Try to merge this command with a subsequent command
         * @param other The command to merge with
         * @return true if merged successfully (other can be discarded)
         */
        virtual bool MergeWith(const EditorCommand* other) { return false; }
    };

    /**
     * @brief Property value variant type for generic property changes
     */
    using PropertyValue = std::variant<bool, int, float, double, std::string, XMFLOAT3, XMFLOAT4>;

    /**
     * @brief Composite command that groups multiple commands into a single undo step
     */
    class CompositeCommand : public EditorCommand
    {
      public:
        /**
         * @brief Construct a composite command
         * @param description Description of the grouped operation
         */
        explicit CompositeCommand(const std::string& description) : m_description(description) {}

        /**
         * @brief Add a sub-command to this composite
         * @param command Command to add
         */
        void AddCommand(std::unique_ptr<EditorCommand> command) { m_commands.push_back(std::move(command)); }

        void Execute() override
        {
            for (auto& cmd : m_commands)
            {
                cmd->Execute();
            }
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

        /**
         * @brief Get number of sub-commands
         * @return Number of commands in this composite
         */
        size_t GetCommandCount() const { return m_commands.size(); }

      private:
        std::string m_description;
        std::vector<std::unique_ptr<EditorCommand>> m_commands;
    };

    /**
     * @brief Generic command backed by execute/undo lambdas.
     *
     * Used to bridge legacy command callsites onto UndoRedoManager while
     * preserving undo/redo behavior.
     */
    class LambdaEditorCommand : public EditorCommand
    {
      public:
        LambdaEditorCommand(std::function<void()> execute, std::function<void()> undo, std::string description)
            : m_execute(std::move(execute)), m_undo(std::move(undo)), m_description(std::move(description))
        {
        }

        void Execute() override
        {
            if (m_execute)
            {
                m_execute();
            }
        }

        void Undo() override
        {
            if (m_undo)
            {
                m_undo();
            }
        }

        std::string GetDescription() const override { return m_description; }

      private:
        std::function<void()> m_execute;
        std::function<void()> m_undo;
        std::string m_description;
    };

} // namespace SparkEditor
