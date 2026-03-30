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
#include "BuildPipeline.h"
#include <memory>
#include <string>
#include <vector>

namespace SparkEditor
{

    /** @brief Build, cook, and packaging configuration panel for standalone game builds. */
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
            BuildProfile profile = BuildProfile::Development;     ///< Active build profile.
            TargetPlatform platform = TargetPlatform::WindowsX64; ///< Target platform.

            // Debug/Development features
            bool includeDebugSymbols = true;     ///< Include PDB/DWARF debug info.
            bool includeConsole = true;          ///< Include the in-game console.
            bool includeEditor = false;          ///< Bundle the editor runtime.
            bool includeProfiler = true;         ///< Include the performance profiler.
            bool includeDevCommands = true;      ///< Include developer console commands.
            bool includeLogSystem = true;        ///< Include file-based log output.
            bool enableAsserts = true;           ///< Enable runtime assertion checks.
            bool enableHotReload = false;        ///< Enable script/asset hot-reload.
            bool enableValidationLayers = false; ///< Enable GPU validation layers.

            // Shipping stripping
            bool stripDebugSymbols = false; ///< Remove debug symbols from the final binary.
            bool stripConsole = false;      ///< Remove console system from shipping builds.
            bool stripProfiler = false;     ///< Remove profiler from shipping builds.
            bool stripDevCommands = false;  ///< Remove dev commands from shipping builds.
            bool stripLogFiles = false;     ///< Disable log file output in shipping.
            bool stripEditorAssets = true;  ///< Exclude editor-only assets from the package.
            bool stripUnusedAssets = true;  ///< Exclude assets not referenced by any scene.
            bool stripTestContent = true;   ///< Exclude test/placeholder content.

            // Optimization
            bool enableLTO = false;    ///< Enable link-time optimization.
            bool enablePGO = false;    ///< Enable profile-guided optimization.
            int optimizationLevel = 2; ///< 0=None, 1=Size, 2=Speed, 3=Full.

            // Asset cooking
            bool cookAssets = true;       ///< Convert assets to platform-optimized formats.
            bool compressTextures = true; ///< Apply texture compression (BC/ASTC).
            int textureMaxSize = 2048;    ///< Maximum texture dimension (pixels).
            bool generateMipmaps = true;  ///< Auto-generate mipmap chains.
            bool compressAudio = true;    ///< Compress audio to Vorbis/Opus.
            int audioQuality = 80;        ///< Audio compression quality [0-100].
            bool compressMeshes = true;   ///< Apply mesh quantization/compression.
            bool generateLODs = false;    ///< Auto-generate LOD meshes.
            int lodLevels = 3;            ///< Number of LOD levels to generate.

            // Packaging
            bool createInstaller = false;                 ///< Generate a platform installer (NSIS/DMG).
            bool compressPackage = true;                  ///< Compress the output package archive.
            int compressionLevel = 6;                     ///< Compression strength [1-9].
            std::string outputDirectory = "Build/Output"; ///< Output directory for the build.
            std::string executableName = "SparkGame";     ///< Name of the output executable.

            // Code signing
            bool signExecutable = false;    ///< Whether to digitally sign the executable.
            std::string signingCertificate; ///< Path to the signing certificate.
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
            std::string message; ///< Log message text.
            std::string level;   ///< Severity: "info", "warning", or "error".
        };
        std::vector<BuildLogEntry> m_buildLog; ///< Build output log entries.
        bool m_isBuildRunning = false;         ///< Whether a build is currently in progress.
        float m_buildProgress = 0.0f;          ///< Build completion [0, 1].
        std::string m_buildStatus = "Idle";    ///< Current build phase description.

        /// @brief Async build orchestrator — manages CMake subprocess.
        BuildPipeline m_pipeline;
    };

} // namespace SparkEditor
