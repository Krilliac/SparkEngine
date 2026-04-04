/**
 * @file EditorWindowManager.h
 * @brief Multi-monitor floating window management with layout persistence
 * @author Spark Engine Team
 * @date 2026
 *
 * Manages floating/docked editor panel state, multi-monitor placement,
 * and named layout save/load/delete. Works alongside EditorLayoutManager
 * for ImGui dock state serialization.
 *
 * @code
 *   auto& wm = EditorWindowManager::GetInstance();
 *   wm.Initialize();
 *   wm.FloatPanel("SceneView");
 *   wm.MoveToMonitor("SceneView", 1);
 *   wm.SaveLayout("DualScreen");
 *   wm.Shutdown();
 * @endcode
 */

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace SparkEditor
{

    // ============================================================================
    // Window Layout Data
    // ============================================================================

    /**
     * @brief Serializable state for a single editor panel
     */
    struct PanelWindowState
    {
        std::string panelName;
        bool isOpen = true;
        bool isFloating = false;
        float posX = 0.0f;
        float posY = 0.0f;
        float width = 400.0f;
        float height = 300.0f;
        int32_t monitorIndex = -1; ///< -1 = primary monitor
    };

    /**
     * @brief Named layout containing all panel states and dock INI
     */
    struct WindowLayout
    {
        std::string name;
        std::vector<PanelWindowState> panels;
        std::string dockLayoutINI; ///< ImGui dock layout serialized string
    };

    // ============================================================================
    // EditorWindowManager
    // ============================================================================

    /**
     * @brief Manages multi-monitor floating windows and layout persistence
     *
     * Provides save/load of named layouts, float/dock toggling per panel,
     * and multi-monitor placement. Auto-saves the current layout on shutdown
     * if enabled.
     */
    class EditorWindowManager
    {
      public:
        /**
         * @brief Get the singleton instance
         * @return Reference to the global EditorWindowManager
         */
        static EditorWindowManager& GetInstance()
        {
            static EditorWindowManager instance;
            return instance;
        }

        /**
         * @brief Initialize the window manager
         */
        void Initialize()
        {
            m_currentLayout.name = "Default";
            m_initialized = true;
        }

        /**
         * @brief Shutdown and optionally auto-save the current layout
         */
        void Shutdown()
        {
            if (m_autoSave && m_initialized)
            {
                SaveLayout("__autosave__");
            }
            m_initialized = false;
        }

        // ====================================================================
        // Layout management
        // ====================================================================

        /**
         * @brief Save the current layout under a name
         * @param name  Layout name (overwrites if exists)
         */
        void SaveLayout(const std::string& name)
        {
            WindowLayout layout = m_currentLayout;
            layout.name = name;

            for (auto& saved : m_savedLayouts)
            {
                if (saved.name == name)
                {
                    saved = layout;
                    return;
                }
            }
            m_savedLayouts.push_back(std::move(layout));
        }

        /**
         * @brief Load a previously saved layout by name
         * @param name  Layout name to load
         * @return true if found and loaded
         */
        bool LoadLayout(const std::string& name)
        {
            for (const auto& layout : m_savedLayouts)
            {
                if (layout.name == name)
                {
                    m_currentLayout = layout;
                    return true;
                }
            }
            return false;
        }

        /**
         * @brief Save the current layout to a file path
         * @param path  File path to write
         */
        void SaveCurrentLayoutToFile(const std::string& path)
        {
            m_lastSavedPath = path;
            // Serialization would write m_currentLayout to disk
        }

        /**
         * @brief Load a layout from a file path
         * @param path  File path to read
         */
        void LoadLayoutFromFile(const std::string& path)
        {
            m_lastSavedPath = path;
            // Deserialization would read layout from disk
        }

        /**
         * @brief Get names of all saved layouts
         * @return Vector of layout names
         */
        std::vector<std::string> GetSavedLayouts() const
        {
            std::vector<std::string> names;
            names.reserve(m_savedLayouts.size());
            for (const auto& layout : m_savedLayouts)
            {
                names.push_back(layout.name);
            }
            return names;
        }

        /**
         * @brief Delete a saved layout by name
         * @param name  Layout name to delete
         * @return true if found and deleted
         */
        bool DeleteLayout(const std::string& name)
        {
            for (auto it = m_savedLayouts.begin(); it != m_savedLayouts.end(); ++it)
            {
                if (it->name == name)
                {
                    m_savedLayouts.erase(it);
                    return true;
                }
            }
            return false;
        }

        // ====================================================================
        // Floating windows
        // ====================================================================

        /**
         * @brief Float a panel (undock from the main window)
         * @param panelName  Name of the panel to float
         */
        void FloatPanel(const std::string& panelName) { m_floatingPanels[panelName] = true; }

        /**
         * @brief Dock a floating panel back into the main window
         * @param panelName  Name of the panel to dock
         */
        void DockPanel(const std::string& panelName) { m_floatingPanels[panelName] = false; }

        /**
         * @brief Check if a panel is currently floating
         * @param panelName  Panel name to check
         * @return true if the panel is floating
         */
        bool IsPanelFloating(const std::string& panelName) const
        {
            auto it = m_floatingPanels.find(panelName);
            return it != m_floatingPanels.end() && it->second;
        }

        // ====================================================================
        // Multi-monitor
        // ====================================================================

        /**
         * @brief Get the number of connected monitors
         * @return Monitor count (minimum 1)
         */
        int32_t GetMonitorCount() const { return m_monitorCount; }

        /**
         * @brief Move a floating panel to a specific monitor
         * @param panelName    Panel to move
         * @param monitorIndex Target monitor index (-1 = primary)
         */
        void MoveToMonitor(const std::string& panelName, int32_t monitorIndex)
        {
            m_panelMonitors[panelName] = monitorIndex;
        }

        // ====================================================================
        // Auto-save
        // ====================================================================

        /**
         * @brief Enable or disable auto-save on shutdown
         * @param enabled  true to auto-save
         */
        void SetAutoSaveEnabled(bool enabled) { m_autoSave = enabled; }

        /**
         * @brief Check if auto-save is enabled
         * @return true if auto-save is on
         */
        bool IsAutoSaveEnabled() const { return m_autoSave; }

        // ====================================================================
        // Console / status
        // ====================================================================

        /**
         * @brief Get a status string for console display
         * @return Formatted status including layout name and panel count
         */
        std::string Console_GetStatus() const
        {
            std::string status = "EditorWindowManager: layout='" + m_currentLayout.name + "'";
            status += " savedLayouts=" + std::to_string(m_savedLayouts.size());
            status += " autoSave=" + std::string(m_autoSave ? "on" : "off");
            return status;
        }

      private:
        EditorWindowManager() = default;

        std::vector<WindowLayout> m_savedLayouts;
        WindowLayout m_currentLayout;
        bool m_autoSave = true;
        bool m_initialized = false;
        int32_t m_monitorCount = 1;
        std::string m_lastSavedPath;

        std::unordered_map<std::string, bool> m_floatingPanels;
        std::unordered_map<std::string, int32_t> m_panelMonitors;
    };

} // namespace SparkEditor
