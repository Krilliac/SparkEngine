/**
 * @file BuildCookPanel.cpp
 * @brief Build, cook, and package settings panel implementation
 * @author Spark Engine Team
 * @date 2025
 */

#include "BuildCookPanel.h"
#include <iostream>

#include <imgui.h>

#include "BuildPipeline.h"
#include "../Core/ProjectManager.h"
#include "../Core/EditorIcons.h"
#include "../../../SparkEngine/Source/Utils/Validate.h"
#include "SparkUI/Runtime.h"
#include "SparkUI/Widgets.h"

namespace SparkEditor
{

    BuildCookPanel::BuildCookPanel() : EditorPanel("Build & Cook", "build_cook_panel") {}

    BuildCookPanel::~BuildCookPanel() {}

    bool BuildCookPanel::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        std::cout << "Initializing Build & Cook panel\n";
        m_pipeline = std::make_unique<BuildPipeline>();
        m_settings.platform = BuildPipeline::NativeTargetPlatform();
        ApplyProfileDefaults(BuildProfile::Development);
        return true;
    }

    void BuildCookPanel::Update(float /*deltaTime*/)
    {
        // Poll the async BuildPipeline for log lines and progress
        if (m_pipeline && (m_pipeline->IsRunning() || m_isBuildRunning))
        {
            auto lines = m_pipeline->DrainLogLines();
            for (auto& line : lines)
            {
                std::string level = "info";
                if (line.level == BuildLogLine::Level::Warning)
                    level = "warning";
                else if (line.level == BuildLogLine::Level::Error)
                    level = "error";
                m_buildLog.push_back({std::move(line.text), std::move(level)});
            }

            m_buildProgress = m_pipeline->GetProgress();
            m_buildStatus = m_pipeline->GetStatusText();

            if (!m_pipeline->IsRunning())
            {
                m_isBuildRunning = false;
                auto result = m_pipeline->GetResult();
                const bool cooked = m_pipeline->IsCookOperation();
                if (result == BuildResult::Success)
                {
                    m_buildLog.push_back(
                        {cooked ? "Cook completed successfully!" : "Build completed successfully!", "info"});
                    SPARK_LOG_INFO(Spark::LogCategory::Editor, "%s completed successfully", cooked ? "Cook" : "Build");
                }
                else if (result == BuildResult::Failed)
                {
                    m_buildLog.push_back(
                        {cooked ? "Cook failed — see log above" : "Build failed — see log above", "error"});
                    SPARK_LOG_INFO(Spark::LogCategory::Editor, "%s failed", cooked ? "Cook" : "Build");
                }
                else if (result == BuildResult::Cancelled)
                {
                    m_buildLog.push_back({"Build cancelled by user", "warning"});
                }
            }
        }
    }

    void BuildCookPanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            RenderProfileSelector();
            ImGui::Separator();
            RenderPlatformSelector();
            ImGui::Separator();

            if (Spark::Ui::SectionHeader("Build Options", ICON_FA_COG))
            {
                RenderBuildOptions();
            }

            if (Spark::Ui::SectionHeader("Stripping Options", ICON_FA_TRASH, false))
            {
                RenderStrippingOptions();
            }

            if (Spark::Ui::SectionHeader("Asset Cooking", ICON_FA_COGS, false))
            {
                RenderAssetCooking();
            }

            if (Spark::Ui::SectionHeader("Packaging", ICON_FA_DOWNLOAD, false))
            {
                RenderPackagingOptions();
            }

            ImGui::Separator();
            RenderBuildActions();

            if (!m_buildLog.empty())
            {
                ImGui::Separator();
                RenderBuildLog();
            }
        }
        EndPanel();
    }

    void BuildCookPanel::Shutdown()
    {
        std::cout << "Shutting down Build & Cook panel\n";
    }

    bool BuildCookPanel::HandleEvent(const std::string& eventType, void* eventData)
    {
        return false;
    }

    void BuildCookPanel::RenderProfileSelector()
    {
        Spark::Ui::SubHeader(ICON_FA_HAMMER " Build Profile");

        // Segment order matches the BuildProfile enumerators.
        static constexpr const char* kProfileLabels[] = {ICON_FA_BUG " Debug", ICON_FA_CODE " Dev",
                                                         ICON_FA_CHECK " Release", ICON_FA_ROCKET " Ship"};
        int profileIndex = static_cast<int>(m_settings.profile);
        if (Spark::Ui::SegmentedControl("##BuildProfile", kProfileLabels, 4, &profileIndex))
        {
            m_settings.profile = static_cast<BuildProfile>(profileIndex);
            ApplyProfileDefaults(m_settings.profile);
        }
        // SegmentedControl reports only a change of profile, so re-clicking the active profile no
        // longer resets hand-edited settings; this button does that explicitly.
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_UNDO " Reset to profile defaults"))
        {
            ApplyProfileDefaults(m_settings.profile);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Discard the edited settings below and restore the %s profile defaults.",
                              GetProfileName(m_settings.profile));
        }

        const char* description = "";
        switch (m_settings.profile)
        {
        case BuildProfile::Debug:
            description = "Full debug symbols, assertions, console, no optimizations. "
                          "Best for development and debugging.";
            break;
        case BuildProfile::Development:
            description = "Debug symbols, console, profiler, moderate optimizations. "
                          "Good balance of debuggability and performance.";
            break;
        case BuildProfile::Release:
            description = "Optional debug symbols, full optimizations. "
                          "Console and profiler optional. Good for QA testing.";
            break;
        case BuildProfile::Shipping:
            description = "No debug symbols, no console, no profiler. "
                          "Maximum optimization. For end-user distribution.";
            break;
        }
        ImGui::PushTextWrapPos(0.0f);
        Spark::Ui::SecondaryText("%s", description);
        ImGui::PopTextWrapPos();

        Spark::Ui::SecondaryText("Profile: %s | Platform: %s", GetProfileName(m_settings.profile),
                                 GetPlatformName(m_settings.platform));
    }

    void BuildCookPanel::RenderPlatformSelector()
    {
        Spark::Ui::SubHeader(ICON_FA_GLOBE " Target Platform");
        m_settings.platform = BuildPipeline::NativeTargetPlatform();
        ImGui::Text("%s (native)", GetPlatformName(m_settings.platform));
        ImGui::PushTextWrapPos(0.0f);
        Spark::Ui::SecondaryText("This editor produces complete native packages only. Cross-compilation requires an "
                                 "external toolchain and matching runtime host.");
        ImGui::PopTextWrapPos();
    }

    void BuildCookPanel::RenderBuildOptions()
    {
        ImGui::PushTextWrapPos(0.0f);
        Spark::Ui::SecondaryText("The selected profile controls module optimization and debug information. "
                                 "Engine features are fixed by the installed SparkEngine SDK/runtime.");
        ImGui::PopTextWrapPos();
        ImGui::Checkbox("Package available debug symbols", &m_settings.includeDebugSymbols);
        ImGui::Checkbox("Link-Time Optimization (LTO)", &m_settings.enableLTO);
    }

    void BuildCookPanel::RenderStrippingOptions()
    {
        Spark::Ui::UnavailableFeature("Runtime feature stripping",
                                      "Standalone modules cannot strip runtime features yet. The choices below show "
                                      "what the selected profile requests.");
        ImGui::Spacing();

        const Spark::Ui::ScopedDisabled disabled(true);
        Spark::Ui::SubHeader("Strip from build");

        ImGui::Checkbox("Debug Symbols", &m_settings.stripDebugSymbols);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Remove .pdb files and debug info.\nReduces build size significantly.");

        ImGui::Checkbox("Console (SparkConsole.exe)", &m_settings.stripConsole);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Remove SparkConsole executable.\nEnd users won't have access to the debug console.");

        ImGui::Checkbox("Profiler", &m_settings.stripProfiler);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Remove performance profiling instrumentation.\nSlightly reduces binary size.");

        ImGui::Checkbox("Developer Commands", &m_settings.stripDevCommands);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(
                "Remove dev/cheat commands (god, noclip, teleport, etc.).\nPrevents end users from using cheats.");

        ImGui::Checkbox("Log Files", &m_settings.stripLogFiles);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Disable file-based logging.\nReduces disk I/O in shipping builds.");

        ImGui::Checkbox("Editor Assets", &m_settings.stripEditorAssets);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Remove editor-only assets (fonts, icons, themes).\nNot needed in standalone game.");

        ImGui::Checkbox("Unused Assets", &m_settings.stripUnusedAssets);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Analyze asset references and remove unreferenced assets.\nReduces package size.");

        ImGui::Checkbox("Test Content", &m_settings.stripTestContent);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Remove test scenes, debug levels, and test-only content.");
    }

    void BuildCookPanel::RenderAssetCooking()
    {
        ImGui::Checkbox("Include Assets directory", &m_settings.cookAssets);
        if (!m_settings.cookAssets)
        {
            Spark::Ui::StatusText(Spark::Ui::Status::Warning, "Assets will be omitted from the package.");
            return;
        }

        Spark::Ui::UnavailableFeature("Platform asset transforms",
                                      "Content packaging currently copies source assets unchanged. Platform texture, "
                                      "audio, mesh, mip, and LOD transforms are not implemented yet.");

        const Spark::Ui::ScopedDisabled disabled(true);

        Spark::Ui::SubHeader(ICON_FA_IMAGE " Textures");
        ImGui::Checkbox("Compress Textures", &m_settings.compressTextures);
        if (m_settings.compressTextures)
        {
            const char* maxSizes[] = {"256", "512", "1024", "2048", "4096"};
            int sizeIdx = 0;
            if (m_settings.textureMaxSize == 512)
                sizeIdx = 1;
            else if (m_settings.textureMaxSize == 1024)
                sizeIdx = 2;
            else if (m_settings.textureMaxSize == 2048)
                sizeIdx = 3;
            else if (m_settings.textureMaxSize == 4096)
                sizeIdx = 4;
            if (ImGui::Combo("Max Texture Size", &sizeIdx, maxSizes, 5))
            {
                int sizes[] = {256, 512, 1024, 2048, 4096};
                m_settings.textureMaxSize = sizes[sizeIdx];
            }
        }
        ImGui::Checkbox("Generate Mipmaps", &m_settings.generateMipmaps);

        Spark::Ui::SubHeader(ICON_FA_VOLUME_UP " Audio");
        ImGui::Checkbox("Compress Audio", &m_settings.compressAudio);
        if (m_settings.compressAudio)
        {
            ImGui::SliderInt("Audio Quality", &m_settings.audioQuality, 0, 100, "%d%%");
        }

        Spark::Ui::SubHeader(ICON_FA_CUBE " Meshes");
        ImGui::Checkbox("Compress Meshes", &m_settings.compressMeshes);
        ImGui::Checkbox("Generate LODs", &m_settings.generateLODs);
        if (m_settings.generateLODs)
        {
            ImGui::SliderInt("LOD Levels", &m_settings.lodLevels, 1, 6);
        }
    }

    void BuildCookPanel::RenderPackagingOptions()
    {
        if (Spark::Ui::BeginPropertyGrid("BuildPackaging"))
        {
            Spark::Ui::PropertyLabel("Output directory");
            char outputDir[512];
            strncpy(outputDir, m_settings.outputDirectory.c_str(), sizeof(outputDir) - 1);
            outputDir[sizeof(outputDir) - 1] = '\0';
            if (ImGui::InputText("##OutputDir", outputDir, sizeof(outputDir)))
            {
                m_settings.outputDirectory = outputDir;
            }

            Spark::Ui::PropertyLabel("Executable name");
            char exeName[256];
            strncpy(exeName, m_settings.executableName.c_str(), sizeof(exeName) - 1);
            exeName[sizeof(exeName) - 1] = '\0';
            if (ImGui::InputText("##ExeName", exeName, sizeof(exeName)))
            {
                m_settings.executableName = exeName;
            }
            Spark::Ui::EndPropertyGrid();
        }

        ImGui::Separator();
        ImGui::Checkbox("Run packaged game smoke test", &m_settings.runAutomationAfterBuild);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Launch the package through SparkAutomation with a frame limit, timeout, log, JSON, "
                              "and JUnit reports.");
        if (m_settings.runAutomationAfterBuild && Spark::Ui::BeginPropertyGrid("BuildAutomation"))
        {
            Spark::Ui::PropertyLabel("Automation Frames");
            ImGui::SliderInt("##AutomationFrames", &m_settings.automationFrames, 1, 3600);
            Spark::Ui::PropertyLabel("Automation Timeout (seconds)");
            ImGui::SliderInt("##AutomationTimeout", &m_settings.automationTimeoutSeconds, 1, 300);
            Spark::Ui::EndPropertyGrid();
        }

        ImGui::Separator();
        // No archive, installer or signing backend exists, so the settings can never be requested.
        m_settings.compressPackage = false;
        m_settings.createInstaller = false;
        m_settings.signExecutable = false;
        Spark::Ui::UnavailableFeature("Archives, installers and code signing",
                                      "Packages are written as plain folders; no archive, installer or signing step "
                                      "is implemented.");
    }

    void BuildCookPanel::RenderBuildActions()
    {
        Spark::Ui::SubHeader(ICON_FA_HAMMER " Actions");

        const std::string projectRoot = ProjectManager::GetActiveProjectPath();
        const bool hasOpenProject = !projectRoot.empty();
        if (!hasOpenProject)
        {
            Spark::Ui::StatusText(Spark::Ui::Status::Warning, "Open a project before building or cooking.");
            ImGui::Spacing();
        }
        else
        {
            Spark::Ui::SecondaryText("Project:");
            ImGui::SameLine();
            Spark::Ui::MonoText("%s", projectRoot.c_str());
        }

        const float halfWidth = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2.0f;
        const ImVec2 btnSize(halfWidth, Spark::Ui::Px(36.0f));
        const bool canBuild = !m_isBuildRunning && hasOpenProject;
        const char* disabledReason = hasOpenProject ? "A build is already running." : "Open a project first.";

        {
            const Spark::Ui::ScopedDisabled disabled(!canBuild, disabledReason);
            if (Spark::Ui::PrimaryButton(ICON_FA_HAMMER " Build", btnSize))
            {
                SPARK_LOG_INFO(Spark::LogCategory::Editor, "Build started");
                m_buildLog.clear();
                m_buildLog.push_back({"Starting " + std::string(GetProfileName(m_settings.profile)) + " build for " +
                                          GetPlatformName(m_settings.platform) + "...",
                                      "info"});
                // Launch async build via BuildPipeline
                if (m_pipeline->StartBuild(m_settings, projectRoot))
                {
                    m_isBuildRunning = true;
                    m_buildProgress = 0.0f;
                    m_buildStatus = "Starting...";
                }
                else
                {
                    for (auto& line : m_pipeline->DrainLogLines())
                        m_buildLog.push_back(
                            {std::move(line.text), line.level == BuildLogLine::Level::Error ? "error" : "info"});
                    m_buildStatus = m_pipeline->GetStatusText();
                    if (m_buildLog.empty())
                        m_buildLog.push_back({"Failed to start build — another build may be running", "error"});
                }
            }
        }

        ImGui::SameLine();

        {
            const Spark::Ui::ScopedDisabled disabled(!canBuild, disabledReason);
            if (Spark::Ui::SecondaryButton(ICON_FA_FIRE " Cook Only", btnSize))
            {
                m_buildLog.clear();
                m_buildLog.push_back(
                    {"Cooking assets for " + std::string(GetPlatformName(m_settings.platform)) + "...", "info"});
                if (m_pipeline->StartCookOnly(m_settings, projectRoot))
                {
                    m_isBuildRunning = true;
                    m_buildProgress = 0.0f;
                    m_buildStatus = "Cooking assets...";
                }
            }
        }

        // Progress bar
        if (m_isBuildRunning || m_buildProgress > 0.0f)
        {
            ImGui::Spacing();
            ImGui::ProgressBar(m_buildProgress, ImVec2(-1.0f, Spark::Ui::Px(20.0f)));
            Spark::Ui::SecondaryText("%s", m_buildStatus.c_str());

            // Cancel button — only visible during an active build
            if (m_pipeline->IsRunning())
            {
                ImGui::SameLine();
                if (Spark::Ui::DangerButton(ICON_FA_TIMES " Cancel"))
                {
                    m_pipeline->Cancel();
                }
            }
        }
    }

    void BuildCookPanel::RenderBuildLog()
    {
        if (Spark::Ui::SectionHeader("Build Log", ICON_FA_TERMINAL))
        {
            ImGui::BeginChild("##BuildLog", Spark::Ui::Px2(0, 120), true);
            for (const auto& entry : m_buildLog)
            {
                if (entry.level == "warning")
                {
                    Spark::Ui::StatusText(Spark::Ui::Status::Warning, "%s", entry.message.c_str());
                }
                else if (entry.level == "error")
                {
                    Spark::Ui::StatusText(Spark::Ui::Status::Error, "%s", entry.message.c_str());
                }
                else
                {
                    Spark::Ui::MonoText("%s", entry.message.c_str());
                }
            }
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();
        }
    }

    void BuildCookPanel::ApplyProfileDefaults(BuildProfile profile)
    {
        switch (profile)
        {
        case BuildProfile::Debug:
            m_settings.includeDebugSymbols = true;
            m_settings.includeConsole = true;
            m_settings.includeProfiler = true;
            m_settings.includeDevCommands = true;
            m_settings.includeLogSystem = true;
            m_settings.enableAsserts = true;
            m_settings.enableHotReload = true;
            m_settings.enableValidationLayers = true;
            m_settings.optimizationLevel = 0;
            m_settings.enableLTO = false;
            m_settings.enablePGO = false;
            m_settings.stripDebugSymbols = false;
            m_settings.stripConsole = false;
            m_settings.stripProfiler = false;
            m_settings.stripDevCommands = false;
            m_settings.stripLogFiles = false;
            m_settings.stripEditorAssets = false;
            m_settings.stripUnusedAssets = false;
            m_settings.stripTestContent = false;
            break;

        case BuildProfile::Development:
            m_settings.includeDebugSymbols = true;
            m_settings.includeConsole = true;
            m_settings.includeProfiler = true;
            m_settings.includeDevCommands = true;
            m_settings.includeLogSystem = true;
            m_settings.enableAsserts = true;
            m_settings.enableHotReload = false;
            m_settings.enableValidationLayers = false;
            m_settings.optimizationLevel = 2;
            m_settings.enableLTO = false;
            m_settings.enablePGO = false;
            m_settings.stripDebugSymbols = false;
            m_settings.stripConsole = false;
            m_settings.stripProfiler = false;
            m_settings.stripDevCommands = false;
            m_settings.stripLogFiles = false;
            m_settings.stripEditorAssets = true;
            m_settings.stripUnusedAssets = false;
            m_settings.stripTestContent = true;
            break;

        case BuildProfile::Release:
            m_settings.includeDebugSymbols = true;
            m_settings.includeConsole = false;
            m_settings.includeProfiler = false;
            m_settings.includeDevCommands = false;
            m_settings.includeLogSystem = true;
            m_settings.enableAsserts = false;
            m_settings.enableHotReload = false;
            m_settings.enableValidationLayers = false;
            m_settings.optimizationLevel = 2;
            m_settings.enableLTO = true;
            m_settings.enablePGO = false;
            m_settings.stripDebugSymbols = false;
            m_settings.stripConsole = true;
            m_settings.stripProfiler = true;
            m_settings.stripDevCommands = true;
            m_settings.stripLogFiles = false;
            m_settings.stripEditorAssets = true;
            m_settings.stripUnusedAssets = true;
            m_settings.stripTestContent = true;
            break;

        case BuildProfile::Shipping:
            m_settings.includeDebugSymbols = false;
            m_settings.includeConsole = false;
            m_settings.includeProfiler = false;
            m_settings.includeDevCommands = false;
            m_settings.includeLogSystem = false;
            m_settings.enableAsserts = false;
            m_settings.enableHotReload = false;
            m_settings.enableValidationLayers = false;
            m_settings.optimizationLevel = 3;
            m_settings.enableLTO = true;
            m_settings.enablePGO = false;
            m_settings.stripDebugSymbols = true;
            m_settings.stripConsole = true;
            m_settings.stripProfiler = true;
            m_settings.stripDevCommands = true;
            m_settings.stripLogFiles = true;
            m_settings.stripEditorAssets = true;
            m_settings.stripUnusedAssets = true;
            m_settings.stripTestContent = true;
            break;
        }
    }

    const char* BuildCookPanel::GetProfileName(BuildProfile profile)
    {
        switch (profile)
        {
        case BuildProfile::Debug:
            return "Debug";
        case BuildProfile::Development:
            return "Development";
        case BuildProfile::Release:
            return "Release";
        case BuildProfile::Shipping:
            return "Shipping";
        default:
            return "Unknown";
        }
    }

    const char* BuildCookPanel::GetPlatformName(TargetPlatform platform)
    {
        switch (platform)
        {
        case TargetPlatform::WindowsX64:
            return "Windows x64";
        case TargetPlatform::WindowsX86:
            return "Windows x86";
        case TargetPlatform::LinuxX64:
            return "Linux x64";
        case TargetPlatform::LinuxARM64:
            return "Linux ARM64";
        case TargetPlatform::MacOSX64:
            return "macOS x64";
        case TargetPlatform::MacOSARM64:
            return "macOS ARM64";
        default:
            return "Unknown";
        }
    }

    const char* BuildCookPanel::GetOptimizationName(int level)
    {
        switch (level)
        {
        case 0:
            return "None (O0)";
        case 1:
            return "Size (Os)";
        case 2:
            return "Speed (O2)";
        case 3:
            return "Full (O3 + LTO)";
        default:
            return "Unknown";
        }
    }

} // namespace SparkEditor
