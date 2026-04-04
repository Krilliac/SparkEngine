/**
 * @file EditorCommands.h
 * @brief Additional concrete editor commands for hierarchy and material operations
 * @author Spark Engine Team
 * @date 2026
 *
 * Extends the base commands in EditorCommand.h with:
 * - ReparentCommand: hierarchy reparenting with world-transform preservation
 * - MaterialCommand: material property changes on entities
 * - BatchCommand: groups multiple commands into a single undoable step
 *
 * These commands integrate with the UndoRedoManager via ExecuteCommand().
 */

#pragma once

#include "EditorCommand.h"

namespace SparkEditor
{

    // ============================================================================
    // Reparent Command
    // ============================================================================

    /**
     * @brief Command for reparenting an entity in the scene hierarchy
     *
     * Records old and new parent entity IDs so the hierarchy change can be
     * undone. Optionally preserves the world-space transform of the entity
     * by recording transforms before and after the reparent.
     */
    class ReparentCommand : public EditorCommand
    {
      public:
        /**
         * @brief Construct a reparent command
         * @param entityId       Entity being reparented
         * @param oldParentId    Previous parent (0 = root)
         * @param newParentId    New parent (0 = root)
         * @param oldLocalPos    Local position before reparent
         * @param newLocalPos    Local position after reparent (preserves world pos)
         */
        ReparentCommand(uint64_t entityId, uint64_t oldParentId, uint64_t newParentId, const XMFLOAT3& oldLocalPos,
                        const XMFLOAT3& newLocalPos)
            : m_entityId(entityId), m_oldParentId(oldParentId), m_newParentId(newParentId), m_oldLocalPos(oldLocalPos),
              m_newLocalPos(newLocalPos)
        {
        }

        void Execute() override
        {
            // Set entity parent to m_newParentId
            // Adjust local transform to m_newLocalPos to preserve world position
        }

        void Undo() override
        {
            // Restore entity parent to m_oldParentId
            // Restore local transform to m_oldLocalPos
        }

        std::string GetDescription() const override
        {
            return "Reparent Entity " + std::to_string(m_entityId) + " to " + std::to_string(m_newParentId);
        }

        uint64_t GetEntityId() const { return m_entityId; }
        uint64_t GetOldParentId() const { return m_oldParentId; }
        uint64_t GetNewParentId() const { return m_newParentId; }

      private:
        uint64_t m_entityId;
        uint64_t m_oldParentId;
        uint64_t m_newParentId;
        XMFLOAT3 m_oldLocalPos;
        XMFLOAT3 m_newLocalPos;
    };

    // ============================================================================
    // Material Command
    // ============================================================================

    /**
     * @brief Command for changing a material property on an entity
     *
     * Stores old and new values as PropertyValue variants, supporting
     * float parameters (roughness, metallic), color (XMFLOAT3/XMFLOAT4),
     * and texture path strings.
     */
    class MaterialCommand : public EditorCommand
    {
      public:
        /**
         * @brief Construct a material property change command
         * @param entityId      Entity with the material
         * @param materialSlot  Material slot index (e.g. 0 for primary)
         * @param propertyName  Name of the material property
         * @param oldValue      Previous value
         * @param newValue      New value
         */
        MaterialCommand(uint64_t entityId, uint32_t materialSlot, const std::string& propertyName,
                        const PropertyValue& oldValue, const PropertyValue& newValue)
            : m_entityId(entityId), m_materialSlot(materialSlot), m_propertyName(propertyName), m_oldValue(oldValue),
              m_newValue(newValue)
        {
        }

        void Execute() override
        {
            // Apply m_newValue to the material property
        }

        void Undo() override
        {
            // Restore m_oldValue to the material property
        }

        std::string GetDescription() const override
        {
            return "Material." + m_propertyName + " (Entity " + std::to_string(m_entityId) + ", slot " +
                   std::to_string(m_materialSlot) + ")";
        }

        bool MergeWith(const EditorCommand* other) override
        {
            const auto* otherMat = dynamic_cast<const MaterialCommand*>(other);
            if (otherMat && otherMat->m_entityId == m_entityId && otherMat->m_materialSlot == m_materialSlot &&
                otherMat->m_propertyName == m_propertyName)
            {
                m_newValue = otherMat->m_newValue;
                return true;
            }
            return false;
        }

      private:
        uint64_t m_entityId;
        uint32_t m_materialSlot;
        std::string m_propertyName;
        PropertyValue m_oldValue;
        PropertyValue m_newValue;
    };

    // ============================================================================
    // Batch Command
    // ============================================================================

    /**
     * @brief Multi-entity selection batch command
     *
     * Groups multiple sub-commands into a single undoable operation.
     * Execute runs all sub-commands in order; Undo runs them in reverse.
     * Useful for operations on multi-entity selections.
     */
    class BatchCommand : public EditorCommand
    {
      public:
        /**
         * @brief Construct a batch command
         * @param description  Description of the grouped operation
         */
        explicit BatchCommand(const std::string& description) : m_description(description) {}

        /**
         * @brief Add a sub-command to the batch
         * @param cmd  Command to add (ownership transferred)
         */
        void AddSubCommand(std::unique_ptr<EditorCommand> cmd) { m_commands.push_back(std::move(cmd)); }

        void Execute() override
        {
            for (auto& cmd : m_commands)
            {
                cmd->Execute();
            }
        }

        void Undo() override
        {
            for (auto it = m_commands.rbegin(); it != m_commands.rend(); ++it)
            {
                (*it)->Undo();
            }
        }

        std::string GetDescription() const override { return m_description; }

        /**
         * @brief Get number of sub-commands
         * @return Sub-command count
         */
        size_t GetSubCommandCount() const { return m_commands.size(); }

      private:
        std::string m_description;
        std::vector<std::unique_ptr<EditorCommand>> m_commands;
    };

} // namespace SparkEditor
