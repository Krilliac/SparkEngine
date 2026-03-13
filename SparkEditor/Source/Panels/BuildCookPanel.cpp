/**
 * @file BuildCookPanel.cpp
 * @brief Build, cook, and package settings panel implementation
 * @author Spark Engine Team
 * @date 2025
 */

#include <iostream>

#include <imgui.h>

#include "BuildCookPanel.h"
#include "../Core/EditorIcons.h"
#include "../../../SparkEngine/Source/Utils/Validate.h"

namespace SparkEditor
{

    BuildCookPanel::BuildCookPanel() : EditorPanel("Build & Cook", "build_cook_panel") {}

    BuildCookPanel::~BuildCookPanel() {}

    bool BuildCookPanel::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        std::cout << "Initializing Build & Cook panel\n";
        ApplyProfileDefaults(BuildProfile::Development);
        return true;
    }

    void BuildCookPanel::Update(float deltaTime)
    {
        // Simulate build progress if running
        if (m_isBuildRunning && m_buildProgress < 1.0f)
        {
            m_buildProgress += deltaTime * 0.05f;
            if (m_buildProgress >= 1.0f)
            {
                m_buildProgress = 1.0f;
                m_isBuildRunning = false;
                m_buildStatus = "Build Complete";
                m_buildLog.push_back({"Build completed successfully!", "info"});
                SPARK_LOG_INFO(Spark::LogCategory::Editor, "Build completed successfully");
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

            if (ImGui::CollapsingHeader(ICON_FA_COG " Build Options", ImGuiTreeNodeFlags_DefaultOpen))
            {
                RenderBuildOptions();
            }

            if (ImGui::CollapsingHeader(ICON_FA_TRASH " Stripping Options"))
            {
                RenderStrippingOptions();
            }

            if (ImGui::CollapsingHeader(ICON_FA_COGS " Asset Cooking"))
            {
                RenderAssetCooking();
            }

            if (ImGui::CollapsingHeader(ICON_FA_DOWNLOAD " Packaging"))
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
        ImGui::Text(ICON_FA_HAMMER " Build Profile");
        ImGui::Spacing();

        float btnWidth = (ImGui::GetContentRegionAvail().x - 12.0f) / 4.0f;
        ImVec2 btnSize(btnWidth, 32.0f);

        auto ProfileButton = [&](const char* label, BuildProfile profile, const ImVec4& color)
        {
            bool active = (m_settings.profile == profile);
            if (active)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, color);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                      ImVec4(color.x + 0.1f, color.y + 0.1f, color.z + 0.1f, 1.0f));
            }
            if (ImGui::Button(label, btnSize))
            {
                m_settings.profile = profile;
                ApplyProfileDefaults(profile);
            }
            if (active)
                ImGui::PopStyleColor(2);
            if (ImGui::IsItemHovered())
            {
                const char* desc = "";
                switch (profile)
                {
                case BuildProfile::Debug:
                    desc = "Full debug symbols, assertions, console, no optimizations.\n"
                           "Best for development and debugging.";
                    break;
                case BuildProfile::Development:
                    desc = "Debug symbols, console, profiler, moderate optimizations.\n"
                           "Good balance of debuggability and performance.";
                    break;
                case BuildProfile::Release:
                    desc = "Optional debug symbols, full optimizations.\n"
                           "Console and profiler optional. Good for QA testing.";
                    break;
                case BuildProfile::Shipping:
                    desc = "No debug symbols, no console, no profiler.\n"
                           "Maximum optimization. For end-user distribution.";
                    break;
                }
                ImGui::SetTooltip("%s", desc);
            }
            ImGui::SameLine();
        };

        ProfileButton(ICON_FA_BUG " Debug", BuildProfile::Debug, ImVec4(0.7f, 0.3f, 0.3f, 1.0f));
        ProfileButton(ICON_FA_CODE " Dev", BuildProfile::Development, ImVec4(0.3f, 0.5f, 0.7f, 1.0f));
        ProfileButton(ICON_FA_CHECK " Release", BuildProfile::Release, ImVec4(0.3f, 0.65f, 0.3f, 1.0f));
        ProfileButton(ICON_FA_ROCKET " Ship", BuildProfile::Shipping, ImVec4(0.8f, 0.55f, 0.1f, 1.0f));
        ImGui::NewLine();

        // Profile summary
        ImVec4 summaryColor(0.6f, 0.6f, 0.6f, 1.0f);
        ImGui::TextColored(summaryColor, "Profile: %s | Platform: %s", GetProfileName(m_settings.profile),
                           GetPlatformName(m_settings.platform));
    }

    void BuildCookPanel::RenderPlatformSelector()
    {
        ImGui::Text(ICON_FA_GLOBE " Target Platform");
        const char* platforms[] = {"Windows x64", "Windows x86", "Linux x64", "macOS x64", "macOS ARM64"};
        int platformIdx = static_cast<int>(m_settings.platform);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##Platform", &platformIdx, platforms, 5))
        {
            m_settings.platform = static_cast<TargetPlatform>(platformIdx);
        }
    }

    void BuildCookPanel::RenderBuildOptions()
    {
        ImGui::Indent(8.0f);

        ImGui::Text("Included Features:");
        ImGui::Checkbox("Debug Symbols (.pdb)", &m_settings.includeDebugSymbols);
        ImGui::Checkbox("Console (SparkConsole)", &m_settings.includeConsole);
        ImGui::Checkbox("Profiler", &m_settings.includeProfiler);
        ImGui::Checkbox("Developer Commands", &m_settings.includeDevCommands);
        ImGui::Checkbox("Log System", &m_settings.includeLogSystem);
        ImGui::Checkbox("Assertions", &m_settings.enableAsserts);
        ImGui::Checkbox("Hot Reload", &m_settings.enableHotReload);
        ImGui::Checkbox("Validation Layers (GPU)", &m_settings.enableValidationLayers);

        ImGui::Separator();
        ImGui::Text("Optimization:");
        const char* optLevels[] = {"None (O0)", "Size (Os)", "Speed (O2)", "Full (O3 + LTO)"};
        ImGui::Combo("Optimization Level", &m_settings.optimizationLevel, optLevels, 4);
        ImGui::Checkbox("Link-Time Optimization (LTO)", &m_settings.enableLTO);
        ImGui::Checkbox("Profile-Guided Optimization (PGO)", &m_settings.enablePGO);

        ImGui::Unindent(8.0f);
    }

    void BuildCookPanel::RenderStrippingOptions()
    {
        ImGui::Indent(8.0f);

        ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f),
                           ICON_FA_EXCLAMATION " Stripping removes features from the build.");
        ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f), "  Use for Shipping builds distributed to end users.");
        ImGui::Spacing();

        ImGui::Text("Strip from build:");
        ImGui::Checkbox("Debug Symbols", &m_settings.stripDebugSymbols);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Remove .pdb files and debug info.\nReduces build size significantly.");

        ImGui::Checkbox("Console (SparkConsole.exe)", &m_settings.stripConsole);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Remove SparkConsole executable.\nEnd users won't have access to the debug console.");

        ImGui::Checkbox("Profiler", &m_settings.stripProfiler);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Remove performance profiling instrumentation.\nSlightly reduces binary size.");

        ImGui::Checkbox("Developer Commands", &m_settings.stripDevCommands);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Remove dev/cheat commands (god, noclip, teleport, etc.).\nPrevents end users from using cheats.");

        ImGui::Checkbox("Log Files", &m_settings.stripLogFiles);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Disable file-based logging.\nReduces disk I/O in shipping builds.");

        ImGui::Checkbox("Editor Assets", &m_settings.stripEditorAssets);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Remove editor-only assets (fonts, icons, themes).\nNot needed in standalone game.");

        ImGui::Checkbox("Unused Assets", &m_settings.stripUnusedAssets);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Analyze asset references and remove unreferenced assets.\nReduces package size.");

        ImGui::Checkbox("Test Content", &m_settings.stripTestContent);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Remove test scenes, placeholder assets, debug levels.");

        ImGui::Unindent(8.0f);
    }

    void BuildCookPanel::RenderAssetCooking()
    {
        ImGui::Indent(8.0f);

        ImGui::Checkbox("Cook Assets", &m_settings.cookAssets);
        if (!m_settings.cookAssets)
        {
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.3f, 1.0f), "Assets will be used as-is (raw format).");
            ImGui::Unindent(8.0f);
            return;
        }

        ImGui::Separator();
        ImGui::Text(ICON_FA_IMAGE " Textures:");
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

        ImGui::Separator();
        ImGui::Text(ICON_FA_VOLUME_UP " Audio:");
        ImGui::Checkbox("Compress Audio", &m_settings.compressAudio);
        if (m_settings.compressAudio)
        {
            ImGui::SliderInt("Audio Quality", &m_settings.audioQuality, 0, 100, "%d%%");
        }

        ImGui::Separator();
        ImGui::Text(ICON_FA_CUBE " Meshes:");
        ImGui::Checkbox("Compress Meshes", &m_settings.compressMeshes);
        ImGui::Checkbox("Generate LODs", &m_settings.generateLODs);
        if (m_settings.generateLODs)
        {
            ImGui::SliderInt("LOD Levels", &m_settings.lodLevels, 1, 6);
        }

        ImGui::Unindent(8.0f);
    }

    void BuildCookPanel::RenderPackagingOptions()
    {
        ImGui::Indent(8.0f);

        ImGui::Text("Output:");
        char outputDir[512];
        strncpy(outputDir, m_settings.outputDirectory.c_str(), sizeof(outputDir) - 1);
        outputDir[sizeof(outputDir) - 1] = '\0';
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##OutputDir", outputDir, sizeof(outputDir)))
        {
            m_settings.outputDirectory = outputDir;
        }

        char exeName[256];
        strncpy(exeName, m_settings.executableName.c_str(), sizeof(exeName) - 1);
        exeName[sizeof(exeName) - 1] = '\0';
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##ExeName", exeName, sizeof(exeName)))
        {
            m_settings.executableName = exeName;
        }

        ImGui::Separator();
        ImGui::Checkbox("Compress Package", &m_settings.compressPackage);
        if (m_settings.compressPackage)
        {
            ImGui::SliderInt("Compression Level", &m_settings.compressionLevel, 1, 9);
        }

        ImGui::Checkbox("Create Installer", &m_settings.createInstaller);

        ImGui::Separator();
        ImGui::Checkbox("Sign Executable", &m_settings.signExecutable);
        if (m_settings.signExecutable)
        {
            char certPath[512];
            strncpy(certPath, m_settings.signingCertificate.c_str(), sizeof(certPath) - 1);
            certPath[sizeof(certPath) - 1] = '\0';
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("Certificate Path", certPath, sizeof(certPath)))
            {
                m_settings.signingCertificate = certPath;
            }
        }

        ImGui::Unindent(8.0f);
    }

    void BuildCookPanel::RenderBuildActions()
    {
        ImGui::Text(ICON_FA_HAMMER " Actions");
        ImGui::Spacing();

        float halfWidth = (ImGui::GetContentRegionAvail().x - 4.0f) / 2.0f;
        ImVec2 btnSize(halfWidth, 36.0f);

        // Build button
        ImVec4 buildColor(0.2f, 0.55f, 0.2f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, buildColor);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.65f, 0.3f, 1.0f));
        bool canBuild = !m_isBuildRunning;
        if (!canBuild)
            ImGui::BeginDisabled();
        if (ImGui::Button(ICON_FA_HAMMER " Build", btnSize))
        {
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "Build started");
            m_isBuildRunning = true;
            m_buildProgress = 0.0f;
            m_buildStatus = "Building...";
            m_buildLog.clear();
            m_buildLog.push_back({"Starting " + std::string(GetProfileName(m_settings.profile)) + " build for " +
                                      GetPlatformName(m_settings.platform) + "...",
                                  "info"});
            if (m_settings.cookAssets)
                m_buildLog.push_back({"Cooking assets...", "info"});
            if (m_settings.stripConsole)
                m_buildLog.push_back({"Stripping: SparkConsole", "warning"});
            if (m_settings.stripDevCommands)
                m_buildLog.push_back({"Stripping: Developer commands", "warning"});
            if (m_settings.stripProfiler)
                m_buildLog.push_back({"Stripping: Profiler", "warning"});
            if (m_settings.stripDebugSymbols)
                m_buildLog.push_back({"Stripping: Debug symbols (.pdb)", "warning"});
        }
        if (!canBuild)
            ImGui::EndDisabled();
        ImGui::PopStyleColor(2);

        ImGui::SameLine();

        // Cook Only button
        if (!canBuild)
            ImGui::BeginDisabled();
        if (ImGui::Button(ICON_FA_FIRE " Cook Only", btnSize))
        {
            m_isBuildRunning = true;
            m_buildProgress = 0.0f;
            m_buildStatus = "Cooking assets...";
            m_buildLog.clear();
            m_buildLog.push_back(
                {"Cooking assets for " + std::string(GetPlatformName(m_settings.platform)) + "...", "info"});
        }
        if (!canBuild)
            ImGui::EndDisabled();

        // Progress bar
        if (m_isBuildRunning || m_buildProgress > 0.0f)
        {
            ImGui::Spacing();
            ImGui::ProgressBar(m_buildProgress, ImVec2(-1, 20));
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s", m_buildStatus.c_str());
        }
    }

    void BuildCookPanel::RenderBuildLog()
    {
        if (ImGui::CollapsingHeader(ICON_FA_TERMINAL " Build Log", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::BeginChild("##BuildLog", ImVec2(0, 120), true);
            for (const auto& entry : m_buildLog)
            {
                ImVec4 color(0.8f, 0.8f, 0.8f, 1.0f);
                const char* prefix = ICON_FA_INFO_CIRCLE;
                if (entry.level == "warning")
                {
                    color = ImVec4(0.9f, 0.7f, 0.2f, 1.0f);
                    prefix = ICON_FA_EXCLAMATION;
                }
                else if (entry.level == "error")
                {
                    color = ImVec4(0.9f, 0.3f, 0.3f, 1.0f);
                    prefix = ICON_FA_TIMES;
                }
                ImGui::TextColored(color, "%s %s", prefix, entry.message.c_str());
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
