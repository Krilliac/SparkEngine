/**
 * @file BuildDeploymentSystem.h
 * @brief Build and deployment system for automated game packaging and distribution
 * @author Spark Engine Team
 * @date 2025
 * 
 * This file implements a comprehensive build and deployment system similar to
 * Unity Build Settings and Unreal's UnrealBuildTool, providing automated
 * building, packaging, and distribution for multiple platforms.
 */

#pragma once

#include "../Core/EditorPanel.h"
#include <imgui.h>
#include <vector>
#include <memory>
#include <unordered_map>
#include <string>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>
#include <queue>
#include <chrono>
#include <filesystem>
#include <condition_variable>

namespace SparkEditor
{

    /**
 * @brief Target platforms for building
 */
    enum class BuildPlatform
    {
        WINDOWS_X64 = 0,      ///< Windows 64-bit
        WINDOWS_X86 = 1,      ///< Windows 32-bit
        LINUX_X64 = 2,        ///< Linux 64-bit
        MACOS_X64 = 3,        ///< macOS Intel
        MACOS_ARM64 = 4,      ///< macOS Apple Silicon
        ANDROID_ARM64 = 5,    ///< Android ARM64
        ANDROID_ARM32 = 6,    ///< Android ARM32
        IOS_ARM64 = 7,        ///< iOS ARM64
        WEBGL = 8,            ///< WebGL/WebAssembly
        XBOX_ONE = 9,         ///< Xbox One
        XBOX_SERIES = 10,     ///< Xbox Series X/S
        PLAYSTATION_4 = 11,   ///< PlayStation 4
        PLAYSTATION_5 = 12,   ///< PlayStation 5
        NINTENDO_SWITCH = 13, ///< Nintendo Switch
        CUSTOM = 14           ///< Custom platform
    };

    /**
 * @brief Build configuration types
 */
    enum class BuildConfiguration
    {
        DEBUG = 0,       ///< Debug build with symbols
        DEVELOPMENT = 1, ///< Development build with some optimizations
        RELEASE = 2,     ///< Release build with full optimizations
        SHIPPING = 3,    ///< Shipping build with no debug features
        PROFILING = 4    ///< Profiling build with instrumentation
    };

    /**
 * @brief Build architecture
 */
    enum class BuildArchitecture
    {
        X86 = 0,      ///< x86 32-bit
        X64 = 1,      ///< x64 64-bit
        ARM32 = 2,    ///< ARM 32-bit
        ARM64 = 3,    ///< ARM 64-bit
        UNIVERSAL = 4 ///< Universal binary (multiple architectures)
    };

    /**
 * @brief Build status
 */
    enum class BuildStatus
    {
        IDLE = 0,      ///< No build in progress
        QUEUED = 1,    ///< Build queued
        PREPARING = 2, ///< Preparing build
        COMPILING = 3, ///< Compiling source code
        LINKING = 4,   ///< Linking executables
        PACKAGING = 5, ///< Packaging assets
        DEPLOYING = 6, ///< Deploying build
        COMPLETED = 7, ///< Build completed successfully
        FAILED = 8,    ///< Build failed
        CANCELLED = 9  ///< Build cancelled
    };

    /**
 * @brief Platform-specific build settings
 */
    struct PlatformBuildSettings
    {
        BuildPlatform platform;           ///< Target platform
        BuildConfiguration configuration; ///< Build configuration
        BuildArchitecture architecture;   ///< Target architecture
        bool enabled = true;              ///< Whether platform is enabled for building

        // Compiler settings
        std::string compiler;                   ///< Compiler to use
        std::string compilerVersion;            ///< Compiler version
        std::vector<std::string> compilerFlags; ///< Additional compiler flags
        std::vector<std::string> linkerFlags;   ///< Additional linker flags
        std::vector<std::string> defines;       ///< Preprocessor defines
        std::vector<std::string> includePaths;  ///< Include directories
        std::vector<std::string> libraryPaths;  ///< Library directories
        std::vector<std::string> libraries;     ///< Libraries to link

