/**
 * @file GameModuleSelectorPanel.cpp
 * @brief Game module discovery, metadata probing, separate-process launch, and manifest generation
 */

#include "GameModuleSelectorPanel.h"
#include "Core/ModuleManager.h"
#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"
#include <imgui.h>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

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

        // Auto-refresh every 5 seconds to pick up newly built DLLs. This only
        // runs the safe candidate scan (no DllMain) — metadata probing stays
        // strictly opt-in via the Probe button.
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
        if (m_hasProbed)
            ImGui::Text("Available: %zu  |  Game-kind: %zu", m_modules.size(), gameKindCount);
        else
            ImGui::Text("Available: %zu  |  Game-kind: ? (probe for metadata)", m_modules.size());

        if (ImGui::Button("Refresh"))
            m_needsRefresh = true;
        ImGui::SameLine();
        if (ImGui::Button("Probe Metadata"))
            ProbeModuleMetadata();
        if (ImGui::IsItemHovered())
        {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted("Reads name / version / kind from each DLL's ModuleInfo.\n"
                                   "This briefly loads and unloads each candidate DLL inside the editor\n"
                                   "process (no module init runs). The plain list scan never loads DLLs.");
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
        m_modules.clear();
#ifdef _WIN32
        if (m_gameProcess)
        {
            // Do NOT terminate the game — the launched process is independent.
            CloseHandle(static_cast<HANDLE>(m_gameProcess));
            m_gameProcess = nullptr;
        }
#endif
    }

    std::string GameModuleSelectorPanel::GetExecutableDirectory()
    {
#ifdef _WIN32
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        return std::filesystem::path(exePath).parent_path().string();
#else
        return std::filesystem::canonical("/proc/self/exe").parent_path().string();
#endif
    }

    void GameModuleSelectorPanel::RefreshModuleList()
    {
        SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "GameModuleSelectorPanel: refreshing module list");

        // Preserve user state (selection, probed metadata) across refreshes
        std::vector<ModuleEntry> previous = std::move(m_modules);
        m_modules.clear();

        const std::string scanDir = GetExecutableDirectory();

        // Safe enumeration: name hint + export probe with DONT_RESOLVE_DLL_REFERENCES.
        // Never runs DllMain — cheap enough for the 5-second auto-refresh.
        for (const auto& candidate : ModuleManager::DiscoverModuleCandidates(scanDir))
        {
            ModuleEntry mod;
            mod.path = candidate;
            mod.name = std::filesystem::path(candidate).stem().string();
            mod.version = "?";
            mod.kindLabel = "?";
            mod.kindKnown = false;

            for (const auto& prev : previous)
            {
                if (prev.path == mod.path)
                {
                    mod = prev;
                    break;
                }
            }

            m_modules.push_back(std::move(mod));
        }

        // Re-resolve the launch selection by path (indices may have shifted)
        m_launchSelection = -1;
        if (!m_launchSelectionPath.empty())
        {
            for (size_t i = 0; i < m_modules.size(); ++i)
            {
                if (m_modules[i].path == m_launchSelectionPath)
                {
                    m_launchSelection = static_cast<int>(i);
                    break;
                }
            }
        }

        // NOTE: deliberately no auto re-probe here — probing loads every DLL,
        // and this refresh runs on a 5-second timer. Previously probed entries
        // keep their metadata (matched by path above); newly appearing or
        // rebuilt DLLs show "?" until the user clicks Probe Metadata again.
    }

    void GameModuleSelectorPanel::ProbeModuleMetadata()
    {
        const std::string scanDir = GetExecutableDirectory();

        // DiscoverModules briefly loads each DLL (CreateModule + GetModuleInfo +
        // DestroyModule + FreeLibrary — no OnLoad, no engine context). An empty
        // local ModuleManager suffices; it never loads anything persistently.
        ModuleManager probe;
        const auto discovered = probe.DiscoverModules(scanDir);

        for (auto& mod : m_modules)
        {
            for (const auto& info : discovered)
            {
                if (info.path != mod.path)
                    continue;

                mod.name = info.name;
                mod.version = info.version;
                mod.kindKnown = info.kindKnown;
                if (info.kindKnown)
                {
                    mod.isGameKind = (info.kind == Spark::ModuleKind::Game);
                    mod.kindLabel = mod.isGameKind ? "Game" : "Addon";
                }
                else
                {
                    mod.kindLabel = "?";
                }
                break;
            }
        }

        m_hasProbed = true;
        m_statusMessage = "Probed metadata for " + std::to_string(m_modules.size()) + " module(s)";
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "GameModuleSelectorPanel: %s", m_statusMessage.c_str());
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
                    ImGui::TextDisabled("Kind/version unknown — click 'Probe Metadata'.");
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

#ifdef _WIN32
        ImGui::BeginDisabled(!hasSelection);

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
#else
        ImGui::TextDisabled("Process launch is available on Windows builds only.");
