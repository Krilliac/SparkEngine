/**
 * @file BasicMaterialEditorPanel.cpp
 * @brief Editor for the basic-path material JSONs under Assets/Materials
 * @author Spark Engine Team
 * @date 2026
 *
 * Parsing mirrors GraphicsEngine::GetOrLoadBasicMaterial's minimal extractor
 * (string keys, roughness path-or-scalar disambiguated by peeking the first
 * non-whitespace character after the colon, tiling as a 2-float array) so what
 * this panel writes is exactly what the engine reads back.
 *
 * Contains: construction/lifecycle, ScanMaterials, LoadDoc, SaveDoc. The
 * implementation is split across sibling files:
 *  - BasicMaterialEditorDrawing.cpp — ImGui drawing (toolbar, list, properties, thumbnails)
 *  - BasicMaterialEditorInternal.h  — shared helpers for the split
 */

#include "BasicMaterialEditorPanel.h"

#include "BasicMaterialEditorInternal.h"
#include "Graphics/GraphicsEngine.h"
#include "Utils/LogMacros.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

namespace fs = std::filesystem;

namespace SparkEditor
{

    namespace
    {
        /// @brief Extract a quoted string value for @p key ("key" : "value"). Empty if absent.
        std::string FindStringValue(const std::string& content, const char* key)
        {
            const size_t kp = content.find(std::string("\"") + key + "\"");
            if (kp == std::string::npos)
                return {};
            const size_t colon = content.find(':', kp);
            if (colon == std::string::npos)
                return {};
            const size_t q1 = content.find('"', colon + 1);
            if (q1 == std::string::npos)
                return {};
            const size_t q2 = content.find('"', q1 + 1);
            if (q2 == std::string::npos)
                return {};
            return content.substr(q1 + 1, q2 - q1 - 1);
        }

        /// @brief Extract a scalar numeric value for @p key. Returns false if absent or non-numeric.
        bool FindNumberValue(const std::string& content, const char* key, float& out)
        {
            const size_t kp = content.find(std::string("\"") + key + "\"");
            if (kp == std::string::npos)
                return false;
            const size_t colon = content.find(':', kp);
            if (colon == std::string::npos)
                return false;
            const size_t vp = content.find_first_not_of(" \t\r\n", colon + 1);
            if (vp == std::string::npos || content[vp] == '"')
                return false;
            float v = 0.0f;
            if (sscanf_s(content.c_str() + vp, "%f", &v) != 1)
                return false;
            out = v;
            return true;
        }

        /// @brief Minimal JSON string escape (backslash + double quote).
        std::string EscapeJson(const std::string& s)
        {
            std::string out;
            out.reserve(s.size());
            for (char c : s)
            {
                if (c == '\\' || c == '"')
                    out.push_back('\\');
                out.push_back(c);
            }
            return out;
        }

        /// @brief Compact float formatting for JSON output ("0.3", "8", "1").
        std::string FormatNumber(float v)
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.6g", static_cast<double>(v));
            return buf;
        }

