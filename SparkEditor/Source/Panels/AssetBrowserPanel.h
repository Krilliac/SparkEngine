/**
 * @file AssetBrowserPanel.h
 * @brief Asset browser panel for the Spark Engine Editor
 * @author Spark Engine Team
 * @date 2025
 */

#pragma once

#include "../Core/EditorPanel.h"
#include <filesystem>
#include <string>
#include <vector>
#include <memory>

class GraphicsEngine;

namespace SparkEditor
{

    /**
 * @brief Asset browser panel
 * 
 * Shows project assets and allows browsing, importing, and managing assets.
 */
    class AssetBrowserPanel : public EditorPanel
    {
      public:
        /**
     * @brief Constructor
     */
        AssetBrowserPanel();

        /**
     * @brief Destructor
     */
        ~AssetBrowserPanel() override = default;

        /**
     * @brief Initialize the asset browser panel
     * @return true if initialization succeeded
     */
        bool Initialize() override;

        /**
     * @brief Update asset browser panel
     * @param deltaTime Time elapsed since last update
     */
        void Update(float deltaTime) override;

        /**
     * @brief Render asset browser panel
     */
        void Render() override;

        /**
     * @brief Shutdown the asset browser panel
     */
        void Shutdown() override;

        /**
     * @brief Handle panel events
     * @param eventType Event type
     * @param eventData Event data
     * @return true if event was handled
     */
        bool HandleEvent(const std::string& eventType, void* eventData) override;

        /**
     * @brief Set project path
     * @param projectPath Path to the project assets
     */
        void SetProjectPath(const std::string& projectPath);

        /**
         * @brief Forget the active project and every path/selection derived from it.
         *
         * Project-close handlers must call this so a later import cannot target the
         * assets directory of a project that is no longer open.
         */
        void ClearProject();

        /**
         * @brief Navigate to a directory contained by the active asset root.
         * @return true when the directory exists and is contained by the root.
         */
        bool NavigateToFolder(const std::string& folderPath);

        /**
         * @brief Import a file into the current folder of the active project.
         * @return true only when the file was copied successfully.
         */
        bool ImportAsset(const std::string& filePath);

        /// @brief Wire live basic-path cache invalidation after an import.
        void SetGraphics(GraphicsEngine* graphics) { m_graphics = graphics; }

        const std::string& GetProjectPath() const { return m_projectPath; }
        const std::string& GetCurrentFolder() const { return m_currentFolder; }
        const std::vector<std::string>& GetAssets() const { return m_assets; }
        const std::vector<std::string>& GetFolders() const { return m_folders; }
        const std::string& GetLastOperationMessage() const { return m_lastOperationMessage; }
        bool LastOperationSucceeded() const { return m_lastOperationSucceeded; }

      private:
        void RenderFolderTree();
        void RenderFolderNode(const std::filesystem::path& folderPath);
        void RenderAssetGrid();
        void RenderAssetDetails();
        void RefreshAssets();
        bool HasValidProjectRoot() const;
        bool IsContainedByProject(const std::filesystem::path& candidate) const;
        void SetOperationResult(bool succeeded, std::string message);

      private:
        std::string m_projectPath;
        std::string m_currentFolder;
        std::vector<std::string> m_assets;
        std::vector<std::string> m_folders;
        std::string m_selectedAsset;
        std::string m_lastOperationMessage;
        bool m_lastOperationSucceeded = false;
        float m_thumbnailSize = 64.0f;
        GraphicsEngine* m_graphics = nullptr; ///< Non-owning; owned by EditorUI.
    };

} // namespace SparkEditor
