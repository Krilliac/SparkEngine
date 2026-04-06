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

        /**
         * @brief Get the timestamp when this command was created
         * @return Creation time in milliseconds since epoch
         */
        uint64_t GetTimestamp() const { return m_timestamp; }

      protected:
        uint64_t m_timestamp = 0;
    };

    /**
     * @brief Property value variant type for generic property changes
     */
    using PropertyValue = std::variant<bool, int, float, double, std::string, XMFLOAT3, XMFLOAT4>;

    /**
     * @brief Command for changing a transform component
     */
    class TransformChangeCommand : public EditorCommand
    {
      public:
        /**
         * @brief Construct a transform change command
         * @param entityId The entity whose transform is changing
         * @param oldPosition Old position value
         * @param oldRotation Old rotation value
         * @param oldScale Old scale value
         * @param newPosition New position value
         * @param newRotation New rotation value
         * @param newScale New scale value
         */
        TransformChangeCommand(uint64_t entityId, const XMFLOAT3& oldPosition, const XMFLOAT4& oldRotation,
                               const XMFLOAT3& oldScale, const XMFLOAT3& newPosition, const XMFLOAT4& newRotation,
                               const XMFLOAT3& newScale)
            : m_entityId(entityId), m_oldPosition(oldPosition), m_oldRotation(oldRotation), m_oldScale(oldScale),
              m_newPosition(newPosition), m_newRotation(newRotation), m_newScale(newScale)
        {
        }

        void Execute() override
        {
            auto* scene = EngineContext::Get() ? EngineContext::Get()->GetSceneManager() : nullptr;
            if (!scene)
                return;
            auto* node = scene->GetNode(static_cast<int>(m_entityId));
            if (!node)
                return;
            node->position = m_newPosition;
            node->scale = m_newScale;
        }

        void Undo() override
        {
            auto* scene = EngineContext::Get() ? EngineContext::Get()->GetSceneManager() : nullptr;
            if (!scene)
                return;
            auto* node = scene->GetNode(static_cast<int>(m_entityId));
            if (!node)
                return;
            node->position = m_oldPosition;
            node->scale = m_oldScale;
        }

        std::string GetDescription() const override
        {
            return "Transform Change (Entity " + std::to_string(m_entityId) + ")";
        }

        bool MergeWith(const EditorCommand* other) override
        {
            const auto* otherTransform = dynamic_cast<const TransformChangeCommand*>(other);
            if (otherTransform && otherTransform->m_entityId == m_entityId)
            {
                m_newPosition = otherTransform->m_newPosition;
                m_newRotation = otherTransform->m_newRotation;
                m_newScale = otherTransform->m_newScale;
                return true;
            }
            return false;
        }

        uint64_t GetEntityId() const { return m_entityId; }

      private:
        uint64_t m_entityId;
        XMFLOAT3 m_oldPosition;
        XMFLOAT4 m_oldRotation;
        XMFLOAT3 m_oldScale;
        XMFLOAT3 m_newPosition;
        XMFLOAT4 m_newRotation;
        XMFLOAT3 m_newScale;
    };

    /**
     * @brief Command for changing a generic property value
     */
    class PropertyChangeCommand : public EditorCommand
    {
      public:
        /**
         * @brief Construct a property change command
         * @param entityId Entity owning the component
         * @param componentType Name of the component type
         * @param propertyName Name of the property being changed
         * @param oldValue Previous value
         * @param newValue New value
         */
        PropertyChangeCommand(uint64_t entityId, const std::string& componentType, const std::string& propertyName,
                              const PropertyValue& oldValue, const PropertyValue& newValue)
            : m_entityId(entityId), m_componentType(componentType), m_propertyName(propertyName), m_oldValue(oldValue),
              m_newValue(newValue)
        {
        }

        void Execute() override
        {
            auto* scene = EngineContext::Get() ? EngineContext::Get()->GetSceneManager() : nullptr;
            if (!scene)
                return;
            auto* node = scene->GetNode(static_cast<int>(m_entityId));
            if (!node)
                return;
            node->properties[m_componentType + "." + m_propertyName] = std::visit(
                [](const auto& v) -> std::string
                {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, std::string>)
                        return v;
                    else if constexpr (std::is_same_v<T, bool>)
                        return v ? "true" : "false";
                    else if constexpr (std::is_arithmetic_v<T>)
                        return std::to_string(v);
                    else
                        return {};
                },
                m_newValue);
        }

        void Undo() override
        {
            auto* scene = EngineContext::Get() ? EngineContext::Get()->GetSceneManager() : nullptr;
            if (!scene)
                return;
            auto* node = scene->GetNode(static_cast<int>(m_entityId));
            if (!node)
                return;
            node->properties[m_componentType + "." + m_propertyName] = std::visit(
                [](const auto& v) -> std::string
                {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, std::string>)
                        return v;
                    else if constexpr (std::is_same_v<T, bool>)
                        return v ? "true" : "false";
                    else if constexpr (std::is_arithmetic_v<T>)
                        return std::to_string(v);
                    else
                        return {};
                },
                m_oldValue);
        }

        std::string GetDescription() const override
        {
            return "Change " + m_componentType + "." + m_propertyName + " (Entity " + std::to_string(m_entityId) + ")";
        }

      private:
        uint64_t m_entityId;
        std::string m_componentType;
        std::string m_propertyName;
        PropertyValue m_oldValue;
        PropertyValue m_newValue;
    };

    /**
     * @brief Command for creating an entity
     */
    class CreateEntityCommand : public EditorCommand
    {
      public:
        /**
         * @brief Construct a create entity command
         * @param entityName Name for the new entity
         */
        explicit CreateEntityCommand(const std::string& entityName) : m_entityName(entityName) {}

        void Execute() override
        {
            auto* scene = EngineContext::Get() ? EngineContext::Get()->GetSceneManager() : nullptr;
            if (!scene)
                return;
            ::SceneNode node;
            node.name = m_entityName;
            m_createdEntityId = static_cast<uint64_t>(scene->AddNode(node));
        }

        void Undo() override
        {
            auto* scene = EngineContext::Get() ? EngineContext::Get()->GetSceneManager() : nullptr;
            if (!scene || m_createdEntityId == 0)
                return;
            scene->RemoveNode(static_cast<int>(m_createdEntityId));
        }

        std::string GetDescription() const override { return "Create Entity '" + m_entityName + "'"; }

        uint64_t GetCreatedEntityId() const { return m_createdEntityId; }

      private:
        std::string m_entityName;
        uint64_t m_createdEntityId = 0;
    };

    /**
     * @brief Command for deleting an entity
     */
    class DeleteEntityCommand : public EditorCommand
    {
      public:
        /**
         * @brief Construct a delete entity command
         * @param entityId Entity to delete
         * @param entityName Name of the entity (for description)
         */
        DeleteEntityCommand(uint64_t entityId, const std::string& entityName)
            : m_entityId(entityId), m_entityName(entityName)
        {
        }

        void Execute() override
        {
            auto* scene = EngineContext::Get() ? EngineContext::Get()->GetSceneManager() : nullptr;
            if (!scene)
                return;
            const auto* node = scene->GetNode(static_cast<int>(m_entityId));
            if (node)
            {
                m_savedNode = *node;
                scene->RemoveNode(static_cast<int>(m_entityId));
            }
        }

        void Undo() override
        {
            auto* scene = EngineContext::Get() ? EngineContext::Get()->GetSceneManager() : nullptr;
            if (!scene)
                return;
            scene->AddNode(m_savedNode);
        }

        std::string GetDescription() const override { return "Delete Entity '" + m_entityName + "'"; }

      private:
        uint64_t m_entityId;
        std::string m_entityName;
        ::SceneNode m_savedNode;
        // Snapshot of the entity's components for restoration
        std::unordered_map<std::string, std::unordered_map<std::string, PropertyValue>> m_componentSnapshot;
    };

    /**
     * @brief Command for adding a component to an entity
     */
    class AddComponentCommand : public EditorCommand
    {
      public:
        /**
         * @brief Construct an add component command
         * @param entityId Entity to add the component to
         * @param componentType Type name of the component
         */
        AddComponentCommand(uint64_t entityId, const std::string& componentType)
            : m_entityId(entityId), m_componentType(componentType)
        {
        }

        void Execute() override
        {
            auto* scene = EngineContext::Get() ? EngineContext::Get()->GetSceneManager() : nullptr;
            if (!scene)
                return;
            auto* node = scene->GetNode(static_cast<int>(m_entityId));
            if (node)
                node->properties["component:" + m_componentType] = "1";
        }

        void Undo() override
        {
            auto* scene = EngineContext::Get() ? EngineContext::Get()->GetSceneManager() : nullptr;
            if (!scene)
                return;
            auto* node = scene->GetNode(static_cast<int>(m_entityId));
            if (node)
                node->properties.erase("component:" + m_componentType);
        }

        std::string GetDescription() const override
        {
            return "Add " + m_componentType + " (Entity " + std::to_string(m_entityId) + ")";
        }

      private:
        uint64_t m_entityId;
        std::string m_componentType;
    };

    /**
     * @brief Command for removing a component from an entity
     */
    class RemoveComponentCommand : public EditorCommand
    {
      public:
        /**
         * @brief Construct a remove component command
         * @param entityId Entity to remove the component from
         * @param componentType Type name of the component
         */
        RemoveComponentCommand(uint64_t entityId, const std::string& componentType)
            : m_entityId(entityId), m_componentType(componentType)
        {
        }

        void Execute() override
        {
            auto* scene = EngineContext::Get() ? EngineContext::Get()->GetSceneManager() : nullptr;
            if (!scene)
                return;
            auto* node = scene->GetNode(static_cast<int>(m_entityId));
            if (node)
            {
                m_hadComponent = node->properties.count("component:" + m_componentType) > 0;
                node->properties.erase("component:" + m_componentType);
            }
        }

        void Undo() override
        {
            auto* scene = EngineContext::Get() ? EngineContext::Get()->GetSceneManager() : nullptr;
            if (!scene)
                return;
            auto* node = scene->GetNode(static_cast<int>(m_entityId));
            if (node && m_hadComponent)
                node->properties["component:" + m_componentType] = "1";
        }

        std::string GetDescription() const override
        {
            return "Remove " + m_componentType + " (Entity " + std::to_string(m_entityId) + ")";
        }

      private:
        uint64_t m_entityId;
        std::string m_componentType;
        bool m_hadComponent = false;
        std::unordered_map<std::string, PropertyValue> m_componentSnapshot;
    };

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

} // namespace SparkEditor
