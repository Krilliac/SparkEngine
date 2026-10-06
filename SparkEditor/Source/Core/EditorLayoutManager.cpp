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
 * obvious way, and ignores keys it does not understand. This is
 * enough for the engine's own files and gives tests a real serialize
 * → deserialize round trip without pulling in a JSON library.
 *
 * Compatibility (SAVE-230): the reader accepts "version" 1 and treats a file
 * without one as the legacy dialect of version 1. A newer version, a damaged
 * panel or a truncated file fails closed with GetLastError() naming the file,
 * and nothing is applied until the whole file has parsed. Saves replace the
 * file atomically through SaveFileDurability::WriteFileAtomically.
 */

#include "EditorLayoutManager.h"
#include "Utils/SaveFileDurability.h"
#include "Utils/FileUtils.h"
#include "../Utils/EditorFileRead.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

namespace SparkEditor
{

    namespace fs = std::filesystem;

    namespace
    {
        bool IsSafeLayoutName(const std::string& name)
        {
            if (name.empty() || name == "." || name == "..")
                return false;
            return std::none_of(name.begin(), name.end(),
                                [](unsigned char c) { return c < 0x20 || c == '/' || c == '\\' || c == ':'; });
        }
    } // namespace

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
                                              const std::string& description, std::string& error) const
    {
        std::ostringstream f;
        f.imbue(std::locale::classic());
        f << std::setprecision(std::numeric_limits<float>::max_digits10);

        f << "{\n";
        f << "  \"layout\": {\n";
        f << "    \"name\": \"" << EscapeJsonString(name) << "\",\n";
        f << "    \"description\": \"" << EscapeJsonString(description) << "\",\n";
        f << "    \"version\": " << kLayoutFormatVersion << ",\n";
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

        // Staged write and rename: a failed or interrupted save leaves the previous file intact.
        std::error_code writeError;
        if (!Spark::SaveFileDurability::WriteFileAtomically(fs::path(path), f.str(), /*retainBackup*/ false,
                                                            writeError))
        {
            error = "Layout '" + path + "' was not saved: " + writeError.message() + ". The previous file is unchanged";
            return false;
        }
        return true;
    }

    bool EditorLayoutManager::SaveCurrentLayout(const std::string& name, const std::string& description)
    {
        m_lastError.clear();
        if (!m_initialized || !IsSafeLayoutName(name))
        {
            m_lastError =
                "Layout name '" + name + "' is not a valid file name, or the layout manager is not initialized";
            return false;
        }

        std::error_code ec;
        fs::create_directories(m_layoutDirectory, ec);

        const std::string path = LayoutFilePath(name);
        if (!WriteLayoutFile(path, name, description, m_lastError))
        {
            return false;
        }

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

            // Find a key `"name":` at any position >= pos; move pos to the
            // character after the closing quote + colon. Returns true if
            // the key was found. Only a match followed by ':' is a key: a
            // string value equal to the key text ("name": "version") is
            // followed by ',' or '}' and is skipped, so a layout or panel
            // named after a header key cannot shadow that key.
            bool FindKey(const std::string& key)
            {
                const std::string quoted = std::string("\"") + key + "\"";
                size_t found = s.find(quoted, pos);
                while (found != std::string::npos)
                {
                    size_t after = found + quoted.size();
                    while (after < s.size() && std::isspace(static_cast<unsigned char>(s[after])))
                    {
                        ++after;
                    }
                    if (after < s.size() && s[after] == ':')
                    {
                        pos = after;
                        SkipWhitespaceAndPunct();
                        return true;
                    }
                    found = s.find(quoted, found + 1);
                }
                return false;
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

        /// Largest layout file the editor reads; a layout is a few kilobytes of panel state.
        constexpr std::uint64_t kMaxLayoutFileBytes = std::uint64_t{1024} * 1024;

        /// A float field must stay finite as a float. ReadNumber's double used to be narrowed
        /// unchecked, so 1e300 became +inf, which the writer saved as "inf" and the reader then
        /// loaded as 0. An absent key keeps @p out.
        bool ReadFloatField(Cursor& sub, const char* key, float& out)
        {
            sub.pos = 0;
            if (!sub.FindKey(key))
            {
                return true;
            }
            const double value = sub.ReadNumber();
            if (!(std::fabs(value) <= static_cast<double>(std::numeric_limits<float>::max())))
            {
                return false;
            }
            out = static_cast<float>(value);
            return true;
        }

        /// An integer field must fit an int: static_cast<int> of a larger double is undefined
        /// behaviour. In-range fractions still truncate toward zero, as they always did. An
        /// absent key keeps @p out.
        bool ReadIntField(Cursor& sub, const char* key, int& out)
        {
            sub.pos = 0;
            if (!sub.FindKey(key))
            {
                return true;
            }
            const double value = sub.ReadNumber();
            if (!(value > static_cast<double>(std::numeric_limits<int>::min()) - 1.0 &&
                  value < static_cast<double>(std::numeric_limits<int>::max()) + 1.0))
            {
                return false;
            }
            out = static_cast<int>(value);
            return true;
        }

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
            int dock = static_cast<int>(out.dockPosition);
            if (!ReadIntField(sub, "dock", dock) || !ReadFloatField(sub, "sizeX", out.sizeX) ||
                !ReadFloatField(sub, "sizeY", out.sizeY) || !ReadFloatField(sub, "posX", out.posX) ||
                !ReadFloatField(sub, "posY", out.posY))
            {
                return false;
            }
            out.dockPosition = static_cast<LayoutDockPosition>(dock);
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
            if (!ReadFloatField(sub, "dockRatio", out.dockRatio) || !ReadIntField(sub, "tabOrder", out.tabOrder))
            {
                return false;
            }
            sub.pos = 0;
            if (sub.FindKey("parentDock"))
                out.parentDock = sub.ReadString();

            cursor.pos = objEnd + 1;
            return !out.name.empty();
        }
    } // namespace

    bool EditorLayoutManager::ReadLayoutFile(const std::string& path)
    {
        const std::string prefix = "Layout '" + path + "' ";
        // Read through one opened handle, bounded: the whole file used to be read by name
        // without a limit after LoadLayout's by-path existence check.
        std::string contents;
        switch (ReadRegularFileBounded(fs::path(path), kMaxLayoutFileBytes, contents))
        {
        case BoundedReadStatus::Ok:
            break;
        case BoundedReadStatus::TooLarge:
            m_lastError = prefix + "is larger than " + std::to_string(kMaxLayoutFileBytes) + " bytes";
            return false;
        case BoundedReadStatus::Missing:
        case BoundedReadStatus::NotRegularFile:
        case BoundedReadStatus::Failed:
            m_lastError = prefix + "could not be opened";
            return false;
        }
        if (contents.empty())
        {
            m_lastError = prefix + "is empty";
            return false;
        }

        Cursor cursor(contents);
        if (!cursor.FindKey("panels"))
        {
            m_lastError = prefix + "has no \"panels\" array; it is damaged or not a layout file";
            return false;
        }
        const size_t panelsKey = cursor.pos;

        // Only a "version" key before "panels" belongs to the layout header; later keys are inside panel objects.
        cursor.pos = 0;
        long long version = kLayoutFormatVersion; // no version key: the legacy dialect, identical to 1
        if (cursor.FindKey("version") && cursor.pos < panelsKey)
        {
            const double declared = cursor.ReadNumber();
            if (!(declared >= 1.0 && declared <= 1.0e9) || declared != std::floor(declared))
            {
                m_lastError = prefix + "has an invalid \"version\" value; this build reads layout version " +
                              std::to_string(kLayoutFormatVersion);
                return false;
            }
            version = static_cast<long long>(declared);
        }
        if (version > kLayoutFormatVersion)
        {
            m_lastError = prefix + "is layout format version " + std::to_string(version) +
                          "; this build reads layout version " + std::to_string(kLayoutFormatVersion) +
                          " only. Open it with a newer SparkEditor";
            return false;
        }

        // Parse every panel before applying any, so a damaged file changes nothing.
        cursor.pos = panelsKey;
        cursor.SkipTo('[');
        std::vector<PanelConfig> parsed;
        bool closed = false;
        while (true)
        {
            cursor.SkipWhitespaceAndPunct();
            if (cursor.Eof())
            {
                break;
            }
            if (contents[cursor.pos] == ']')
            {
                closed = true;
                ++cursor.pos;
                break;
            }
            PanelConfig panel;
            if (!ParsePanel(cursor, panel))
            {
                m_lastError = prefix + "has a malformed or truncated panel " + std::to_string(parsed.size() + 1) +
                              "; no panel was changed";
                return false;
            }
            parsed.push_back(std::move(panel));
        }
        // The writer closes the panels array, then the "layout" object and the document.
        size_t closingBraces = 0;
        for (; closed && !cursor.Eof(); ++cursor.pos)
        {
            const char c = contents[cursor.pos];
            if (c == '}')
            {
                ++closingBraces;
            }
            else if (!std::isspace(static_cast<unsigned char>(c)))
            {
                break;
            }
        }
        if (!closed || closingBraces != 2 || !cursor.Eof())
        {
            m_lastError = prefix + "is truncated or has content after its panels array; no panel was changed";
            return false;
        }

        // Only apply to panels that are already registered.
        for (PanelConfig& panel : parsed)
        {
            auto it = m_panels.find(panel.name);
            if (it != m_panels.end())
                it->second = std::move(panel);
        }
        return true;
    }

    bool EditorLayoutManager::LoadLayout(const std::string& name)
    {
        m_lastError.clear();
        if (!m_initialized || !IsSafeLayoutName(name))
        {
            m_lastError =
                "Layout name '" + name + "' is not a valid file name, or the layout manager is not initialized";
            return false;
        }

        const std::string path = LayoutFilePath(name);
        if (!ReadLayoutFile(path))
        {
            return false;
        }

        m_currentLayoutName = name;
        return true;
    }

    bool EditorLayoutManager::DeleteLayout(const std::string& name)
    {
        if (!m_initialized || !IsSafeLayoutName(name))
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

            // Layouts are reloaded by name through narrow std::string paths. A file
            // name the Windows ANSI code page cannot spell has none, and
            // path::string() throws for it, which ended the listing: skip it. Once
            // the full path has a narrow spelling, its stem does too.
            std::optional<std::string> narrowPath = Spark::FileUtils::TryPathToNarrow(p);
            if (!narrowPath)
            {
                continue;
            }

            LayoutInfo info;
            info.name = p.stem().string();
            info.filePath = std::move(*narrowPath);

            // Peek at the description field — a failed read is fine, just
            // means no description for this entry.
            std::string content;
            if (ReadRegularFileBounded(p, kMaxLayoutFileBytes, content) == BoundedReadStatus::Ok)
            {
                Cursor cursor(content);
                if (cursor.FindKey("description"))
                {
                    info.description = cursor.ReadString();
                }
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
