/**
 * @file SimpleBuildSystem.cpp
 * @brief Implementation of simplified build system
 * @author Spark Engine Team
 * @date 2025
 */

#include "SimpleBuildSystem.h"
#include <imgui.h>
#include <iostream>

namespace SparkEditor
{

    SimpleBuildSystem::SimpleBuildSystem() : EditorPanel("Build System", "build_system_panel") {}

    SimpleBuildSystem::~SimpleBuildSystem() {}

    bool SimpleBuildSystem::Initialize()
    {
        std::cout << "Initializing Simple Build System\n";

        // Add default build targets
        BuildTarget gameTarget;
        gameTarget.name = "SparkGame";
        gameTarget.description = "Main game executable";
        gameTarget.outputPath = "build/bin/";
        gameTarget.enabled = true;
        gameTarget.platforms = {BuildPlatform::WINDOWS_X64, BuildPlatform::WINDOWS_X86};
        AddBuildTarget(gameTarget);

        BuildTarget editorTarget;
        editorTarget.name = "SparkEditor";
        editorTarget.description = "Editor executable";
        editorTarget.outputPath = "build/bin/";
        editorTarget.enabled = true;
        editorTarget.platforms = {BuildPlatform::WINDOWS_X64};
        AddBuildTarget(editorTarget);

        return true;
    }

    void SimpleBuildSystem::Update(float deltaTime)
    {
        // Update build system logic
    }

    void SimpleBuildSystem::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            // Tab bar for different views
            if (ImGui::BeginTabBar("BuildTabs"))
            {
                if (ImGui::BeginTabItem("Targets"))
                {
                    RenderBuildTargets();
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("History"))
                {
                    RenderBuildHistory();
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Configuration"))
                {
                    RenderBuildConfiguration();
                    ImGui::EndTabItem();
                }

                ImGui::EndTabBar();
            }
        }
        EndPanel();
    }

    void SimpleBuildSystem::Shutdown()
    {
        std::cout << "Shutting down Simple Build System\n";
    }

    bool SimpleBuildSystem::HandleEvent(const std::string& eventType, void* eventData)
    {
        return false;
    }

    std::string SimpleBuildSystem::StartBuild(const std::string& targetName, BuildPlatform platform,
                                              BuildConfiguration config)
    {
        std::string jobId = "build_" + std::to_string(++m_jobCounter);

        std::string status =
            "Building " + targetName + " for " + GetPlatformName(platform) + " (" + GetConfigurationName(config) + ")";

        m_buildJobs[jobId] = status;

        std::cout << "Started build job " << jobId << ": " << status << "\n";

        return jobId;
    }

    std::string SimpleBuildSystem::GetBuildStatus(const std::string& jobId)
    {
        auto it = m_buildJobs.find(jobId);
        return (it != m_buildJobs.end()) ? it->second : "Unknown job";
    }

    void SimpleBuildSystem::AddBuildTarget(const BuildTarget& target)
    {
        m_buildTargets.push_back(target);
    }

    void SimpleBuildSystem::RenderBuildTargets()
    {
        ImGui::Text("Build Targets");
        ImGui::Separator();

        for (const auto& target : m_buildTargets)
        {
            ImGui::PushID(target.name.c_str());

            if (ImGui::Selectable(target.name.c_str(), m_selectedTarget == target.name))
            {
                m_selectedTarget = target.name;
            }

            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%s\nOutput: %s", target.description.c_str(), target.outputPath.c_str());
            }

            ImGui::PopID();
        }

        ImGui::Separator();

        // Platform selection
        ImGui::Text("Platform:");
        ImGui::SameLine();
        if (ImGui::BeginCombo("##Platform", GetPlatformName(m_selectedPlatform).c_str()))
        {
            for (int i = 0; i < 4; ++i)
            {
                BuildPlatform platform = static_cast<BuildPlatform>(i);
                bool isSelected = (m_selectedPlatform == platform);

                if (ImGui::Selectable(GetPlatformName(platform).c_str(), isSelected))
                {
                    m_selectedPlatform = platform;
                }

                if (isSelected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        // Configuration selection
        ImGui::Text("Configuration:");
        ImGui::SameLine();
        if (ImGui::BeginCombo("##Configuration", GetConfigurationName(m_selectedConfig).c_str()))
        {
            for (int i = 0; i < 3; ++i)
            {
                BuildConfiguration config = static_cast<BuildConfiguration>(i);
                bool isSelected = (m_selectedConfig == config);

                if (ImGui::Selectable(GetConfigurationName(config).c_str(), isSelected))
                {
                    m_selectedConfig = config;
                }

                if (isSelected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        ImGui::Separator();

        if (ImGui::Button("Build Selected") && !m_selectedTarget.empty())
        {
            StartBuild(m_selectedTarget, m_selectedPlatform, m_selectedConfig);
        }

        ImGui::SameLine();
        if (ImGui::Button("Build All"))
        {
            for (const auto& target : m_buildTargets)
            {
                if (target.enabled)
                {
                    StartBuild(target.name, m_selectedPlatform, m_selectedConfig);
                }
            }
        }
    }

    void SimpleBuildSystem::RenderBuildHistory()
    {
        ImGui::Text("Build History");
        ImGui::Separator();

        if (m_buildJobs.empty())
        {
            ImGui::Text("No builds yet");
        }
        else
        {
            for (const auto& [jobId, status] : m_buildJobs)
            {
                ImGui::Text("%s: %s", jobId.c_str(), status.c_str());
            }
        }

        ImGui::Separator();
        if (ImGui::Button("Clear History"))
        {
            m_buildJobs.clear();
        }
    }

    void SimpleBuildSystem::RenderBuildConfiguration()
    {
        ImGui::Text("Build Configuration");
        ImGui::Separator();

        ImGui::Text("Output Directory: build/bin/");
        ImGui::Text("Compiler: MSVC 2022");
        ImGui::Text("C++ Standard: C++20");

        ImGui::Separator();

        static bool enableOptimizations = true;
        static bool generateDebugInfo = true;
        static bool enableWarnings = true;

        ImGui::Checkbox("Enable Optimizations", &enableOptimizations);
        ImGui::Checkbox("Generate Debug Info", &generateDebugInfo);
        ImGui::Checkbox("Enable Warnings", &enableWarnings);
    }

    std::string SimpleBuildSystem::GetPlatformName(BuildPlatform platform)
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
        default:
            return "Unknown";
        }
    }

    std::string SimpleBuildSystem::GetConfigurationName(BuildConfiguration config)
    {
        switch (config)
        {
        case BuildConfiguration::DEBUG:
            return "Debug";
        case BuildConfiguration::RELEASE:
            return "Release";
        case BuildConfiguration::SHIPPING:
            return "Shipping";
        default:
            return "Unknown";
        }
    }

} // namespace SparkEditor