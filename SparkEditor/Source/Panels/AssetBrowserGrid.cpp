/**
 * @file AssetBrowserGrid.cpp
 * @brief Asset browser toolbar, thumbnails, drag payloads, and details.
 */
#include "AssetBrowserPanel.h"
#include "AssetBrowserPathUtils.h"
#include "../AssetPipeline/EditorAssetDrag.h"
#include "../Core/EditorIcons.h"
#include "../Core/EditorFonts.h"
#include "Utils/LogMacros.h"
#include <algorithm>
#include <cctype>
#include <imgui.h>
#include "SparkUI/Runtime.h"
#include "SparkUI/Widgets.h"
namespace SparkEditor
{
    using AssetBrowserDetail::PathFromUtf8;
    using AssetBrowserDetail::PathToUtf8;
    // Helper to get icon for file extension
    static const char* GetFileTypeIcon(const std::string& ext)
    {
        if (ext == ".fbx" || ext == ".obj" || ext == ".gltf" || ext == ".glb")
            return ICON_FA_CUBE;
        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp" || ext == ".dds")
            return ICON_FA_IMAGE;
        if (ext == ".wav" || ext == ".mp3" || ext == ".ogg")
            return ICON_FA_VOLUME_UP;
        if (ext == ".cpp" || ext == ".h" || ext == ".hpp" || ext == ".lua")
            return ICON_FA_CODE;
        if (ext == ".hlsl" || ext == ".glsl" || ext == ".shader")
            return ICON_FA_PAINT_BRUSH;
        if (ext == ".mat" || ext == ".material" || ext == ".spkmat")
            return ICON_FA_CIRCLE;
        if (ext == ".scene" || ext == ".map")
            return ICON_FA_MAP;
        if (ext == ".prefab")
            return ICON_FA_SHAPES;
        if (ext == ".ttf" || ext == ".otf")
            return ICON_FA_FONT;
        if (ext == ".json" || ext == ".xml" || ext == ".ini" || ext == ".cfg")
            return ICON_FA_COG;
        return ICON_FA_FILE;
    }

    // File types tint their icon with a theme category colour; the icon shape and the
    // tooltip's "Type" row carry the same information, so colour is never the only cue.
    static ImU32 GetFileTypeColor(const std::string& ext)
    {
        if (ext == ".fbx" || ext == ".obj" || ext == ".gltf" || ext == ".glb")
            return Spark::Ui::CategoryColor(0);
        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga")
            return Spark::Ui::CategoryColor(1);
        if (ext == ".wav" || ext == ".mp3" || ext == ".ogg")
            return Spark::Ui::CategoryColor(2);
        if (ext == ".cpp" || ext == ".h" || ext == ".hpp" || ext == ".lua")
            return Spark::Ui::CategoryColor(3);
        if (ext == ".hlsl" || ext == ".glsl" || ext == ".shader")
            return Spark::Ui::CategoryColor(4);
        return Spark::Ui::Col(Spark::Ui::Role::TextSecondary);
    }

