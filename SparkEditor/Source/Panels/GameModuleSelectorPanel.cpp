/**
 * @file GameModuleSelectorPanel.cpp
 * @brief Safe module discovery, separate-process launch, and manifest generation
 */

#include "GameModuleSelectorPanel.h"
#include "Core/ModuleManager.h"
#include "Core/ProjectManager.h"
#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"
#include "../Utils/EditorLaunchContext.h"
#include "../Utils/EditorProcessLaunch.h"
#include <imgui.h>
#include <filesystem>
#include <fstream>
#include <vector>

namespace SparkEditor
{

    GameModuleSelectorPanel::GameModuleSelectorPanel() : EditorPanel("Game Module Selector", "GameModuleSelector") {}

    bool GameModuleSelectorPanel::Initialize()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "GameModuleSelectorPanel initialized");
        m_isInitialized = true;
        RefreshModuleList();
        return true;
    }

    void GameModuleSelectorPanel::Update(float deltaTime)
    {
        m_refreshTimer += deltaTime;

        // Auto-refresh every 5 seconds to pick up newly built DLLs. This uses
        // the safe candidate scan and never executes code from an unloaded DLL.
        if (m_refreshTimer >= 5.0f)
        {
            m_refreshTimer = 0.0f;
            m_needsRefresh = true;
        }

        if (m_needsRefresh)
        {
            RefreshModuleList();
            m_needsRefresh = false;
        }

        PollLaunchedProcess();
    }

    void GameModuleSelectorPanel::Render()
    {
        if (!BeginPanel())
        {
            EndPanel();
            return;
        }

        ImGui::Text("Discover game module DLLs and launch the game from the editor.");
        ImGui::Separator();

        // Single-game-module policy banner
        size_t gameKindCount = 0;
        for (const auto& mod : m_modules)
        {
            if (mod.kindKnown && mod.isGameKind)
                gameKindCount++;
        }
        ImGui::TextColored(ImVec4(0.9f, 0.75f, 0.3f, 1.0f), "Policy: exactly ONE Game-kind module loads per process.");
        if (ImGui::IsItemHovered())
        {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted("The engine hard-refuses a second ModuleKind::Game module. Addon-kind\n"
                                   "modules (libraries/extensions with no simulation ownership) may stack\n"
                                   "freely alongside the one game module.");
            ImGui::EndTooltip();
        }
        ImGui::Text("Available: %zu  |  Known Game-kind: %zu", m_modules.size(), gameKindCount);

        if (ImGui::Button("Refresh"))
            m_needsRefresh = true;
        if (ImGui::IsItemHovered())
        {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted("Refreshes candidates without loading DLLs or executing DllMain/factories.\n"
                                   "Unloaded modules use filename/unknown metadata until a sidecar or\n"
                                   "manifest supplies trusted metadata.");
            ImGui::EndTooltip();
        }
        ImGui::SameLine();
        if (ImGui::Button("Save Module Manifest"))
            SaveModuleManifest();

        if (!m_statusMessage.empty())
        {
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1.0f), "%s", m_statusMessage.c_str());
        }

        ImGui::Separator();
        RenderModuleList();

        ImGui::Separator();
        RenderLaunchControls();

        ImGui::Separator();
        RenderManifestControls();

        EndPanel();
    }

    void GameModuleSelectorPanel::Shutdown()
    {
        if (m_gameProcess.IsRunning())
            StopLaunchedProcess();
        m_modules.clear();
        m_isInitialized = false;
    }

    void GameModuleSelectorPanel::RefreshModuleList()
    {
        SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "GameModuleSelectorPanel: refreshing module list");

        // Preserve user state and any trusted metadata across refreshes.
        std::vector<ModuleEntry> previous = std::move(m_modules);
        m_modules.clear();

        const std::filesystem::path editorDirectory = LaunchContext::PathFromUtf8(GetEditorExecutableDirectory());
        const auto scanDirectories = LaunchContext::ModuleDiscoveryDirectories(
            editorDirectory, LaunchContext::PathFromUtf8(ProjectManager::GetActiveProjectPath()));
        const auto candidates = LaunchContext::DiscoverUniqueModules(
            scanDirectories,
            [&](const std::filesystem::path& directory)
            {
                const auto mode = LaunchContext::SamePath(directory, editorDirectory)
                                      ? ModuleManager::DiscoveryMode::ConservativeNameHints
                                      : ModuleManager::DiscoveryMode::CompatibleSidecars;
                const std::u8string utf8 = directory.generic_u8string();
                return ModuleManager::DiscoverModuleCandidates(
                    std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size()), mode);
            });

        // Safe enumeration: sidecar validation never maps the library or runs
        // DllMain. The directory set above is fixed and bounded, so
        // project discovery remains cheap enough for the 5-second auto-refresh.
        for (const auto& candidate : candidates)
        {
            ModuleEntry mod;
            mod.path = LaunchContext::PathToUtf8(candidate);
            mod.name = LaunchContext::PathToUtf8(candidate.stem());
            mod.version = "?";
            mod.kindLabel = "?";
            mod.kindKnown = false;

            for (const auto& prev : previous)
            {
                if (LaunchContext::SamePath(LaunchContext::PathFromUtf8(prev.path),
                                            LaunchContext::PathFromUtf8(mod.path)))
                {
                    mod = prev;
                    mod.path = LaunchContext::PathToUtf8(candidate);
                    break;
                }
            }

            m_modules.push_back(std::move(mod));
        }

        // Re-resolve the launch selection by path (indices may have shifted).
        // If a module was deleted or moved, clear both forms of selection so
        // neither launch surface can hold a stale DLL path.
        m_launchSelection = -1;
        if (!m_launchSelectionPath.empty())
        {
            std::vector<std::filesystem::path> discoveredPaths;
            discoveredPaths.reserve(m_modules.size());
            for (const ModuleEntry& module : m_modules)
                discoveredPaths.push_back(LaunchContext::PathFromUtf8(module.path));

            const auto selected = LaunchContext::FindSelectedModuleIndex(
                discoveredPaths, LaunchContext::PathFromUtf8(m_launchSelectionPath));
            if (selected)
            {
                m_launchSelection = static_cast<int>(*selected);
                m_launchSelectionPath = m_modules[*selected].path;
            }
            else
            {
                m_launchSelectionPath.clear();
            }
        }

        // Previously known metadata is preserved by the path match above.
        // New or rebuilt unloaded DLLs intentionally remain unknown: obtaining
        // ModuleInfo would require calling the C++ factory inside the editor.
    }

    void GameModuleSelectorPanel::RenderModuleList()
    {
        if (m_modules.empty())
        {
            ImGui::TextDisabled("No game modules found next to the editor executable.");
            ImGui::TextDisabled("Build SparkGame / SparkGameMMOFPS to see them here.");
            return;
        }

        ImGui::BeginChild("ModuleList", ImVec2(0, 250), true);

        for (size_t i = 0; i < m_modules.size(); ++i)
        {
            auto& mod = m_modules[i];
            ImGui::PushID(static_cast<int>(i));

            // Radio: which module the Launch buttons use
            if (ImGui::RadioButton("##launch", m_launchSelection == static_cast<int>(i)))
            {
                m_launchSelection = static_cast<int>(i);
                m_launchSelectionPath = mod.path;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Select for launch");
            ImGui::SameLine();

            // Checkbox for manifest selection
            ImGui::Checkbox("##select", &mod.isSelected);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Include in spark.modules.json");
            ImGui::SameLine();

            // Kind tag
            if (!mod.kindKnown)
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "[?]");
            else if (mod.isGameKind)
                ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), "[GAME]");
            else
                ImGui::TextColored(ImVec4(0.4f, 0.7f, 0.95f, 1.0f), "[ADDON]");
            ImGui::SameLine();

            // Module name and version
            ImGui::Text("%s", mod.name.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("v%s", mod.version.c_str());

            // Show path on hover
            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::Text("Path: %s", mod.path.c_str());
                if (!mod.kindKnown)
                    ImGui::TextDisabled("Kind/version unknown — unloaded DLL code is not probed.");
                ImGui::EndTooltip();
            }

            ImGui::PopID();
        }

        ImGui::EndChild();
    }

    void GameModuleSelectorPanel::RenderLaunchControls()
    {
        ImGui::TextUnformatted("Launch");

        const bool hasSelection = m_launchSelection >= 0 && m_launchSelection < static_cast<int>(m_modules.size());
        if (hasSelection)
            ImGui::Text("Selected: %s", m_modules[static_cast<size_t>(m_launchSelection)].name.c_str());
        else
            ImGui::TextDisabled("Select a module (radio button) to enable launching.");

        const bool running = m_gameProcess.IsRunning();
        ImGui::BeginDisabled(!hasSelection || running);

        if (ImGui::Button("Launch Game"))
            LaunchGame(false);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted("Starts SparkEngine.exe -game <module dll> as a SEPARATE process.\n"
                                   "In-editor play is deliberately not offered: module DLLs statically\n"
                                   "link the engine, so per-image globals and type-ids make in-process\n"
                                   "play unsafe.");
            ImGui::EndTooltip();
        }
        ImGui::SameLine();
        if (ImGui::Button("Launch Dedicated (Headless)"))
            LaunchGame(true);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted("Starts SparkEngine.exe -game <module dll> -headless — a dedicated\n"
                                   "server with no window. Watch server.log / the console for output.");
            ImGui::EndTooltip();
        }

        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled(!running);
        if (ImGui::Button("Stop"))
            StopLaunchedProcess();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted("Requests a normal game-window close, then terminates the complete\n"
                                   "owned process tree if it does not exit within the grace period.");
            ImGui::EndTooltip();
        }
        ImGui::EndDisabled();

        if (!m_launchStatus.empty())
        {
            ImGui::TextColored(m_gameProcess.IsRunning() ? ImVec4(0.3f, 0.9f, 0.3f, 1.0f)
                                                         : ImVec4(0.75f, 0.75f, 0.75f, 1.0f),
                               "%s", m_launchStatus.c_str());
        }
    }

    void GameModuleSelectorPanel::LaunchGame(bool headless)
    {
        if (m_gameProcess.IsRunning())
        {
            m_launchStatus = "A game process is already running (PID " + std::to_string(m_gameProcess.GetPid()) +
                             "); stop it before launching another";
            return;
        }

        if (m_launchSelection < 0 || m_launchSelection >= static_cast<int>(m_modules.size()))
        {
            m_launchStatus = "No module selected";
            return;
        }

        const auto& mod = m_modules[static_cast<size_t>(m_launchSelection)];
        namespace fs = std::filesystem;
        const fs::path modulePath = LaunchContext::PathFromUtf8(mod.path);
        if (const std::string moduleError = LaunchContext::ValidateGameModuleForLaunch(modulePath);
            !moduleError.empty())
        {
            m_launchSelection = -1;
            m_launchSelectionPath.clear();
            m_launchStatus = moduleError;
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "GameModuleSelectorPanel: %s", m_launchStatus.c_str());
            return;
        }

        fs::path engineExe;
        std::string findError;
        if (!FindEngineExecutable(engineExe, findError))
        {
            m_launchStatus = findError;
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "GameModuleSelectorPanel: %s", m_launchStatus.c_str());
            return;
        }
        const fs::path workingDir = LaunchContext::ResolveWorkingDirectory(
            LaunchContext::PathFromUtf8(ProjectManager::GetActiveProjectPath()), modulePath, engineExe);
        if (workingDir.empty())
        {
            m_launchStatus = "No valid project, module, or engine launch directory is available";
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "GameModuleSelectorPanel: %s", m_launchStatus.c_str());
            return;
        }

        // Shared CreateProcessW path (also used by PlayControlPanel).
        fs::path manifestPath;
        std::error_code manifestError;
        const fs::path workingManifest = workingDir / "spark.modules.json";
        if (fs::is_regular_file(workingManifest, manifestError) && !manifestError)
            manifestPath = workingManifest;

        std::string buildError;
        const std::wstring cmd =
            BuildGameLaunchCommandLine(engineExe, modulePath, headless, {}, manifestPath, L"", buildError);
        if (cmd.empty() && !buildError.empty())
        {
            m_launchStatus = buildError;
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "GameModuleSelectorPanel: %s", m_launchStatus.c_str());
            return;
        }

        const ProcessLaunchResult launch = LaunchOwnedEditorProcess(engineExe, cmd, workingDir);
        if (!launch.success)
        {
            m_launchStatus = launch.error;
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "GameModuleSelectorPanel: %s", m_launchStatus.c_str());
            Spark::SimpleConsole::GetInstance().LogError("[Editor] " + m_launchStatus);
            return;
        }

        if (!m_gameProcess.Adopt(launch))
        {
            m_launchStatus = "Launch succeeded but process ownership could not be established";
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "GameModuleSelectorPanel: %s", m_launchStatus.c_str());
            return;
        }

        m_launchStatus = std::string(headless ? "Dedicated (headless)" : "Game") + " running — " + mod.name + ", PID " +
                         std::to_string(m_gameProcess.GetPid());
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "GameModuleSelectorPanel: %s", m_launchStatus.c_str());
        Spark::SimpleConsole::GetInstance().LogSuccess("[Editor] " + m_launchStatus);
    }

    void GameModuleSelectorPanel::PollLaunchedProcess()
    {
        if (!m_gameProcess.IsRunning())
            return;

        const unsigned long pid = m_gameProcess.GetPid();
        unsigned long exitCode = 0;
        if (m_gameProcess.Poll(exitCode))
        {
            m_launchStatus =
                "Last launch (PID " + std::to_string(pid) + ") exited with code " + std::to_string(exitCode);
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "GameModuleSelectorPanel: %s", m_launchStatus.c_str());
        }
    }

    void GameModuleSelectorPanel::StopLaunchedProcess()
    {
        const unsigned long pid = m_gameProcess.GetPid();
        switch (m_gameProcess.Stop())
        {
        case EditorProcessStopResult::NotRunning:
            m_launchStatus = "No game process is running";
            break;
        case EditorProcessStopResult::Graceful:
            m_launchStatus = "Game process exited during the stop grace period (PID " + std::to_string(pid) + ")";
            break;
        case EditorProcessStopResult::Terminated:
            m_launchStatus = "Game process tree terminated after grace period (PID " + std::to_string(pid) + ")";
            break;
        case EditorProcessStopResult::Failed:
            m_launchStatus = "Failed to stop game process tree (PID " + std::to_string(pid) + ")";
            break;
        }
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "GameModuleSelectorPanel: %s", m_launchStatus.c_str());
    }

    void GameModuleSelectorPanel::RenderManifestControls()
    {
        ImGui::TextWrapped("The spark.modules.json manifest controls which modules load on startup. "
                           "Select modules above (checkbox) and click 'Save Module Manifest' to persist your choice. "
                           "The engine will load only the selected modules on next launch — at most ONE of them "
                           "may be Game-kind.");

        ImGui::Spacing();
        ImGui::TextDisabled("Tip: Use -game <path> on the command line to override the manifest.");
    }

    void GameModuleSelectorPanel::SaveModuleManifest()
    {
        const std::filesystem::path manifestPath = LaunchContext::ResolveContextFile(
            LaunchContext::PathFromUtf8(ProjectManager::GetActiveProjectPath()),
            LaunchContext::PathFromUtf8(GetEditorExecutableDirectory()), "spark.modules.json");
        if (manifestPath.empty())
        {
            m_statusMessage = "No valid project or editor directory for spark.modules.json";
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "GameModuleSelectorPanel: %s", m_statusMessage.c_str());
            return;
        }

        // Build JSON manually (no dependency on a JSON library)
        std::string json = "{\n    \"modules\": [\n";
        bool first = true;
        int loadOrder = 1000;

        for (const auto& mod : m_modules)
        {
            if (!mod.isSelected)
                continue;

            if (!first)
                json += ",\n";
            first = false;

            const std::string moduleReference = LaunchContext::ManifestModuleReference(
                manifestPath.parent_path(), LaunchContext::PathFromUtf8(mod.path));
            json += "        {\n";
            json += "            \"name\": \"" + mod.name + "\",\n";
            json += "            \"path\": \"" + moduleReference + "\",\n";
            json += "            \"loadOrder\": " + std::to_string(loadOrder) + "\n";
            json += "        }";
            loadOrder++;
        }

        json += "\n    ]\n}\n";

        std::ofstream file(manifestPath);
        if (file.is_open())
        {
            file << json;
            file.close();
            m_statusMessage = "Saved " + LaunchContext::PathToUtf8(manifestPath) + " (" +
                              std::to_string(loadOrder - 1000) + " modules)";
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "GameModuleSelectorPanel: %s", m_statusMessage.c_str());

            auto& console = Spark::SimpleConsole::GetInstance();
            console.LogSuccess("[Editor] " + m_statusMessage);
        }
        else
        {
            m_statusMessage = "Failed to write spark.modules.json";
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "GameModuleSelectorPanel: %s", m_statusMessage.c_str());
            auto& console = Spark::SimpleConsole::GetInstance();
            console.LogError("[Editor] " + m_statusMessage);
        }
    }

} // namespace SparkEditor