        // Optimization settings
        bool enableOptimizations = true; ///< Enable compiler optimizations
        bool enableLTO = false;          ///< Enable Link Time Optimization
        bool enablePGO = false;          ///< Enable Profile Guided Optimization
        bool stripSymbols = true;        ///< Strip debug symbols in release
        int optimizationLevel = 2;       ///< Optimization level (0-3)

        // Platform-specific settings
        std::unordered_map<std::string, std::string> platformSettings; ///< Platform-specific settings

        // Asset cooking
        bool cookAssets = true;                ///< Cook assets for platform
        std::vector<std::string> assetFormats; ///< Preferred asset formats
        bool compressAssets = true;            ///< Compress assets
        float compressionLevel = 0.8f;         ///< Asset compression level

        // Packaging
        std::string packageFormat;      ///< Package format (zip, installer, etc.)
        std::string outputDirectory;    ///< Output directory
        std::string executableName;     ///< Executable name
        std::string iconPath;           ///< Application icon
        bool createInstaller = false;   ///< Create installer package
        bool signBuild = false;         ///< Sign the build
        std::string signingCertificate; ///< Signing certificate path

        // Runtime settings
        bool includeDebugInfo = false;            ///< Include debug information
        bool includeSymbols = false;              ///< Include symbol files
        bool includePDB = false;                  ///< Include PDB files
        std::vector<std::string> excludedFiles;   ///< Files to exclude from package
        std::vector<std::string> additionalFiles; ///< Additional files to include
    };

    /**
 * @brief Build target configuration
 */
    struct BuildTarget
    {
        std::string name;                             ///< Target name
        std::string description;                      ///< Target description
        std::vector<PlatformBuildSettings> platforms; ///< Platform settings
        std::string outputPath;                       ///< Output path template
        bool enabled = true;                          ///< Whether target is enabled

        // Build dependencies
        std::vector<std::string> dependencies;      ///< Other targets this depends on
        std::vector<std::string> excludedPlatforms; ///< Platforms to exclude

        // Pre/post build steps
        std::vector<std::string> preBuildSteps;  ///< Commands to run before build
        std::vector<std::string> postBuildSteps; ///< Commands to run after build

        // Deployment settings
        struct DeploymentSettings
        {
            bool enableAutoDeployment = false;                               ///< Enable automatic deployment
            std::string deploymentMethod;                                    ///< Deployment method (ftp, steam, etc.)
            std::unordered_map<std::string, std::string> deploymentSettings; ///< Deployment settings
            std::vector<std::string> deploymentPlatforms;                    ///< Platforms to deploy
        } deployment;
    };

    /**
 * @brief Build job information
 */
    struct BuildJob
    {
        std::string id;                           ///< Unique job ID
        std::string targetName;                   ///< Build target name
        BuildPlatform platform;                   ///< Target platform
        BuildConfiguration configuration;         ///< Build configuration
        BuildStatus status = BuildStatus::QUEUED; ///< Current status
        float progress = 0.0f;                    ///< Build progress (0-1)

        // Timing
        std::chrono::steady_clock::time_point startTime; ///< Build start time
        std::chrono::steady_clock::time_point endTime;   ///< Build end time
        float duration = 0.0f;                           ///< Build duration in seconds

        // Results
        bool success = false;                 ///< Whether build succeeded
        std::string errorMessage;             ///< Error message if failed
        std::vector<std::string> warnings;    ///< Build warnings
        std::vector<std::string> outputFiles; ///< Generated output files
        size_t outputSize = 0;                ///< Total output size in bytes

        // Build log
        std::vector<std::string> buildLog; ///< Complete build log
        std::string logFilePath;           ///< Path to log file

