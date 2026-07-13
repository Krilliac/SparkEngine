/**
 * @file AssetValidator.h
 * @brief Asset validation pipeline for content integrity checking
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Spark
{

    enum class ValidationSeverity : uint8_t
    {
        Info,
        Warning,
        Error,
        Critical
    };

    struct ValidationResult
    {
        ValidationSeverity severity = ValidationSeverity::Info;
        std::string assetPath;
        std::string message;
        std::string suggestion;
        uint32_t errorCode = 0;
    };

    struct ValidationReport
    {
        std::vector<ValidationResult> results;
        uint32_t totalAssets = 0;
        uint32_t passCount = 0;
        uint32_t failCount = 0;
        uint32_t warningCount = 0;
        std::string timestamp;
        float durationMs = 0.0f;
    };

    class IAssetValidationRule
    {
      public:
        virtual ~IAssetValidationRule() = default;
        virtual void ValidateAsset(const std::filesystem::path& path, ValidationReport& report) = 0;
        [[nodiscard]] virtual std::string_view GetRuleName() const = 0;
    };

    class MaterialTextureValidator final : public IAssetValidationRule
    {
      public:
        void ValidateAsset(const std::filesystem::path& path, ValidationReport& report) override;
        [[nodiscard]] std::string_view GetRuleName() const override;
    };

    class SceneReferenceValidator final : public IAssetValidationRule
    {
      public:
        void ValidateAsset(const std::filesystem::path& path, ValidationReport& report) override;
        [[nodiscard]] std::string_view GetRuleName() const override;
    };

    class ShaderCompilationValidator final : public IAssetValidationRule
    {
      public:
        void ValidateAsset(const std::filesystem::path& path, ValidationReport& report) override;
        [[nodiscard]] std::string_view GetRuleName() const override;
    };

    class AssetMetadataValidator final : public IAssetValidationRule
    {
      public:
        void ValidateAsset(const std::filesystem::path& path, ValidationReport& report) override;
        [[nodiscard]] std::string_view GetRuleName() const override;
    };

    class AssetValidator
    {
      public:
        static AssetValidator& GetInstance();

        void Initialize();
        void Shutdown();
        void RegisterRule(std::unique_ptr<IAssetValidationRule> rule);

        [[nodiscard]] ValidationReport ValidateAll();
        [[nodiscard]] ValidationReport ValidateFile(const std::filesystem::path& path);
        [[nodiscard]] ValidationReport ValidateDirectory(const std::filesystem::path& dir);

        [[nodiscard]] std::vector<std::string_view> GetRegisteredRules() const;
        [[nodiscard]] std::string Console_GetStatus() const;
        [[nodiscard]] std::string Console_GetLastReport() const;

        /// Register the assetvalidate.* console commands (status/report/dir).
        /// Called once during engine startup alongside the sibling diagnostic
        /// singletons (AssetStallDetector, HitchDetector, ...) so the pipeline
        /// is reachable from the console.
        void RegisterConsoleCommands();

      private:
        AssetValidator() = default;
        ~AssetValidator() = default;
        AssetValidator(const AssetValidator&) = delete;
        AssetValidator& operator=(const AssetValidator&) = delete;

        [[nodiscard]] static std::string GetTimestamp();

      private:
        bool m_initialized = false;
        std::vector<std::unique_ptr<IAssetValidationRule>> m_rules;
        ValidationReport m_lastReport;
    };

} // namespace Spark
