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
     * Shows save metadata (name, scene, play time, timestamp),
     * allows creating/deleting saves, and configuring autosave.
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
            int slot = 0;
            bool occupied = false;
        };

        void RenderSaveSlots();
        void RenderAutosaveSettings();

        std::vector<SaveSlotInfo> m_slots;
        int m_selectedSlot = -1;
        int m_maxSlots = 10;
        bool m_autosaveEnabled = true;
        float m_autosaveInterval = 300.0f; // seconds
        int m_autosaveRotation = 3;
    };

} // namespace SparkEditor