        // Build statistics
        int sourceFiles = 0;          ///< Number of source files compiled
        int assetFiles = 0;           ///< Number of assets processed
        int objectFiles = 0;          ///< Number of object files generated
        float compilationTime = 0.0f; ///< Compilation time
        float linkingTime = 0.0f;     ///< Linking time
        float packagingTime = 0.0f;   ///< Packaging time
    };

    /**
 * @brief Asset cooking settings
 */
    struct AssetCookingSettings
    {
        bool enableCooking = true;       ///< Enable asset cooking
        bool enableCompression = true;   ///< Enable asset compression
        bool enableOptimization = true;  ///< Enable asset optimization
        bool generateMipMaps = true;     ///< Generate texture mipmaps
        bool enableLODGeneration = true; ///< Generate mesh LODs

        // Platform-specific formats
        std::unordered_map<BuildPlatform, std::vector<std::string>> platformFormats;

        // Compression settings
        float textureCompressionQuality = 0.8f; ///< Texture compression quality
        float audioCompressionQuality = 0.7f;   ///< Audio compression quality
        float meshCompressionLevel = 0.9f;      ///< Mesh compression level

        // Optimization settings
        bool removeUnusedAssets = true; ///< Remove unused assets
        bool optimizeForSize = false;   ///< Optimize for package size
        bool optimizeForSpeed = true;   ///< Optimize for loading speed

        // Cache settings
        bool enableCookedAssetCache = true;                ///< Enable cooked asset cache
        std::string cacheDirectory = "Temp/CookedAssets/"; ///< Cache directory
        bool useDependencyCache = true;                    ///< Use dependency-based caching
    };

    /**
 * @brief Deployment configuration
 */
    struct DeploymentConfig
    {
        enum Method
        {
            NONE = 0,
            LOCAL_COPY = 1,
            FTP_UPLOAD = 2,
            STEAM_UPLOAD = 3,
            GOOGLE_PLAY = 4,
            APP_STORE = 5,
            MICROSOFT_STORE = 6,
            CUSTOM_SCRIPT = 7
        } method = NONE;

        std::string name;        ///< Deployment configuration name
        std::string description; ///< Description
        bool enabled = false;    ///< Whether deployment is enabled

        // Target settings
        std::string targetPath; ///< Target deployment path
        std::string targetURL;  ///< Target URL (for uploads)
        std::string username;   ///< Username for authentication
        std::string password;   ///< Password for authentication
        std::string apiKey;     ///< API key for services

        // Upload settings
        bool enableCompression = true;       ///< Compress before upload
        bool enableEncryption = false;       ///< Encrypt uploads
        bool enableIncrementalUpload = true; ///< Only upload changed files
        int maxRetries = 3;                  ///< Maximum upload retries
        float timeoutSeconds = 300.0f;       ///< Upload timeout

        // Notification settings
        bool notifyOnCompletion = true; ///< Notify when deployment completes
        bool notifyOnFailure = true;    ///< Notify when deployment fails
        std::string notificationEmail;  ///< Email for notifications
        std::string webhookURL;         ///< Webhook for notifications

        // Platform-specific settings
        std::unordered_map<std::string, std::string> platformSettings;
    };

    /**
 * @brief Build system configuration
 */
    struct BuildSystemConfig
    {
        // General settings
        int maxConcurrentJobs = 4;                            ///< Maximum concurrent build jobs
        bool enableParallelCompilation = true;                ///< Enable parallel compilation
        bool enableDistributedBuilds = false;                 ///< Enable distributed builds
        bool enableBuildCache = true;                         ///< Enable build caching
        std::string buildCacheDirectory = "Temp/BuildCache/"; ///< Build cache directory

