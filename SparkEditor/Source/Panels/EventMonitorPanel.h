/**
 * @file EventMonitorPanel.h
 * @brief Event system monitor panel for the Spark Engine Editor
 */

#pragma once

#include "../Core/EditorPanel.h"
#include <string>
#include <vector>
#include <chrono>

namespace SparkEditor
{

    /**
     * @brief Panel for monitoring the engine's EventBus in real-time
     *
     * Shows a scrolling log of events fired during Play mode,
     * with filtering by event type. Useful for debugging event flow.
     */
    class EventMonitorPanel : public EditorPanel
    {
      public:
        EventMonitorPanel();
        ~EventMonitorPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

      private:
        struct EventEntry
        {
            float timestamp = 0.0f;
            char eventType[128] = {};
            char details[256] = {};
        };

        std::vector<EventEntry> m_events;
        char m_filterText[128] = {};
        bool m_autoScroll = true;
        bool m_paused = false;
        float m_elapsed = 0.0f;
        int m_maxEntries = 500;
    };

} // namespace SparkEditor
