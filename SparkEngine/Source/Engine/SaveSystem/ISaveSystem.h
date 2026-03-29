/**
 * @file ISaveSystem.h
 * @brief Abstract interface for the save/load system
 * @author Spark Engine Team
 * @date 2026
 *
 * Defines the interface for game state persistence. This abstraction
 * decouples engine code from the concrete SaveSystem implementation,
 * enabling access through EngineContext and testability with mock saves.
 */

#pragma once

#include "SaveSystemTypes.h"

#include <string>
#include <vector>

class World;

namespace Spark
{

    /**
     * @brief Abstract interface for save/load operations.
     *
     * All save/load operations go through this interface. The concrete
     * SaveSystem class implements JSON serialization with compression.
     */
    class ISaveSystem
    {
      public:
        virtual ~ISaveSystem() = default;

        // ================================================================
        // Lifecycle
        // ================================================================

        /// @brief Initialize the save system and prepare the save directory.
        /// @param saveDirectory Path to the save directory.
        /// @return true on success.
        virtual bool Initialize(const std::string& saveDirectory = "Saves") = 0;

        // ================================================================
        // Save / Load
        // ================================================================

        /// @brief Serialize and write the current world state to the specified slot.
        virtual bool Save(const std::string& slotName, World& world, const SaveMetadata& metadata) = 0;

        /// @brief Load a previously saved game state from the specified slot.
        virtual bool Load(const std::string& slotName, World& world) = 0;

        /// @brief Quick save (single dedicated slot).
        virtual bool QuickSave(World& world, const SaveMetadata& metadata) = 0;

        /// @brief Quick load from the dedicated quicksave slot.
        virtual bool QuickLoad(World& world) = 0;

        /// @brief Save to a rotating autosave slot.
        virtual bool AutoSave(World& world, const SaveMetadata& metadata) = 0;

        // ================================================================
        // Slot Management
        // ================================================================

        /// @brief Delete the save file for the specified slot.
        virtual bool DeleteSave(const std::string& slotName) = 0;

        /// @brief Check whether a save exists for the given slot.
        virtual bool SaveExists(const std::string& slotName) const = 0;

        /// @brief Return metadata for all save slots found in the save directory.
        virtual std::vector<SaveMetadata> GetSaveSlots() const = 0;
    };

} // namespace Spark