    void AssetBrowserPanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            // Toolbar with icons
            ImGui::BeginDisabled(!HasValidProjectRoot());
            if (ImGui::Button(ICON_FA_DOWNLOAD " Import"))
            {
                ImGui::OpenPopup("ImportAssetPath");
            }
            ImGui::EndDisabled();
            if (!HasValidProjectRoot() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Open a project before importing assets");

            // Simple path input popup for importing an asset
            if (ImGui::BeginPopup("ImportAssetPath"))
            {
                static char importPathBuf[512] = "";
                Spark::Ui::SubHeader("Enter file path to import");
                ImGui::SetNextItemWidth(Spark::Ui::Em(28.0f));
                bool enterPressed = ImGui::InputText("##ImportPath", importPathBuf, sizeof(importPathBuf),
                                                     ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::SameLine();
                if (Spark::Ui::PrimaryButton("Import") || enterPressed)
                {
                    if (importPathBuf[0] != '\0')
                    {
                        if (ImportAsset(std::string(importPathBuf)))
                        {
                            importPathBuf[0] = '\0';
                            ImGui::CloseCurrentPopup();
                        }
                    }
                    else
                    {
                        SetOperationResult(false, "Enter a source file path");
                    }
                }
                ImGui::SameLine();
                if (Spark::Ui::SecondaryButton("Cancel"))
                {
                    importPathBuf[0] = '\0';
                    ImGui::CloseCurrentPopup();
                }
                if (!m_lastOperationMessage.empty())
                {
                    Spark::Ui::StatusText(m_lastOperationSucceeded ? Spark::Ui::Status::Success
                                                                   : Spark::Ui::Status::Error,
                                          "%s", m_lastOperationMessage.c_str());
                }
                ImGui::EndPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_SYNC_ALT " Refresh"))
            {
                RefreshAssets();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(Spark::Ui::Em(7.0f));
            ImGui::SliderFloat("##Size", &m_thumbnailSize, 32.0f, 128.0f, "%.0f px");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Thumbnail Size");

            // Breadcrumb navigation
            Spark::Ui::ToolbarSeparator();

            // Build breadcrumb path
            std::filesystem::path currentPath = PathFromUtf8(m_currentFolder);
            std::filesystem::path rootPath = PathFromUtf8(m_projectPath);
            std::vector<std::filesystem::path> breadcrumbs;
            std::filesystem::path tempPath = currentPath;
            while (tempPath != rootPath && tempPath.has_parent_path() && tempPath != tempPath.parent_path())
            {
                breadcrumbs.push_back(tempPath);
                tempPath = tempPath.parent_path();
            }
            breadcrumbs.push_back(rootPath);
            std::reverse(breadcrumbs.begin(), breadcrumbs.end());

            for (size_t i = 0; i < breadcrumbs.size(); ++i)
            {
                if (i > 0)
                {
                    const float crumbGap = Spark::Ui::Px(2.0f);
                    ImGui::SameLine(0.0f, crumbGap);
                    Spark::Ui::TextRole(Spark::Ui::Role::TextDisabled, "%s", ICON_FA_CHEVRON_RIGHT);
                    ImGui::SameLine(0.0f, crumbGap);
                }
                std::string label = (i == 0) ? ICON_FA_HOME " Assets" : PathToUtf8(breadcrumbs[i].filename());
                if (ImGui::SmallButton(label.c_str()))
                {
                    NavigateToFolder(PathToUtf8(breadcrumbs[i]));
                }
            }

            if (!m_lastOperationMessage.empty())
            {
                ImGui::SameLine();
                Spark::Ui::StatusText(m_lastOperationSucceeded ? Spark::Ui::Status::Success : Spark::Ui::Status::Error,
                                      "%s", m_lastOperationMessage.c_str());
            }

            ImGui::Separator();

            // Split view
            if (ImGui::BeginTable("AssetBrowserTable", 2, ImGuiTableFlags_Resizable))
            {
                ImGui::TableSetupColumn("Folders", ImGuiTableColumnFlags_WidthFixed, Spark::Ui::Px(200.0f));
                ImGui::TableSetupColumn("Assets", ImGuiTableColumnFlags_WidthStretch);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                RenderFolderTree();

                ImGui::TableSetColumnIndex(1);
                RenderAssetGrid();

                ImGui::EndTable();
            }

            ImGui::Separator();
            RenderAssetDetails();
        }
        EndPanel();
    }

