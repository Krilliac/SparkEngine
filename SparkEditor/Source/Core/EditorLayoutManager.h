/**
 * @file EditorLayoutManager.h
 * @brief Panel-layout persistence for the Spark Engine Editor
 * @author Spark Engine Team
 * @date 2026
 *
 * Manages visibility, position and size of editor panels, and persists
 * named layouts to disk as JSON files under a configurable layout
 * directory. Panels register their default configuration on startup and
 * can be queried, toggled, or restored to defaults by name.
 *
 * Serialization is intentionally hand-rolled JSON (no third-party
 * dependency) so the manager can be linked into both the full editor and
 * the headless test binary without pulling in Dear ImGui. Panel geometry
 * is stored as plain floats rather than ImVec2 for the same reason.
 */

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace SparkEditor
{

    /**
     * @brief Dock anchor for a panel within the editor dockspace.
     */
    enum class LayoutDockPosition
    {
        Left,
        Right,
        Top,
        Bottom,
        Center,
        Float
    };

    /**
     * @brief Configuration for a single editor panel.
     *
     * Mirrors the subset of ImGui panel state that the editor cares
     * about saving: docking target, position/size, visibility, tab
     * order. Defaults are copied from the registration call and can be
     * restored via ResetToDefault().
     */
    struct PanelConfig
    {
        std::string name;                                             ///< Unique panel identifier
        std::string displayName;                                      ///< User-visible name
        LayoutDockPosition dockPosition = LayoutDockPosition::Center; ///< Dock anchor
        float sizeX = 300.0f;                                         ///< Width in pixels
        float sizeY = 200.0f;                                         ///< Height in pixels
        float posX = 0.0f;                                            ///< Floating X position
        float posY = 0.0f;                                            ///< Floating Y position
        bool isVisible = true;                                        ///< Shown this frame
        bool isFloating = false;                                      ///< Detached from dockspace
        bool canClose = true;                                         ///< User can hide the panel
        bool canDock = true;                                          ///< Accepts docking drops
        float dockRatio = 0.25f;                                      ///< Fraction of parent dock
        int tabOrder = 0;                                             ///< Tab ordering within a dock
        std::string parentDock;                                       ///< Parent dock identifier
    };

    /**
     * @brief Metadata for a layout file on disk.
     */
    struct LayoutInfo
    {
        std::string name;        ///< Layout name (filename without extension)
        std::string description; ///< Optional human-readable description
        std::string filePath;    ///< Absolute path of the JSON file
    };

    /**
     * @brief Disk-backed layout manager with named save/load.
     *
     * Initialize() creates the layout directory if missing and scans any
     * previously-saved layouts. SaveCurrentLayout/LoadLayout perform JSON
     * round-trips per layout. All methods are main-thread only.
     */
    class EditorLayoutManager
    {
      public:
        EditorLayoutManager();
        ~EditorLayoutManager();

        /**
         * @brief Initialize the layout manager.
         * @param layoutDirectory  Directory where layout JSON files are stored.
         * @return true on success (directory exists or was created).
         */
        bool Initialize(const std::string& layoutDirectory = "Layouts");

        /**
         * @brief Shutdown the layout manager. Does not auto-save.
         */
        void Shutdown();

        /**
         * @brief True between Initialize() and Shutdown().
         */
        bool IsInitialized() const { return m_initialized; }

        // =====================================================================
        // Panel registration / state
        // =====================================================================

        /**
         * @brief Register a panel and its default configuration.
         *
         * The `config` is copied to both the current state and the default
         * snapshot; subsequent calls with the same name overwrite the
         * current state but preserve the original default (so
         * ResetToDefault() still restores the first-seen configuration).
         */
        void RegisterPanel(const PanelConfig& config);

        /**
         * @brief Toggle panel visibility.
         */
        void SetPanelVisible(const std::string& panelName, bool visible);

        /**
         * @brief Query panel visibility. Returns false for unknown panels.
         */
        bool IsPanelVisible(const std::string& panelName) const;

        /**
         * @brief Set a panel's floating position.
         */
        void SetPanelPosition(const std::string& panelName, float x, float y);

        /**
         * @brief Set a panel's size in pixels.
         */
        void SetPanelSize(const std::string& panelName, float w, float h);

        /**
         * @brief Lookup a panel's current configuration.
         * @return pointer to the config, or nullptr if not registered.
         */
        const PanelConfig* GetPanelConfig(const std::string& panelName) const;

        /**
         * @brief Number of panels currently registered.
         */
        uint32_t GetRegisteredPanelCount() const { return static_cast<uint32_t>(m_panels.size()); }

        /**
         * @brief Names of all registered panels, in insertion order.
         */
        std::vector<std::string> GetRegisteredPanels() const;

        // =====================================================================
        // Layout save / load
        // =====================================================================

        /**
         * @brief Save the current panel state as a named layout to disk.
         * @param name         Layout name (used as filename: <dir>/<name>.json)
         * @param description  Optional human-readable description stored in the file.
         * @return true on successful file write.
         */
        bool SaveCurrentLayout(const std::string& name, const std::string& description = "");

        /**
         * @brief Load a layout from disk and apply it to the current panel state.
         * Panels that are in the file but not registered are ignored; panels
         * that are registered but missing from the file keep their current
         * state.
         * @param name  Layout name (looked up as <dir>/<name>.json)
         * @return true on successful file read and parse.
         */
        bool LoadLayout(const std::string& name);

        /**
         * @brief Alias for LoadLayout; kept for symmetry with the previous API.
         */
        bool ApplyLayout(const std::string& name) { return LoadLayout(name); }

        /**
         * @brief Delete a saved layout file.
         * @return true if the file existed and was removed.
         */
        bool DeleteLayout(const std::string& name);

        /**
         * @brief Restore all panels to the configuration they had when first
         *        registered. Does not touch any layout files on disk.
         */
        void ResetToDefault();

        /**
         * @brief List all known saved layouts on disk.
         *
         * Re-scans the layout directory each call so files dropped in
         * externally are visible to the editor.
         */
        std::vector<LayoutInfo> GetSavedLayouts();

        /**
         * @brief Get the name of the most recently loaded/saved layout.
         */
        const std::string& GetCurrentLayoutName() const { return m_currentLayoutName; }

        /**
         * @brief Get the directory where layouts are persisted.
         */
        const std::string& GetLayoutDirectory() const { return m_layoutDirectory; }

        /**
         * @brief Describe the manager for console output.
         */
        std::string Console_GetStatus() const;

      private:
        // Serialization helpers implemented in the .cpp
        bool WriteLayoutFile(const std::string& path, const std::string& name, const std::string& description) const;
        bool ReadLayoutFile(const std::string& path, std::string& outDescription);
        std::string LayoutFilePath(const std::string& name) const;
        static std::string EscapeJsonString(const std::string& s);

        std::unordered_map<std::string, PanelConfig> m_panels;   ///< Live panel state
        std::unordered_map<std::string, PanelConfig> m_defaults; ///< Registered defaults
        std::vector<std::string> m_panelOrder;                   ///< Insertion order of m_panels
        std::string m_layoutDirectory = "Layouts";
        std::string m_currentLayoutName = "Default";
        bool m_initialized = false;
    };

} // namespace SparkEditor