        // Toolchain settings
        // Supported compiler identifiers:
        //   "msvc"        - MSVC (auto-detect installed VS version)
        //   "vs2022"      - Visual Studio 2022 (v143, _MSC_VER 1930-1939)
        //   "vs2026"      - Visual Studio 2026 (v144, _MSC_VER 1940+)
        //   "gcc"         - GCC (Linux/macOS)
        //   "clang"       - LLVM Clang
        //   "apple-clang" - Apple Clang (macOS)
        //   "icpx"        - Intel oneAPI DPC++/C++ Compiler (ICPX/IntelLLVM)
        std::string defaultCompiler = "msvc";                        ///< Default compiler
        std::string buildToolPath;                                   ///< Build tool path
        std::string sdkPath;                                         ///< SDK path
        std::unordered_map<std::string, std::string> toolchainPaths; ///< Toolchain paths

        // Asset cooking
        AssetCookingSettings assetCooking; ///< Asset cooking settings

        // Build automation
        bool enableAutomaticBuilds = false;     ///< Enable automatic builds
        std::string buildTrigger = "on_commit"; ///< Build trigger event
        std::vector<std::string> buildSchedule; ///< Build schedule (cron format)

        // Quality assurance
        bool enableUnitTests = true;       ///< Run unit tests during build
        bool enableStaticAnalysis = false; ///< Run static analysis
        bool enableCodeCoverage = false;   ///< Collect code coverage
        bool failOnWarnings = false;       ///< Fail build on warnings

        // Deployment
        std::vector<DeploymentConfig> deploymentConfigs; ///< Deployment configurations
        bool enableAutomaticDeployment = false;          ///< Enable automatic deployment

        // Notifications
        bool enableBuildNotifications = true;       ///< Enable build notifications
        std::string notificationMethod = "desktop"; ///< Notification method
        std::string slackWebhook;                   ///< Slack webhook for notifications
        std::string discordWebhook;                 ///< Discord webhook for notifications
    };

    /**
 * @brief Build statistics
 */
    struct BuildStatistics
    {
        int totalBuilds = 0;                                 ///< Total builds executed
        int successfulBuilds = 0;                            ///< Successful builds
        int failedBuilds = 0;                                ///< Failed builds
        float averageBuildTime = 0.0f;                       ///< Average build time
        float totalBuildTime = 0.0f;                         ///< Total build time
        size_t totalOutputSize = 0;                          ///< Total output size
        std::chrono::system_clock::time_point lastBuildTime; ///< Last build time

        // Platform statistics
        std::unordered_map<BuildPlatform, int> platformBuilds;       ///< Builds per platform
        std::unordered_map<BuildPlatform, float> platformBuildTimes; ///< Build times per platform

        // Performance metrics
        float averageCompilationTime = 0.0f; ///< Average compilation time
        float averageLinkingTime = 0.0f;     ///< Average linking time
        float averagePackagingTime = 0.0f;   ///< Average packaging time
        float cacheHitRate = 0.0f;           ///< Build cache hit rate
    };

    /**
 * @brief Build and deployment system
 * 
 * Provides comprehensive build automation including:
 * - Multi-platform build configuration and execution
 * - Automated asset cooking and optimization
 * - Parallel and distributed build support
 * - Build caching and incremental builds
 * - Integrated testing and quality assurance
 * - Automated deployment to multiple platforms
 * - Build statistics and performance monitoring
 * - Integration with version control and CI/CD
 * - Custom build scripts and hooks
 * 
 * Inspired by Unity Build Settings, Unreal's UnrealBuildTool, and modern CI/CD systems.
 */
    class BuildDeploymentSystem : public EditorPanel
    {
      public:
        /**
     * @brief Constructor
     */
        BuildDeploymentSystem();

        /**
     * @brief Destructor
     */
        ~BuildDeploymentSystem() override;

        /**
     * @brief Initialize the build system
     * @return true if initialization succeeded
     */
        bool Initialize() override;

        /**
     * @brief Update build system
     * @param deltaTime Time elapsed since last update
     */
        void Update(float deltaTime) override;

        /**
     * @brief Render build system UI
     */
        void Render() override;

        /**
     * @brief Shutdown the build system
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
     * @brief Add build target
     * @param target Build target to add
     */
        void AddBuildTarget(const BuildTarget& target);

