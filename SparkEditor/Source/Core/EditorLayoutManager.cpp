/**
 * @file EditorLayoutManager.cpp
 * @brief Implementation of EditorLayoutManager — disk-backed panel layouts
 *
 * File format (hand-rolled JSON, one file per layout):
 *
 * @code
 * {
 *   "layout": {
 *     "name": "MyLayout",
 *     "description": "optional",
 *     "version": 1,
 *     "panels": [
 *       {
 *         "name": "Hierarchy",
 *         "displayName": "Hierarchy",
 *         "dock": 0,
 *         "sizeX": 300, "sizeY": 600,
 *         "posX": 0,   "posY": 0,
 *         "visible": true, "floating": false,
 *         "canClose": true, "canDock": true,
 *         "dockRatio": 0.25, "tabOrder": 0,
 *         "parentDock": ""
 *       }, ...
 *     ]
 *   }
 * }
 * @endcode
 *
 * The parser is deliberately simple: it walks keys linearly, assumes
 * whitespace is OK anywhere, accepts numbers / strings / booleans in the
 * obvious way, and ignores anything it does not understand. This is
 * enough for the engine's own files and gives tests a real serialize
 * → deserialize round trip without pulling in a JSON library.
 */

#include "EditorLayoutManager.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace SparkEditor
{

    namespace fs = std::filesystem;

    EditorLayoutManager::EditorLayoutManager() = default;
    EditorLayoutManager::~EditorLayoutManager() = default;

    // ========================================================================
    // Lifecycle
    // ========================================================================

    bool EditorLayoutManager::Initialize(const std::string& layoutDirectory)
    {
        m_layoutDirectory = layoutDirectory.empty() ? std::string("Layouts") : layoutDirectory;

        std::error_code ec;
        if (!fs::exists(m_layoutDirectory, ec))
        {
            fs::create_directories(m_layoutDirectory, ec);
            if (ec)
            {
                // Failed to create the directory — still mark initialized
                // so the editor can continue running without persistence.
                m_initialized = true;
                m_currentLayoutName = "Default";
                return false;
            }
        }

        m_currentLayoutName = "Default";
        m_initialized = true;
        return true;
    }

    void EditorLayoutManager::Shutdown()
    {
        m_panels.clear();
        m_defaults.clear();
        m_panelOrder.clear();
        m_currentLayoutName = "Default";
        m_initialized = false;
    }

    // ========================================================================
    // Panel registration
    // ========================================================================

    void EditorLayoutManager::RegisterPanel(const PanelConfig& config)
    {
        if (config.name.empty())
            return;

        if (m_panels.find(config.name) == m_panels.end())
        {
            m_panelOrder.push_back(config.name);
            m_defaults[config.name] = config;
        }
        m_panels[config.name] = config;
    }

    void EditorLayoutManager::SetPanelVisible(const std::string& panelName, bool visible)
    {
        auto it = m_panels.find(panelName);
        if (it == m_panels.end())
            return;
        it->second.isVisible = visible;
    }

    bool EditorLayoutManager::IsPanelVisible(const std::string& panelName) const
    {
        auto it = m_panels.find(panelName);
        if (it == m_panels.end())
            return false;
        return it->second.isVisible;
    }

    void EditorLayoutManager::SetPanelPosition(const std::string& panelName, float x, float y)
    {
        auto it = m_panels.find(panelName);
        if (it == m_panels.end())
            return;
        it->second.posX = x;
        it->second.posY = y;
    }

    void EditorLayoutManager::SetPanelSize(const std::string& panelName, float w, float h)
    {
        auto it = m_panels.find(panelName);
        if (it == m_panels.end())
            return;
        it->second.sizeX = w;
        it->second.sizeY = h;
    }

    const PanelConfig* EditorLayoutManager::GetPanelConfig(const std::string& panelName) const
    {
        auto it = m_panels.find(panelName);
        if (it == m_panels.end())
            return nullptr;
        return &it->second;
    }

    std::vector<std::string> EditorLayoutManager::GetRegisteredPanels() const
    {
        return m_panelOrder;
    }

    void EditorLayoutManager::ResetToDefault()
    {
        m_panels = m_defaults;
    }

    // ========================================================================
    // File paths & JSON helpers
    // ========================================================================

    std::string EditorLayoutManager::LayoutFilePath(const std::string& name) const
    {
        fs::path p = m_layoutDirectory;
        p /= (name + ".json");
        return p.string();
    }

    std::string EditorLayoutManager::EscapeJsonString(const std::string& s)
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

    // ========================================================================
    // Save
    // ========================================================================

    bool EditorLayoutManager::WriteLayoutFile(const std::string& path, const std::string& name,
                                              const std::string& description) const
    {
        std::ofstream f(path, std::ios::trunc);
        if (!f.is_open())
            return false;

        f << "{\n";
        f << "  \"layout\": {\n";
        f << "    \"name\": \"" << EscapeJsonString(name) << "\",\n";
        f << "    \"description\": \"" << EscapeJsonString(description) << "\",\n";
        f << "    \"version\": 1,\n";
        f << "    \"panels\": [\n";

        bool first = true;
        for (const std::string& panelName : m_panelOrder)
        {
            auto it = m_panels.find(panelName);
            if (it == m_panels.end())
                continue;
            const PanelConfig& p = it->second;

            if (!first)
                f << ",\n";
            first = false;

            f << "      {\n";
            f << "        \"name\": \"" << EscapeJsonString(p.name) << "\",\n";
            f << "        \"displayName\": \"" << EscapeJsonString(p.displayName) << "\",\n";
            f << "        \"dock\": " << static_cast<int>(p.dockPosition) << ",\n";
            f << "        \"sizeX\": " << p.sizeX << ",\n";
            f << "        \"sizeY\": " << p.sizeY << ",\n";
            f << "        \"posX\": " << p.posX << ",\n";
            f << "        \"posY\": " << p.posY << ",\n";
            f << "        \"visible\": " << (p.isVisible ? "true" : "false") << ",\n";
            f << "        \"floating\": " << (p.isFloating ? "true" : "false") << ",\n";
            f << "        \"canClose\": " << (p.canClose ? "true" : "false") << ",\n";
            f << "        \"canDock\": " << (p.canDock ? "true" : "false") << ",\n";
            f << "        \"dockRatio\": " << p.dockRatio << ",\n";
            f << "        \"tabOrder\": " << p.tabOrder << ",\n";
            f << "        \"parentDock\": \"" << EscapeJsonString(p.parentDock) << "\"\n";
            f << "      }";
        }
        f << "\n    ]\n";
        f << "  }\n}\n";
        return f.good();
    }

    bool EditorLayoutManager::SaveCurrentLayout(const std::string& name, const std::string& description)
    {
        if (!m_initialized || name.empty())
            return false;

        std::error_code ec;
        fs::create_directories(m_layoutDirectory, ec);

        const std::string path = LayoutFilePath(name);
        if (!WriteLayoutFile(path, name, description))
            return false;

        m_currentLayoutName = name;
        return true;
    }

    // ========================================================================
    // Load — minimal JSON walker
    // ========================================================================

    namespace
    {
        struct Cursor
        {
            const std::string& s;
            size_t pos = 0;

            explicit Cursor(const std::string& str) : s(str) {}

            bool Eof() const { return pos >= s.size(); }

            void SkipWhitespaceAndPunct()
            {
                while (pos < s.size())
                {
                    char c = s[pos];
                    if (std::isspace(static_cast<unsigned char>(c)) || c == ',' || c == ':')
                    {
                        ++pos;
                    }
                    else
                    {
                        break;
                    }
                }
            }

            // Seek to the next key-value pair starting point after a `{`
            // or `[` token. Used to step past structural noise.
            void SkipTo(char target)
            {
                while (pos < s.size() && s[pos] != target)
                    ++pos;
                if (pos < s.size())
                    ++pos;
            }

            // Find a key `"name"` at any position >= pos; move pos to the
            // character after the closing quote + colon. Returns true if
            // the key was found.
            bool FindKey(const std::string& key)
            {
                const std::string quoted = std::string("\"") + key + "\"";
                const size_t found = s.find(quoted, pos);
                if (found == std::string::npos)
                    return false;
                pos = found + quoted.size();
                SkipWhitespaceAndPunct();
                return true;
            }

            std::string ReadString()
            {
                SkipWhitespaceAndPunct();
                if (pos >= s.size() || s[pos] != '"')
                    return {};
                ++pos; // opening quote
                std::string out;
                while (pos < s.size() && s[pos] != '"')
                {
                    if (s[pos] == '\\' && pos + 1 < s.size())
                    {
                        char e = s[pos + 1];
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
                        pos += 2;
                    }
                    else
                    {
                        out += s[pos];
                        ++pos;
                    }
                }
                if (pos < s.size())
                    ++pos; // closing quote
                return out;
            }

            bool ReadBool()
            {
                SkipWhitespaceAndPunct();
                if (pos + 4 <= s.size() && s.compare(pos, 4, "true") == 0)
                {
                    pos += 4;
                    return true;
                }
                if (pos + 5 <= s.size() && s.compare(pos, 5, "false") == 0)
                {
                    pos += 5;
                    return false;
                }
                return false;
            }

            double ReadNumber()
            {
                SkipWhitespaceAndPunct();
                size_t start = pos;
                while (pos < s.size())
                {
                    char c = s[pos];
                    if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E')
                        ++pos;
                    else
                        break;
                }
                if (start == pos)
                    return 0.0;
                try
                {
                    return std::stod(s.substr(start, pos - start));
                }
                catch (...)
                {
                    return 0.0;
                }
            }
        };

        // Read one panel object starting at `cursor.pos` pointing at the
        // opening `{`. Advances `cursor.pos` past the closing `}`.
        bool ParsePanel(Cursor& cursor, PanelConfig& out)
        {
            cursor.SkipWhitespaceAndPunct();
            if (cursor.pos >= cursor.s.size() || cursor.s[cursor.pos] != '{')
                return false;

            // Find the matching closing brace so we restrict key lookups
            // to this object.
            int depth = 0;
            size_t objStart = cursor.pos;
            size_t objEnd = std::string::npos;
            for (size_t i = cursor.pos; i < cursor.s.size(); ++i)
            {
                char c = cursor.s[i];
                if (c == '{')
                    ++depth;
                else if (c == '}')
                {
                    --depth;
                    if (depth == 0)
                    {
                        objEnd = i;
                        break;
                    }
                }
            }
            if (objEnd == std::string::npos)
                return false;

            const std::string objStr = cursor.s.substr(objStart, objEnd - objStart + 1);
            Cursor sub(objStr);

            if (sub.FindKey("name"))
                out.name = sub.ReadString();
            sub.pos = 0;
            if (sub.FindKey("displayName"))
                out.displayName = sub.ReadString();
            sub.pos = 0;
            if (sub.FindKey("dock"))
                out.dockPosition = static_cast<LayoutDockPosition>(static_cast<int>(sub.ReadNumber()));
            sub.pos = 0;
            if (sub.FindKey("sizeX"))
                out.sizeX = static_cast<float>(sub.ReadNumber());
            sub.pos = 0;
            if (sub.FindKey("sizeY"))
                out.sizeY = static_cast<float>(sub.ReadNumber());
            sub.pos = 0;
            if (sub.FindKey("posX"))
                out.posX = static_cast<float>(sub.ReadNumber());
            sub.pos = 0;
            if (sub.FindKey("posY"))
                out.posY = static_cast<float>(sub.ReadNumber());
            sub.pos = 0;
            if (sub.FindKey("visible"))
                out.isVisible = sub.ReadBool();
            sub.pos = 0;
            if (sub.FindKey("floating"))
                out.isFloating = sub.ReadBool();
            sub.pos = 0;
            if (sub.FindKey("canClose"))
                out.canClose = sub.ReadBool();
            sub.pos = 0;
            if (sub.FindKey("canDock"))
                out.canDock = sub.ReadBool();
            sub.pos = 0;
            if (sub.FindKey("dockRatio"))
                out.dockRatio = static_cast<float>(sub.ReadNumber());
            sub.pos = 0;
            if (sub.FindKey("tabOrder"))
                out.tabOrder = static_cast<int>(sub.ReadNumber());
            sub.pos = 0;
            if (sub.FindKey("parentDock"))
                out.parentDock = sub.ReadString();

            cursor.pos = objEnd + 1;
            return !out.name.empty();
        }
    } // namespace

    bool EditorLayoutManager::ReadLayoutFile(const std::string& path, std::string& outDescription)
    {
        std::ifstream f(path);
        if (!f.is_open())
            return false;

        std::stringstream buffer;
        buffer << f.rdbuf();
        const std::string contents = buffer.str();
        if (contents.empty())
            return false;

        Cursor cursor(contents);

        outDescription.clear();
        if (cursor.FindKey("description"))
            outDescription = cursor.ReadString();

        cursor.pos = 0;
        if (!cursor.FindKey("panels"))
            return false;

        cursor.SkipTo('[');
        while (!cursor.Eof())
        {
            cursor.SkipWhitespaceAndPunct();
            if (cursor.pos >= contents.size() || contents[cursor.pos] == ']')
                break;

            PanelConfig panel;
            if (!ParsePanel(cursor, panel))
                break;

            // Only apply to panels that are already registered.
            auto it = m_panels.find(panel.name);
            if (it != m_panels.end())
            {
                it->second = panel;
            }
        }
        return true;
    }

    bool EditorLayoutManager::LoadLayout(const std::string& name)
    {
        if (!m_initialized || name.empty())
            return false;

        const std::string path = LayoutFilePath(name);
        std::error_code ec;
        if (!fs::exists(path, ec))
            return false;

        std::string description;
        if (!ReadLayoutFile(path, description))
            return false;

        m_currentLayoutName = name;
        return true;
    }

    bool EditorLayoutManager::DeleteLayout(const std::string& name)
    {
        if (name.empty())
            return false;

        const std::string path = LayoutFilePath(name);
        std::error_code ec;
        if (!fs::exists(path, ec))
            return false;

        fs::remove(path, ec);
        return !ec;
    }

    std::vector<LayoutInfo> EditorLayoutManager::GetSavedLayouts()
    {
        std::vector<LayoutInfo> result;
        std::error_code ec;
        if (!fs::exists(m_layoutDirectory, ec))
            return result;

        for (const auto& entry : fs::directory_iterator(m_layoutDirectory, ec))
        {
            if (ec)
                break;
            if (!entry.is_regular_file())
                continue;
            const fs::path& p = entry.path();
            if (p.extension() != ".json")
                continue;

            LayoutInfo info;
            info.name = p.stem().string();
            info.filePath = p.string();

            // Peek at the description field — a failed read is fine, just
            // means no description for this entry.
            std::ifstream f(info.filePath);
            if (f.is_open())
            {
                std::stringstream buf;
                buf << f.rdbuf();
                const std::string content = buf.str();
                Cursor cursor(content);
                if (cursor.FindKey("description"))
                    info.description = cursor.ReadString();
            }
            result.push_back(std::move(info));
        }
        std::sort(result.begin(), result.end(),
                  [](const LayoutInfo& a, const LayoutInfo& b) { return a.name < b.name; });
        return result;
    }

    std::string EditorLayoutManager::Console_GetStatus() const
    {
        std::ostringstream os;
        os << "EditorLayoutManager: dir='" << m_layoutDirectory << "' panels=" << m_panels.size() << " current='"
           << m_currentLayoutName << "'";
        return os.str();
    }

} // namespace SparkEditor
