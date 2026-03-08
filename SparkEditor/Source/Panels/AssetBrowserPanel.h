/**
 * @file AssetBrowserPanel.h
 * @brief Asset browser panel for the Spark Engine Editor
 * @author Spark Engine Team
 * @date 2025
 */

#pragma once

#include "../Core/EditorPanel.h"
#include <string>
#include <vector>
#include <memory>

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

      private:
        void RenderFolderTree();
        void RenderAssetGrid();
        void RenderAssetDetails();
        void RefreshAssets();
        void ImportAsset(const std::string& filePath);

      private:
        std::string m_projectPath;
        std::string m_currentFolder;
        std::vector<std::string> m_assets;
        std::string m_selectedAsset;
        float m_thumbnailSize = 64.0f;
    };

} // namespace SparkEditor