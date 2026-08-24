/**
 * @file AssetBrowserPanel.cpp
 * @brief Implementation of the Asset Browser panel
 * @author Spark Engine Team
 * @date 2025
 */

#include "AssetBrowserPanel.h"

#include "Graphics/GraphicsEngine.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <utility>

#include <imgui.h>

#include "../Core/EditorIcons.h"
#include "../Core/EditorFonts.h"
#include "../../../SparkEngine/Source/Utils/Validate.h"
#include "Utils/LogMacros.h"

namespace SparkEditor
{
    namespace
    {
        namespace fs = std::filesystem;

        std::string PathToUtf8(const fs::path& path) noexcept
        {
            try
            {
                const auto utf8 = path.generic_u8string();
                return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
            }
            catch (...)
            {
                return {};
            }
        }

        fs::path PathFromUtf8(std::string_view value) noexcept
        {
            try
            {
                const auto* begin = reinterpret_cast<const char8_t*>(value.data());
                return fs::path(std::u8string(begin, begin + value.size()));
            }
            catch (...)
            {
                return {};
            }
        }

        std::string CanonicalDirectoryString(const fs::path& path, std::error_code& ec)
        {
            if (path.empty() || !fs::is_directory(path, ec) || ec)
            {
                return {};
            }

            fs::path canonical = fs::weakly_canonical(path, ec);
            return ec ? std::string{} : PathToUtf8(canonical);
        }

        bool IsContainedPath(const fs::path& root, const fs::path& candidate)
        {
            std::error_code ec;
            const fs::path canonicalRoot = fs::weakly_canonical(root, ec);
            if (ec)
                return false;

            const fs::path canonicalCandidate = fs::weakly_canonical(candidate, ec);
            if (ec)
                return false;

            const fs::path relative = fs::relative(canonicalCandidate, canonicalRoot, ec);
            if (ec || relative.is_absolute())
                return false;

            for (const auto& component : relative)
            {
                if (component == "..")
                    return false;
            }
            return true;
        }

        std::vector<fs::path> ChildDirectories(const fs::path& folder)
        {
            std::vector<fs::path> directories;
            std::error_code ec;
            for (fs::directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end;
                 !ec && it != end; it.increment(ec))
            {
                std::error_code typeError;
                const fs::file_status linkStatus = it->symlink_status(typeError);
                if (!typeError && !fs::is_symlink(linkStatus) && it->is_directory(typeError) && !typeError)
                    directories.push_back(it->path());
            }
            std::sort(directories.begin(), directories.end(), [](const fs::path& lhs, const fs::path& rhs)
                      { return PathToUtf8(lhs.filename()) < PathToUtf8(rhs.filename()); });
            return directories;
        }
    } // namespace

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
        ClearProject();

