/**
 * @file BasicMaterialEditorDrawing.cpp
 * @brief ImGui drawing for the basic-path material editor panel
 * @author Spark Engine Team
 * @date 2026
 *
 * Contains: Render, RenderToolbar, RenderMaterialList, RenderTextureField,
 * RenderThumbnail, RenderMaterialProperties, and the texture-path resolution
 * helpers they use (ResolveAssetDiskPath, TexturePathExists).
 */

#include "BasicMaterialEditorPanel.h"

#include "../Core/EditorIcons.h"
#include "BasicMaterialEditorInternal.h"
#include "Graphics/GraphicsEngine.h"
#include "Graphics/ProjectAssetPath.h"

#include <imgui.h>

#include <cstdio>
#include <filesystem>
#include <string>

#include "SparkUI/Runtime.h"
#include "SparkUI/Widgets.h"

namespace fs = std::filesystem;

namespace SparkEditor
{

    namespace
    {
        fs::path PathFromUtf8(std::string_view value)
        {
            const auto* begin = reinterpret_cast<const char8_t*>(value.data());
            return fs::path(std::u8string(begin, begin + value.size()));
        }

        std::string PathToUtf8(const fs::path& value)
        {
            const auto utf8 = value.generic_u8string();
            return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
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

    fs::path BasicMaterialEditorPanel::ResolveAssetDiskPath(const std::string& assetRelPath) const
    {
        if (assetRelPath.empty())
            return {};
        const auto resolved = Spark::ResolveProjectAssetPath(m_projectRoot, PrefixAssets(assetRelPath));
        return resolved ? resolved->nativePath : fs::path{};
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

            const float listWidth = Spark::Ui::Px(240.0f);
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
                if (m_materials.empty())
                {
                    const std::string materialRoot = PathToUtf8(PathFromUtf8(m_projectRoot) / "Assets" / "Materials");
                    const std::string hint = "No material JSONs found under '" + materialRoot + "'.";
                    Spark::Ui::EmptyState(ICON_FA_PALETTE, "Select a material JSON to edit.", hint.c_str());
                }
                else
                {
                    Spark::Ui::EmptyState(ICON_FA_PALETTE, "Select a material JSON to edit.");
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
        ImGui::SetNextItemWidth(Spark::Ui::Px(180.0f));
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
            Spark::Ui::TextRole(Spark::Ui::Role::WarningText, "%d unsaved", modifiedCount);
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
                groupOpen = Spark::Ui::SectionHeader(header.c_str());
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
            Spark::Ui::TextRole(Spark::Ui::Role::ErrorText, ICON_FA_EXCLAMATION_TRIANGLE);
            if (ImGui::IsItemHovered())
            {
                const std::string missingPath = PathToUtf8(ResolveAssetDiskPath(buf));
                ImGui::SetTooltip("File not found: %s", missingPath.c_str());
            }
        }
        return changed;
    }

    void BasicMaterialEditorPanel::RenderThumbnail(const char* label, const char* assetRelPath, float tilingX,
                                                   float tilingY)
    {
        ImGui::BeginGroup();
        Spark::Ui::SecondaryText("%s", label);

        const float thumbSize = Spark::Ui::Px(m_thumbSize);
        const fs::path disk = ResolveAssetDiskPath(assetRelPath ? assetRelPath : "");
        ID3D11ShaderResourceView* srv = nullptr;
        if (m_graphics && !disk.empty())
        {
            std::error_code ec;
            if (fs::exists(disk, ec))
                srv = m_graphics->GetOrLoadTextureSRV(PathToUtf8(disk));
        }

        if (srv)
        {
            ImGui::Image(static_cast<void*>(srv), ImVec2(thumbSize, thumbSize), ImVec2(0.0f, 0.0f),
                         ImVec2(tilingX, tilingY));
        }
        else
        {
            // Placeholder box so the row keeps its layout without a texture.
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const ImVec2 boxMax(pos.x + thumbSize, pos.y + thumbSize);
            const float rounding = Spark::Ui::Px(2.0f);
            dl->AddRectFilled(pos, boxMax, Spark::Ui::Col(Spark::Ui::Role::SurfaceSunken), rounding);
            dl->AddRect(pos, boxMax, Spark::Ui::Col(Spark::Ui::Role::BorderSubtle), rounding);
            const char* text = (assetRelPath && assetRelPath[0]) ? "n/a" : "none";
            const ImVec2 ts = ImGui::CalcTextSize(text);
            dl->AddText(ImVec2(pos.x + (thumbSize - ts.x) * 0.5f, pos.y + (thumbSize - ts.y) * 0.5f),
                        Spark::Ui::Col(Spark::Ui::Role::TextSecondary), text);
            ImGui::Dummy(ImVec2(thumbSize, thumbSize));
        }
        ImGui::EndGroup();
    }

    void BasicMaterialEditorPanel::RenderMaterialProperties(MaterialDoc& doc)
    {
        ImGui::Text(ICON_FA_PALETTE " %s", doc.fileName.c_str());
        ImGui::SameLine();
        Spark::Ui::SecondaryText("(%s)", doc.enginePath.c_str());
        if (doc.modified)
        {
            ImGui::SameLine();
            Spark::Ui::TextRole(Spark::Ui::Role::WarningText, "(Modified)");
        }
        if (!doc.loaded)
        {
            Spark::Ui::StatusText(Spark::Ui::Status::Error, "Failed to load: %s", doc.loadError.c_str());
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
        Spark::Ui::SubHeader(ICON_FA_IMAGE " Textures");
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
        Spark::Ui::SubHeader(ICON_FA_COG " Surface");
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
            ImGui::SetNextItemWidth(Spark::Ui::Px(160.0f));
            if (ImGui::DragFloat2("##TilingXY", doc.tiling, 0.05f, 0.01f, 64.0f, "%.2f"))
                doc.modified = true;
        }

        // --- Preview ----------------------------------------------------------
        ImGui::Spacing();
        Spark::Ui::SubHeader(ICON_FA_CUBE " Preview");
        if (!m_graphics)
        {
            Spark::Ui::UnavailableFeature("Thumbnail preview", "No GraphicsEngine is attached to the editor.");
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