        /**
     * @brief Remove build target
     * @param targetName Name of target to remove
     * @return true if target was removed
     */
        bool RemoveBuildTarget(const std::string& targetName);

        /**
     * @brief Get build target
     * @param targetName Target name
     * @return Pointer to build target, or nullptr if not found
     */
        BuildTarget* GetBuildTarget(const std::string& targetName);

        /**
     * @brief Get all build targets
     * @return Vector of all build targets
     */
        const std::vector<BuildTarget>& GetBuildTargets() const { return m_buildTargets; }

        /**
     * @brief Start build job
     * @param targetName Build target name
     * @param platform Target platform
     * @param configuration Build configuration
     * @param progressCallback Progress callback
     * @return Build job ID
     */
        std::string StartBuild(const std::string& targetName, BuildPlatform platform, BuildConfiguration configuration,
                               std::function<void(float)> progressCallback = nullptr);

        /**
     * @brief Start builds for all platforms in target
     * @param targetName Build target name
     * @param configuration Build configuration
     * @return Vector of build job IDs
     */
        std::vector<std::string> StartBuildAll(const std::string& targetName, BuildConfiguration configuration);

        /**
     * @brief Cancel build job
     * @param jobID Build job ID
     * @return true if job was cancelled
     */
        bool CancelBuild(const std::string& jobID);

        /**
     * @brief Get build job information
     * @param jobID Build job ID
     * @return Pointer to build job, or nullptr if not found
     */
        const BuildJob* GetBuildJob(const std::string& jobID) const;

        /**
     * @brief Get all build jobs
     * @return Vector of all build jobs
     */
        const std::vector<BuildJob>& GetBuildJobs() const { return m_buildJobs; }

        /**
     * @brief Get active build jobs
     * @return Vector of active build jobs
     */
        std::vector<const BuildJob*> GetActiveBuildJobs() const;

        /**
     * @brief Cook assets for platform
     * @param platform Target platform
     * @param outputPath Output path for cooked assets
     * @param progressCallback Progress callback
     * @return true if cooking succeeded
     */
        bool CookAssets(BuildPlatform platform, const std::string& outputPath,
                        std::function<void(float)> progressCallback = nullptr);

        /**
     * @brief Package build for distribution
     * @param jobID Build job ID
     * @param packageFormat Package format
     * @param outputPath Output path
     * @return true if packaging succeeded
     */
        bool PackageBuild(const std::string& jobID, const std::string& packageFormat, const std::string& outputPath);

        /**
     * @brief Deploy build to configured targets
     * @param jobID Build job ID
     * @param deploymentConfig Deployment configuration name
     * @param progressCallback Progress callback
     * @return true if deployment started successfully
     */
        bool DeployBuild(const std::string& jobID, const std::string& deploymentConfig,
                         std::function<void(float)> progressCallback = nullptr);

        /**
     * @brief Add deployment configuration
     * @param config Deployment configuration
     */
        void AddDeploymentConfig(const DeploymentConfig& config);

        /**
     * @brief Remove deployment configuration
     * @param configName Configuration name
     * @return true if configuration was removed
     */
        bool RemoveDeploymentConfig(const std::string& configName);

        /**
     * @brief Get deployment configuration
     * @param configName Configuration name
     * @return Pointer to deployment config, or nullptr if not found
     */
        const DeploymentConfig* GetDeploymentConfig(const std::string& configName) const;

        /**
     * @brief Set build system configuration
     * @param config Build system configuration
     */
        void SetConfiguration(const BuildSystemConfig& config);

        /**
     * @brief Get build system configuration
     * @return Reference to build system configuration
     */
        const BuildSystemConfig& GetConfiguration() const { return m_config; }

        /**
     * @brief Get build statistics
     * @return Current build statistics
     */
        BuildStatistics GetBuildStatistics() const;

        /**
     * @brief Clear build history
     */
        void ClearBuildHistory();

