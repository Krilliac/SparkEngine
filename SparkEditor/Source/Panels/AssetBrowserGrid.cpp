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

    static ImU32 GetFileTypeColor(const std::string& ext)
    {
        if (ext == ".fbx" || ext == ".obj" || ext == ".gltf" || ext == ".glb")
            return IM_COL32(100, 200, 255, 255);
        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga")
            return IM_COL32(200, 150, 255, 255);
        if (ext == ".wav" || ext == ".mp3" || ext == ".ogg")
            return IM_COL32(255, 200, 100, 255);
        if (ext == ".cpp" || ext == ".h" || ext == ".hpp" || ext == ".lua")
            return IM_COL32(100, 255, 150, 255);
        if (ext == ".hlsl" || ext == ".glsl" || ext == ".shader")
            return IM_COL32(255, 150, 150, 255);
        return IM_COL32(180, 180, 180, 255);
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
                ImGui::Text("Enter file path to import:");
                ImGui::SetNextItemWidth(400);
                bool enterPressed = ImGui::InputText("##ImportPath", importPathBuf, sizeof(importPathBuf),
                                                     ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::SameLine();
                if (ImGui::Button("OK") || enterPressed)
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
                if (ImGui::Button("Cancel"))
                {
                    importPathBuf[0] = '\0';
                    ImGui::CloseCurrentPopup();
                }
                if (!m_lastOperationMessage.empty())
                {
                    const ImVec4 color =
                        m_lastOperationSucceeded ? ImVec4(0.45f, 0.85f, 0.5f, 1.0f) : ImVec4(1.0f, 0.45f, 0.45f, 1.0f);
                    ImGui::TextColored(color, "%s", m_lastOperationMessage.c_str());
                }
                ImGui::EndPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_SYNC_ALT " Refresh"))
            {
                RefreshAssets();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(100);
            ImGui::SliderFloat("##Size", &m_thumbnailSize, 32.0f, 128.0f, "%.0f px");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Thumbnail Size");

            // Breadcrumb navigation
            ImGui::SameLine();
            ImGui::Text("|");
            ImGui::SameLine();

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
                    ImGui::SameLine(0, 2);
                    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), ICON_FA_CHEVRON_RIGHT);
                    ImGui::SameLine(0, 2);
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
                const ImVec4 color =
                    m_lastOperationSucceeded ? ImVec4(0.45f, 0.85f, 0.5f, 1.0f) : ImVec4(1.0f, 0.45f, 0.45f, 1.0f);
                ImGui::TextColored(color, "%s", m_lastOperationMessage.c_str());
            }

            ImGui::Separator();

            // Split view
            if (ImGui::BeginTable("AssetBrowserTable", 2, ImGuiTableFlags_Resizable))
            {
                ImGui::TableSetupColumn("Folders", ImGuiTableColumnFlags_WidthFixed, 200.0f);
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
        float cellWidth = m_thumbnailSize + 14.0f;
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
                drawList->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(40, 43, 50, 255), 4.0f);
                drawList->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(70, 75, 86, 255), 4.0f);

                const char* icon = ICON_FA_FOLDER;
                const ImVec2 iconSize = ImGui::CalcTextSize(icon);
                drawList->AddText(
                    ImVec2(pos.x + (size.x - iconSize.x) * 0.5f, pos.y + (size.y - iconSize.y) * 0.5f - 4.0f),
                    IM_COL32(235, 190, 85, 255), icon);

                ImGui::SetCursorScreenPos(pos);
                ImGui::InvisibleButton(("##folder_" + folder).c_str(), size);
                if (ImGui::IsItemClicked())
                    requestedFolder = folder;
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Open folder: %s", folderName.c_str());

                ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + size.y + 2.0f));
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
                ImU32 bgColor = isSelected ? IM_COL32(45, 140, 240, 60) : IM_COL32(40, 43, 50, 255);
                ImU32 borderColor = isSelected ? IM_COL32(45, 140, 240, 255) : IM_COL32(55, 58, 66, 255);

                drawList->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bgColor, 4.0f);
                drawList->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), borderColor, 4.0f);

                // Large icon centered in thumbnail
                ImVec2 iconTextSize = ImGui::CalcTextSize(fileIcon);
                ImVec2 iconRenderPos(pos.x + (size.x - iconTextSize.x) * 0.5f,
                                     pos.y + (size.y - iconTextSize.y) * 0.5f - 4.0f);
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
                    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Type: %s",
                                       ext.empty() ? "Unknown" : ext.c_str());
                    try
                    {
                        if (std::filesystem::exists(assetPath))
                        {
                            auto fsize = std::filesystem::file_size(assetPath);
                            if (fsize > 1024 * 1024)
                                ImGui::Text("Size: %.1f MB", fsize / (1024.0f * 1024.0f));
                            else if (fsize > 1024)
                                ImGui::Text("Size: %.1f KB", fsize / 1024.0f);
                            else
                                ImGui::Text("Size: %lld bytes", (long long)fsize);
                        }
                    }
                    catch (...)
                    {
                    }
                    ImGui::EndTooltip();
                }

                // Filename below thumbnail (truncated)
                ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + size.y + 2.0f));
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
        ImGui::BeginChild("AssetDetails", ImVec2(0, 100));

        if (!m_selectedAsset.empty())
        {
            ImGui::Text("Selected: %s", PathToUtf8(PathFromUtf8(m_selectedAsset).filename()).c_str());
            ImGui::Text("Path: %s", m_selectedAsset.c_str());

            try
            {
                const std::filesystem::path selectedPath = PathFromUtf8(m_selectedAsset);
                if (std::filesystem::exists(selectedPath))
                {
                    auto fileSize = std::filesystem::file_size(selectedPath);
                    ImGui::Text("Size: %ju bytes", static_cast<uintmax_t>(fileSize));

                    (void)std::filesystem::last_write_time(selectedPath);
                    ImGui::Text("Modified: [File timestamp]");
                }
            }
            catch (const std::exception&)
            {
                ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "Error reading file info");
            }
        }
        else
        {
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "No asset selected");
        }

        ImGui::EndChild();
    }

} // namespace SparkEditor
