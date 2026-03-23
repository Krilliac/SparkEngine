/**
 * @file SaveSystemPanel.h
 * @brief Save system management panel for the Spark Engine Editor
 */

#pragma once

#include "../Core/EditorPanel.h"
#include <string>
#include <vector>

namespace SparkEditor
{

    /**
     * @brief Panel for browsing and managing save slots
     *
     * Shows save metadata (name, scene, play time, file size, timestamp),
     * allows creating/deleting/renaming saves, configuring autosave,
     * and exporting/importing save files.
     */
    class SaveSystemPanel : public EditorPanel
    {
      public:
        SaveSystemPanel();
        ~SaveSystemPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

      private:
        struct SaveSlotInfo
        {
            char name[128] = {};
            char sceneName[128] = {};
            float playTime = 0.0f;
            float fileSizeKB = 0.0f;
            char lastModified[32] = {};
            int slot = 0;
            bool occupied = false;
        };

        void RenderSaveSlots();
        void RenderAutosaveSettings();
        void RenderSlotActions();

        std::vector<SaveSlotInfo> m_slots;
        int m_selectedSlot = -1;
        int m_maxSlots = 10;
        bool m_autosaveEnabled = true;
        bool m_quickSaveEnabled = true;
        float m_autosaveInterval = 300.0f; // seconds
        int m_autosaveRotation = 3;
        bool m_renaming = false;
        char m_renameBuffer[128] = {};
    };

} // namespace SparkEditor
