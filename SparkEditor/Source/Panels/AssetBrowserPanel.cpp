/**
 * @file AssetBrowserPanel.cpp
 * @brief Implementation of the Asset Browser panel
 * @author Spark Engine Team
 * @date 2025
 */

#include "AssetBrowserPanel.h"
#include "AssetBrowserPathUtils.h"
#include "../AssetPipeline/EditorAssetDrag.h"

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
        using AssetBrowserDetail::PathFromUtf8;
        using AssetBrowserDetail::PathToUtf8;

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

    void AssetBrowserPanel::Shutdown()
    {
        std::cout << "Shutting down Asset Browser panel\n";
    }

    bool AssetBrowserPanel::HandleEvent(const std::string& eventType, void* eventData)
    {
        return false;
    }

    bool AssetBrowserPanel::OpenAsset(const std::string& filePath)
    {
        std::error_code error;
        const auto path = std::filesystem::weakly_canonical(PathFromUtf8(filePath), error);
        if (error || !IsContainedByProject(path) || !std::filesystem::is_regular_file(path, error) || error)
        {
            SetOperationResult(false, "Cannot open an asset outside the active project or a missing file");
            return false;
        }
        std::string extension = PathToUtf8(path.extension());
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
        if (extension != ".spkmat" || !m_onMaterialOpened)
        {
            SetOperationResult(false, "No material editor is available for this asset");
            return false;
        }
        m_onMaterialOpened(PathToUtf8(path));
        SetOperationResult(true, "Material open requested");
        return true;
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
