/**
 * @file GamePackager.h
 * @brief Game packaging pipeline for distribution builds
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Spark
{

    enum class TargetPlatform : uint8_t
    {
        Windows,
        Linux,
        macOS
    };

    enum class PackageBuildConfig : uint8_t
    {
        Debug,
        Release
    };

    struct PackageConfig
    {
        std::string outputDir = "Build/Package";
        std::string projectName = "SparkGame";
        TargetPlatform platform = TargetPlatform::Windows;
        PackageBuildConfig buildConfig = PackageBuildConfig::Release;
        bool stripDebugSymbols = true;
        bool compressAssets = true;
        bool includeEditor = false;
    };

    struct PackageResult
    {
        bool success = false;
        std::string outputPath;
        float totalSizeMB = 0.0f;
        std::vector<std::string> errors;
        std::vector<std::string> warnings;
        uint32_t assetCount = 0;
        uint32_t dllCount = 0;
    };

    class GamePackager
    {
      public:
        static GamePackager& GetInstance();

        void Initialize();
        void Shutdown();

        PackageResult Package(const PackageConfig& config);
        [[nodiscard]] std::vector<std::string> ValidateConfig(const PackageConfig& config) const;
        [[nodiscard]] std::vector<TargetPlatform> GetSupportedPlatforms() const;

        [[nodiscard]] std::string Console_GetStatus() const;

      private:
        GamePackager() = default;
        ~GamePackager() = default;
        GamePackager(const GamePackager&) = delete;
        GamePackager& operator=(const GamePackager&) = delete;

        [[nodiscard]] static std::string_view PlatformToString(TargetPlatform p);
        [[nodiscard]] static std::string_view GetDllExtension(TargetPlatform p);
        [[nodiscard]] static std::string_view GetExeExtension(TargetPlatform p);

        [[nodiscard]] std::pair<uint32_t, std::vector<std::string>> CookAssets(
            const PackageConfig& config, const std::filesystem::path& outputRoot) const;
        [[nodiscard]] std::pair<uint32_t, std::vector<std::string>> CopyBinaries(
            const PackageConfig& config, const std::filesystem::path& outputRoot) const;
        [[nodiscard]] std::vector<std::string> StripSymbols(const std::filesystem::path& outputRoot) const;
        [[nodiscard]] bool CreateManifest(const PackageConfig& config, const std::filesystem::path& outputRoot,
                                          const PackageResult& result) const;
        [[nodiscard]] std::vector<std::string> CompressOutput(const std::filesystem::path& outputRoot) const;

      private:
        bool m_initialized = false;
        std::vector<TargetPlatform> m_supportedPlatforms;
        PackageResult m_lastResult;
    };

} // namespace Spark
