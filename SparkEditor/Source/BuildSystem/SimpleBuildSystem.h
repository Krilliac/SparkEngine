/**
 * @file SimpleBuildSystem.h
 * @brief Simplified build system for Spark Engine Editor
 * @author Spark Engine Team
 * @date 2025
 */

#pragma once

#include "../Core/EditorPanel.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>

namespace SparkEditor
{

    enum class BuildPlatform
    {
        WINDOWS_X64 = 0,
        WINDOWS_X86 = 1,
        LINUX_X64 = 2,
        MACOS_X64 = 3
    };

    enum class BuildConfiguration
    {
        DEBUG = 0,
        RELEASE = 1,
        SHIPPING = 2
    };

    struct BuildTarget
    {
        std::string name;
        std::string description;
        std::string outputPath;
        bool enabled = true;
        std::vector<BuildPlatform> platforms;
    };

    class SimpleBuildSystem : public EditorPanel
    {
      public:
        SimpleBuildSystem();
        ~SimpleBuildSystem() override;

        // EditorPanel overrides
        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;
        bool HandleEvent(const std::string& eventType, void* eventData) override;

        /**
     * @brief Start a build for a target
     * @param targetName Name of the build target
     * @param platform Target platform
     * @param config Build configuration
     * @return Build job ID
     */
        std::string StartBuild(const std::string& targetName, BuildPlatform platform, BuildConfiguration config);

        /**
     * @brief Get build status
     * @param jobId Build job ID
     * @return Status string
     */
        std::string GetBuildStatus(const std::string& jobId);

        /**
     * @brief Add a build target
     * @param target Build target to add
     */
        void AddBuildTarget(const BuildTarget& target);

      private:
        void RenderBuildTargets();
        void RenderBuildHistory();
        void RenderBuildConfiguration();

        std::string GetPlatformName(BuildPlatform platform);
        std::string GetConfigurationName(BuildConfiguration config);

      private:
        std::vector<BuildTarget> m_buildTargets;
        std::unordered_map<std::string, std::string> m_buildJobs;
        std::string m_selectedTarget;
        BuildPlatform m_selectedPlatform = BuildPlatform::WINDOWS_X64;
        BuildConfiguration m_selectedConfig = BuildConfiguration::DEBUG;
        int m_jobCounter = 0;
    };

} // namespace SparkEditor