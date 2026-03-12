/**
 * @file RuntimeInspectorPanel.h
 * @brief Live property inspector for editing entity components during play mode
 * @author Spark Engine Team
 * @date 2025
 *
 * Extends the standard Inspector to allow live property editing while the game
 * is running. Changes are tracked, can be reverted individually, and may
 * optionally persist when play mode ends. Supports watching specific properties
 * with real-time value graphs.
 */

#pragma once

#include "../Core/EditorPanel.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::Editor
{
    class PlayModeManager;
    struct LiveEditRecord;
} // namespace Spark::Editor

namespace SparkEditor
{

    // ========================================================================
    // Property Watch — tracks a single numeric value over time
    // ========================================================================

    struct PropertyWatch
    {
        uint32_t entityId = 0;
        std::string componentType;
        std::string propertyName;
        std::string displayLabel;

        static constexpr size_t MAX_HISTORY = 240;
        float history[MAX_HISTORY] = {};
        size_t historyIndex = 0;
        size_t historySamples = 0;
        float currentValue = 0.0f;
        float minValue = 0.0f;
        float maxValue = 1.0f;
        bool autoRange = true;

        void PushSample(float value)
        {
            history[historyIndex] = value;
            historyIndex = (historyIndex + 1) % MAX_HISTORY;
            if (historySamples < MAX_HISTORY)
                historySamples++;
            currentValue = value;

            if (autoRange && historySamples > 0)
            {
                minValue = history[0];
                maxValue = history[0];
                for (size_t i = 1; i < historySamples; ++i)
                {
                    if (history[i] < minValue)
                        minValue = history[i];
                    if (history[i] > maxValue)
                        maxValue = history[i];
                }
                // Add 10% padding
                float range = maxValue - minValue;
                if (range < 0.001f)
                    range = 1.0f;
                minValue -= range * 0.1f;
                maxValue += range * 0.1f;
            }
        }
    };

    // ========================================================================
    // Editable Property — represents a single property that can be live-edited
    // ========================================================================

    enum class PropertyType
    {
        Float,
        Int,
        Bool,
        String,
        Vector3,
        Color
    };

    struct EditableProperty
    {
        std::string name;
        std::string componentType;
        PropertyType type = PropertyType::Float;
        std::string currentValue;
        std::string originalValue; ///< Value at the start of play mode
        bool isModified = false;
        bool isReadOnly = false;
    };

    // ========================================================================
    // Runtime Inspector Panel
    // ========================================================================

    /**
     * @brief Live property editor panel for play-mode inspection
     *
     * Features:
     * - Browse all entities and their components while the game runs
     * - Edit numeric, boolean, string, and vector properties live
     * - Track all changes with timestamp and old/new values
     * - Watch specific properties with real-time sparkline graphs
     * - Revert individual or all changes
     * - Optionally persist changes when play mode stops
     * - Filter properties by component type, name, or modified-only
     */
    class RuntimeInspectorPanel : public EditorPanel
    {
      public:
        RuntimeInspectorPanel();
        ~RuntimeInspectorPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;
        bool HandleEvent(const std::string& eventType, void* eventData) override;

        /// @brief Connect to the play mode manager for live edit tracking.
        void SetPlayModeManager(Spark::Editor::PlayModeManager* manager) { m_playModeManager = manager; }

        /// @brief Set the currently inspected entity.
        void SetSelectedEntity(uint32_t entityId);

        /// @brief Add a property to the watch list for graphing.
        void WatchProperty(uint32_t entityId, const std::string& componentType, const std::string& propertyName);

        /// @brief Remove a property from the watch list.
        void UnwatchProperty(uint32_t entityId, const std::string& componentType, const std::string& propertyName);

      private:
        void RenderEntitySelector();
        void RenderComponentList();
        void RenderPropertyEditor(const EditableProperty& prop);
        void RenderPropertyWatches();
        void RenderEditHistory();
        void RenderToolbar();
        void RenderSearchFilter();

        std::string MakeWatchKey(uint32_t entityId, const std::string& comp, const std::string& prop) const;

        Spark::Editor::PlayModeManager* m_playModeManager = nullptr;

        // Selection
        uint32_t m_selectedEntityId = 0;
        bool m_hasSelection = false;

        // Properties for the selected entity
        std::vector<EditableProperty> m_properties;

        // Property watches
        std::unordered_map<std::string, PropertyWatch> m_watches;

        // Filter
        std::string m_searchFilter;
        bool m_showModifiedOnly = false;
        bool m_showReadOnly = true;

        // Edit history display
        bool m_showEditHistory = false;
        int m_editHistoryMaxEntries = 50;

        // UI state
        float m_refreshTimer = 0.0f;
        float m_refreshInterval = 0.1f; ///< How often to poll entity state (seconds)
        bool m_autoRefresh = true;
        bool m_freezeOnPause = true; ///< Stop refreshing when paused

        // Timing
        std::chrono::steady_clock::time_point m_lastRefresh;
    };

} // namespace SparkEditor