    void AssetBrowserPanel::RenderAssetGrid()
    {
        ImGui::BeginChild("AssetGrid");

        // Asset grid
        float panelWidth = ImGui::GetContentRegionAvail().x;
        // A cell is the thumbnail plus the table's cell padding on both sides.
        float cellWidth = m_thumbnailSize + ImGui::GetStyle().CellPadding.x * 2.0f + ImGui::GetStyle().ItemSpacing.x;
        int columns = std::max(1, (int)(panelWidth / cellWidth));
        std::string requestedFolder;

        if (ImGui::BeginTable("AssetGridTable", columns))
        {
            int itemIndex = 0;
            for (const auto& folder : m_folders)
            {
                if (itemIndex % columns == 0)
                    ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(itemIndex % columns);

                const std::filesystem::path folderPath = PathFromUtf8(folder);
                const std::string folderName = PathToUtf8(folderPath.filename());
                const ImVec2 pos = ImGui::GetCursorScreenPos();
                const ImVec2 size(m_thumbnailSize, m_thumbnailSize);
                ImDrawList* drawList = ImGui::GetWindowDrawList();
                const float radius = Spark::Ui::Px(4.0f);
                drawList->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                                        Spark::Ui::Col(Spark::Ui::Role::SurfaceRaised), radius);
                drawList->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                                  Spark::Ui::Col(Spark::Ui::Role::BorderStrong), radius);

                // Folders take the next category colour after the file types (0-4 in GetFileTypeColor);
                // a status role such as WarningText would make every folder read as a warning.
                const char* icon = ICON_FA_FOLDER;
                const ImVec2 iconSize = ImGui::CalcTextSize(icon);
                drawList->AddText(ImVec2(pos.x + (size.x - iconSize.x) * 0.5f,
                                         pos.y + (size.y - iconSize.y) * 0.5f - Spark::Ui::Px(4.0f)),
                                  Spark::Ui::CategoryColor(5), icon);

                ImGui::SetCursorScreenPos(pos);
                ImGui::InvisibleButton(("##folder_" + folder).c_str(), size);
                if (ImGui::IsItemClicked())
                    requestedFolder = folder;
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Open folder: %s", folderName.c_str());

                ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + size.y + Spark::Ui::Px(2.0f)));
                ImGui::TextWrapped("%s", folderName.c_str());
                ++itemIndex;
            }

            for (const auto& asset : m_assets)
            {
                if (itemIndex % columns == 0)
                {
                    ImGui::TableNextRow();
                }
                ImGui::TableSetColumnIndex(itemIndex % columns);

                std::filesystem::path assetPath = PathFromUtf8(asset);
                std::string ext = PathToUtf8(assetPath.extension());
                std::transform(ext.begin(), ext.end(), ext.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                std::string filename = PathToUtf8(assetPath.filename());
                const char* fileIcon = GetFileTypeIcon(ext);
                ImU32 iconColor = GetFileTypeColor(ext);

                // Thumbnail
                ImDrawList* drawList = ImGui::GetWindowDrawList();
                ImVec2 pos = ImGui::GetCursorScreenPos();
                ImVec2 size(m_thumbnailSize, m_thumbnailSize);

                bool isSelected = (asset == m_selectedAsset);
                const ImU32 bgColor =
                    Spark::Ui::Col(isSelected ? Spark::Ui::Role::SurfaceSelected : Spark::Ui::Role::SurfaceRaised);
                const ImU32 borderColor =
                    Spark::Ui::Col(isSelected ? Spark::Ui::Role::AccentFill : Spark::Ui::Role::BorderSubtle);
                const float radius = Spark::Ui::Px(4.0f);

                drawList->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bgColor, radius);
                // The selected thumbnail also gets a thicker outline, not just a different hue.
                drawList->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), borderColor, radius, 0,
                                  isSelected ? 2.0f * Spark::Ui::Scale() : 1.0f);

                // Large icon centered in thumbnail
                ImVec2 iconTextSize = ImGui::CalcTextSize(fileIcon);
                ImVec2 iconRenderPos(pos.x + (size.x - iconTextSize.x) * 0.5f,
                                     pos.y + (size.y - iconTextSize.y) * 0.5f - Spark::Ui::Px(4.0f));
                drawList->AddText(iconRenderPos, iconColor, fileIcon);

                // Click handling
                ImGui::SetCursorScreenPos(pos);
                ImGui::InvisibleButton(asset.c_str(), size);
                if (ImGui::IsItemClicked())
                {
                    SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "Asset selected: %s", filename.c_str());
                    m_selectedAsset = asset;
                }
                if (ext == ".spkmat" && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    OpenAsset(asset);
                }

                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
                {
                    const std::string reference = IsContainedByProject(assetPath)
                                                      ? MakeAssetDragReference(assetPath, PathFromUtf8(m_projectPath))
                                                      : std::string{};
                    if (!reference.empty())
                    {
                        ImGui::SetDragDropPayload(kAssetDragPayloadType, reference.c_str(), reference.size() + 1);
                        ImGui::Text("%s", filename.c_str());
                        ImGui::TextDisabled("%s", reference.c_str());
                    }
                    ImGui::EndDragDropSource();
                }

                // Hover tooltip
                if (ImGui::IsItemHovered())
                {
                    ImGui::BeginTooltip();
                    ImGui::Text("%s %s", fileIcon, filename.c_str());
                    Spark::Ui::KeyValue("Type", "%s", ext.empty() ? "Unknown" : ext.c_str());
                    try
                    {
                        if (std::filesystem::exists(assetPath))
                        {
                            auto fsize = std::filesystem::file_size(assetPath);
                            if (fsize > 1024 * 1024)
                                Spark::Ui::KeyValue("Size", "%.1f MB", fsize / (1024.0f * 1024.0f));
                            else if (fsize > 1024)
                                Spark::Ui::KeyValue("Size", "%.1f KB", fsize / 1024.0f);
                            else
                                Spark::Ui::KeyValue("Size", "%lld bytes", (long long)fsize);
                        }
                    }
                    catch (...)
                    {
                    }
                    ImGui::EndTooltip();
                }

                // Filename below thumbnail (truncated)
                ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + size.y + Spark::Ui::Px(2.0f)));
                int maxChars = (int)(m_thumbnailSize / 7.0f);
                if ((int)filename.length() > maxChars)
                {
                    filename = filename.substr(0, maxChars - 3) + "...";
                }
                ImGui::TextWrapped("%s", filename.c_str());

                itemIndex++;
            }
            ImGui::EndTable();
        }

        if (!requestedFolder.empty())
            NavigateToFolder(requestedFolder);

        ImGui::EndChild();
    }

    void AssetBrowserPanel::RenderAssetDetails()
    {
        // Four text rows tall, so the details never clip at larger fonts or UI scales.
        ImGui::BeginChild("AssetDetails", ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 4.0f +
                                                           ImGui::GetStyle().WindowPadding.y * 2.0f));

        if (!m_selectedAsset.empty())
        {
            Spark::Ui::KeyValue("Selected", "%s", PathToUtf8(PathFromUtf8(m_selectedAsset).filename()).c_str());
            Spark::Ui::KeyValue("Path", "%s", m_selectedAsset.c_str());

            try
            {
                const std::filesystem::path selectedPath = PathFromUtf8(m_selectedAsset);
                if (std::filesystem::exists(selectedPath))
                {
                    auto fileSize = std::filesystem::file_size(selectedPath);
                    Spark::Ui::KeyValue("Size", "%ju bytes", static_cast<uintmax_t>(fileSize));
                }
            }
            catch (const std::exception&)
            {
                Spark::Ui::StatusText(Spark::Ui::Status::Error, "Error reading file info");
            }
        }
        else
        {
            Spark::Ui::SecondaryText("No asset selected");
        }

        ImGui::EndChild();
    }

} // namespace SparkEditor
