/**
 * @file EventMonitorPanel.h
 * @brief Event system monitor panel for the Spark Engine Editor
 */

#pragma once

#include "../Core/EditorPanel.h"
#include <imgui.h>
#include <string>
#include <vector>
#include <chrono>

namespace SparkEditor
{

    /**
     * @brief Panel for monitoring the engine's EventBus in real-time
     *
     * Shows a scrolling log of events fired during Play mode,
     * with filtering by event type and category. Color-codes events
     * by category and supports a circular buffer to cap memory usage.
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
        enum class EventCategory : int
        {
            All = 0,
            Entity,
            Physics,
            Gameplay,
            Audio,
            UI,
            Count
        };

        struct EventEntry
        {
            float timestamp = 0.0f;
            char eventType[128] = {};
            char details[256] = {};
            EventCategory category = EventCategory::Entity;
        };

        void RenderToolbar();
        void RenderEventLog();
        void RenderStats();

        static ImVec4 GetCategoryColor(EventCategory category);
        static const char* GetCategoryName(EventCategory category);

        std::vector<EventEntry> m_events;
        char m_filterText[128] = {};
        bool m_autoScroll = true;
        bool m_paused = false;
        float m_elapsed = 0.0f;
        int m_maxEntries = 500;
        int m_categoryFilter = 0; // 0 = All
        int m_categoryCounts[6] = {};
    };

} // namespace SparkEditor
