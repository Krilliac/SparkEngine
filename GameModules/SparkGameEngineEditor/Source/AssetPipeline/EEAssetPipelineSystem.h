/**
 * @file EEAssetPipelineSystem.h
 * @brief Asset import, processing, LOD generation, and packaging
 * @author Spark Engine Team
 * @date 2026
 *
 * Manages the full asset pipeline: format import (FBX, glTF, OBJ, PNG, etc.),
 * validation, optimization, LOD generation, texture compression, and
 * packaging for deployment.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/EngineEditorEnums.h"

#include <cstdint>
#include <string>
#include <vector>

namespace EngineEditor
{

    /// @brief An asset in the pipeline
    struct PipelineAsset
    {
        uint32_t assetId = 0;
        std::string name;
        std::string sourcePath;
        std::string outputPath;
        AssetFormat format = AssetFormat::FBX;
        PipelineStage currentStage = PipelineStage::Import;
        bool isProcessed = false;
        bool hasErrors = false;
        std::string errorMessage;
        uint64_t fileSizeBytes = 0;
        uint64_t processedSizeBytes = 0;
    };

    /// @brief LOD configuration for a mesh asset
    struct LODConfig
    {
        uint32_t lodId = 0;
        uint32_t assetId = 0;
        LODStrategy strategy = LODStrategy::ScreenSize;
        uint32_t lodCount = 4;
        float reductionPerLevel = 0.5f; ///< Triangle reduction factor per LOD
        float screenSizeLOD0 = 1.0f;
        float screenSizeLOD1 = 0.5f;
        float screenSizeLOD2 = 0.25f;
        float screenSizeLOD3 = 0.1f;
    };

    /// @brief Texture compression settings
    struct TextureSettings
    {
        uint32_t settingsId = 0;
        uint32_t assetId = 0;
        bool generateMipmaps = true;
        uint32_t maxResolution = 4096;
        bool compressBC = true; ///< Use BC1/BC3/BC5/BC7 compression
        bool sRGB = true;
        bool isNormalMap = false;
    };

    /// @brief An import rule mapping source format to processing options
    struct ImportRule
    {
        uint32_t ruleId = 0;
        std::string name;
        AssetFormat format = AssetFormat::FBX;
        std::string sourcePattern; ///< Glob pattern (e.g., "Characters/*.fbx")
        bool autoImport = true;
        bool generateLODs = true;
        bool optimizeMesh = true;
        float uniformScale = 1.0f;
    };

    /**
     * @brief Asset pipeline system for no-code asset management
     *
     * Manages asset import, validation, optimization, LOD generation,
     * texture processing, and packaging.
     */
    class EEAssetPipelineSystem
    {
      public:
        EEAssetPipelineSystem() = default;
        ~EEAssetPipelineSystem() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Update(float deltaTime);
        void Shutdown();
        void RenderDebugUI();

        // Asset management
        uint32_t ImportAsset(const std::string& sourcePath, AssetFormat format);
        bool ProcessAsset(uint32_t assetId);
        bool ProcessAll();
        bool RemoveAsset(uint32_t assetId);

        // LOD configuration
        uint32_t ConfigureLOD(uint32_t assetId, LODStrategy strategy, uint32_t lodCount);
        bool GenerateLODs(uint32_t assetId);

        // Texture settings
        uint32_t ConfigureTexture(uint32_t assetId, bool mipmaps, uint32_t maxRes, bool compress);

        // Import rules
        uint32_t AddImportRule(const std::string& name, AssetFormat format, const std::string& pattern);
        bool RemoveImportRule(uint32_t ruleId);

        // Packaging
        bool PackageAssets(const std::string& outputDir);

        // Queries
        size_t GetAssetCount() const { return m_assets.size(); }
        size_t GetRuleCount() const { return m_rules.size(); }
        size_t GetProcessedCount() const;
        uint64_t GetTotalSizeBytes() const;
        std::string GetAssetListString() const;
        std::string GetAssetDetailString(uint32_t assetId) const;
        std::string GetRuleListString() const;
        std::string GetPipelineStatusString() const;

      private:
        void RegisterDefaultRules();
        bool ValidateAsset(PipelineAsset& asset);
        bool OptimizeAsset(PipelineAsset& asset);
        bool CompressAsset(PipelineAsset& asset);

        Spark::IEngineContext* m_context{nullptr};
        std::vector<PipelineAsset> m_assets;
        std::vector<LODConfig> m_lodConfigs;
        std::vector<TextureSettings> m_textureSettings;
        std::vector<ImportRule> m_rules;
        uint32_t m_nextAssetId{1};
        uint32_t m_nextLodId{1};
        uint32_t m_nextTexSettingsId{1};
        uint32_t m_nextRuleId{1};
        bool m_initialized{false};
    };

} // namespace EngineEditor
