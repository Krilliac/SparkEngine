/**
 * @file AssetBrowserPanel.cpp
 * @brief Implementation of the Asset Browser panel
 * @author Spark Engine Team
 * @date 2025
 */

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>

#include <imgui.h>

#include "AssetBrowserPanel.h"
#include "../Core/EditorIcons.h"
#include "../Core/EditorFonts.h"
#include "../../../SparkEngine/Source/Utils/Validate.h"
#include "Utils/LogMacros.h"

namespace SparkEditor
{

    AssetBrowserPanel::AssetBrowserPanel() : EditorPanel("Asset Browser", "asset_browser_panel") {}

    bool AssetBrowserPanel::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Initializing Asset Browser panel");
        return true;
    }

    void AssetBrowserPanel::Update(float deltaTime)
    {
        // Update asset browser logic
    }

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
        if (ext == ".mat" || ext == ".material")
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
            if (ImGui::Button(ICON_FA_DOWNLOAD " Import"))
            {
                ImGui::OpenPopup("ImportAssetPath");
            }
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
                        ImportAsset(std::string(importPathBuf));
                        importPathBuf[0] = '\0';
                    }
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel"))
                {
                    importPathBuf[0] = '\0';
                    ImGui::CloseCurrentPopup();
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
            std::filesystem::path currentPath(m_currentFolder);
            std::filesystem::path rootPath(m_projectPath);
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
                std::string label = (i == 0) ? ICON_FA_HOME " Assets" : breadcrumbs[i].filename().string();
                if (ImGui::SmallButton(label.c_str()))
                {
                    m_currentFolder = breadcrumbs[i].string();
                    RefreshAssets();
                }
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

    void AssetBrowserPanel::Shutdown()
    {
        std::cout << "Shutting down Asset Browser panel\n";
    }

    bool AssetBrowserPanel::HandleEvent(const std::string& eventType, void* eventData)
    {
        return false;
    }

    void AssetBrowserPanel::SetProjectPath(const std::string& projectPath)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Asset Browser: project path set to '%s'", projectPath.c_str());
        m_projectPath = projectPath;
        m_currentFolder = projectPath;
        RefreshAssets();
    }

    void AssetBrowserPanel::RenderFolderTree()
    {
        ImGui::BeginChild("FolderTree");

        std::string rootLabel = std::string(ICON_FA_FOLDER) + "  Assets";
        if (ImGui::TreeNodeEx(rootLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
        {
            try
            {
                if (!m_projectPath.empty() && std::filesystem::exists(m_projectPath))
                {
                    for (const auto& entry : std::filesystem::directory_iterator(m_projectPath))
                    {
                        if (entry.is_directory())
                        {
                            std::string folderName = entry.path().filename().string();
                            bool isCurrentFolder = (entry.path().string() == m_currentFolder);
                            std::string nodeLabel =
                                std::string(isCurrentFolder ? ICON_FA_FOLDER_OPEN : ICON_FA_FOLDER) + "  " + folderName;

                            ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_OpenOnArrow;
                            if (isCurrentFolder)
                                nodeFlags |= ImGuiTreeNodeFlags_Selected;

                            if (ImGui::TreeNodeEx(nodeLabel.c_str(), nodeFlags))
                            {
                                if (ImGui::IsItemClicked())
                                {
                                    m_currentFolder = entry.path().string();
                                    RefreshAssets();
                                }
                                ImGui::TreePop();
                            }
                            else if (ImGui::IsItemClicked())
                            {
                                m_currentFolder = entry.path().string();
                                RefreshAssets();
                            }
                        }
                    }
                }
            }
            catch (const std::exception&)
            {
                ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), ICON_FA_EXCLAMATION " Error reading folders");
            }

            ImGui::TreePop();
        }

        ImGui::EndChild();
    }

    void AssetBrowserPanel::RenderAssetGrid()
    {
        ImGui::BeginChild("AssetGrid");

        // Asset grid
        float panelWidth = ImGui::GetContentRegionAvail().x;
        float cellWidth = m_thumbnailSize + 14.0f;
        int columns = std::max(1, (int)(panelWidth / cellWidth));

        if (ImGui::BeginTable("AssetGridTable", columns))
        {
            int itemIndex = 0;
            for (const auto& asset : m_assets)
            {
                if (itemIndex % columns == 0)
                {
                    ImGui::TableNextRow();
                }
                ImGui::TableSetColumnIndex(itemIndex % columns);

                std::filesystem::path assetPath(asset);
                std::string ext = assetPath.extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                std::string filename = assetPath.filename().string();
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

                // Hover tooltip
                if (ImGui::IsItemHovered())
                {
                    ImGui::BeginTooltip();
                    ImGui::Text("%s %s", fileIcon, filename.c_str());
                    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Type: %s",
                                       ext.empty() ? "Unknown" : ext.c_str());
                    try
                    {
                        if (std::filesystem::exists(asset))
                        {
                            auto fsize = std::filesystem::file_size(asset);
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

        ImGui::EndChild();
    }

    void AssetBrowserPanel::RenderAssetDetails()
    {
        ImGui::BeginChild("AssetDetails", ImVec2(0, 100));

        if (!m_selectedAsset.empty())
        {
            ImGui::Text("Selected: %s", std::filesystem::path(m_selectedAsset).filename().string().c_str());
            ImGui::Text("Path: %s", m_selectedAsset.c_str());

            try
            {
                if (std::filesystem::exists(m_selectedAsset))
                {
                    auto fileSize = std::filesystem::file_size(m_selectedAsset);
                    ImGui::Text("Size: %ju bytes", static_cast<uintmax_t>(fileSize));

                    (void)std::filesystem::last_write_time(m_selectedAsset);
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

    void AssetBrowserPanel::RefreshAssets()
    {
        m_assets.clear();

        try
        {
            if (!m_currentFolder.empty() && std::filesystem::exists(m_currentFolder))
            {
                for (const auto& entry : std::filesystem::directory_iterator(m_currentFolder))
                {
                    if (entry.is_regular_file())
                    {
                        m_assets.push_back(entry.path().string());
                    }
                }
            }
        }
        catch (const std::exception& e)
        {
            std::cerr << "Error refreshing assets: " << e.what() << std::endl;
        }
    }

    void AssetBrowserPanel::ImportAsset(const std::string& filePath)
    {
        SPARK_VALIDATE_NOT_EMPTY(Spark::LogCategory::Editor, filePath);
        std::filesystem::path sourcePath(filePath);

        if (!std::filesystem::exists(sourcePath))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Import failed: file does not exist: %s", filePath.c_str());
            return;
        }

        if (!std::filesystem::is_regular_file(sourcePath))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Import failed: not a regular file: %s", filePath.c_str());
            return;
        }

        // Determine destination inside the current folder
        std::filesystem::path destDir(m_currentFolder.empty() ? m_projectPath : m_currentFolder);
        std::filesystem::path destPath = destDir / sourcePath.filename();

        // Avoid overwriting: append a numeric suffix if a file with the same name exists
        if (std::filesystem::exists(destPath))
        {
            std::string stem = sourcePath.stem().string();
            std::string ext = sourcePath.extension().string();
            int counter = 1;
            do
            {
                destPath = destDir / (stem + "_" + std::to_string(counter) + ext);
                ++counter;
            } while (std::filesystem::exists(destPath));
        }

        try
        {
            std::filesystem::copy_file(sourcePath, destPath, std::filesystem::copy_options::overwrite_existing);
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "Imported asset: %s -> %s",
                           sourcePath.filename().string().c_str(), destPath.string().c_str());
        }
        catch (const std::exception& e)
        {
            std::cerr << "Import failed: " << e.what() << std::endl;
            return;
        }

        // Refresh the file list so the newly imported asset appears in the grid
        RefreshAssets();
    }

} // namespace SparkEditor