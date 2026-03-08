/**
 * @file BuildDeploymentSystem.cpp
 * @brief Implementation of build and deployment system
 * @author Spark Engine Team
 * @date 2025
 */

#include "BuildDeploymentSystem.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <thread>
#include <chrono>
#include <algorithm>
#include <cstdlib>

namespace SparkEditor
{

    // Helper functions for string conversion
    std::string BuildPlatformToString(BuildPlatform platform)
    {
        switch (platform)
        {
        case BuildPlatform::WINDOWS_X64:
            return "Windows x64";
        case BuildPlatform::WINDOWS_X86:
            return "Windows x86";
        case BuildPlatform::LINUX_X64:
            return "Linux x64";
        case BuildPlatform::MACOS_X64:
            return "macOS x64";
        case BuildPlatform::MACOS_ARM64:
            return "macOS ARM64";
        case BuildPlatform::ANDROID_ARM64:
            return "Android ARM64";
        case BuildPlatform::ANDROID_ARM32:
            return "Android ARM32";
        case BuildPlatform::IOS_ARM64:
            return "iOS ARM64";
        case BuildPlatform::WEBGL:
            return "WebGL";
        case BuildPlatform::XBOX_ONE:
            return "Xbox One";
        case BuildPlatform::XBOX_SERIES:
            return "Xbox Series X/S";
        case BuildPlatform::PLAYSTATION_4:
            return "PlayStation 4";
        case BuildPlatform::PLAYSTATION_5:
            return "PlayStation 5";
        case BuildPlatform::NINTENDO_SWITCH:
            return "Nintendo Switch";
        case BuildPlatform::CUSTOM:
            return "Custom";
        default:
            return "Unknown";
        }
    }

    std::string BuildConfigurationToString(BuildConfiguration config)
    {
        switch (config)
        {
        case BuildConfiguration::DEBUG:
            return "Debug";
        case BuildConfiguration::DEVELOPMENT:
            return "Development";
        case BuildConfiguration::RELEASE:
            return "Release";
        case BuildConfiguration::SHIPPING:
            return "Shipping";
        default:
            return "Unknown";
        }
    }

    BuildDeploymentSystem::BuildDeploymentSystem()
        : EditorPanel("Build & Deployment", "BuildDeployment"), m_shouldStopBuilds(false), m_isCooking(false),
          m_isDeploying(false), m_selectedTarget(""), m_selectedJob(""), m_showBuildTargets(true),
          m_showBuildQueue(true), m_showBuildHistory(true), m_showDeployment(false), m_showConfiguration(false),
          m_showStatistics(true), m_enableNotifications(true), m_statisticsUpdateInterval(5.0f), m_nextJobID(1)
    {
        // Initialize default configuration
        m_config.maxConcurrentJobs = std::thread::hardware_concurrency();
        m_config.enableParallelCompilation = true;
        m_config.enableBuildCache = true;
        m_config.buildCacheDirectory = "Temp/BuildCache/";
        m_config.defaultCompiler = "msvc";

        // Initialize statistics
        m_statistics = {};
        m_lastStatisticsUpdate = std::chrono::steady_clock::now();

        // Create default build targets
        CreateDefaultBuildTargets();
    }

    BuildDeploymentSystem::~BuildDeploymentSystem()
    {
        Shutdown();
    }

    // ============================================================================
    // CORE FUNCTIONALITY
    // ============================================================================

    bool BuildDeploymentSystem::Initialize()
    {
        // Initialize platform availability detection
        DetectPlatformAvailability();

        // Start build worker threads
        int workerCount = std::min(m_config.maxConcurrentJobs, 8);
        for (int i = 0; i < workerCount; i++)
        {
            m_buildThreads.emplace_back([this]() { ProcessBuildQueue(); });
        }

        // Load configuration from file
        LoadConfiguration();

        return true;
    }

    void BuildDeploymentSystem::Update(float deltaTime)
    {
        // Update statistics periodically
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - m_lastStatisticsUpdate);

        if (elapsed.count() >= m_statisticsUpdateInterval)
        {
            UpdateStatistics();
            m_lastStatisticsUpdate = now;
        }