        /**
     * @brief Export build configuration
     * @param filePath Output file path
     * @return true if export succeeded
     */
        bool ExportBuildConfiguration(const std::string& filePath);

        /**
     * @brief Import build configuration
     * @param filePath Input file path
     * @return true if import succeeded
     */
        bool ImportBuildConfiguration(const std::string& filePath);

        /**
     * @brief Validate build environment
     * @param platform Target platform
     * @param errors Output error messages
     * @return true if environment is valid
     */
        bool ValidateBuildEnvironment(BuildPlatform platform, std::vector<std::string>& errors);

        /**
     * @brief Install platform SDK
     * @param platform Platform to install SDK for
     * @param progressCallback Progress callback
     * @return true if installation succeeded
     */
        bool InstallPlatformSDK(BuildPlatform platform, std::function<void(float)> progressCallback = nullptr);

        /**
     * @brief Check for build system updates
     * @return true if updates are available
     */
        bool CheckForUpdates();

        /**
     * @brief Run unit tests
     * @param platform Target platform
     * @param configuration Build configuration
     * @return Test results
     */
        std::string RunUnitTests(BuildPlatform platform, BuildConfiguration configuration);

        /**
     * @brief Run static analysis
     * @param sourceFiles Files to analyze
     * @return Analysis results
     */
        std::string RunStaticAnalysis(const std::vector<std::string>& sourceFiles);

        /**
     * @brief Generate build report
     * @param jobID Build job ID
     * @param format Report format ("html", "json", "xml")
     * @return Generated report content
     */
        std::string GenerateBuildReport(const std::string& jobID, const std::string& format = "html");

      private:
        /**
     * @brief Render build targets panel
     */
        void RenderBuildTargetsPanel();

        /**
     * @brief Render build queue panel
     */
        void RenderBuildQueuePanel();

        /**
     * @brief Render build history panel
     */
        void RenderBuildHistoryPanel();

        /**
     * @brief Render deployment panel
     */
        void RenderDeploymentPanel();

        /**
     * @brief Render configuration panel
     */
        void RenderConfigurationPanel();

        /**
     * @brief Render statistics panel
     */
        void RenderStatisticsPanel();

        /**
     * @brief Process build queue
     */
        void ProcessBuildQueue();

        /**
     * @brief Execute build job
     * @param job Build job to execute
     * @return true if build succeeded
     */
        bool ExecuteBuildJob(BuildJob& job);

        /**
     * @brief Compile source code
     * @param job Build job
     * @param settings Platform build settings
     * @return true if compilation succeeded
     */
        bool CompileSourceCode(BuildJob& job, const PlatformBuildSettings& settings);

        /**
     * @brief Link executables
     * @param job Build job
     * @param settings Platform build settings
     * @return true if linking succeeded
     */
        bool LinkExecutables(BuildJob& job, const PlatformBuildSettings& settings);

        /**
     * @brief Package assets
     * @param job Build job
     * @param settings Platform build settings
     * @return true if packaging succeeded
     */
        bool PackageAssets(BuildJob& job, const PlatformBuildSettings& settings);

        /**
     * @brief Run pre-build steps
     * @param target Build target
     * @param job Build job
     * @return true if all steps succeeded
     */
        bool RunPreBuildSteps(const BuildTarget& target, BuildJob& job);

        /**
     * @brief Run post-build steps
     * @param target Build target
     * @param job Build job
     * @return true if all steps succeeded
     */
        bool RunPostBuildSteps(const BuildTarget& target, BuildJob& job);

        /**
     * @brief Execute shell command
     * @param command Command to execute
     * @param workingDirectory Working directory
     * @param output Output buffer
     * @return Exit code
     */
        int ExecuteCommand(const std::string& command, const std::string& workingDirectory, std::string& output);

        /**
     * @brief Get platform-specific settings
     * @param platform Target platform
     * @return Platform build settings
     */
        PlatformBuildSettings GetPlatformSettings(BuildPlatform platform) const;

