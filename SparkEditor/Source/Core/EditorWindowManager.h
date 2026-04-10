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
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
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
         * @return true on successful write
         *
         * File format: hand-rolled JSON matching the in-memory
         * WindowLayout. Creates parent directories as needed. On failure
         * the path is still recorded in m_lastSavedPath so status reads
         * reflect the attempted path.
         */
        bool SaveCurrentLayoutToFile(const std::string& path)
        {
            m_lastSavedPath = path;
            if (path.empty())
                return false;

            std::error_code ec;
            auto parent = std::filesystem::path(path).parent_path();
            if (!parent.empty())
            {
                std::filesystem::create_directories(parent, ec);
            }

            std::ofstream f(path, std::ios::trunc);
            if (!f.is_open())
                return false;

            f << "{\n";
            f << "  \"windowLayout\": {\n";
            f << "    \"name\": \"" << EscapeJson(m_currentLayout.name) << "\",\n";
            f << "    \"version\": 1,\n";
            f << "    \"dockLayoutINI\": \"" << EscapeJson(m_currentLayout.dockLayoutINI) << "\",\n";
            f << "    \"panels\": [\n";
            for (size_t i = 0; i < m_currentLayout.panels.size(); ++i)
            {
                const PanelWindowState& p = m_currentLayout.panels[i];
                f << "      {\n";
                f << "        \"panelName\": \"" << EscapeJson(p.panelName) << "\",\n";
                f << "        \"isOpen\": " << (p.isOpen ? "true" : "false") << ",\n";
                f << "        \"isFloating\": " << (p.isFloating ? "true" : "false") << ",\n";
                f << "        \"posX\": " << p.posX << ",\n";
                f << "        \"posY\": " << p.posY << ",\n";
                f << "        \"width\": " << p.width << ",\n";
                f << "        \"height\": " << p.height << ",\n";
                f << "        \"monitorIndex\": " << p.monitorIndex << "\n";
                f << "      }";
                if (i + 1 < m_currentLayout.panels.size())
                    f << ",";
                f << "\n";
            }
            f << "    ]\n";
            f << "  }\n}\n";
            return f.good();
        }

        /**
         * @brief Load a layout from a file path
         * @param path  File path to read
         * @return true on successful read and parse
         *
         * Replaces m_currentLayout with the contents of the file. Unknown
         * or malformed fields are skipped; a successful load requires at
         * least a recognisable "windowLayout" key.
         */
        bool LoadLayoutFromFile(const std::string& path)
        {
            m_lastSavedPath = path;
            if (path.empty())
                return false;

            std::ifstream f(path);
            if (!f.is_open())
                return false;

            std::stringstream buf;
            buf << f.rdbuf();
            const std::string contents = buf.str();
            if (contents.empty())
                return false;

            if (contents.find("\"windowLayout\"") == std::string::npos)
                return false;

            WindowLayout loaded;

            // Name
            loaded.name = ReadJsonString(contents, "\"name\"");
            if (loaded.name.empty())
                loaded.name = "Default";

            // Dock layout INI (may contain escaped newlines)
            loaded.dockLayoutINI = ReadJsonString(contents, "\"dockLayoutINI\"");

            // Panels: walk each {...} block inside the "panels" array.
            size_t panelsStart = contents.find("\"panels\"");
            if (panelsStart != std::string::npos)
            {
                size_t bracket = contents.find('[', panelsStart);
                size_t end = (bracket != std::string::npos) ? contents.find(']', bracket) : std::string::npos;
                if (bracket != std::string::npos && end != std::string::npos)
                {
                    size_t cursor = bracket + 1;
                    while (cursor < end)
                    {
                        size_t objStart = contents.find('{', cursor);
                        if (objStart == std::string::npos || objStart >= end)
                            break;
                        size_t objEnd = contents.find('}', objStart);
                        if (objEnd == std::string::npos || objEnd > end)
                            break;

                        const std::string obj = contents.substr(objStart, objEnd - objStart + 1);
                        PanelWindowState p;
                        p.panelName = ReadJsonString(obj, "\"panelName\"");
                        p.isOpen = ReadJsonBool(obj, "\"isOpen\"", true);
                        p.isFloating = ReadJsonBool(obj, "\"isFloating\"", false);
                        p.posX = ReadJsonNumber(obj, "\"posX\"", 0.0f);
                        p.posY = ReadJsonNumber(obj, "\"posY\"", 0.0f);
                        p.width = ReadJsonNumber(obj, "\"width\"", 400.0f);
                        p.height = ReadJsonNumber(obj, "\"height\"", 300.0f);
                        p.monitorIndex = static_cast<int32_t>(ReadJsonNumber(obj, "\"monitorIndex\"", -1.0f));
                        if (!p.panelName.empty())
                            loaded.panels.push_back(p);

                        cursor = objEnd + 1;
                    }
                }
            }

            m_currentLayout = std::move(loaded);
            return true;
        }

        /**
         * @brief Last file path passed to Save/LoadCurrentLayoutToFile.
         */
        const std::string& GetLastSavedPath() const { return m_lastSavedPath; }

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

        // =====================================================================
        // Minimal JSON helpers (avoid pulling a library into a header-only class)
        // =====================================================================

        static std::string EscapeJson(const std::string& s)
        {
            std::string out;
            out.reserve(s.size() + 2);
            for (char c : s)
            {
                switch (c)
                {
                case '"':
                    out += "\\\"";
                    break;
                case '\\':
                    out += "\\\\";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                default:
                    out += c;
                    break;
                }
            }
            return out;
        }

        static std::string UnescapeJson(const std::string& s)
        {
            std::string out;
            out.reserve(s.size());
            for (size_t i = 0; i < s.size(); ++i)
            {
                if (s[i] == '\\' && i + 1 < s.size())
                {
                    char e = s[i + 1];
                    switch (e)
                    {
                    case '"':
                        out += '"';
                        break;
                    case '\\':
                        out += '\\';
                        break;
                    case 'n':
                        out += '\n';
                        break;
                    case 'r':
                        out += '\r';
                        break;
                    case 't':
                        out += '\t';
                        break;
                    default:
                        out += e;
                        break;
                    }
                    ++i;
                }
                else
                {
                    out += s[i];
                }
            }
            return out;
        }

        static std::string ReadJsonString(const std::string& src, const std::string& quotedKey)
        {
            const size_t keyPos = src.find(quotedKey);
            if (keyPos == std::string::npos)
                return {};
            const size_t colon = src.find(':', keyPos + quotedKey.size());
            if (colon == std::string::npos)
                return {};
            const size_t quote = src.find('"', colon);
            if (quote == std::string::npos)
                return {};
            // Find closing quote, skipping escaped quotes.
            size_t end = quote + 1;
            while (end < src.size())
            {
                if (src[end] == '\\' && end + 1 < src.size())
                {
                    end += 2;
                    continue;
                }
                if (src[end] == '"')
                    break;
                ++end;
            }
            if (end >= src.size())
                return {};
            return UnescapeJson(src.substr(quote + 1, end - quote - 1));
        }

        static bool ReadJsonBool(const std::string& src, const std::string& quotedKey, bool defaultValue)
        {
            const size_t keyPos = src.find(quotedKey);
            if (keyPos == std::string::npos)
                return defaultValue;
            const size_t colon = src.find(':', keyPos + quotedKey.size());
            if (colon == std::string::npos)
                return defaultValue;
            const size_t t = src.find("true", colon);
            const size_t fa = src.find("false", colon);
            const size_t comma = src.find(',', colon);
            const size_t brace = src.find('}', colon);
            const size_t stop = std::min(comma == std::string::npos ? src.size() : comma,
                                         brace == std::string::npos ? src.size() : brace);
            if (t != std::string::npos && t < stop)
                return true;
            if (fa != std::string::npos && fa < stop)
                return false;
            return defaultValue;
        }

        static float ReadJsonNumber(const std::string& src, const std::string& quotedKey, float defaultValue)
        {
            const size_t keyPos = src.find(quotedKey);
            if (keyPos == std::string::npos)
                return defaultValue;
            size_t colon = src.find(':', keyPos + quotedKey.size());
            if (colon == std::string::npos)
                return defaultValue;
            ++colon;
            // Skip whitespace.
            while (colon < src.size() && (src[colon] == ' ' || src[colon] == '\t'))
                ++colon;
            size_t start = colon;
            while (colon < src.size())
            {
                char c = src[colon];
                if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E')
                    ++colon;
                else
                    break;
            }
            if (start == colon)
                return defaultValue;
            try
            {
                return std::stof(src.substr(start, colon - start));
            }
            catch (...)
            {
                return defaultValue;
            }
        }

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
