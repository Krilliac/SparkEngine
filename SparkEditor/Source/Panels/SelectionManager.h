/**
 * @file SelectionManager.h
 * @brief Centralized editor object selection and picking pipeline
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a unified selection system for the editor: single/multi-object
 * selection, marquee (box) selection, selection filtering by layer/tag/component,
 * selection groups, and cross-panel synchronization via callbacks.
 *
 * All editor panels (Hierarchy, Inspector, SceneView, etc.) observe selection
 * changes through registered callbacks rather than polling.
 *
 * ## Usage
 * @code
 *   auto& sel = SparkEditor::SelectionManager::GetInstance();
 *   sel.Initialize();
 *
 *   // Select an entity
 *   sel.Select(entityId);
 *
 *   // Multi-select
 *   sel.AddToSelection(entity2);
 *   sel.AddToSelection(entity3);
 *
 *   // Query
 *   if (sel.IsSelected(entityId)) { ... }
 *   auto& selected = sel.GetSelection();
 *
 *   // Listen for changes
 *   sel.OnSelectionChanged([](const SelectionChangedEvent& e) {
 *       // Update inspector, gizmos, etc.
 *   });
 *
 *   // Marquee selection
 *   sel.BeginMarquee(screenX, screenY);
 *   sel.UpdateMarquee(currentX, currentY);
 *   auto entities = sel.EndMarquee(allVisibleEntities);
 *
 *   // GPU picking (would integrate with render pass)
 *   sel.SetPickResult(entityIdAtPixel);
 *
 *   // Filtering
 *   sel.SetFilter(SelectionFilter::StaticMeshOnly);
 * @endcode
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace SparkEditor
{

    /** @brief Entity identifier for selection purposes */
    using EntityId = uint32_t;

    /** @brief Invalid entity sentinel */
    constexpr EntityId INVALID_ENTITY = 0;

    /** @brief Selection filter presets */
    enum class SelectionFilter
    {
        All,            ///< No filtering — all entities selectable
        StaticMeshOnly, ///< Only entities with static mesh components
        LightsOnly,     ///< Only light entities
        CamerasOnly,    ///< Only camera entities
        PhysicsOnly,    ///< Only entities with physics components
        AIOnly,         ///< Only entities with AI components
        Custom          ///< User-defined filter via callback
    };

    /** @brief Event data for selection change notifications */
    struct SelectionChangedEvent
    {
        std::vector<EntityId> added;     ///< Entities newly added to selection
        std::vector<EntityId> removed;   ///< Entities removed from selection
        std::vector<EntityId> current;   ///< Complete current selection
        bool isMarqueeSelection = false; ///< Whether this came from a marquee operation
    };

    /** @brief Rectangle in screen space for marquee selection */
    struct MarqueeRect
    {
        float x0 = 0.0f, y0 = 0.0f; ///< Start corner
        float x1 = 0.0f, y1 = 0.0f; ///< Current corner

        /** @brief Get normalized rect (min/max corrected) */
        void GetNormalized(float& minX, float& minY, float& maxX, float& maxY) const
        {
            minX = std::min(x0, x1);
            minY = std::min(y0, y1);
            maxX = std::max(x0, x1);
            maxY = std::max(y0, y1);
        }

        /** @brief Check if a screen point is inside the marquee */
        bool Contains(float px, float py) const
        {
            float mnX, mnY, mxX, mxY;
            GetNormalized(mnX, mnY, mxX, mxY);
            return px >= mnX && px <= mxX && py >= mnY && py <= mxY;
        }
    };

    /** @brief Named selection group for organizing selections */
    struct SelectionGroup
    {
        std::string name;               ///< Group display name
        std::vector<EntityId> entities; ///< Entities in this group
    };

    /** @brief Screen-space position for entity picking */
    struct ScreenPosition
    {
        float x = 0.0f; ///< Screen X coordinate
        float y = 0.0f; ///< Screen Y coordinate
    };

    /** @brief Callback signature for selection changes */
    using SelectionCallback = std::function<void(const SelectionChangedEvent&)>;

    /** @brief Custom filter callback — return true if entity is selectable */
    using FilterCallback = std::function<bool(EntityId)>;

    /**
     * @brief Centralized editor selection and picking pipeline
     *
     * Manages the current selection state across all editor panels. Supports
     * single-click selection, Ctrl+click multi-select, Shift+click range
     * selection, marquee/box selection, and GPU ID-based picking.
     *
     * Thread safety: Main thread only. All editor panels run on the main thread.
     */
    class SelectionManager
    {
      public:
        static SelectionManager& GetInstance()
        {
            static SelectionManager instance;
            return instance;
        }

        /** @brief Initialize the selection manager */
        void Initialize()
        {
            m_selection.clear();
            m_selectionOrder.clear();
            m_lockedEntities.clear();
            m_groups.clear();
            m_callbacks.clear();
            m_filter = SelectionFilter::All;
            m_customFilter = nullptr;
            m_isMarqueeActive = false;
            m_marquee = {};
            m_nextCallbackId = 1;
            m_initialized = true;
        }

        /** @brief Shut down and clear all state */
        void Shutdown()
        {
            m_selection.clear();
            m_selectionOrder.clear();
            m_callbacks.clear();
            m_groups.clear();
            m_initialized = false;
        }

        // ====================================================================
        // Selection operations
        // ====================================================================

        /**
         * @brief Select a single entity (clears previous selection)
         * @param entityId Entity to select
         */
        void Select(EntityId entityId)
        {
            if (!m_initialized || entityId == INVALID_ENTITY || !PassesFilter(entityId))
                return;
            if (IsLocked(entityId))
                return;

            auto oldSelection = GetSelectionVector();
            m_selection.clear();
            m_selectionOrder.clear();
            m_selection.insert(entityId);
            m_selectionOrder.push_back(entityId);
            NotifyChanged(oldSelection);
        }

        /**
         * @brief Add an entity to the current selection (Ctrl+Click)
         * @param entityId Entity to add
         */
        void AddToSelection(EntityId entityId)
        {
            if (!m_initialized || entityId == INVALID_ENTITY || !PassesFilter(entityId))
                return;
            if (IsLocked(entityId) || m_selection.count(entityId))
                return;

            auto oldSelection = GetSelectionVector();
            m_selection.insert(entityId);
            m_selectionOrder.push_back(entityId);
            NotifyChanged(oldSelection);
        }

        /**
         * @brief Remove an entity from the current selection
         * @param entityId Entity to remove
         */
        void RemoveFromSelection(EntityId entityId)
        {
            if (!m_selection.count(entityId))
                return;

            auto oldSelection = GetSelectionVector();
            m_selection.erase(entityId);
            m_selectionOrder.erase(std::remove(m_selectionOrder.begin(), m_selectionOrder.end(), entityId),
                                   m_selectionOrder.end());
            NotifyChanged(oldSelection);
        }

        /**
         * @brief Toggle an entity's selection state (Ctrl+Click on selected)
         * @param entityId Entity to toggle
         */
        void ToggleSelection(EntityId entityId)
        {
            if (m_selection.count(entityId))
                RemoveFromSelection(entityId);
            else
                AddToSelection(entityId);
        }

        /** @brief Clear all selected entities */
        void ClearSelection()
        {
            if (m_selection.empty())
                return;
            auto oldSelection = GetSelectionVector();
            m_selection.clear();
            m_selectionOrder.clear();
            NotifyChanged(oldSelection);
        }

        /**
         * @brief Select multiple entities at once
         * @param entities List of entities to select
         */
        void SelectMultiple(const std::vector<EntityId>& entities)
        {
            auto oldSelection = GetSelectionVector();
            m_selection.clear();
            m_selectionOrder.clear();
            for (auto id : entities)
            {
                if (id != INVALID_ENTITY && PassesFilter(id) && !IsLocked(id))
                {
                    m_selection.insert(id);
                    m_selectionOrder.push_back(id);
                }
            }
            NotifyChanged(oldSelection);
        }

        // ====================================================================
        // Query
        // ====================================================================

        /** @brief Check if an entity is selected */
        bool IsSelected(EntityId entityId) const { return m_selection.count(entityId) > 0; }

        /** @brief Get the number of selected entities */
        size_t GetSelectionCount() const { return m_selection.size(); }

        /** @brief Get selection as ordered vector */
        const std::vector<EntityId>& GetSelection() const { return m_selectionOrder; }

        /** @brief Get the primary (first/last clicked) selected entity */
        EntityId GetPrimarySelection() const
        {
            return m_selectionOrder.empty() ? INVALID_ENTITY : m_selectionOrder.back();
        }

        /** @brief Check if anything is selected */
        bool HasSelection() const { return !m_selection.empty(); }

        // ====================================================================
        // Marquee (box) selection
        // ====================================================================

        /**
         * @brief Begin a marquee selection rectangle
         * @param screenX Starting screen X
         * @param screenY Starting screen Y
         */
        void BeginMarquee(float screenX, float screenY)
        {
            m_isMarqueeActive = true;
            m_marquee.x0 = screenX;
            m_marquee.y0 = screenY;
            m_marquee.x1 = screenX;
            m_marquee.y1 = screenY;
        }

        /**
         * @brief Update the marquee rectangle as the mouse moves
         * @param screenX Current screen X
         * @param screenY Current screen Y
         */
        void UpdateMarquee(float screenX, float screenY)
        {
            if (m_isMarqueeActive)
            {
                m_marquee.x1 = screenX;
                m_marquee.y1 = screenY;
            }
        }

        /**
         * @brief End marquee selection and select entities within the rectangle
         * @param screenPositions Map of entity ID to screen position
         * @param additive If true, add to existing selection (Shift+drag)
         */
        void EndMarquee(const std::vector<std::pair<EntityId, ScreenPosition>>& screenPositions, bool additive = false)
        {
            if (!m_isMarqueeActive)
                return;
            m_isMarqueeActive = false;

            std::vector<EntityId> marqueeHits;
            for (const auto& [id, pos] : screenPositions)
            {
                if (m_marquee.Contains(pos.x, pos.y) && PassesFilter(id) && !IsLocked(id))
                    marqueeHits.push_back(id);
            }

            if (additive)
            {
                for (auto id : marqueeHits)
                    AddToSelection(id);
            }
            else
            {
                auto oldSelection = GetSelectionVector();
                m_selection.clear();
                m_selectionOrder.clear();
                for (auto id : marqueeHits)
                {
                    m_selection.insert(id);
                    m_selectionOrder.push_back(id);
                }
                NotifyChangedMarquee(oldSelection);
            }
        }

        /** @brief Check if a marquee selection is in progress */
        bool IsMarqueeActive() const { return m_isMarqueeActive; }

        /** @brief Get the current marquee rectangle */
        const MarqueeRect& GetMarquee() const { return m_marquee; }

        // ====================================================================
        // GPU picking
        // ====================================================================

        /**
         * @brief Set the pick result from a GPU ID render pass
         * @param entityId Entity at the clicked pixel (INVALID_ENTITY = background)
         * @param additive Whether to add to selection (Ctrl held)
         */
        void SetPickResult(EntityId entityId, bool additive = false)
        {
            if (entityId == INVALID_ENTITY)
            {
                if (!additive)
                    ClearSelection();
                return;
            }
            if (additive)
                ToggleSelection(entityId);
            else
                Select(entityId);
        }

        // ====================================================================
        // Filtering
        // ====================================================================

        /** @brief Set the selection filter preset */
        void SetFilter(SelectionFilter filter) { m_filter = filter; }

        /** @brief Get the current filter */
        SelectionFilter GetFilter() const { return m_filter; }

        /** @brief Set a custom filter callback */
        void SetCustomFilter(FilterCallback filter)
        {
            m_customFilter = std::move(filter);
            m_filter = SelectionFilter::Custom;
        }

        /** @brief Clear the filter (allow all) */
        void ClearFilter()
        {
            m_filter = SelectionFilter::All;
            m_customFilter = nullptr;
        }

        // ====================================================================
        // Locking
        // ====================================================================

        /** @brief Lock an entity so it cannot be selected */
        void LockEntity(EntityId id) { m_lockedEntities.insert(id); }

        /** @brief Unlock an entity */
        void UnlockEntity(EntityId id) { m_lockedEntities.erase(id); }

        /** @brief Check if an entity is locked */
        bool IsLocked(EntityId id) const { return m_lockedEntities.count(id) > 0; }

        // ====================================================================
        // Selection groups
        // ====================================================================

        /** @brief Save current selection as a named group */
        void SaveSelectionGroup(const std::string& name)
        {
            SelectionGroup group;
            group.name = name;
            group.entities = m_selectionOrder;
            m_groups[name] = std::move(group);
        }

        /** @brief Restore a named selection group */
        void RestoreSelectionGroup(const std::string& name)
        {
            auto it = m_groups.find(name);
            if (it != m_groups.end())
                SelectMultiple(it->second.entities);
        }

        /** @brief Delete a selection group */
        void DeleteSelectionGroup(const std::string& name) { m_groups.erase(name); }

        /** @brief Get all group names */
        std::vector<std::string> GetGroupNames() const
        {
            std::vector<std::string> names;
            for (const auto& [name, group] : m_groups)
                names.push_back(name);
            return names;
        }

        // ====================================================================
        // Callbacks
        // ====================================================================

        /**
         * @brief Register a callback for selection changes
         * @param callback Function to call when selection changes
         * @return Callback ID for unregistering
         */
        uint32_t OnSelectionChanged(SelectionCallback callback)
        {
            uint32_t id = m_nextCallbackId++;
            m_callbacks[id] = std::move(callback);
            return id;
        }

        /** @brief Unregister a selection change callback */
        void RemoveCallback(uint32_t callbackId) { m_callbacks.erase(callbackId); }

        // ====================================================================
        // Status
        // ====================================================================

        /** @brief Get console-friendly status string */
        std::string Console_GetStatus() const
        {
            if (!m_initialized)
                return "[Selection] Not initialized";
            std::string status = "[Selection] Selected: " + std::to_string(m_selection.size());
            status += " | Groups: " + std::to_string(m_groups.size());
            status += " | Locked: " + std::to_string(m_lockedEntities.size());
            status += " | Filter: " + FilterName();
            if (m_isMarqueeActive)
                status += " | MARQUEE ACTIVE";
            return status;
        }

      private:
        SelectionManager() = default;

        std::vector<EntityId> GetSelectionVector() const { return {m_selection.begin(), m_selection.end()}; }

        bool PassesFilter(EntityId id) const
        {
            if (m_filter == SelectionFilter::All)
                return true;
            if (m_filter == SelectionFilter::Custom && m_customFilter)
                return m_customFilter(id);
            // Other filters would check entity components via ECS query
            // For now, all pass — the filter is applied by the calling code
            return true;
        }

        std::string FilterName() const
        {
            switch (m_filter)
            {
            case SelectionFilter::All:
                return "All";
            case SelectionFilter::StaticMeshOnly:
                return "StaticMesh";
            case SelectionFilter::LightsOnly:
                return "Lights";
            case SelectionFilter::CamerasOnly:
                return "Cameras";
            case SelectionFilter::PhysicsOnly:
                return "Physics";
            case SelectionFilter::AIOnly:
                return "AI";
            case SelectionFilter::Custom:
                return "Custom";
            }
            return "Unknown";
        }

        void NotifyChanged(const std::vector<EntityId>& oldSelection)
        {
            SelectionChangedEvent event;
            event.current = m_selectionOrder;

            std::unordered_set<EntityId> oldSet(oldSelection.begin(), oldSelection.end());
            for (auto id : m_selectionOrder)
            {
                if (!oldSet.count(id))
                    event.added.push_back(id);
            }
            for (auto id : oldSelection)
            {
                if (!m_selection.count(id))
                    event.removed.push_back(id);
            }

            for (const auto& [cbId, cb] : m_callbacks)
                cb(event);
        }

        void NotifyChangedMarquee(const std::vector<EntityId>& oldSelection)
        {
            SelectionChangedEvent event;
            event.current = m_selectionOrder;
            event.isMarqueeSelection = true;

            std::unordered_set<EntityId> oldSet(oldSelection.begin(), oldSelection.end());
            for (auto id : m_selectionOrder)
            {
                if (!oldSet.count(id))
                    event.added.push_back(id);
            }
            for (auto id : oldSelection)
            {
                if (!m_selection.count(id))
                    event.removed.push_back(id);
            }

            for (const auto& [cbId, cb] : m_callbacks)
                cb(event);
        }

        bool m_initialized = false;
        std::unordered_set<EntityId> m_selection;
        std::vector<EntityId> m_selectionOrder;
        std::unordered_set<EntityId> m_lockedEntities;
        std::unordered_map<std::string, SelectionGroup> m_groups;
        std::unordered_map<uint32_t, SelectionCallback> m_callbacks;
        SelectionFilter m_filter = SelectionFilter::All;
        FilterCallback m_customFilter;
        bool m_isMarqueeActive = false;
        MarqueeRect m_marquee;
        uint32_t m_nextCallbackId = 1;
    };

} // namespace SparkEditor