        std::error_code ec;
        m_projectPath = CanonicalDirectoryString(PathFromUtf8(projectPath), ec);
        if (m_projectPath.empty())
        {
            SetOperationResult(false, "Asset root is missing or inaccessible");
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Asset Browser rejected invalid asset root: %s",
                            projectPath.c_str());
            return;
        }

        m_currentFolder = m_projectPath;
        SetOperationResult(true, "Project assets ready");
        RefreshAssets();
    }

    void AssetBrowserPanel::ClearProject()
    {
        m_projectPath.clear();
        m_currentFolder.clear();
        m_assets.clear();
        m_folders.clear();
        m_selectedAsset.clear();
        m_lastOperationMessage.clear();
        m_lastOperationSucceeded = false;
    }

    bool AssetBrowserPanel::NavigateToFolder(const std::string& folderPath)
    {
        std::error_code ec;
        const std::string canonical = CanonicalDirectoryString(PathFromUtf8(folderPath), ec);
        if (canonical.empty() || !IsContainedByProject(PathFromUtf8(canonical)))
        {
            SetOperationResult(false, "Cannot leave the active project's Assets folder");
            return false;
        }

        m_currentFolder = canonical;
        m_selectedAsset.clear();
        SetOperationResult(true, {});
        RefreshAssets();
        return true;
    }

    void AssetBrowserPanel::RenderFolderTree()
    {
        ImGui::BeginChild("FolderTree");

        std::string rootLabel = std::string(ICON_FA_FOLDER) + "  Assets";
        ImGuiTreeNodeFlags rootFlags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_OpenOnArrow;
        if (m_currentFolder == m_projectPath && !m_projectPath.empty())
            rootFlags |= ImGuiTreeNodeFlags_Selected;

        const bool rootOpen = ImGui::TreeNodeEx((rootLabel + "###asset_root").c_str(), rootFlags);
        if (ImGui::IsItemClicked() && HasValidProjectRoot())
            NavigateToFolder(m_projectPath);

        if (rootOpen)
        {
            if (HasValidProjectRoot())
            {
                for (const auto& folder : ChildDirectories(PathFromUtf8(m_projectPath)))
                    RenderFolderNode(folder);
            }
            ImGui::TreePop();
        }

        if (!HasValidProjectRoot())
            ImGui::TextDisabled("Open a project to browse assets");

        ImGui::EndChild();
    }

    void AssetBrowserPanel::RenderFolderNode(const std::filesystem::path& folderPath)
    {
        if (!IsContainedByProject(folderPath))
            return;

        const auto children = ChildDirectories(folderPath);
        const bool isCurrentFolder = PathFromUtf8(m_currentFolder) == folderPath;
        std::string label = std::string(isCurrentFolder ? ICON_FA_FOLDER_OPEN : ICON_FA_FOLDER) + "  " +
                            PathToUtf8(folderPath.filename()) + "###" + PathToUtf8(folderPath);

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (children.empty())
            flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        if (isCurrentFolder)
            flags |= ImGuiTreeNodeFlags_Selected;

        const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
        if (ImGui::IsItemClicked())
            NavigateToFolder(PathToUtf8(folderPath));

        if (open && !children.empty())
        {
            for (const auto& child : children)
                RenderFolderNode(child);
            ImGui::TreePop();
        }
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

    void AssetBrowserPanel::RefreshAssets()
    {
        m_assets.clear();
        m_folders.clear();

        try
        {
            if (!HasValidProjectRoot())
                return;

            if (!IsContainedByProject(PathFromUtf8(m_currentFolder)) ||
                !std::filesystem::is_directory(PathFromUtf8(m_currentFolder)))
            {
                m_currentFolder = m_projectPath;
                m_selectedAsset.clear();
            }

            for (const auto& entry : std::filesystem::directory_iterator(PathFromUtf8(m_currentFolder)))
            {
                if (entry.is_regular_file())
                {
                    m_assets.push_back(PathToUtf8(entry.path()));
                }
                else if (!entry.is_symlink() && entry.is_directory() && IsContainedByProject(entry.path()))
                {
                    m_folders.push_back(PathToUtf8(entry.path()));
                }
            }

            const auto byFilename = [](const std::string& lhs, const std::string& rhs)
            { return PathToUtf8(PathFromUtf8(lhs).filename()) < PathToUtf8(PathFromUtf8(rhs).filename()); };
            std::sort(m_assets.begin(), m_assets.end(), byFilename);
            std::sort(m_folders.begin(), m_folders.end(), byFilename);
        }
        catch (const std::exception& e)
        {
            SetOperationResult(false, std::string("Unable to read asset folder: ") + e.what());
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Error refreshing assets: %s", e.what());
        }
    }

    bool AssetBrowserPanel::ImportAsset(const std::string& filePath)
    {
        if (filePath.empty())
        {
            SetOperationResult(false, "Import failed: source path is empty");
            return false;
        }
        std::filesystem::path sourcePath = PathFromUtf8(filePath);

        if (!HasValidProjectRoot())
        {
            SetOperationResult(false, "Open a project before importing assets");
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Import failed: no canonical project asset root");
            return false;
        }

        std::error_code ec;
        if (!std::filesystem::exists(sourcePath, ec) || ec)
        {
            SetOperationResult(false, "Import failed: source file does not exist");
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Import failed: file does not exist: %s", filePath.c_str());
            return false;
        }

        if (!std::filesystem::is_regular_file(sourcePath, ec) || ec)
        {
            SetOperationResult(false, "Import failed: source is not a regular file");
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Import failed: not a regular file: %s", filePath.c_str());
            return false;
        }

        // Revalidate the destination every time. The current directory may have
        // been removed or replaced by a symlink since the last UI frame.
        std::filesystem::path destDir = PathFromUtf8(m_currentFolder);
        if (!std::filesystem::is_directory(destDir, ec) || ec || !IsContainedByProject(destDir))
        {
            SetOperationResult(false, "Import failed: destination is outside the active project");
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Import rejected unsafe destination: %s",
                            PathToUtf8(destDir).c_str());
            return false;
        }

        std::filesystem::path destPath = destDir / sourcePath.filename();
        if (!IsContainedByProject(destPath))
        {
            SetOperationResult(false, "Import failed: destination is outside the active project");
            return false;
        }

        // Avoid overwriting: append a numeric suffix if a file with the same name exists
        if (std::filesystem::exists(destPath))
        {
            const std::string stem = PathToUtf8(sourcePath.stem());
            const std::string ext = PathToUtf8(sourcePath.extension());
            int counter = 1;
            do
            {
                destPath = destDir / PathFromUtf8(stem + "_" + std::to_string(counter) + ext);
                ++counter;
            } while (std::filesystem::exists(destPath));
        }

        if (!IsContainedByProject(destPath))
        {
            SetOperationResult(false, "Import failed: destination is outside the active project");
            return false;
        }

        try
        {
            std::filesystem::copy_file(sourcePath, destPath, std::filesystem::copy_options::none);
            if (m_graphics)
            {
                // std::filesystem::path::string() uses the active Windows code
                // page and corrupts non-ASCII cache identities. Convert the
                // native path explicitly to UTF-8 before crossing the graphics
                // API boundary.
                const std::string destinationUtf8 = PathToUtf8(destPath);
                if (!destinationUtf8.empty())
                    m_graphics->InvalidateBasicTexture(destinationUtf8);

                std::string extension = PathToUtf8(destPath.extension());
                std::transform(extension.begin(), extension.end(), extension.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (extension == ".json")
                {
                    const std::filesystem::path projectRoot = PathFromUtf8(m_projectPath).parent_path();
                    std::error_code relativeError;
                    const std::filesystem::path projectRelative =
                        std::filesystem::relative(destPath, projectRoot, relativeError);
                    if (!relativeError && !projectRelative.empty())
                    {
                        // Missing material JSONs are negatively cached. Clear
                        // that marker as part of the import transaction so the
                        // first render after import reparses the new file.
                        const std::string relativeUtf8 = PathToUtf8(projectRelative);
                        const std::string projectRootUtf8 = PathToUtf8(projectRoot);
                        if (!relativeUtf8.empty() && !projectRootUtf8.empty())
                            m_graphics->InvalidateBasicMaterial(relativeUtf8, projectRootUtf8);
                    }
                }
            }
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "Imported asset: %s -> %s",
                           PathToUtf8(sourcePath.filename()).c_str(), PathToUtf8(destPath).c_str());
        }
        catch (const std::exception& e)
        {
            SetOperationResult(false, std::string("Import failed: ") + e.what());
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Import failed: %s", e.what());
            return false;
        }

        // Refresh the file list so the newly imported asset appears in the grid
        SetOperationResult(true, std::string("Imported ") + PathToUtf8(destPath.filename()));
        RefreshAssets();
        return true;
    }

    bool AssetBrowserPanel::HasValidProjectRoot() const
    {
        if (m_projectPath.empty())
            return false;
        std::error_code ec;
        const std::string canonical = CanonicalDirectoryString(PathFromUtf8(m_projectPath), ec);
        return !canonical.empty() && PathFromUtf8(canonical) == PathFromUtf8(m_projectPath);
    }

    bool AssetBrowserPanel::IsContainedByProject(const std::filesystem::path& candidate) const
    {
        return HasValidProjectRoot() && IsContainedPath(PathFromUtf8(m_projectPath), candidate);
    }

    void AssetBrowserPanel::SetOperationResult(bool succeeded, std::string message)
    {
        m_lastOperationSucceeded = succeeded;
        m_lastOperationMessage = std::move(message);
    }

} // namespace SparkEditor