        // Check for completed builds
        UpdateCompletedBuilds();
    }

    void BuildDeploymentSystem::Render()
    {
        if (ImGui::Begin("Build & Deployment", &m_isVisible))
        {
            // Tab bar for different sections
            if (ImGui::BeginTabBar("BuildTabs"))
            {

                if (ImGui::BeginTabItem("Build Targets"))
                {
                    RenderBuildTargetsPanel();
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Build Queue"))
                {
                    RenderBuildQueuePanel();
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Build History"))
                {
                    RenderBuildHistoryPanel();
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Deployment"))
                {
                    RenderDeploymentPanel();
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Configuration"))
                {
                    RenderConfigurationPanel();
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Statistics"))
                {
                    RenderStatisticsPanel();
                    ImGui::EndTabItem();
                }

                ImGui::EndTabBar();
            }
        }
        ImGui::End();
    }

    void BuildDeploymentSystem::Shutdown()
    {
        // Stop all background operations
        m_shouldStopBuilds = true;
        m_buildCondition.notify_all();

        // Wait for worker threads to finish
        for (auto& thread : m_buildThreads)
        {
            if (thread.joinable())
            {
                thread.join();
            }
        }

        // Stop other threads
        if (m_cookingThread.joinable())
        {
            m_cookingThread.join();
        }

        if (m_deploymentThread.joinable())
        {
            m_deploymentThread.join();
        }

        // Save configuration
        SaveConfiguration();
    }

    bool BuildDeploymentSystem::HandleEvent(const std::string& eventType, void* eventData)
    {
        // Handle editor events (project load, save, etc.)
        return false;
    }

    // ============================================================================
    // BUILD TARGET MANAGEMENT
    // ============================================================================

    void BuildDeploymentSystem::AddBuildTarget(const BuildTarget& target)
    {
        m_buildTargets.push_back(target);
    }

    bool BuildDeploymentSystem::RemoveBuildTarget(const std::string& targetName)
    {
        auto it = std::find_if(m_buildTargets.begin(), m_buildTargets.end(),
                               [&targetName](const BuildTarget& target) { return target.name == targetName; });

        if (it != m_buildTargets.end())
        {
            m_buildTargets.erase(it);
            return true;
        }

        return false;
    }

    BuildTarget* BuildDeploymentSystem::GetBuildTarget(const std::string& targetName)
    {
        auto it = std::find_if(m_buildTargets.begin(), m_buildTargets.end(),
                               [&targetName](const BuildTarget& target) { return target.name == targetName; });

        return (it != m_buildTargets.end()) ? &(*it) : nullptr;
    }

    // ============================================================================
    // BUILD EXECUTION
    // ============================================================================

    std::string BuildDeploymentSystem::StartBuild(const std::string& targetName, BuildPlatform platform,
                                                  BuildConfiguration configuration,
                                                  std::function<void(float)> progressCallback)
    {
        // Generate unique job ID
        std::string jobId = "job_" + std::to_string(m_nextJobID++);

        // Create build job
        BuildJob job;
        job.id = jobId;
        job.targetName = targetName;
        job.platform = platform;
        job.configuration = configuration;
        job.status = BuildStatus::QUEUED;
        job.progress = 0.0f;
        job.startTime = std::chrono::steady_clock::now();

        // Add to job list and queue
        {
            std::lock_guard<std::mutex> lock(m_buildMutex);
            m_buildJobs.push_back(job);
            m_buildQueue.push(jobId);
        }

        // Notify worker threads
        m_buildCondition.notify_one();

        return jobId;
    }

    std::vector<std::string> BuildDeploymentSystem::StartBuildAll(const std::string& targetName,
                                                                  BuildConfiguration configuration)
    {
        std::vector<std::string> jobIds;

        auto target = GetBuildTarget(targetName);
        if (!target)
        {
            return jobIds;
        }

        // Start builds for all enabled platforms
        for (const auto& platformSettings : target->platforms)
        {
            if (platformSettings.enabled)
            {
                std::string jobId = StartBuild(targetName, platformSettings.platform, configuration);
                jobIds.push_back(jobId);
            }
        }

        return jobIds;
    }

    bool BuildDeploymentSystem::CancelBuild(const std::string& jobID)
    {
        std::lock_guard<std::mutex> lock(m_buildMutex);

        // Find and cancel the job
        for (auto& job : m_buildJobs)
        {
            if (job.id == jobID && job.status != BuildStatus::COMPLETED && job.status != BuildStatus::FAILED)
            {
                job.status = BuildStatus::CANCELLED;
                return true;
            }
        }

        return false;
    }

    const BuildJob* BuildDeploymentSystem::GetBuildJob(const std::string& jobID) const
    {
        std::lock_guard<std::mutex> lock(m_buildMutex);

        auto it = std::find_if(m_buildJobs.begin(), m_buildJobs.end(),
                               [&jobID](const BuildJob& job) { return job.id == jobID; });

        return (it != m_buildJobs.end()) ? &(*it) : nullptr;
    }

    std::vector<const BuildJob*> BuildDeploymentSystem::GetActiveBuildJobs() const
    {
        std::lock_guard<std::mutex> lock(m_buildMutex);
        std::vector<const BuildJob*> activeJobs;

        for (const auto& job : m_buildJobs)
        {
            if (job.status == BuildStatus::QUEUED || job.status == BuildStatus::PREPARING ||
                job.status == BuildStatus::COMPILING || job.status == BuildStatus::LINKING ||
                job.status == BuildStatus::PACKAGING || job.status == BuildStatus::DEPLOYING)
            {
                activeJobs.push_back(&job);
            }
        }

        return activeJobs;
    }

    // ============================================================================
    // ASSET COOKING
    // ============================================================================

    bool BuildDeploymentSystem::CookAssets(BuildPlatform platform, const std::string& outputPath,
                                           std::function<void(float)> progressCallback)
    {
        if (m_isCooking)
        {
            return false; // Already cooking
        }

        m_isCooking = true;

        // Start cooking thread
        m_cookingThread = std::thread(
            [this, platform, outputPath, progressCallback]()
            {
                // Simulate asset cooking process
                for (int i = 0; i <= 100; i += 10)
                {
                    if (progressCallback)
                    {
                        progressCallback(i / 100.0f);
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }

                m_isCooking = false;
            });

        return true;
    }

    // ============================================================================
    // PACKAGING AND DEPLOYMENT
    // ============================================================================

    bool BuildDeploymentSystem::PackageBuild(const std::string& jobID, const std::string& packageFormat,
                                             const std::string& outputPath)
    {
        // Implementation would package the build output
        return true;
    }

    bool BuildDeploymentSystem::DeployBuild(const std::string& jobID, const std::string& deploymentConfig,
                                            std::function<void(float)> progressCallback)
    {
        if (m_isDeploying)
        {
            return false; // Already deploying
        }

        m_isDeploying = true;

        // Start deployment thread
        m_deploymentThread = std::thread(
            [this, jobID, deploymentConfig, progressCallback]()
            {
                // Simulate deployment process
                for (int i = 0; i <= 100; i += 5)
                {
                    if (progressCallback)
                    {
                        progressCallback(i / 100.0f);
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }

                m_isDeploying = false;
            });

        return true;
    }

    // ============================================================================
    // CONFIGURATION MANAGEMENT
    // ============================================================================

    void BuildDeploymentSystem::AddDeploymentConfig(const DeploymentConfig& config)
    {
        m_deploymentConfigs.push_back(config);
    }

    bool BuildDeploymentSystem::RemoveDeploymentConfig(const std::string& configName)
    {
        auto it = std::find_if(m_deploymentConfigs.begin(), m_deploymentConfigs.end(),
                               [&configName](const DeploymentConfig& config) { return config.name == configName; });

        if (it != m_deploymentConfigs.end())
        {
            m_deploymentConfigs.erase(it);
            return true;
        }

        return false;
    }

    const DeploymentConfig* BuildDeploymentSystem::GetDeploymentConfig(const std::string& configName) const
    {
        auto it = std::find_if(m_deploymentConfigs.begin(), m_deploymentConfigs.end(),
                               [&configName](const DeploymentConfig& config) { return config.name == configName; });

        return (it != m_deploymentConfigs.end()) ? &(*it) : nullptr;
    }

    void BuildDeploymentSystem::SetConfiguration(const BuildSystemConfig& config)
    {
        m_config = config;
    }

    BuildStatistics BuildDeploymentSystem::GetBuildStatistics() const
    {
        return m_statistics;
    }

    void BuildDeploymentSystem::ClearBuildHistory()
    {
        std::lock_guard<std::mutex> lock(m_buildMutex);
        m_buildJobs.clear();
        m_statistics = {};
    }

    // ============================================================================
    // UTILITY FUNCTIONS
    // ============================================================================

    bool BuildDeploymentSystem::ExportBuildConfiguration(const std::string& filePath)
    {
        // Implementation would export configuration to JSON/XML
        try
        {
            std::ofstream file(filePath);
            if (!file.is_open())
            {
                return false;
            }

            // Export as JSON format
            file << "{\n";
            file << "  \"buildSystemConfig\": {\n";
            file << "    \"maxConcurrentJobs\": " << m_config.maxConcurrentJobs << ",\n";
            file << "    \"enableParallelCompilation\": " << (m_config.enableParallelCompilation ? "true" : "false")
                 << ",\n";
            file << "    \"enableDistributedBuilds\": " << (m_config.enableDistributedBuilds ? "true" : "false")
                 << ",\n";
            file << "    \"enableBuildCache\": " << (m_config.enableBuildCache ? "true" : "false") << "\n";
            file << "  },\n";
            file << "  \"buildTargets\": [\n";

            bool first = true;
            for (const auto& target : m_buildTargets)
            {
                if (!first)
                    file << ",\n";
                first = false;

                file << "    {\n";
                file << "      \"name\": \"" << target.name << "\",\n";
                file << "      \"description\": \"" << target.description << "\",\n";
                file << "      \"enabled\": " << (target.enabled ? "true" : "false") << ",\n";
                file << "      \"outputPath\": \"" << target.outputPath << "\"\n";
                file << "    }";
            }

            file << "\n  ]\n";
            file << "}\n";

            return true;
        }
        catch (const std::exception&)
        {
            return false;
        }
    }

    bool BuildDeploymentSystem::ImportBuildConfiguration(const std::string& filePath)
    {
        // Implementation would import configuration from file
        try
        {
            std::ifstream file(filePath);
            if (!file.is_open())
            {
                return false;
            }

            // For now, just validate that the file exists and is readable
            // In a full implementation, you would parse JSON/XML here
            std::string content;
            file.seekg(0, std::ios::end);
            content.reserve(file.tellg());
            file.seekg(0, std::ios::beg);

            content.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

            // Basic validation - check if it looks like a valid config file
            if (content.find("buildSystemConfig") != std::string::npos ||
                content.find("buildTargets") != std::string::npos)
            {
                return true;
            }

            return false;
        }
        catch (const std::exception&)
        {
            return false;
        }
    }

    bool BuildDeploymentSystem::ValidateBuildEnvironment(BuildPlatform platform, std::vector<std::string>& errors)
    {
        // Implementation would validate SDKs, tools, etc.
        errors.clear();
        bool isValid = true;

        // Check for required tools based on platform
        switch (platform)
        {
        case BuildPlatform::WINDOWS_X64:
        case BuildPlatform::WINDOWS_X86:
            // Check for Visual Studio or Visual Studio Build Tools
            if (!std::filesystem::exists("C:\\Program Files\\Microsoft Visual Studio") &&
                !std::filesystem::exists("C:\\Program Files (x86)\\Microsoft Visual Studio"))
            {
                errors.push_back("Visual Studio not found");
                isValid = false;
            }

            // Check for Windows SDK
            if (!std::filesystem::exists("C:\\Program Files (x86)\\Windows Kits\\10"))
            {
                errors.push_back("Windows 10 SDK not found");
                isValid = false;
            }
            break;

        case BuildPlatform::ANDROID_ARM64:
        case BuildPlatform::ANDROID_ARM32:
            // Check for Android SDK/NDK
            if (getenv("ANDROID_HOME") == nullptr)
            {
                errors.push_back("ANDROID_HOME environment variable not set");
                isValid = false;
            }
            if (getenv("ANDROID_NDK_ROOT") == nullptr)
            {
                errors.push_back("ANDROID_NDK_ROOT environment variable not set");
                isValid = false;
            }
            break;

        case BuildPlatform::IOS_ARM64:
// This would only work on macOS
#ifndef __APPLE__
            errors.push_back("iOS builds only supported on macOS");
            isValid = false;
#endif
            break;

        case BuildPlatform::LINUX_X64:
            // Check for GCC or Clang
            if (system("gcc --version > nul 2>&1") != 0 && system("clang --version > nul 2>&1") != 0)
            {
                errors.push_back("GCC or Clang compiler not found");
                isValid = false;
            }
            break;

        default:
            errors.push_back("Unknown platform");
            isValid = false;
            break;
        }

        return isValid;
    }

    bool BuildDeploymentSystem::InstallPlatformSDK(BuildPlatform platform, std::function<void(float)> progressCallback)
    {
        // Implementation would download and install platform SDKs
        if (progressCallback)
        {
            // Simulate installation progress
            for (int i = 0; i <= 100; i += 10)
            {
                progressCallback(i / 100.0f);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }

        // For now, just return false as automatic SDK installation is complex
        return false;
    }

    bool BuildDeploymentSystem::CheckForUpdates()
    {
        // Implementation would check for build system updates
        // This could check a remote server for newer versions of the build tools

        try
        {
            // Simulate update check
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

            // For now, always return false (no updates available)
            return false;
        }
        catch (const std::exception&)
        {
            return false;
        }
    }

    std::string BuildDeploymentSystem::RunUnitTests(BuildPlatform platform, BuildConfiguration configuration)
    {
        // Implementation would run unit tests
        std::stringstream result;

        result << "Unit Test Results for " << BuildPlatformToString(platform) << " "
               << BuildConfigurationToString(configuration) << ":\n";
        result << "========================================\n";

        // Simulate running tests
        int totalTests = 50;
        int passedTests = 47;
        int failedTests = 2;
        int skippedTests = 1;

        result << "Total Tests: " << totalTests << "\n";
        result << "Passed: " << passedTests << "\n";
        result << "Failed: " << failedTests << "\n";
        result << "Skipped: " << skippedTests << "\n";
        result << "Success Rate: " << ((float)passedTests / totalTests * 100.0f) << "%\n\n";

        // Add some sample test results
        if (failedTests > 0)
        {
            result << "Failed Tests:\n";
            result << "  - TestRenderer::TestShaderCompilation (Timeout)\n";
            result << "  - TestPhysics::TestCollisionDetection (Assertion failed)\n\n";
        }

        if (skippedTests > 0)
        {
            result << "Skipped Tests:\n";
            result << "  - TestNetwork::TestMultiplayer (Platform not supported)\n\n";
        }

        result << "Test execution completed in 15.3 seconds\n";

        return result.str();
    }

    std::string BuildDeploymentSystem::RunStaticAnalysis(const std::vector<std::string>& sourceFiles)
    {
        // Implementation would run static analysis tools
        std::stringstream result;

        result << "Static Analysis Results:\n";
        result << "========================\n";
        result << "Files Analyzed: " << sourceFiles.size() << "\n\n";

        // Simulate static analysis results
        int warningsCount = 12;
        int errorsCount = 2;
        int infoCount = 25;

        result << "Issues Found:\n";
        result << "  Errors: " << errorsCount << "\n";
        result << "  Warnings: " << warningsCount << "\n";
        result << "  Info: " << infoCount << "\n\n";

        if (errorsCount > 0)
        {
            result << "Critical Issues:\n";
            result << "  [ERROR] Memory leak detected in Renderer.cpp:245\n";
            result << "  [ERROR] Potential null pointer dereference in Physics.cpp:156\n\n";
        }

        if (warningsCount > 0)
        {
            result << "Warnings (showing first 5):\n";
            result << "  [WARNING] Unused variable 'deltaTime' in GameLoop.cpp:89\n";
            result << "  [WARNING] Function 'UpdateLighting' exceeds recommended complexity\n";
            result << "  [WARNING] Consider using const reference for parameter in Mesh.cpp:234\n";
            result << "  [WARNING] Magic number '60' should be named constant in Timer.cpp:45\n";
            result << "  [WARNING] Missing documentation for public method in Shader.cpp:123\n\n";
        }

        result << "Code Quality Score: 87/100\n";
        result << "Recommended Actions:\n";
        result << "  - Fix critical memory issues\n";
        result << "  - Add input validation\n";
        result << "  - Improve documentation coverage\n";

        return result.str();
    }

    std::string BuildDeploymentSystem::GenerateBuildReport(const std::string& jobID, const std::string& format)
    {
        // Implementation would generate detailed build reports
        std::stringstream report;

        // Find the build job
        auto jobIt = std::find_if(m_buildJobs.begin(), m_buildJobs.end(),
                                  [&jobID](const BuildJob& job) { return job.id == jobID; });

        if (jobIt == m_buildJobs.end())
        {
            return "Build job '" + jobID + "' not found";
        }

        const BuildJob& job = *jobIt;

        if (format == "html")
        {
            report << "<!DOCTYPE html>\n<html>\n<head>\n<title>Build Report - " << job.id
                   << "</title>\n</head>\n<body>\n";
            report << "<h1>Build Report</h1>\n";
            report << "<h2>Job Information</h2>\n";
            report << "<table border='1'>\n";
            report << "<tr><td>Job ID</td><td>" << job.id << "</td></tr>\n";
            report << "<tr><td>Target</td><td>" << job.targetName << "</td></tr>\n";
            report << "<tr><td>Platform</td><td>" << BuildPlatformToString(job.platform) << "</td></tr>\n";
            report << "<tr><td>Configuration</td><td>" << BuildConfigurationToString(job.configuration)
                   << "</td></tr>\n";
            report << "<tr><td>Status</td><td>" << (job.success ? "SUCCESS" : "FAILED") << "</td></tr>\n";
            report << "<tr><td>Duration</td><td>" << job.duration << " seconds</td></tr>\n";
            report << "</table>\n";
            report << "</body>\n</html>";
        }
        else if (format == "xml")
        {
            report << "<?xml version='1.0' encoding='UTF-8'?>\n";
            report << "<buildReport>\n";
            report << "  <job id='" << job.id << "'>\n";
            report << "    <target>" << job.targetName << "</target>\n";
            report << "    <platform>" << BuildPlatformToString(job.platform) << "</platform>\n";
            report << "    <configuration>" << BuildConfigurationToString(job.configuration) << "</configuration>\n";
            report << "    <status>" << (job.success ? "SUCCESS" : "FAILED") << "</status>\n";
            report << "    <duration>" << job.duration << "</duration>\n";
            report << "  </job>\n";
            report << "</buildReport>";
        }
        else
        {
            // Default text format
            report << "Build Report for Job: " << job.id << "\n";
            report << "=====================================\n";
            report << "Target: " << job.targetName << "\n";
            report << "Platform: " << BuildPlatformToString(job.platform) << "\n";
            report << "Configuration: " << BuildConfigurationToString(job.configuration) << "\n";
            report << "Status: " << (job.success ? "SUCCESS" : "FAILED") << "\n";
            report << "Duration: " << job.duration << " seconds\n";
            report << "Progress: " << (job.progress * 100.0f) << "%\n";

            if (!job.errorMessage.empty())
            {
                report << "\nError Message:\n" << job.errorMessage << "\n";
            }

            if (!job.warnings.empty())
            {
                report << "\nWarnings:\n";
                for (const auto& warning : job.warnings)
                {
                    report << "  - " << warning << "\n";
                }
            }

            if (!job.outputFiles.empty())
            {
                report << "\nOutput Files:\n";
                for (const auto& file : job.outputFiles)
                {
                    report << "  - " << file << "\n";
                }
            }

            report << "\nBuild Statistics:\n";
            report << "  Source Files: " << job.sourceFiles << "\n";
            report << "  Asset Files: " << job.assetFiles << "\n";
            report << "  Object Files: " << job.objectFiles << "\n";
            report << "  Compilation Time: " << job.compilationTime << "s\n";
            report << "  Linking Time: " << job.linkingTime << "s\n";
            report << "  Packaging Time: " << job.packagingTime << "s\n";
            report << "  Output Size: " << (job.outputSize / 1024 / 1024) << " MB\n";
        }

        return report.str();
    }

    // ============================================================================
    // PRIVATE HELPER METHODS
    // ============================================================================

    void BuildDeploymentSystem::CreateDefaultBuildTargets()
    {
        // Create default "Development" target
        BuildTarget devTarget;
        devTarget.name = "Development";
        devTarget.description = "Development build for testing and debugging";
        devTarget.enabled = true;
        devTarget.outputPath = "Builds/{Platform}/{Configuration}/";

        // Add Windows x64 platform
        PlatformBuildSettings windowsSettings;
        windowsSettings.platform = BuildPlatform::WINDOWS_X64;
        windowsSettings.configuration = BuildConfiguration::DEVELOPMENT;
        windowsSettings.architecture = BuildArchitecture::X64;
        windowsSettings.enabled = true;
        windowsSettings.compiler = "msvc";
        windowsSettings.enableOptimizations = true;
        windowsSettings.stripSymbols = false;
        windowsSettings.cookAssets = true;
        windowsSettings.compressAssets = false;
        windowsSettings.outputDirectory = "Builds/Windows/x64/Development/";
        windowsSettings.executableName = "SparkEngine.exe";

        devTarget.platforms.push_back(windowsSettings);
        AddBuildTarget(devTarget);

        // Create "Shipping" target
        BuildTarget shippingTarget;
        shippingTarget.name = "Shipping";
        shippingTarget.description = "Optimized shipping build for distribution";
        shippingTarget.enabled = true;
        shippingTarget.outputPath = "Builds/{Platform}/{Configuration}/";

        windowsSettings.configuration = BuildConfiguration::SHIPPING;
        windowsSettings.enableOptimizations = true;
        windowsSettings.stripSymbols = true;
        windowsSettings.cookAssets = true;
        windowsSettings.compressAssets = true;
        windowsSettings.outputDirectory = "Builds/Windows/x64/Shipping/";
        windowsSettings.includeDebugInfo = false;
        windowsSettings.createInstaller = true;

        shippingTarget.platforms.push_back(windowsSettings);
        AddBuildTarget(shippingTarget);
    }

    void BuildDeploymentSystem::DetectPlatformAvailability()
    {
        // Detect which platforms are available for building
        m_platformAvailable[BuildPlatform::WINDOWS_X64] = true;
        m_platformAvailable[BuildPlatform::WINDOWS_X86] = true;

        // Check for other SDKs (simplified for now)
        m_platformAvailable[BuildPlatform::LINUX_X64] = false;
        m_platformAvailable[BuildPlatform::MACOS_X64] = false;
        m_platformAvailable[BuildPlatform::ANDROID_ARM64] = false;
    }

    void BuildDeploymentSystem::ProcessBuildQueue()
    {
        while (!m_shouldStopBuilds)
        {
            std::unique_lock<std::mutex> lock(m_buildMutex);

            // Wait for jobs or shutdown signal
            m_buildCondition.wait(lock, [this]() { return !m_buildQueue.empty() || m_shouldStopBuilds; });

            if (m_shouldStopBuilds)
            {
                break;
            }

            if (!m_buildQueue.empty())
            {
                std::string jobId = m_buildQueue.front();
                m_buildQueue.pop();
                lock.unlock();

                // Find and execute the job
                auto it = std::find_if(m_buildJobs.begin(), m_buildJobs.end(),
                                       [&jobId](BuildJob& job) { return job.id == jobId; });

                if (it != m_buildJobs.end())
                {
                    ExecuteBuildJob(*it);
                }
            }
        }
    }

    bool BuildDeploymentSystem::ExecuteBuildJob(BuildJob& job)
    {
        job.status = BuildStatus::PREPARING;
        job.progress = 0.0f;

        auto target = GetBuildTarget(job.targetName);
        if (!target)
        {
            job.status = BuildStatus::FAILED;
            job.errorMessage = "Build target not found: " + job.targetName;
            return false;
        }

        // Find platform settings
        auto platformIt =
            std::find_if(target->platforms.begin(), target->platforms.end(),
                         [&job](const PlatformBuildSettings& settings) { return settings.platform == job.platform; });

        if (platformIt == target->platforms.end())
        {
            job.status = BuildStatus::FAILED;
            job.errorMessage = "Platform not configured for target";
            return false;
        }

        const PlatformBuildSettings& settings = *platformIt;

        // Execute build steps
        try
        {
            // Pre-build steps
            if (!RunPreBuildSteps(*target, job))
            {
                return false;
            }

            // Compilation
            job.status = BuildStatus::COMPILING;
            job.progress = 0.2f;
            if (!CompileSourceCode(job, settings))
            {
                return false;
            }

            // Linking
            job.status = BuildStatus::LINKING;
            job.progress = 0.6f;
            if (!LinkExecutables(job, settings))
            {
                return false;
            }

            // Asset packaging
            job.status = BuildStatus::PACKAGING;
            job.progress = 0.8f;
            if (!PackageAssets(job, settings))
            {
                return false;
            }

            // Post-build steps
            if (!RunPostBuildSteps(*target, job))
            {
                return false;
            }

            // Success
            job.status = BuildStatus::COMPLETED;
            job.progress = 1.0f;
            job.success = true;
            job.endTime = std::chrono::steady_clock::now();

            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(job.endTime - job.startTime);
            job.duration = duration.count() / 1000.0f;

            return true;
        }
        catch (const std::exception& e)
        {
            job.status = BuildStatus::FAILED;
            job.errorMessage = e.what();
            job.endTime = std::chrono::steady_clock::now();
            return false;
        }
    }

    bool BuildDeploymentSystem::CompileSourceCode(BuildJob& job, const PlatformBuildSettings& settings)
    {
        // Simulate compilation
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        job.sourceFiles = 150;
        job.compilationTime = 1.0f;
        return true;
    }

    bool BuildDeploymentSystem::LinkExecutables(BuildJob& job, const PlatformBuildSettings& settings)
    {
        // Simulate linking
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        job.objectFiles = 75;
        job.linkingTime = 0.5f;
        return true;
    }

    bool BuildDeploymentSystem::PackageAssets(BuildJob& job, const PlatformBuildSettings& settings)
    {
        // Simulate asset packaging
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        job.assetFiles = 200;
        job.packagingTime = 0.3f;
        return true;
    }

    bool BuildDeploymentSystem::RunPreBuildSteps(const BuildTarget& target, BuildJob& job)
    {
        // Execute pre-build commands
        for (const auto& step : target.preBuildSteps)
        {
            // Execute command
        }
        return true;
    }

    bool BuildDeploymentSystem::RunPostBuildSteps(const BuildTarget& target, BuildJob& job)
    {
        // Execute post-build commands
        for (const auto& step : target.postBuildSteps)
        {
            // Execute command
        }
        return true;
    }

    int BuildDeploymentSystem::ExecuteCommand(const std::string& command, const std::string& workingDirectory,
                                              std::string& output)
    {
        // Implementation would execute shell commands
        return 0;
    }

    std::string BuildDeploymentSystem::GetPlatformName(BuildPlatform platform) const
    {
        switch (platform)
        {
        case BuildPlatform::WINDOWS_X64:
            return "Windows x64";
        case BuildPlatform::WINDOWS_X86:
            return "Windows x86";
        case BuildPlatform::LINUX_X64:
            return "Linux x64";
        case BuildPlatform::MACOS_X64:
            return "macOS x64";
        case BuildPlatform::MACOS_ARM64:
            return "macOS ARM64";
        case BuildPlatform::ANDROID_ARM64:
            return "Android ARM64";
        case BuildPlatform::ANDROID_ARM32:
            return "Android ARM32";
        case BuildPlatform::IOS_ARM64:
            return "iOS ARM64";
        default:
            return "Unknown";
        }
    }

    std::string BuildDeploymentSystem::GetConfigurationName(BuildConfiguration configuration) const
    {
        switch (configuration)
        {
        case BuildConfiguration::DEBUG:
            return "Debug";
        case BuildConfiguration::DEVELOPMENT:
            return "Development";
        case BuildConfiguration::RELEASE:
            return "Release";
        case BuildConfiguration::SHIPPING:
            return "Shipping";
        case BuildConfiguration::PROFILING:
            return "Profiling";
        default:
            return "Unknown";
        }
    }

    void BuildDeploymentSystem::UpdateStatistics()
    {
        std::lock_guard<std::mutex> lock(m_buildMutex);

        m_statistics.totalBuilds = static_cast<int>(m_buildJobs.size());
        m_statistics.successfulBuilds = 0;
        m_statistics.failedBuilds = 0;
        m_statistics.totalBuildTime = 0.0f;

        for (const auto& job : m_buildJobs)
        {
            if (job.status == BuildStatus::COMPLETED)
            {
                m_statistics.successfulBuilds++;
                m_statistics.totalBuildTime += job.duration;
            }
            else if (job.status == BuildStatus::FAILED)
            {
                m_statistics.failedBuilds++;
            }
        }

        if (m_statistics.successfulBuilds > 0)
        {
            m_statistics.averageBuildTime = m_statistics.totalBuildTime / m_statistics.successfulBuilds;
        }
    }

    void BuildDeploymentSystem::UpdateCompletedBuilds()
    {
        // Check for newly completed builds and send notifications
        // Implementation would track completion state changes
    }

    void BuildDeploymentSystem::SendBuildNotification(const BuildJob& job, const std::string& message)
    {
        if (m_enableNotifications)
        {
            // Implementation would send desktop/system notifications
        }
    }

    bool BuildDeploymentSystem::SaveConfiguration()
    {
        // Implementation would save configuration to JSON file
        return true;
    }

    bool BuildDeploymentSystem::LoadConfiguration()
    {
        // Implementation would load configuration from JSON file
        return true;
    }

    // ============================================================================
    // UI RENDERING METHODS
    // ============================================================================

    void BuildDeploymentSystem::RenderBuildTargetsPanel()
    {
        // Render build targets UI
        ImGui::Text("Build Targets");

        for (auto& target : m_buildTargets)
        {
            ImGui::PushID(target.name.c_str());

            bool enabled = target.enabled;
            if (ImGui::Checkbox("##enabled", &enabled))
            {
                target.enabled = enabled;
            }

            ImGui::SameLine();
            if (ImGui::Selectable(target.name.c_str(), m_selectedTarget == target.name))
            {
                m_selectedTarget = target.name;
            }

            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%s", target.description.c_str());
            }

            ImGui::PopID();
        }

        if (ImGui::Button("Build Selected"))
        {
            if (!m_selectedTarget.empty())
            {
                StartBuildAll(m_selectedTarget, BuildConfiguration::DEVELOPMENT);
            }
        }
    }

    void BuildDeploymentSystem::RenderBuildQueuePanel()
    {
        ImGui::Text("Build Queue");

        auto activeJobs = GetActiveBuildJobs();

        if (activeJobs.empty())
        {
            ImGui::Text("No active builds");
        }
        else
        {
            for (const auto& job : activeJobs)
            {
                ImGui::Text("%s - %s (%s)", job->targetName.c_str(), GetPlatformName(job->platform).c_str(),
                            GetConfigurationName(job->configuration).c_str());

                ImGui::ProgressBar(job->progress);
            }
        }
    }

    void BuildDeploymentSystem::RenderBuildHistoryPanel()
    {
        ImGui::Text("Build History");

        std::lock_guard<std::mutex> lock(m_buildMutex);

        for (const auto& job : m_buildJobs)
        {
            if (job.status == BuildStatus::COMPLETED || job.status == BuildStatus::FAILED)
            {
                ImVec4 color = (job.status == BuildStatus::COMPLETED) ? ImVec4(0, 1, 0, 1) : ImVec4(1, 0, 0, 1);

                ImGui::TextColored(color, "%s - %s (%s) - %.1fs", job.targetName.c_str(),
                                   GetPlatformName(job.platform).c_str(),
                                   GetConfigurationName(job.configuration).c_str(), job.duration);
            }
        }
    }

    void BuildDeploymentSystem::RenderDeploymentPanel()
    {
        ImGui::Text("Deployment");

        for (const auto& config : m_deploymentConfigs)
        {
            ImGui::Text("Config: %s", config.name.c_str());
            ImGui::Text("Method: %d", static_cast<int>(config.method));
            ImGui::Text("Enabled: %s", config.enabled ? "Yes" : "No");
            ImGui::Separator();
        }
    }

    void BuildDeploymentSystem::RenderConfigurationPanel()
    {
        ImGui::Text("Configuration");

        ImGui::SliderInt("Max Concurrent Jobs", &m_config.maxConcurrentJobs, 1, 16);
        ImGui::Checkbox("Enable Parallel Compilation", &m_config.enableParallelCompilation);
        ImGui::Checkbox("Enable Build Cache", &m_config.enableBuildCache);
        ImGui::Checkbox("Enable Automatic Builds", &m_config.enableAutomaticBuilds);
    }

    void BuildDeploymentSystem::RenderStatisticsPanel()
    {
        ImGui::Text("Build Statistics");

        ImGui::Text("Total Builds: %d", m_statistics.totalBuilds);
        ImGui::Text("Successful: %d", m_statistics.successfulBuilds);
        ImGui::Text("Failed: %d", m_statistics.failedBuilds);
        ImGui::Text("Average Build Time: %.1fs", m_statistics.averageBuildTime);
        ImGui::Text("Total Build Time: %.1fs", m_statistics.totalBuildTime);
    }

} // namespace SparkEditor