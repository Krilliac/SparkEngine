/**
 * @file GamePackager.h
 * @brief Compatibility surface for the canonical build packager.
 *
 * The packaging implementation is owned by Engine/Build/GamePackager.h/.cpp.
 * This header remains source-compatible with the original Core API while
 * forwarding all work to that implementation.
 */

#pragma once

#include "Engine/Build/GamePackager.h"

#include <cstdint>
#include <string>
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

    /**
     * @brief Legacy Core facade over Spark::Build::GamePackager.
     *
     * [game thread] The canonical packager is intentionally not thread-safe;
     * callers must invoke this facade from the main/game thread.
     */
    class GamePackager
    {
      public:
        static GamePackager& GetInstance();

        // [game thread]
        void Initialize();
        // [game thread]
        void Shutdown();
        // [game thread]
        PackageResult Package(const PackageConfig& config);
        // [game thread, non-thread-safe]
        [[nodiscard]] std::vector<std::string> ValidateConfig(const PackageConfig& config) const;
        // [game thread, non-thread-safe]
        [[nodiscard]] std::vector<TargetPlatform> GetSupportedPlatforms() const;
        // [game thread, non-thread-safe]
        [[nodiscard]] std::string Console_GetStatus() const;

      private:
        GamePackager() = default;
        ~GamePackager() = default;
        GamePackager(const GamePackager&) = delete;
        GamePackager& operator=(const GamePackager&) = delete;

        bool m_initialized = false;
        std::vector<TargetPlatform> m_supportedPlatforms;
        PackageResult m_lastResult;
    };

} // namespace Spark
