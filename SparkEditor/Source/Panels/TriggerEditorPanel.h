/**
 * @file TriggerEditorPanel.h
 * @brief Proximity trigger volume editor panel for the Spark Engine Editor
 */

#pragma once

#include "../Core/EditorPanel.h"
#include <cstdint>
#include <string>
#include <vector>

namespace SparkEditor
{

    /**
     * @brief Panel for managing ProximityTriggerSystem trigger volumes
     *
     * Provides a list of all trigger volumes with their shape, position,
     * size, enabled state, and occupant count. Supports creating new
     * sphere/AABB triggers and toggling individual triggers.
     */
    class TriggerEditorPanel : public EditorPanel
    {
      public:
        TriggerEditorPanel();
        ~TriggerEditorPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

      private:
        struct TriggerInfo
        {
            uint32_t id = 0;
            int shape = 0; // 0=Sphere, 1=AABB
            float center[3] = {};
            float radius = 1.0f;
            float halfExtents[3] = {1.0f, 1.0f, 1.0f};
            bool enabled = true;
            int occupantCount = 0;
            char label[64] = {};
        };

        void RenderTriggerList();
        void RenderTriggerDetails();
        void RenderCreateDialog();

        std::vector<TriggerInfo> m_triggers;
        int m_selectedTrigger = -1;
        bool m_showInViewport = true;
        bool m_showCreateDialog = false;

        // Create dialog state
        int m_newShape = 0;
        float m_newCenter[3] = {};
        float m_newRadius = 5.0f;
        float m_newExtents[3] = {5.0f, 5.0f, 5.0f};
        char m_newLabel[64] = {};
    };

} // namespace SparkEditor
