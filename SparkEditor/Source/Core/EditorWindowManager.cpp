/**
 * @file EditorWindowManager.cpp
 * @brief Implementation of EditorWindowManager methods (layout persistence, JSON helpers)
 */

#include "EditorWindowManager.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace SparkEditor
{

    // ========================================================================
    // Layout management
    // ========================================================================

    void EditorWindowManager::SaveLayout(const std::string& name)
    {
        SyncRuntimeStateIntoCurrentLayout();

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

    bool EditorWindowManager::LoadLayout(const std::string& name)
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

    bool EditorWindowManager::SaveCurrentLayoutToFile(const std::string& path)
    {
        m_lastSavedPath = path;
        if (path.empty())
            return false;

        // Fold the runtime floating / monitor-placement maps into
        // m_currentLayout.panels before serializing. FloatPanel /
        // DockPanel / MoveToMonitor mutate the side maps but never
        // touch m_currentLayout.panels directly, so without this sync
        // the serialized file would omit whatever the user just
        // reconfigured and a subsequent Load would silently lose it.
        SyncRuntimeStateIntoCurrentLayout();

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

    bool EditorWindowManager::LoadLayoutFromFile(const std::string& path)
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

    std::vector<std::string> EditorWindowManager::GetSavedLayouts() const
    {
        std::vector<std::string> names;
        names.reserve(m_savedLayouts.size());
        for (const auto& layout : m_savedLayouts)
        {
            names.push_back(layout.name);
        }
        return names;
    }

    bool EditorWindowManager::DeleteLayout(const std::string& name)
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

    bool EditorWindowManager::IsPanelFloating(const std::string& panelName) const
    {
        auto it = m_floatingPanels.find(panelName);
        return it != m_floatingPanels.end() && it->second;
    }

    std::string EditorWindowManager::Console_GetStatus() const
    {
        std::string status = "EditorWindowManager: layout='" + m_currentLayout.name + "'";
        status += " savedLayouts=" + std::to_string(m_savedLayouts.size());
        status += " autoSave=" + std::string(m_autoSave ? "on" : "off");
        return status;
    }

    // ========================================================================
    // Runtime state synchronization
    // ========================================================================

    void EditorWindowManager::SyncRuntimeStateIntoCurrentLayout()
    {
        auto findOrCreate = [this](const std::string& name) -> PanelWindowState*
        {
            for (auto& p : m_currentLayout.panels)
            {
                if (p.panelName == name)
                    return &p;
            }
            PanelWindowState entry;
            entry.panelName = name;
            m_currentLayout.panels.push_back(entry);
            return &m_currentLayout.panels.back();
        };

        for (const auto& [name, floating] : m_floatingPanels)
        {
            PanelWindowState* p = findOrCreate(name);
            p->isFloating = floating;
        }
        for (const auto& [name, monitor] : m_panelMonitors)
        {
            PanelWindowState* p = findOrCreate(name);
            p->monitorIndex = monitor;
        }
    }

    // ========================================================================
    // Minimal JSON helpers
    // ========================================================================

    std::string EditorWindowManager::EscapeJson(const std::string& s)
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

    std::string EditorWindowManager::UnescapeJson(const std::string& s)
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

    std::string EditorWindowManager::ReadJsonString(const std::string& src, const std::string& quotedKey)
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

    bool EditorWindowManager::ReadJsonBool(const std::string& src, const std::string& quotedKey, bool defaultValue)
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
        const size_t stop =
            std::min(comma == std::string::npos ? src.size() : comma, brace == std::string::npos ? src.size() : brace);
        if (t != std::string::npos && t < stop)
            return true;
        if (fa != std::string::npos && fa < stop)
            return false;
        return defaultValue;
    }

    float EditorWindowManager::ReadJsonNumber(const std::string& src, const std::string& quotedKey, float defaultValue)
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

} // namespace SparkEditor