        /**
     * @brief Get platform name string
     * @param platform Build platform
     * @return Platform name
     */
        std::string GetPlatformName(BuildPlatform platform) const;

        /**
     * @brief Get configuration name string
     * @param configuration Build configuration
     * @return Configuration name
     */
        std::string GetConfigurationName(BuildConfiguration configuration) const;

        /**
     * @brief Update build statistics
     */
        void UpdateStatistics();

        /**
     * @brief Send build notification
     * @param job Build job
     * @param message Notification message
     */
        void SendBuildNotification(const BuildJob& job, const std::string& message);

        /**
     * @brief Save build configuration to file
     * @return true if save succeeded
     */
        bool SaveConfiguration();

        /**
     * @brief Load build configuration from file
     * @return true if load succeeded
     */
        bool LoadConfiguration();

        /**
     * @brief Create default build targets
     */
        void CreateDefaultBuildTargets();

        /**
     * @brief Detect available platforms
     */
        void DetectPlatformAvailability();

        /**
     * @brief Check for completed builds
     */
        void UpdateCompletedBuilds();

      private:
        // Build configuration
        std::vector<BuildTarget> m_buildTargets; ///< Build targets
        BuildSystemConfig m_config;              ///< Build system configuration

        // Build queue and jobs
        std::vector<BuildJob> m_buildJobs;           ///< All build jobs (history)
        std::queue<std::string> m_buildQueue;        ///< Pending build job IDs
        std::vector<std::thread> m_buildThreads;     ///< Build worker threads
        mutable std::mutex m_buildMutex;             ///< Build queue mutex
        std::condition_variable m_buildCondition;    ///< Build queue condition
        std::atomic<bool> m_shouldStopBuilds{false}; ///< Stop builds flag

        // Asset cooking
        AssetCookingSettings m_cookingSettings; ///< Asset cooking settings
        std::thread m_cookingThread;            ///< Asset cooking thread
        std::atomic<bool> m_isCooking{false};   ///< Whether cooking is active

        // Deployment
        std::vector<DeploymentConfig> m_deploymentConfigs; ///< Deployment configurations
        std::thread m_deploymentThread;                    ///< Deployment thread
        std::atomic<bool> m_isDeploying{false};            ///< Whether deployment is active

        // Statistics
        mutable BuildStatistics m_statistics; ///< Build statistics

        // UI state
        std::string m_selectedTarget;     ///< Selected build target
        std::string m_selectedJob;        ///< Selected build job
        bool m_showBuildTargets = true;   ///< Show build targets panel
        bool m_showBuildQueue = true;     ///< Show build queue panel
        bool m_showBuildHistory = true;   ///< Show build history panel
        bool m_showDeployment = false;    ///< Show deployment panel
        bool m_showConfiguration = false; ///< Show configuration panel
        bool m_showStatistics = true;     ///< Show statistics panel

        // Build environment
        std::unordered_map<BuildPlatform, bool> m_platformAvailable;       ///< Platform availability
        std::unordered_map<BuildPlatform, std::string> m_platformSDKPaths; ///< Platform SDK paths

        // Cache and optimization
        std::string m_buildCacheDirectory; ///< Build cache directory
        std::unordered_map<std::string, std::time_t>
            m_sourceFileTimestamps; ///< Source file timestamps (using time_t for C++17 compatibility)

        // Notification settings
        bool m_enableNotifications = true;       ///< Enable notifications
        std::vector<std::string> m_recentBuilds; ///< Recent build IDs for quick access

        // Performance monitoring
        std::chrono::steady_clock::time_point m_lastStatisticsUpdate; ///< Last statistics update
        float m_statisticsUpdateInterval = 5.0f;                      ///< Statistics update interval

        // Next ID generators
        std::atomic<int> m_nextJobID{1}; ///< Next job ID
    };

} // namespace SparkEditor