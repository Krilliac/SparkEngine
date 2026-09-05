/**
 * @file SaveSystemPanel.h
 * @brief Save system management panel for the Spark Engine Editor
 */

#pragma once

#include "../Core/EditorPanel.h"
#include "Engine/SaveSystem/SaveSystemTypes.h"
#include <cstdint>
#include <string>
#include <vector>

namespace Spark
{
    class SaveSystem;
}

namespace SparkEditor
{

    /**
     * @brief Panel for browsing and managing the save files on disk
     *
     * Slots are enumerated from the save directory and their metadata is read
     * through Spark::SaveSystem; delete and rename act on the real files. Loading
     * a save needs a live game World and is disabled without one.
     */
    class SaveSystemPanel : public EditorPanel
    {
      public:
        /// One `.spark_save` file discovered in the save directory.
        struct SaveSlotInfo
        {
            std::string slotName; ///< File stem, the identifier SaveSystem uses.
            Spark::SaveMetadata metadata;
            uintmax_t fileSizeBytes = 0;
        };

        SaveSystemPanel();
        ~SaveSystemPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

        /// @brief Re-enumerate the save directory; returns the number of save files found.
        size_t RefreshSlots();

        /// @brief Slots discovered by the last RefreshSlots(), sorted by slot name.
        const std::vector<SaveSlotInfo>& GetSlots() const { return m_slots; }

        /// @brief Point the panel and the SaveSystem at @p directory and re-enumerate.
        void SetSaveDirectory(const std::string& directory);

        /// @brief Delete a save file through SaveSystem::DeleteSave(); refreshes the list.
        bool DeleteSlot(const std::string& slotName);

        /// @brief Rename a save file on disk; returns false if the target exists or I/O fails.
        bool RenameSlot(const std::string& slotName, const std::string& newSlotName);

        /// @brief Load @p slotName into the live game World; false when no session is running.
        bool LoadSlot(const std::string& slotName);

        /// @brief Whether a load is possible: at least one enumerated slot plus a live game World.
        bool CanLoad() const;

      private:
        Spark::SaveSystem& System() const;

        std::string SlotFilePath(const std::string& slotName) const;

        void RenderSaveSlots();
        void RenderAutosaveSettings();
        void RenderSlotActions();

        std::vector<SaveSlotInfo> m_slots;
        std::string m_saveDirectory = "Saves";
        /// True once the user pointed the panel at a directory of its own; until then the
        /// panel follows SaveSystem::GetSaveDirectory() instead of overriding it.
        bool m_saveDirectoryOverridden = false;
        int m_selectedSlot = -1;
        /// Mirror of SaveSystem::SetMaxAutoSaves — written through, never guessed.
        int m_autosaveRotation = 3;
        bool m_renaming = false;
        char m_renameBuffer[128] = {};
    };

} // namespace SparkEditor