        /// @brief Copy a std::string into a fixed char buffer with truncation.
        void CopyToBuf(char* buf, size_t bufSize, const std::string& s)
        {
            std::snprintf(buf, bufSize, "%s", s.c_str());
        }
    } // namespace

    BasicMaterialEditorPanel::BasicMaterialEditorPanel() : EditorPanel("Basic Materials", "basic_material_editor_panel")
    {
        m_category = PanelCategory::Tool;
    }

    bool BasicMaterialEditorPanel::Initialize()
    {
        // The editor normally runs with the repo root as cwd (SceneViewPanel loads
        // "Assets/Models/..." relative), but probe a few parents so the panel also
        // works when launched from a build output directory.
        m_assetsPrefix.clear();
        for (const char* prefix : {"", "../", "../../", "../../../"})
        {
            std::error_code ec;
            if (fs::exists(fs::path(prefix) / "Assets" / "Materials", ec))
            {
                m_assetsPrefix = prefix;
                break;
            }
        }

        ScanMaterials();
        SPARK_LOG_INFO(Spark::LogCategory::Editor,
                       "BasicMaterialEditorPanel: found %d material JSON(s) under '%sAssets/Materials'",
                       static_cast<int>(m_materials.size()), m_assetsPrefix.c_str());
        return true;
    }

    void BasicMaterialEditorPanel::Update(float /*deltaTime*/) {}

    void BasicMaterialEditorPanel::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Shutting down Basic Materials panel");
    }

    void BasicMaterialEditorPanel::ScanMaterials()
    {
        m_materials.clear();
        m_selected = -1;

        const fs::path matRoot = fs::path(m_assetsPrefix) / "Assets" / "Materials";
        std::error_code ec;
        if (!fs::exists(matRoot, ec))
            return;

        fs::recursive_directory_iterator it(matRoot, fs::directory_options::skip_permission_denied, ec);
        const fs::recursive_directory_iterator end;
        for (; !ec && it != end; it.increment(ec))
        {
            std::error_code fec;
            if (!it->is_regular_file(fec))
                continue;
            if (ToLower(it->path().extension().string()) != ".json")
                continue;

            MaterialDoc doc;
            doc.diskPath = it->path().generic_string();
            const fs::path rel = fs::relative(it->path(), matRoot, fec);
            doc.enginePath = (fs::path("Assets/Materials") / rel).generic_string();
            doc.group = rel.has_parent_path() ? rel.parent_path().generic_string() : std::string();
            doc.fileName = it->path().filename().string();
            LoadDoc(doc);
            m_materials.push_back(std::move(doc));
        }

        std::sort(m_materials.begin(), m_materials.end(), [](const MaterialDoc& a, const MaterialDoc& b)
                  { return a.group != b.group ? a.group < b.group : a.fileName < b.fileName; });
    }

    bool BasicMaterialEditorPanel::LoadDoc(MaterialDoc& doc)
    {
        doc.loaded = false;
        doc.modified = false;
        doc.loadError.clear();

        std::ifstream file(doc.diskPath, std::ios::binary);
        if (!file.is_open())
        {
            doc.loadError = "cannot open file";
            return false;
        }
        std::stringstream ss;
        ss << file.rdbuf();
        const std::string content = ss.str();

        std::string name = FindStringValue(content, "name");
        if (name.empty())
        {
            name = doc.fileName;
            const size_t dot = name.rfind('.');
            if (dot != std::string::npos)
                name.resize(dot);
        }
        CopyToBuf(doc.name, sizeof(doc.name), name);

        const std::string shader = FindStringValue(content, "shader");
        CopyToBuf(doc.shader, sizeof(doc.shader), shader.empty() ? "PBR" : shader);

        CopyToBuf(doc.albedoPath, sizeof(doc.albedoPath), FindStringValue(content, "albedo"));
        CopyToBuf(doc.normalPath, sizeof(doc.normalPath), FindStringValue(content, "normal"));

        // roughness: string path OR scalar — peek the first non-whitespace character
        // after the colon, exactly like GetOrLoadBasicMaterial.
        doc.roughnessIsTexture = false;
        doc.roughnessPath[0] = '\0';
        doc.roughness = 0.5f;
        const size_t rp = content.find("\"roughness\"");
        if (rp != std::string::npos)
        {
            const size_t colon = content.find(':', rp);
            const size_t vp =
                (colon == std::string::npos) ? std::string::npos : content.find_first_not_of(" \t\r\n", colon + 1);
            if (vp != std::string::npos)
            {
                if (content[vp] == '"')
                {
                    const size_t q2 = content.find('"', vp + 1);
                    if (q2 != std::string::npos && q2 > vp + 1)
                    {
                        doc.roughnessIsTexture = true;
                        CopyToBuf(doc.roughnessPath, sizeof(doc.roughnessPath), content.substr(vp + 1, q2 - vp - 1));
                    }
                }
                else
                {
                    float r = 0.0f;
                    if (sscanf_s(content.c_str() + vp, "%f", &r) == 1)
                        doc.roughness = r;
                }
            }
        }

        doc.metallic = 0.0f;
        FindNumberValue(content, "metallic", doc.metallic);
        doc.ao = 1.0f;
        FindNumberValue(content, "ao", doc.ao);

        // tiling: [x, y] — tolerate whitespace/newlines between the numbers.
        doc.hasTiling = false;
        doc.tiling[0] = doc.tiling[1] = 1.0f;
        const size_t tp = content.find("\"tiling\"");
        if (tp != std::string::npos)
        {
            const size_t open = content.find('[', tp);
            const size_t close = (open == std::string::npos) ? std::string::npos : content.find(']', open);
            if (open != std::string::npos && close != std::string::npos)
            {
                std::string arr = content.substr(open + 1, close - open - 1);
                for (char& c : arr)
                {
                    if (c == ',')
                        c = ' ';
                }
                std::istringstream as(arr);
                float tx = 1.0f, ty = 1.0f;
                if (as >> tx >> ty)
                {
                    doc.tiling[0] = tx;
                    doc.tiling[1] = ty;
                    doc.hasTiling = true;
                }
            }
        }

        // Preserve unknown top-level keys verbatim (e.g. "emissive", "doubleSided" in
        // the MMO materials) so Save never drops fields this panel doesn't edit. Flat
        // scanner — the schema has no nested objects; a value is a string, an array,
        // or a bare token.
        doc.extraFields.clear();
        {
            static constexpr const char* kKnownKeys[] = {"name",      "shader",   "albedo", "normal",
                                                         "roughness", "metallic", "ao",     "tiling"};
            size_t pos = content.find('{');
            pos = (pos == std::string::npos) ? 0 : pos + 1;
            while (true)
            {
                const size_t k1 = content.find('"', pos);
                if (k1 == std::string::npos)
                    break;
                const size_t k2 = content.find('"', k1 + 1);
                if (k2 == std::string::npos)
                    break;
                const std::string key = content.substr(k1 + 1, k2 - k1 - 1);
                const size_t colon = content.find(':', k2);
                if (colon == std::string::npos)
                    break;
                const size_t vstart = content.find_first_not_of(" \t\r\n", colon + 1);
                if (vstart == std::string::npos)
                    break;
                size_t vend;
                if (content[vstart] == '"')
                {
                    vend = content.find('"', vstart + 1);
                    if (vend == std::string::npos)
                        break;
                    ++vend;
                }
                else if (content[vstart] == '[')
                {
                    vend = content.find(']', vstart);
                    if (vend == std::string::npos)
                        break;
                    ++vend;
                }
                else
                {
                    vend = content.find_first_of(",}\r\n", vstart);
                    if (vend == std::string::npos)
                        vend = content.size();
                }

                const bool known = std::any_of(std::begin(kKnownKeys), std::end(kKnownKeys),
                                               [&key](const char* k) { return key == k; });
                if (!known)
                {
                    std::string value = content.substr(vstart, vend - vstart);
                    while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
                        value.pop_back();
                    doc.extraFields.push_back("\"" + key + "\": " + value);
                }
                pos = vend;
            }
        }

        doc.loaded = true;
        return true;
    }

    bool BasicMaterialEditorPanel::SaveDoc(MaterialDoc& doc)
    {
        // Stable field order matching the shipped materials:
        // name, shader, albedo, normal, metallic, roughness, ao, tiling.
        std::vector<std::string> fields;
        fields.push_back("\"name\": \"" + EscapeJson(doc.name) + "\"");
        fields.push_back("\"shader\": \"" + EscapeJson(doc.shader[0] ? doc.shader : "PBR") + "\"");
        if (doc.albedoPath[0])
            fields.push_back("\"albedo\": \"" + EscapeJson(doc.albedoPath) + "\"");
        if (doc.normalPath[0])
            fields.push_back("\"normal\": \"" + EscapeJson(doc.normalPath) + "\"");
        fields.push_back("\"metallic\": " + FormatNumber(doc.metallic));
        if (doc.roughnessIsTexture && doc.roughnessPath[0])
            fields.push_back("\"roughness\": \"" + EscapeJson(doc.roughnessPath) + "\"");
        else
            fields.push_back("\"roughness\": " + FormatNumber(doc.roughness));
        fields.push_back("\"ao\": " + FormatNumber(doc.ao));
        for (const std::string& extra : doc.extraFields)
            fields.push_back(extra);
        if (doc.hasTiling)
            fields.push_back("\"tiling\": [" + FormatNumber(doc.tiling[0]) + ", " + FormatNumber(doc.tiling[1]) + "]");

        std::string out = "{\n";
        for (size_t i = 0; i < fields.size(); ++i)
        {
            out += "    " + fields[i];
            out += (i + 1 < fields.size()) ? ",\n" : "\n";
        }
        out += "}\n";

        std::ofstream file(doc.diskPath, std::ios::binary | std::ios::trunc);
        if (!file.is_open())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "BasicMaterialEditorPanel: cannot write '%s'",
                            doc.diskPath.c_str());
            return false;
        }
        file << out;
        file.close();
        doc.modified = false;

        // Apply-live: drop the engine's cached parse so the Scene View re-reads the
        // JSON on its next GetOrLoadBasicMaterial. Erase both key spellings — the
        // runtime keys by the MeshRenderer's "Assets/Materials/..." path; the disk
        // path only differs when the editor runs outside the repo root.
        if (m_graphics)
        {
            m_graphics->InvalidateBasicMaterial(doc.enginePath);
            if (doc.diskPath != doc.enginePath)
                m_graphics->InvalidateBasicMaterial(doc.diskPath);
        }

        SPARK_LOG_INFO(Spark::LogCategory::Editor, "BasicMaterialEditorPanel: saved '%s'", doc.diskPath.c_str());
        return true;
    }

} // namespace SparkEditor
