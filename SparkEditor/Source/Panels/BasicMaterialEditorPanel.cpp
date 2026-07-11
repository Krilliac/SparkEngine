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
 */

#include "BasicMaterialEditorPanel.h"
#include "../Core/EditorIcons.h"
#include "Graphics/GraphicsEngine.h"
#include "Utils/LogMacros.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
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

        std::string ToLower(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        }

        /// @brief Material texture paths are relative to Assets/ unless already prefixed
        ///        (same rule as GetOrLoadBasicMaterial's prefixAssets).
        std::string PrefixAssets(std::string p)
        {
            if (p.rfind("Assets/", 0) != 0 && p.rfind("Assets\\", 0) != 0)
                p = "Assets/" + p;
            return p;
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

    std::string BasicMaterialEditorPanel::ResolveAssetDiskPath(const std::string& assetRelPath) const
    {
        if (assetRelPath.empty())
            return {};
        return m_assetsPrefix + PrefixAssets(assetRelPath);
    }

    bool BasicMaterialEditorPanel::TexturePathExists(const char* buf) const
    {
        if (!buf[0])
            return true; // empty = key omitted on save; engine falls back to defaults
        std::error_code ec;
        return fs::exists(ResolveAssetDiskPath(buf), ec);
    }

    void BasicMaterialEditorPanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            RenderToolbar();
            ImGui::Separator();

            const float listWidth = 240.0f;
            ImGui::BeginChild("BasicMatList", ImVec2(listWidth, 0), true);
            RenderMaterialList();
            ImGui::EndChild();

            ImGui::SameLine();

            ImGui::BeginChild("BasicMatDetails", ImVec2(0, 0), true);
            if (m_selected >= 0 && m_selected < static_cast<int>(m_materials.size()))
            {
                RenderMaterialProperties(m_materials[static_cast<size_t>(m_selected)]);
            }
            else
            {
                ImGui::TextDisabled("Select a material JSON to edit.");
                if (m_materials.empty())
                {
                    ImGui::Spacing();
                    ImGui::TextWrapped("No material JSONs found under '%sAssets/Materials'.", m_assetsPrefix.c_str());
                }
            }
            ImGui::EndChild();
        }
        EndPanel();
    }

    void BasicMaterialEditorPanel::RenderToolbar()
    {
        MaterialDoc* selected = (m_selected >= 0 && m_selected < static_cast<int>(m_materials.size()))
                                    ? &m_materials[static_cast<size_t>(m_selected)]
                                    : nullptr;

        ImGui::BeginDisabled(selected == nullptr || !selected->modified);
        if (ImGui::Button(ICON_FA_SAVE " Save"))
        {
            if (selected)
                SaveDoc(*selected);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Write the selected material back to its JSON file\n(also refreshes the engine's material cache)");

        ImGui::SameLine();
        ImGui::BeginDisabled(selected == nullptr);
        if (ImGui::Button(ICON_FA_UNDO " Revert"))
        {
            if (selected)
                LoadDoc(*selected);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Reload the selected material from disk, discarding edits");

        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_SYNC_ALT " Rescan"))
        {
            ScanMaterials();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Rescan Assets/Materials (discards ALL unsaved edits)");

        ImGui::SameLine();
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputTextWithHint("##BasicMatFilter", ICON_FA_SEARCH " filter", m_searchFilter, sizeof(m_searchFilter));

        int modifiedCount = 0;
        for (const auto& doc : m_materials)
        {
            if (doc.modified)
                ++modifiedCount;
        }
        if (modifiedCount > 0)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.96f, 0.65f, 0.14f, 1.0f), "%d unsaved", modifiedCount);
        }
    }

    void BasicMaterialEditorPanel::RenderMaterialList()
    {
        const std::string filter = ToLower(m_searchFilter);

        std::string currentGroup = "\x01"; // sentinel that can't match a real group
        bool groupOpen = false;
        for (int i = 0; i < static_cast<int>(m_materials.size()); ++i)
        {
            const MaterialDoc& doc = m_materials[static_cast<size_t>(i)];

            if (!filter.empty() && ToLower(doc.fileName).find(filter) == std::string::npos &&
                ToLower(doc.name).find(filter) == std::string::npos)
                continue;

            if (doc.group != currentGroup)
            {
                currentGroup = doc.group;
                const std::string header =
                    std::string(ICON_FA_FOLDER " ") + (currentGroup.empty() ? "Materials" : currentGroup);
                groupOpen = ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
            }
            if (!groupOpen)
                continue;

            char label[320];
            std::snprintf(label, sizeof(label), "%s%s##bm%d", doc.modified ? "* " : "", doc.fileName.c_str(), i);
            if (ImGui::Selectable(label, m_selected == i))
                m_selected = i;
            if (!doc.loadError.empty() && ImGui::IsItemHovered())
                ImGui::SetTooltip("Load error: %s", doc.loadError.c_str());
        }
    }

    bool BasicMaterialEditorPanel::RenderTextureField(const char* label, char* buf, size_t bufSize)
    {
        const bool changed = ImGui::InputText(label, buf, bufSize);
        if (!TexturePathExists(buf))
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), ICON_FA_EXCLAMATION_TRIANGLE);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("File not found: %s", ResolveAssetDiskPath(buf).c_str());
        }
        return changed;
    }

    void BasicMaterialEditorPanel::RenderThumbnail(const char* label, const char* assetRelPath, float tilingX,
                                                   float tilingY)
    {
        ImGui::BeginGroup();
        ImGui::TextDisabled("%s", label);

        const std::string disk = ResolveAssetDiskPath(assetRelPath ? assetRelPath : "");
        ID3D11ShaderResourceView* srv = nullptr;
        if (m_graphics && !disk.empty())
        {
            std::error_code ec;
            if (fs::exists(disk, ec))
                srv = m_graphics->GetOrLoadTextureSRV(disk);
        }

        if (srv)
        {
            ImGui::Image(static_cast<void*>(srv), ImVec2(m_thumbSize, m_thumbSize), ImVec2(0.0f, 0.0f),
                         ImVec2(tilingX, tilingY));
        }
        else
        {
            // Placeholder box so the row keeps its layout without a texture.
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            dl->AddRectFilled(pos, ImVec2(pos.x + m_thumbSize, pos.y + m_thumbSize), IM_COL32(30, 32, 38, 255), 2.0f);
            dl->AddRect(pos, ImVec2(pos.x + m_thumbSize, pos.y + m_thumbSize), IM_COL32(70, 75, 85, 255), 2.0f);
            const char* text = (assetRelPath && assetRelPath[0]) ? "n/a" : "none";
            const ImVec2 ts = ImGui::CalcTextSize(text);
            dl->AddText(ImVec2(pos.x + (m_thumbSize - ts.x) * 0.5f, pos.y + (m_thumbSize - ts.y) * 0.5f),
                        IM_COL32(120, 125, 135, 255), text);
            ImGui::Dummy(ImVec2(m_thumbSize, m_thumbSize));
        }
        ImGui::EndGroup();
    }

    void BasicMaterialEditorPanel::RenderMaterialProperties(MaterialDoc& doc)
    {
        ImGui::Text(ICON_FA_PALETTE " %s", doc.fileName.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", doc.enginePath.c_str());
        if (doc.modified)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.96f, 0.65f, 0.14f, 1.0f), "(Modified)");
        }
        if (!doc.loaded)
        {
            ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "Failed to load: %s", doc.loadError.c_str());
            return;
        }
        ImGui::Separator();

        // --- Identity ---------------------------------------------------------
        if (ImGui::InputText("Name", doc.name, sizeof(doc.name)))
            doc.modified = true;
        ImGui::BeginDisabled(true);
        ImGui::InputText("Shader", doc.shader, sizeof(doc.shader));
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Only the basic PBR path is supported by these materials");

        // --- Textures ---------------------------------------------------------
        ImGui::Spacing();
        ImGui::Text(ICON_FA_IMAGE " Textures");
        ImGui::Separator();
        if (RenderTextureField("Albedo", doc.albedoPath, sizeof(doc.albedoPath)))
            doc.modified = true;
        if (RenderTextureField("Normal", doc.normalPath, sizeof(doc.normalPath)))
            doc.modified = true;

        bool roughTex = doc.roughnessIsTexture;
        if (ImGui::Checkbox("Roughness from texture", &roughTex))
        {
            doc.roughnessIsTexture = roughTex;
            doc.modified = true;
        }
        if (doc.roughnessIsTexture)
        {
            if (RenderTextureField("Roughness Map", doc.roughnessPath, sizeof(doc.roughnessPath)))
                doc.modified = true;
        }
        else
        {
            if (ImGui::DragFloat("Roughness", &doc.roughness, 0.005f, 0.0f, 1.0f, "%.3f"))
                doc.modified = true;
        }

        // --- Scalars ----------------------------------------------------------
        ImGui::Spacing();
        ImGui::Text(ICON_FA_COG " Surface");
        ImGui::Separator();
        if (ImGui::DragFloat("Metallic", &doc.metallic, 0.005f, 0.0f, 1.0f, "%.3f"))
            doc.modified = true;
        if (ImGui::DragFloat("AO", &doc.ao, 0.005f, 0.0f, 1.0f, "%.3f"))
            doc.modified = true;

        bool hasTiling = doc.hasTiling;
        if (ImGui::Checkbox("Tiling", &hasTiling))
        {
            doc.hasTiling = hasTiling;
            doc.modified = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Unchecked: the tiling key is omitted (engine default 1x1)");
        if (doc.hasTiling)
        {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::DragFloat2("##TilingXY", doc.tiling, 0.05f, 0.01f, 64.0f, "%.2f"))
                doc.modified = true;
        }

        // --- Preview ----------------------------------------------------------
        ImGui::Spacing();
        ImGui::Text(ICON_FA_CUBE " Preview");
        ImGui::Separator();
        if (!m_graphics)
        {
            ImGui::TextDisabled("No GraphicsEngine attached — thumbnails unavailable.");
            return;
        }

        RenderThumbnail("Albedo", doc.albedoPath, 1.0f, 1.0f);
        ImGui::SameLine();
        RenderThumbnail("Normal", doc.normalPath, 1.0f, 1.0f);
        if (doc.roughnessIsTexture)
        {
            ImGui::SameLine();
            RenderThumbnail("Roughness", doc.roughnessPath, 1.0f, 1.0f);
        }
        ImGui::SameLine();
        // ImGui's D3D11 backend samples with ADDRESS_WRAP, so uv1 = tiling shows
        // the real repeat the basic shader will produce.
        RenderThumbnail("Tiled", doc.albedoPath, doc.hasTiling ? doc.tiling[0] : 1.0f,
                        doc.hasTiling ? doc.tiling[1] : 1.0f);
    }

} // namespace SparkEditor