#endif

        if (!m_launchStatus.empty())
        {
            const bool running = m_gameProcess != nullptr;
            ImGui::TextColored(running ? ImVec4(0.3f, 0.9f, 0.3f, 1.0f) : ImVec4(0.75f, 0.75f, 0.75f, 1.0f), "%s",
                               m_launchStatus.c_str());
        }
    }

    void GameModuleSelectorPanel::LaunchGame(bool headless)
    {
#ifdef _WIN32
        if (m_launchSelection < 0 || m_launchSelection >= static_cast<int>(m_modules.size()))
        {
            m_launchStatus = "No module selected";
            return;
        }

        const auto& mod = m_modules[static_cast<size_t>(m_launchSelection)];
        namespace fs = std::filesystem;

        const fs::path exeDir = fs::path(GetExecutableDirectory());
        const fs::path engineExe = exeDir / "SparkEngine.exe";

        std::error_code ec;
        if (!fs::exists(engineExe, ec) || ec)
        {
            m_launchStatus = "SparkEngine.exe not found next to the editor (" + exeDir.string() + ")";
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "GameModuleSelectorPanel: %s", m_launchStatus.c_str());
            return;
        }

        // The engine's -game parser reads up to the FIRST SPACE and does not
        // understand quotes (FindGameModuleFromCmdLine). A DLL path containing
        // spaces must be converted to its 8.3 short form before being passed.
        std::wstring dllArg = fs::path(mod.path).wstring();
        if (dllArg.find(L' ') != std::wstring::npos)
        {
            wchar_t shortBuf[MAX_PATH];
            const DWORD len = GetShortPathNameW(dllArg.c_str(), shortBuf, MAX_PATH);
            if (len == 0 || len >= MAX_PATH || std::wstring_view(shortBuf, len).find(L' ') != std::wstring_view::npos)
            {
                m_launchStatus = "Module path contains spaces and has no short form — the engine's "
                                 "-game parser cannot receive it. Move the build to a space-free path.";
                SPARK_LOG_ERROR(Spark::LogCategory::Editor, "GameModuleSelectorPanel: %s", m_launchStatus.c_str());
                return;
            }
            dllArg.assign(shortBuf, len);
        }

        std::wstring cmd = L"\"" + engineExe.wstring() + L"\" -game " + dllArg;
        if (headless)
            cmd += L" -headless";

        // CreateProcessW may modify the command-line buffer — pass a writable copy
        std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
        cmdBuf.push_back(L'\0');

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};

        const std::wstring workingDir = exeDir.wstring();
        const BOOL ok = CreateProcessW(engineExe.wstring().c_str(), cmdBuf.data(), nullptr, nullptr, FALSE, 0, nullptr,
                                       workingDir.c_str(), &startup, &process);
        if (!ok)
        {
            m_launchStatus = "Launch failed (Win32 error " + std::to_string(GetLastError()) + ")";
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "GameModuleSelectorPanel: %s", m_launchStatus.c_str());
            Spark::SimpleConsole::GetInstance().LogError("[Editor] " + m_launchStatus);
            return;
        }

        CloseHandle(process.hThread);
        if (m_gameProcess)
            CloseHandle(static_cast<HANDLE>(m_gameProcess)); // forget the previous launch (process keeps running)
        m_gameProcess = process.hProcess;
        m_gamePid = process.dwProcessId;

        m_launchStatus = std::string(headless ? "Dedicated (headless)" : "Game") + " running — " + mod.name + ", PID " +
                         std::to_string(m_gamePid);
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "GameModuleSelectorPanel: %s", m_launchStatus.c_str());
        Spark::SimpleConsole::GetInstance().LogSuccess("[Editor] " + m_launchStatus);
#else
        (void)headless;
        m_launchStatus = "Process launch is available on Windows builds only.";
#endif
    }

    void GameModuleSelectorPanel::PollLaunchedProcess()
    {
#ifdef _WIN32
        if (!m_gameProcess)
            return;

        DWORD exitCode = 0;
        if (GetExitCodeProcess(static_cast<HANDLE>(m_gameProcess), &exitCode) && exitCode != STILL_ACTIVE)
        {
            m_launchStatus =
                "Last launch (PID " + std::to_string(m_gamePid) + ") exited with code " + std::to_string(exitCode);
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "GameModuleSelectorPanel: %s", m_launchStatus.c_str());
            CloseHandle(static_cast<HANDLE>(m_gameProcess));
            m_gameProcess = nullptr;
            m_gamePid = 0;
        }
#endif
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
        const std::string manifestDir = GetExecutableDirectory();
        auto manifestPath = std::filesystem::path(manifestDir) / "spark.modules.json";

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

            auto filename = std::filesystem::path(mod.path).filename().string();
            json += "        {\n";
            json += "            \"name\": \"" + mod.name + "\",\n";
            json += "            \"path\": \"" + filename + "\",\n";
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
            m_statusMessage = "Saved spark.modules.json (" + std::to_string(loadOrder - 1000) + " modules)";
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
