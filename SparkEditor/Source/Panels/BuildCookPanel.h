/**
 * @file BuildCookPanel.h
 * @brief Build, cook, and package settings panel for the Spark Engine Editor
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides UI for configuring build profiles (Debug/Development/Shipping),
 * asset cooking, console/debug stripping, and packaging options.
 */

#pragma once

#include "../Core/EditorPanel.h"
#include <string>
#include <vector>

namespace SparkEditor
{

    class BuildCookPanel : public EditorPanel
    {
      public:
        enum class BuildProfile
        {
            Debug,
            Development,
            Release,
            Shipping
        };

        enum class TargetPlatform
        {
            WindowsX64,
            WindowsX86,
            LinuxX64,
            MacOSX64,
            MacOSARM64
        };

        struct BuildSettings
        {
            BuildProfile profile = BuildProfile::Development;
            TargetPlatform platform = TargetPlatform::WindowsX64;

            // Debug/Development features
            bool includeDebugSymbols = true;
            bool includeConsole = true;
            bool includeEditor = false;
            bool includeProfiler = true;
            bool includeDevCommands = true;
            bool includeLogSystem = true;
            bool enableAsserts = true;
            bool enableHotReload = false;
            bool enableValidationLayers = false;

            // Shipping stripping
            bool stripDebugSymbols = false;
            bool stripConsole = false;
            bool stripProfiler = false;
            bool stripDevCommands = false;
            bool stripLogFiles = false;
            bool stripEditorAssets = true;
            bool stripUnusedAssets = true;
            bool stripTestContent = true;

            // Optimization
            bool enableLTO = false;
            bool enablePGO = false;
            int optimizationLevel = 2; // 0=None, 1=Size, 2=Speed, 3=Full

            // Asset cooking
            bool cookAssets = true;
            bool compressTextures = true;
            int textureMaxSize = 2048;
            bool generateMipmaps = true;
            bool compressAudio = true;
            int audioQuality = 80; // 0-100
            bool compressMeshes = true;
            bool generateLODs = false;
            int lodLevels = 3;

            // Packaging
            bool createInstaller = false;
            bool compressPackage = true;
            int compressionLevel = 6; // 1-9
            std::string outputDirectory = "Build/Output";
            std::string executableName = "SparkGame";

            // Code signing
            bool signExecutable = false;
            std::string signingCertificate;
        };

        BuildCookPanel();
        ~BuildCookPanel() override;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;
        bool HandleEvent(const std::string& eventType, void* eventData) override;

        const BuildSettings& GetSettings() const { return m_settings; }

      private:
        void RenderProfileSelector();
        void RenderPlatformSelector();
        void RenderBuildOptions();
        void RenderStrippingOptions();
        void RenderAssetCooking();
        void RenderPackagingOptions();
        void RenderBuildActions();
        void RenderBuildLog();
        void ApplyProfileDefaults(BuildProfile profile);
        static const char* GetProfileName(BuildProfile profile);
        static const char* GetPlatformName(TargetPlatform platform);
        static const char* GetOptimizationName(int level);

        BuildSettings m_settings;

        // Build log
        struct BuildLogEntry
        {
            std::string message;
            std::string level; // info, warning, error
        };
        std::vector<BuildLogEntry> m_buildLog;
        bool m_isBuildRunning = false;
        float m_buildProgress = 0.0f;
        std::string m_buildStatus = "Idle";
    };

} // namespace SparkEditor
